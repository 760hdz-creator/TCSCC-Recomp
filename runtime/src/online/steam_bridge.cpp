// Steam in a helper process (steam_net.h; ported from the TTT2 port). The game never loads Steam itself:
// Steam Input hides every controller from the process that initialises SteamAPI as app 480, while
// processes Steam has not hooked keep seeing the pads.
//
// Start() runs this exe again as `<exe> --steam-bridge <in> <out>` with two anonymous pipes of its own
// for the frames (their handle values on the command line); its stdout = stderr = steam_helper.log next
// to the exe (Steam's libraries print to stdout: never on the frame pipes). The helper (RunBridge) runs
// steam_net.cpp's in-process code and relays. It dies with the game: EOF on its frame pipe, and a
// kill-on-close job as a backstop. A helper lost while Steam was up is started again (Restart).
//
// Frames on both pipes: u32 size (type + payload), u8 type, payload; integers little-endian.
//   game -> helper: SEND u64 peer, u8 reliable, data | PUBLISH u64 session, record | REMOVE u64 session |
//                   LIST u32 request, i32 max, i32 timeoutMs | FRIENDS u32 request |
//                   INVITE u32 request, u64 session, u64 friend
//   helper -> game: READY u8 ok, u64 steamId, persona name | RECV u64 peer, u8 reliable, data |
//                   LIST_RESULT u32 request, u32 n, n x (u32 size, record) | JOIN record |
//                   FRIENDS_RESULT u32 request, u32 n, n x (u64 id, u8 in game, u16 name size, name) |
//                   INVITE_RESULT u32 request, u8 sent
#include <windows.h>
#include <io.h>
#include "steam_net.h"
#include <runtime.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace steamnet {
namespace {

enum : uint8_t { kSend = 1, kPublish = 2, kRemove = 3, kList = 4, kFriends = 7, kInvite = 8,
                 kReady = 101, kRecv = 102, kListResult = 103, kJoin = 104, kFriendsResult = 106, kInviteResult = 107 };
constexpr uint32_t kMaxFrame = 16u << 20;

void Put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void Put64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }
uint32_t Get32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t Get64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

bool ReadAll(HANDLE h, void* buf, size_t n)
{
    auto* p = static_cast<uint8_t*>(buf);
    while (n)
    {
        DWORD got = 0;
        if (!ReadFile(h, p, DWORD(n), &got, nullptr) || !got)
            return false;
        p += got;
        n -= got;
    }
    return true;
}

bool ReadFrame(HANDLE h, uint8_t& type, std::vector<uint8_t>& payload)
{
    uint8_t head[5];
    if (!ReadAll(h, head, 5))
        return false;
    const uint32_t size = Get32(head);
    if (size == 0 || size > kMaxFrame)
        return false;
    type = head[4];
    payload.resize(size - 1);
    return size == 1 || ReadAll(h, payload.data(), payload.size());
}

// One writer per pipe; frames from several threads never interleave.
struct Writer
{
    HANDLE h = INVALID_HANDLE_VALUE;
    std::mutex m;

    bool WriteAll(const void* buf, size_t n)
    {
        auto* p = static_cast<const uint8_t*>(buf);
        while (n)
        {
            DWORD put = 0;
            if (!WriteFile(h, p, DWORD(n), &put, nullptr) || !put)
                return false;
            p += put;
            n -= put;
        }
        return true;
    }
    bool Frame(uint8_t type, const void* a, size_t an, const void* b = nullptr, size_t bn = 0)
    {
        std::lock_guard l(m);
        if (h == INVALID_HANDLE_VALUE || 1 + an + bn > kMaxFrame)
            return false;
        uint8_t head[5];
        Put32(head, uint32_t(1 + an + bn));
        head[4] = type;
        return WriteAll(head, 5) && (!an || WriteAll(a, an)) && (!bn || WriteAll(b, bn));
    }
};

Writer g_out; // game: to the helper; helper: to the game

