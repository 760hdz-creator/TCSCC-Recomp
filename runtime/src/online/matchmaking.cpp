// Session discovery (matchmaking.h), ported from the TTT2 port's matchmaking.cpp without its community
// server / Discord parts.
#include <windows.h>
#include "matchmaking.h"
#include "net_transport.h"
#include "steam_net.h"
#include <runtime.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

namespace mm {
namespace {

constexpr uint32_t kMagic = 0x314C4353u; // "SCL1"
constexpr int kStaleSeconds = 10;

std::mutex g_mutex;
std::map<uint64_t, SessionRecord> g_owned;
std::map<uint64_t, SessionRecord> g_seen; // listed or invited sessions, for Find
std::deque<SessionRecord> g_invites;
bool g_heartbeat = false;

bool Steam() { return std::strcmp(net::BackendName(), "steam") == 0; }

std::filesystem::path Folder() { return std::filesystem::temp_directory_path() / L"TCSCCRecomp" / L"sessions"; }
std::filesystem::path FileOf(uint64_t id)
{
    wchar_t name[32];
    swprintf(name, 32, L"%016llX.ses", static_cast<unsigned long long>(id));
    return Folder() / name;
}

void Put32(std::vector<uint8_t>& v, uint32_t x)
{
    for (int i = 0; i < 4; ++i)
        v.push_back(uint8_t(x >> (8 * i)));
}
void Put64(std::vector<uint8_t>& v, uint64_t x)
{
    Put32(v, uint32_t(x));
    Put32(v, uint32_t(x >> 32));
}

std::vector<uint8_t> Serialize(const SessionRecord& r)
{
    std::vector<uint8_t> v;
    Put32(v, kMagic);
    Put64(v, r.id);
    Put64(v, r.hostPeer);
    Put64(v, r.hostXuid);
    Put32(v, r.flags);
    Put32(v, r.maxPublic);
    Put32(v, r.maxPrivate);
    Put32(v, r.filledPublic);
    Put32(v, r.filledPrivate);
    v.insert(v.end(), r.info, r.info + sizeof(r.info));
    Put32(v, uint32_t(r.contexts.size()));
    for (const Context& c : r.contexts)
    {
        Put32(v, c.id);
        Put32(v, c.value);
    }
    Put32(v, uint32_t(r.properties.size()));
    for (const Property& p : r.properties)
    {
        Put32(v, p.id);
        Put32(v, uint32_t(p.value.size()));
        v.insert(v.end(), p.value.begin(), p.value.end());
    }
    return v;
}

void Write(const SessionRecord& r)
{
    std::error_code ec;
    std::filesystem::create_directories(Folder(), ec);
    const std::vector<uint8_t> bytes = Serialize(r);
    const std::filesystem::path file = FileOf(r.id), temp = file.wstring() + L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }
    std::filesystem::rename(temp, file, ec);
}

void Heartbeat()
{
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::lock_guard l(g_mutex);
        for (const auto& [id, r] : g_owned)
            Write(r);
    }
}

bool Fresh(const std::filesystem::path& file)
{
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(file, ec);
    return !ec && std::filesystem::file_time_type::clock::now() - t < std::chrono::seconds(kStaleSeconds);
}

bool Load(const std::filesystem::path& file, SessionRecord& r)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return false;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return Parse(bytes, r);
}

} // namespace

bool IsRecord(const std::vector<uint8_t>& bytes)
{
    uint32_t magic = 0;
    if (bytes.size() >= 4)
        std::memcpy(&magic, bytes.data(), 4);
    return magic == kMagic;
}

