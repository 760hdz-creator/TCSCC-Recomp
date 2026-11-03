#include <windows.h>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <runtime.h>
#include <watchdog.h>

struct WatchedThread
{
    PPCContext* ctx;
    DWORD hostThread;
    std::atomic<const char*> blocking{ nullptr };
};

static std::mutex g_watchMutex;
static std::map<uint32_t, WatchedThread*> g_watched;
static thread_local WatchedThread* t_self;

void WatchdogRegisterThread(uint32_t guestThreadId, PPCContext* ctx)
{
    auto* w = new WatchedThread{ ctx, GetCurrentThreadId() };
    std::lock_guard lock(g_watchMutex);
    g_watched[guestThreadId] = w;
    t_self = w;
}

void WatchdogUnregisterThread(uint32_t guestThreadId)
{
    std::lock_guard lock(g_watchMutex);
    auto it = g_watched.find(guestThreadId);
    if (it == g_watched.end())
        return;
    if (t_self == it->second)
        t_self = nullptr;
    delete it->second;
    g_watched.erase(it);
}

WatchdogBlockScope::WatchdogBlockScope(const char* importName)
    : previous(t_self ? t_self->blocking.exchange(importName) : nullptr)
{
}

WatchdogBlockScope::~WatchdogBlockScope()
{
    if (t_self)
        t_self->blocking = previous;
}

PPCContext* WatchdogCurrentContext() { return t_self ? t_self->ctx : nullptr; }

static uint32_t Load32(const uint8_t* base, uint32_t addr) { return _byteswap_ulong(*(const uint32_t*)(base + addr)); }

// The walk also runs from the crash handler: never fault on a guard page or unmapped memory.
static bool Readable(const uint8_t* p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return false;
    return !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (uint8_t*)mbi.BaseAddress + mbi.RegionSize >= p + 4;
}

std::vector<uint32_t> WalkGuestStack(const uint8_t* guestBase, uint32_t r1, uint32_t lr, int maxFrames)
{
    std::vector<uint32_t> frames{ lr };
    uint32_t sp = r1;
    while ((int)frames.size() < maxFrames && sp >= 8 && (sp & 3) == 0)
    {
        if (!Readable(guestBase + sp))
            break;
        uint32_t back = Load32(guestBase, sp);
        if (back == 0 || back <= sp || back < 8)
            break;
        if (!Readable(guestBase + back - 8))
            break;
        frames.push_back(Load32(guestBase, back - 8));
        sp = back;
    }
    return frames;
}

std::string WatchdogReport(const uint8_t* guestBase)
{
    std::lock_guard lock(g_watchMutex);
    std::string out;
    char buf[64];
    for (auto& [id, w] : g_watched)
    {
        const char* in = w->blocking.load();
        // [current] = the thread producing this report (the faulting one, from the crash handler).
        snprintf(buf, sizeof(buf), "[watchdog] thread %u%s in %s stack:", id,
            w->hostThread == GetCurrentThreadId() ? " [current]" : "", in ? in : "(guest code)");
        out += buf;
        for (uint32_t a : WalkGuestStack(guestBase, w->ctx->r1.u32, uint32_t(w->ctx->lr), 16))
        {
            snprintf(buf, sizeof(buf), " 0x%08X", a);
            out += buf;
        }
        // Callee-saved registers as last stored (frames above the stack walk keep their state here).
        snprintf(buf, sizeof(buf), " r28-r31 %08X %08X %08X %08X", w->ctx->r28.u32, w->ctx->r29.u32, w->ctx->r30.u32, w->ctx->r31.u32);
        out += buf;
        out += "\n";
    }
    // SCDA_SCAN=<value hex>/<mask hex>: guest dwords (big-endian) with (v & mask) == value, in
    // committed guest memory below 0xE0000000; first 128 hits with the 8 dwords around each.
    if (const char* s = getenv("SCDA_SCAN"))
    {
        const uint32_t want = strtoul(s, nullptr, 16), mask = strchr(s, '/') ? strtoul(strchr(s, '/') + 1, nullptr, 16) : ~0u;
        int hits = 0;
        for (uint64_t a = 0x10000; a < 0xE0000000ull && hits < 128;)
        {
            MEMORY_BASIC_INFORMATION mi{};
            if (!VirtualQuery(guestBase + a, &mi, sizeof(mi)))
                break;
            const uint64_t end = (uint64_t)((uint8_t*)mi.BaseAddress + mi.RegionSize - guestBase);
            if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                for (uint64_t p = a & ~3ull; p + 4 <= end && hits < 128; p += 4)
                {
                    const uint32_t v = _byteswap_ulong(*(const uint32_t*)(guestBase + p));
                    if ((v & mask) != want)
                        continue;
                    hits++;
                    snprintf(buf, sizeof(buf), "[watchdog] scan hit 0x%08X = 0x%08X ctx", uint32_t(p), v);
                    out += buf;
                    for (int k = -4; k < 4; k++)
                    {
                        const uint64_t q = p + k * 4;
                        snprintf(buf, sizeof(buf), " %08X", q >= a && q + 4 <= end ? _byteswap_ulong(*(const uint32_t*)(guestBase + q)) : 0);
                        out += buf;
                    }
                    out += "\n";
                }
            a = end;
        }
        snprintf(buf, sizeof(buf), "[watchdog] scan %08X/%08X: %d hits\n", want, mask, hits);
        out += buf;
    }
    // SCDA_PEEK=<addr>[,<addr>...]: guest dwords to show with each dump (and the dword each points at).
    for (const char* p = getenv("SCDA_PEEK"); p && *p;)
    {
        char* end;
        uint32_t addr = uint32_t(strtoul(p, &end, 0));
        if (end == p)
            break;
        uint32_t v = GuestLoad32(addr), deref = (v >= 0x10000 && v < 0xE0000000u) ? GuestLoad32(v & ~3u) : 0;
        snprintf(buf, sizeof(buf), "[watchdog] peek 0x%08X = 0x%08X (-> 0x%08X)\n", addr, v, deref);
        out += buf;
        p = *end == ',' ? end + 1 : end;
    }
    return out;
}

void WatchdogStart(unsigned seconds)
{
    std::thread([seconds]
    {
        for (int i = 0; i < 3; i++)
        {
            Sleep(seconds * 1000);
            RuntimeLog("%s", WatchdogReport(g_base).c_str());
        }
    }).detach();
}