// ---- game side
bool g_inproc = false; // SCDA_STEAM_INPROC=1
std::atomic<bool> g_running{ false };
std::atomic<uint64_t> g_local{ 0 };
std::string g_localName; // under g_mutex
Receiver g_receiver = nullptr;
HANDLE g_in = INVALID_HANDLE_VALUE; // from the helper
HANDLE g_helper = nullptr;
std::atomic<uint32_t> g_generation{ 0 }; // the helper started last; an older one's reader leaves no trace
std::atomic<bool> g_restarting{ false };
std::mutex g_launchMutex; // one launch at a time
std::mutex g_hostedMutex;
std::map<uint64_t, std::vector<uint8_t>> g_hostedRecords; // what this game hosts: published again to a new helper
std::mutex g_mutex;
std::condition_variable g_cv;
int g_ready = 0; // 0 waiting, 1 Steam up, -1 failed
bool g_readyFrame = false;
uint32_t g_nextRequest = 1;
std::map<uint32_t, std::vector<std::vector<uint8_t>>> g_lists;
std::deque<std::vector<uint8_t>> g_joins;
std::map<uint32_t, std::vector<FriendInfo>> g_friendLists;
std::map<uint32_t, bool> g_invites;
std::chrono::steady_clock::time_point g_launched;

const char* ExitMeaning(DWORD code)
{
    switch (code)
    {
    case 0: return "it exited normally";
    case 1: return "Steam did not start in it";
    case 2: return "it had no pipes";
    case STILL_ACTIVE: return "it is still running but closed its pipe";
    case 0xC0000005: return "a crash: access violation";
    case 0xC0000409: return "a crash: fail-fast / stack buffer overrun";
    case 0x80000003: return "a crash: breakpoint (an assertion)";
    case 0xE06D7363: return "a crash: unhandled C++ exception";
    default: return "unknown";
    }
}

void Restart();

void Reader(HANDLE in, uint32_t generation)
{
    uint8_t type = 0;
    std::vector<uint8_t> p;
    while (ReadFrame(in, type, p))
    {
        if (type == kRecv && p.size() >= 9)
        {
            if (g_receiver)
                g_receiver(Get64(p.data()), p[8] != 0, p.data() + 9, p.size() - 9);
        }
        else if (type == kReady && p.size() >= 9)
        {
            std::lock_guard l(g_mutex);
            g_ready = p[0] ? 1 : -1;
            g_readyFrame = true;
            g_local = Get64(p.data() + 1);
            g_localName.assign(p.begin() + 9, p.end());
            g_cv.notify_all();
        }
        else if (type == kListResult && p.size() >= 8)
        {
            std::vector<std::vector<uint8_t>> records;
            const uint32_t n = Get32(p.data() + 4);
            size_t at = 8;
            for (uint32_t i = 0; i < n && at + 4 <= p.size(); ++i)
            {
                const uint32_t size = Get32(p.data() + at);
                at += 4;
                if (at + size > p.size())
                    break;
                records.emplace_back(p.begin() + at, p.begin() + at + size);
                at += size;
            }
            std::lock_guard l(g_mutex);
            g_lists[Get32(p.data())] = std::move(records);
            g_cv.notify_all();
        }
        else if (type == kJoin)
        {
            std::lock_guard l(g_mutex);
            g_joins.push_back(p);
        }
        else if (type == kFriendsResult && p.size() >= 8)
        {
            std::vector<FriendInfo> list;
            const uint32_t n = Get32(p.data() + 4);
            size_t at = 8;
            for (uint32_t i = 0; i < n && i < 200 && at + 11 <= p.size(); ++i)
            {
                FriendInfo f;
                f.id = Get64(p.data() + at);
                f.inGame = p[at + 8] != 0;
                const size_t len = size_t(p[at + 9]) | size_t(p[at + 10]) << 8;
                at += 11;
                if (len > 128 || at + len > p.size())
                    break;
                f.name.assign(reinterpret_cast<const char*>(p.data() + at), len);
                at += len;
                list.push_back(std::move(f));
            }
            std::lock_guard l(g_mutex);
            g_friendLists[Get32(p.data())] = std::move(list);
            g_cv.notify_all();
        }
        else if (type == kInviteResult && p.size() >= 5)
        {
            std::lock_guard l(g_mutex);
            g_invites[Get32(p.data())] = p[4] != 0;
            g_cv.notify_all();
        }
    }
    CloseHandle(in);
    if (generation != g_generation.load())
        return; // a helper replaced meanwhile
    const bool wasUp = g_running.exchange(false);
    {
        std::lock_guard l(g_mutex);
        if (!g_ready)
            g_ready = -1;
        g_cv.notify_all();
    }
    DWORD code = STILL_ACTIVE;
    if (g_helper)
    {
        WaitForSingleObject(g_helper, 2000);
        GetExitCodeProcess(g_helper, &code);
    }
    if (!wasUp)
        return; // it never came up: its launch reports that
    RuntimeLog("[steam] helper process ended after %.0f s (exit code 0x%08lX: %s) - Steam online stops until a new helper is up\n",
               std::chrono::duration<double>(std::chrono::steady_clock::now() - g_launched).count(), code, ExitMeaning(code));
    if (!g_restarting.exchange(true))
        std::thread(Restart).detach();
}