bool Parse(const std::vector<uint8_t>& v, SessionRecord& r)
{
    size_t at = 0;
    auto get32 = [&](uint32_t& x) {
        if (at + 4 > v.size())
            return false;
        x = uint32_t(v[at]) | uint32_t(v[at + 1]) << 8 | uint32_t(v[at + 2]) << 16 | uint32_t(v[at + 3]) << 24;
        at += 4;
        return true;
    };
    auto get64 = [&](uint64_t& x) {
        uint32_t lo, hi;
        if (!get32(lo) || !get32(hi))
            return false;
        x = uint64_t(hi) << 32 | lo;
        return true;
    };
    uint32_t magic = 0, n = 0;
    if (!get32(magic) || magic != kMagic || !get64(r.id) || !get64(r.hostPeer) || !get64(r.hostXuid) || !get32(r.flags) || !get32(r.maxPublic) ||
        !get32(r.maxPrivate) || !get32(r.filledPublic) || !get32(r.filledPrivate) || at + sizeof(r.info) > v.size())
        return false;
    std::memcpy(r.info, v.data() + at, sizeof(r.info));
    at += sizeof(r.info);
    if (!get32(n) || n > 64)
        return false;
    r.contexts.resize(n);
    for (Context& c : r.contexts)
        if (!get32(c.id) || !get32(c.value))
            return false;
    if (!get32(n) || n > 64)
        return false;
    r.properties.resize(n);
    for (Property& p : r.properties)
    {
        uint32_t size = 0;
        if (!get32(p.id) || !get32(size) || size > 4096 || at + size > v.size())
            return false;
        p.value.assign(v.begin() + at, v.begin() + at + size);
        at += size;
    }
    return true;
}

void Publish(const SessionRecord& record)
{
    std::lock_guard l(g_mutex);
    g_owned[record.id] = record;
    if (Steam())
        steamnet::PublishLobby(record.id, Serialize(record));
    else
    {
        Write(record);
        if (!g_heartbeat)
        {
            g_heartbeat = true;
            std::thread(Heartbeat).detach();
        }
    }
    RuntimeLog("[mm] published %016llX (slots %u/%u)\n", (unsigned long long)record.id, record.filledPublic, record.maxPublic);
}

void Remove(uint64_t id)
{
    std::lock_guard l(g_mutex);
    if (!g_owned.erase(id))
        return;
    if (Steam())
        steamnet::RemoveLobby(id);
    std::error_code ec;
    std::filesystem::remove(FileOf(id), ec);
    RuntimeLog("[mm] removed %016llX\n", (unsigned long long)id);
}

std::vector<SessionRecord> List(uint32_t max)
{
    std::vector<SessionRecord> out;
    if (Steam())
    {
        static const bool listOwn = std::getenv("SCDA_STEAM_SEE_OWN") != nullptr; // test aid: one account
        for (const std::vector<uint8_t>& bytes : steamnet::ListLobbies(int((std::max)(max, 1u) * 2), 5000))
        {
            SessionRecord r;
            if (out.size() >= max || !Parse(bytes, r) || (r.hostPeer == net::LocalPeer() && !listOwn))
                continue;
            {
                std::lock_guard l(g_mutex);
                g_seen[r.id] = r;
            }
            out.push_back(std::move(r));
        }
        return out;
    }
    std::vector<std::pair<std::filesystem::file_time_type, SessionRecord>> found;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(Folder(), ec))
    {
        if (entry.path().extension() != L".ses" || !Fresh(entry.path()))
            continue;
        SessionRecord r;
        if (!Load(entry.path(), r) || r.hostPeer == net::LocalPeer())
            continue;
        found.emplace_back(entry.last_write_time(ec), std::move(r));
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& f : found)
    {
        if (out.size() >= max)
            break;
        {
            std::lock_guard l(g_mutex);
            g_seen[f.second.id] = f.second;
        }
        out.push_back(std::move(f.second));
    }
    return out;
}

bool Find(uint64_t id, SessionRecord& out)
{
    {
        std::lock_guard l(g_mutex);
        if (const auto it = g_seen.find(id); it != g_seen.end())
        {
            out = it->second;
            return true;
        }
    }
    for (SessionRecord& r : List(64))
        if (r.id == id)
        {
            out = std::move(r);
            return true;
        }
    return false;
}

SessionRecord g_accepted; // under g_mutex
bool g_hasAccepted = false;

bool TakeInvite(SessionRecord& out)
{
    std::lock_guard l(g_mutex);
    if (g_invites.empty())
        return false;
    out = std::move(g_invites.front());
    g_invites.pop_front();
    g_accepted = out;
    g_hasAccepted = true;
    return true;
}

bool AcceptedInvite(SessionRecord& out)
{
    std::lock_guard l(g_mutex);
    if (!g_hasAccepted)
        return false;
    out = g_accepted;
    return true;
}

void QueueInvite(const SessionRecord& record)
{
    std::lock_guard l(g_mutex);
    g_seen[record.id] = record;
    g_invites.push_back(record);
}

} // namespace mm