bool Launch()
{
    wchar_t self[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH))
        return false;
    SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
    HANDLE childIn = nullptr, toChild = nullptr, fromChild = nullptr, childOut = nullptr;
    if (!CreatePipe(&childIn, &toChild, &sa, 1u << 20))
        return false;
    if (!CreatePipe(&fromChild, &childOut, &sa, 1u << 20))
    {
        CloseHandle(childIn);
        CloseHandle(toChild);
        return false;
    }
    SetHandleInformation(toChild, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(fromChild, HANDLE_FLAG_INHERIT, 0);
    // The helper's log: steam_helper.log next to the exe (appended; each start writes a header line).
    std::wstring logPath(self);
    logPath = logPath.substr(0, logPath.find_last_of(L"\\/") + 1) + L"steam_helper.log";
    HANDLE childLog = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (childLog == INVALID_HANDLE_VALUE)
        childLog = nullptr;

    // Inherit exactly these handles, nothing else the game has open.
    HANDLE inherit[3] = { childIn, childOut, childLog };
    const DWORD inheritCount = childLog ? 3 : 2;
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    const bool attrsOk = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                         UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, inheritCount * sizeof(HANDLE), nullptr,
                                                   nullptr);
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nullptr;
    si.StartupInfo.hStdOutput = childLog;
    si.StartupInfo.hStdError = childLog;
    if (attrsOk)
        si.lpAttributeList = attrs;
    wchar_t pipes[64];
    std::swprintf(pipes, 64, L" %llX %llX", (unsigned long long)uintptr_t(childIn), (unsigned long long)uintptr_t(childOut));
    std::wstring line = L"\"" + std::wstring(self) + L"\" --steam-bridge" + pipes;
    PROCESS_INFORMATION pi = {};
    const BOOL created = CreateProcessW(self, line.data(), nullptr, nullptr, TRUE,
                                        CREATE_SUSPENDED | CREATE_NO_WINDOW | (attrsOk ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr, nullptr,
                                        &si.StartupInfo, &pi);
    if (attrsOk)
        DeleteProcThreadAttributeList(attrs);
    CloseHandle(childIn);
    CloseHandle(childOut);
    if (childLog)
        CloseHandle(childLog);
    if (!created)
    {
        RuntimeLog("[steam] could not start the helper process (error %lu)\n", GetLastError());
        CloseHandle(toChild);
        CloseHandle(fromChild);
        return false;
    }
    // Backstop: the helper is killed when the game goes, however it goes.
    if (HANDLE job = CreateJobObjectW(nullptr, nullptr))
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit = {};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof limit);
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_launched = std::chrono::steady_clock::now();
    g_helper = pi.hProcess;
    {
        std::lock_guard l(g_out.m);
        g_out.h = toChild;
    }
    g_in = fromChild;
    return true;
}

// The last helper's process and pipes go (it is ended if it still runs: two helpers would share the Steam id).
void DropHelper()
{
    {
        std::lock_guard l(g_out.m);
        if (g_out.h != INVALID_HANDLE_VALUE)
            CloseHandle(g_out.h);
        g_out.h = INVALID_HANDLE_VALUE;
    }
    if (g_helper)
    {
        if (WaitForSingleObject(g_helper, 0) == WAIT_TIMEOUT)
        {
            TerminateProcess(g_helper, 1);
            WaitForSingleObject(g_helper, 5000);
        }
        CloseHandle(g_helper);
        g_helper = nullptr;
    }
}

// Starts a helper and waits for Steam in it (20 s at most). False: no Steam now.
bool LaunchHelper(const char* why)
{
    std::lock_guard launch(g_launchMutex);
    const uint32_t generation = ++g_generation;
    DropHelper();
    {
        std::lock_guard l(g_mutex);
        g_ready = 0;
        g_readyFrame = false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    if (!Launch())
        return false;
    std::thread(Reader, g_in, generation).detach();
    {
        std::unique_lock w(g_mutex);
        if (!g_cv.wait_for(w, std::chrono::seconds(20), [] { return g_ready != 0; }) || g_ready != 1)
        {
            RuntimeLog("[steam] Steam not available (helper %s; see steam_helper.log)\n",
                       !g_ready ? "timed out" : g_readyFrame ? "reported no Steam" : "ended before Steam was up");
            w.unlock();
            DropHelper();
            return false;
        }
    }
    g_running = true;
    RuntimeLog("[steam] Steam runs in helper process %lu (Steam id %016llX), %s in %.1f s\n", GetProcessId(g_helper),
               (unsigned long long)g_local.load(), why, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    // The lobbies this game hosts: the old helper's Steam lobbies went with it.
    std::lock_guard h(g_hostedMutex);
    for (const auto& [session, record] : g_hostedRecords)
    {
        uint8_t head[8];
        Put64(head, session);
        g_out.Frame(kPublish, head, 8, record.data(), record.size());
    }
    return true;
}

// The helper was lost while Steam was up: new ones are started, 2 s, then 10, 30, 60 and every 120 s.
void Restart()
{
    static const int kDelays[] = { 2, 10, 30, 60, 120 };
    for (int attempt = 0;; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::seconds(kDelays[(std::min)(attempt, 4)]));
        if (LaunchHelper("started again"))
            break;
    }
    g_restarting = false;
}

uint32_t NextRequest()
{
    std::lock_guard l(g_mutex);
    return g_nextRequest++;
}

} // namespace

bool Start(Receiver receiver)
{
    static std::mutex m;
    std::lock_guard l(m);
    if (Running())
        return true;
    if (const char* e = std::getenv("SCDA_STEAM_INPROC"); e && *e == '1')
    {
        g_inproc = true;
        RuntimeLog("[steam] SCDA_STEAM_INPROC=1: Steam in the game process (controllers will be hidden)\n");
        return direct::Start(receiver);
    }
    g_receiver = receiver;
    return LaunchHelper("started");
}

bool Running() { return g_inproc ? direct::Running() : g_running.load(); }
uint64_t LocalId() { return g_inproc ? direct::LocalId() : g_local.load(); }
std::string LocalName()
{
    if (g_inproc)
        return direct::LocalName();
    std::lock_guard l(g_mutex);
    return g_localName;
}

bool Send(uint64_t peer, bool reliable, const void* data, size_t size)
{
    if (g_inproc)
        return direct::Send(peer, reliable, data, size);
    if (!g_running)
        return false;
    uint8_t head[9];
    Put64(head, peer);
    head[8] = reliable ? 1 : 0;
    return g_out.Frame(kSend, head, 9, data, size);
}

void PublishLobby(uint64_t sessionId, const std::vector<uint8_t>& record)
{
    if (g_inproc)
        return direct::PublishLobby(sessionId, record);
    {
        std::lock_guard l(g_hostedMutex);
        g_hostedRecords[sessionId] = record; // a new helper publishes it again
    }
    if (!g_running)
        return;
    uint8_t head[8];
    Put64(head, sessionId);
    g_out.Frame(kPublish, head, 8, record.data(), record.size());
}

void RemoveLobby(uint64_t sessionId)
{
    if (g_inproc)
        return direct::RemoveLobby(sessionId);
    {
        std::lock_guard l(g_hostedMutex);
        g_hostedRecords.erase(sessionId);
    }
    if (!g_running)
        return;
    uint8_t head[8];
    Put64(head, sessionId);
    g_out.Frame(kRemove, head, 8);
}

std::vector<std::vector<uint8_t>> ListLobbies(int max, int timeoutMs)
{
    if (g_inproc)
        return direct::ListLobbies(max, timeoutMs);
    std::vector<std::vector<uint8_t>> out;
    if (!g_running)
        return out;
    const uint32_t request = NextRequest();
    uint8_t body[12];
    Put32(body, request);
    Put32(body + 4, uint32_t(max));
    Put32(body + 8, uint32_t(timeoutMs));
    if (!g_out.Frame(kList, body, 12))
        return out;
    std::unique_lock l(g_mutex);
    g_cv.wait_for(l, std::chrono::milliseconds(timeoutMs + 3000), [&] { return g_lists.count(request) || !g_running; });
    if (const auto it = g_lists.find(request); it != g_lists.end())
    {
        out = std::move(it->second);
        g_lists.erase(it);
    }
    return out;
}

std::vector<FriendInfo> Friends(int timeoutMs)
{
    if (g_inproc)
        return direct::Friends();
    if (!g_running)
        return {};
    const uint32_t request = NextRequest();
    uint8_t body[4];
    Put32(body, request);
    if (!g_out.Frame(kFriends, body, 4))
        return {};
    std::unique_lock l(g_mutex);
    g_cv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return g_friendLists.count(request) || !g_running; });
    std::vector<FriendInfo> out;
    if (const auto it = g_friendLists.find(request); it != g_friendLists.end())
    {
        out = std::move(it->second);
        g_friendLists.erase(it);
    }
    return out;
}

bool InviteToSession(uint64_t sessionId, uint64_t friendId, int timeoutMs)
{
    if (g_inproc)
        return direct::InviteToSession(sessionId, friendId);
    if (!g_running)
        return false;
    const uint32_t request = NextRequest();
    uint8_t body[20];
    Put32(body, request);
    Put64(body + 4, sessionId);
    Put64(body + 12, friendId);
    if (!g_out.Frame(kInvite, body, 20))
        return false;
    std::unique_lock l(g_mutex);
    g_cv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return g_invites.count(request) || !g_running; });
    const auto it = g_invites.find(request);
    if (it == g_invites.end())
        return false;
    const bool sent = it->second;
    g_invites.erase(it);
    return sent;
}

bool TakeJoinRequest(std::vector<uint8_t>& record)
{
    if (g_inproc)
        return direct::TakeJoinRequest(record);
    std::lock_guard l(g_mutex);
    if (g_joins.empty())
        return false;
    record = std::move(g_joins.front());
    g_joins.pop_front();
    return true;
}

// ---- helper side
namespace {
// Crash reports: the module + offset of a crash, and abort() / std::terminate, into the helper's log.
void ModuleOf(const void* addr, char* out, size_t n, uintptr_t& offset)
{
    HMODULE mod = nullptr;
    offset = uintptr_t(addr);
    out[0] = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(addr),
                           &mod) &&
        mod)
    {
        wchar_t path[MAX_PATH];
        const DWORD len = GetModuleFileNameW(mod, path, MAX_PATH);
        const wchar_t* name = path;
        for (DWORD i = 0; i < len; ++i)
            if (path[i] == L'\\' || path[i] == L'/')
                name = path + i + 1;
        std::snprintf(out, n, "%ls", name);
        offset = uintptr_t(addr) - uintptr_t(mod);
    }
}
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* e)
{
    static std::atomic<bool> once{ false };
    if (!once.exchange(true))
    {
        char mod[64];
        uintptr_t off = 0;
        ModuleOf(e->ExceptionRecord->ExceptionAddress, mod, sizeof(mod), off);
        std::fprintf(stderr, "[steam-helper] crashed: exception 0x%08lX at %s+0x%llX\n", e->ExceptionRecord->ExceptionCode, mod[0] ? mod : "?",
                     (unsigned long long)off);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
void InstallCrashReports()
{
    SetUnhandledExceptionFilter(CrashFilter);
    std::signal(SIGABRT, [](int) { std::fprintf(stderr, "[steam-helper] abort() called\n"); });
    std::set_terminate([] {
        std::fprintf(stderr, "[steam-helper] std::terminate (an unhandled C++ exception)\n");
        std::abort();
    });
}
} // namespace

int RunBridge(int argc, char** argv)
{
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(stderr, "[steam-helper] started %04u-%02u-%02u %02u:%02u:%02u, pid %lu\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                 t.wSecond, GetCurrentProcessId());
    InstallCrashReports();
    HANDLE in = nullptr;
    if (argc >= 4)
    {
        in = HANDLE(uintptr_t(std::strtoull(argv[2], nullptr, 16)));
        g_out.h = HANDLE(uintptr_t(std::strtoull(argv[3], nullptr, 16)));
    }
    if (!in || in == INVALID_HANDLE_VALUE || !g_out.h || g_out.h == INVALID_HANDLE_VALUE)
        return 2;
    // SCDA_STEAM_HELPER_FAKE=1 (tests of the bridge itself, no Steam touched): READY with a made-up Steam id,
    // sends dropped, lobbies only logged, searches empty.
    const char* fakeEnv = std::getenv("SCDA_STEAM_HELPER_FAKE");
    const bool fake = fakeEnv && *fakeEnv == '1';
    constexpr uint64_t kFakeId = 0x0110000100000F0Full;
    bool ok = fake;
    if (fake)
        std::fprintf(stderr, "[steam-helper] fake Steam (SCDA_STEAM_HELPER_FAKE=1)\n");
    else
        ok = direct::Start([](uint64_t from, bool reliable, const uint8_t* data, size_t size) {
            uint8_t head[9];
            Put64(head, from);
            head[8] = reliable ? 1 : 0;
            g_out.Frame(kRecv, head, 9, data, size);
        });
    InstallCrashReports(); // again: Steam may have put its own filter in
    uint8_t ready[9] = { uint8_t(ok ? 1 : 0) };
    Put64(ready + 1, ok ? (fake ? kFakeId : direct::LocalId()) : 0);
    const std::string name = ok && !fake ? direct::LocalName() : std::string(fake ? "FakeSteam" : "");
    g_out.Frame(kReady, ready, 9, name.data(), name.size());
    if (!ok)
        return 1;
    std::thread([] {
        for (;;)
        {
            std::vector<uint8_t> record;
            if (direct::TakeJoinRequest(record))
                g_out.Frame(kJoin, record.data(), record.size());
            Sleep(50);
        }
    }).detach();
    uint8_t type = 0;
    std::vector<uint8_t> p;
    while (ReadFrame(in, type, p))
    {
        if (type == kSend && p.size() >= 9)
            direct::Send(Get64(p.data()), p[8] != 0, p.data() + 9, p.size() - 9);
        else if (type == kPublish && p.size() >= 8)
        {
            if (fake)
                std::fprintf(stderr, "[steam-helper] fake: lobby of session %016llX published\n", (unsigned long long)Get64(p.data()));
            direct::PublishLobby(Get64(p.data()), std::vector<uint8_t>(p.begin() + 8, p.end()));
        }
        else if (type == kRemove && p.size() >= 8)
            direct::RemoveLobby(Get64(p.data()));
        else if (type == kFriends && p.size() >= 4)
        {
            const uint32_t request = Get32(p.data());
            std::vector<uint8_t> body(8);
            Put32(body.data(), request);
            const std::vector<FriendInfo> list = direct::Friends();
            Put32(body.data() + 4, uint32_t(list.size()));
            for (const FriendInfo& f : list)
            {
                const size_t len = (std::min)(f.name.size(), size_t(128)), at = body.size();
                body.resize(at + 11 + len);
                Put64(body.data() + at, f.id);
                body[at + 8] = f.inGame ? 1 : 0;
                body[at + 9] = uint8_t(len);
                body[at + 10] = uint8_t(len >> 8);
                if (len)
                    std::memcpy(body.data() + at + 11, f.name.data(), len);
            }
            g_out.Frame(kFriendsResult, body.data(), body.size());
        }
        else if (type == kInvite && p.size() >= 20)
        {
            uint8_t body[5];
            Put32(body, Get32(p.data()));
            body[4] = direct::InviteToSession(Get64(p.data() + 4), Get64(p.data() + 12)) ? 1 : 0;
            g_out.Frame(kInviteResult, body, 5);
        }
        else if (type == kList && p.size() >= 12)
        {
            const uint32_t request = Get32(p.data());
            const int max = int(Get32(p.data() + 4)), timeoutMs = int(Get32(p.data() + 8));
            std::thread([request, max, timeoutMs] {
                const auto records = direct::ListLobbies(max, timeoutMs);
                std::vector<uint8_t> body(8);
                Put32(body.data(), request);
                Put32(body.data() + 4, uint32_t(records.size()));
                for (const auto& r : records)
                {
                    const size_t at = body.size();
                    body.resize(at + 4 + r.size());
                    Put32(body.data() + at, uint32_t(r.size()));
                    if (!r.empty())
                        std::memcpy(body.data() + at + 4, r.data(), r.size());
                }
                g_out.Frame(kListResult, body.data(), body.size());
            }).detach();
        }
    }
    // The game closed its end (it exited or crashed): Steam leaves the lobbies with this process.
    std::fprintf(stderr, "[steam-helper] the game closed its pipe - the helper exits\n");
    return 0;
}

} // namespace steamnet
