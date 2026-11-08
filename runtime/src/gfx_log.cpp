#include <windows.h>
#include <runtime.h>
#include <chrono>
#include <gpu/frame_queue.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <watchdog.h>

// Conviction: Scaleform GFx log sink sub_832FA860(log + 20, fmt, ...) (GFxLoader "Error: ..." strings
// at 0x823EAEF8+). Copies each message to tcscc.log as [gfx], then runs the original.
// Varargs: integer args in r5-r10; %s is a guest char*. Floats are not decoded.
extern "C" PPC_FUNC(__imp__sub_832FA860);

PPC_FUNC(sub_832FA860)
{
    const uint64_t args[] = { ctx.r5.u64, ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64 };
    const uint32_t fmt = ctx.r4.u32;
    std::string out;
    size_t next = 0;
    for (uint32_t p = fmt; fmt && out.size() < 512; p++)
    {
        const char c = char(PPC_LOAD_U8(p));
        if (!c)
            break;
        if (c != '%')
        {
            out += c;
            continue;
        }
        uint32_t q = p + 1;
        while (strchr("-+ #0123456789.lhz", char(PPC_LOAD_U8(q))) && PPC_LOAD_U8(q))
            q++;
        const char conv = char(PPC_LOAD_U8(q));
        p = q;
        if (conv == '%')
        {
            out += '%';
            continue;
        }
        const uint64_t v = next < 6 ? args[next++] : 0;
        char buf[32];
        if (conv == 's')
        {
            for (uint32_t s = uint32_t(v); s && PPC_LOAD_U8(s) && out.size() < 512; s++)
                out += char(PPC_LOAD_U8(s));
            continue;
        }
        if (conv == 'd' || conv == 'i')
            snprintf(buf, sizeof(buf), "%d", int32_t(v));
        else if (conv == 'x' || conv == 'X' || conv == 'p')
            snprintf(buf, sizeof(buf), "%X", uint32_t(v));
        else
            snprintf(buf, sizeof(buf), "%u", uint32_t(v));
        out += buf;
    }
    if (out.empty() || out.back() != '\n')
        out += '\n';
    RuntimeLog("[gfx] %s", out.c_str());
    __imp__sub_832FA860(ctx, base);
}

// GFx file open sub_832FB110(opener, path, log): opener->Open(path), file->IsValid(); returns the
// file or 0. SCDA_TRACE_GFX=1 logs each path and the result.
extern "C" PPC_FUNC(__imp__sub_832FB110);

PPC_FUNC(sub_832FB110)
{
    static const bool trace = getenv("SCDA_TRACE_GFX") != nullptr;
    std::string path;
    for (uint32_t s = ctx.r4.u32; trace && s && PPC_LOAD_U8(s) && path.size() < 260; s++)
        path += char(PPC_LOAD_U8(s));
    const uint32_t lr = uint32_t(ctx.lr);
    __imp__sub_832FB110(ctx, base);
    if (trace)
        RuntimeLog("[gfx] open '%s' -> 0x%08X (lr 0x%08X)\n", path.c_str(), ctx.r3.u32, lr);
}

// SCDA_TRACE_GFX: first call of UI setup functions (DynamicFonts.swf user, HUD_Saving.swf user).
#define PZ_TRACE_ONCE(addr)                                                                      \
    extern "C" PPC_FUNC(__imp__sub_##addr);                                                      \
    PPC_FUNC(sub_##addr)                                                                         \
    {                                                                                            \
        static bool once = false;                                                                \
        if (!once && getenv("SCDA_TRACE_GFX"))                                                   \
        {                                                                                        \
            once = true;                                                                         \
            RuntimeLog("[gfx] first call sub_" #addr " r3 0x%08X r4 0x%08X lr 0x%08X\n", ctx.r3.u32, \
                ctx.r4.u32, uint32_t(ctx.lr));                                                   \
        }                                                                                        \
        __imp__sub_##addr(ctx, base);                                                            \
    }
PZ_TRACE_ONCE(83309A20)

PZ_TRACE_ONCE(83365C28)
PZ_TRACE_ONCE(83366338)
PZ_TRACE_ONCE(82E83DF8)
PZ_TRACE_ONCE(82E4E6B8)

// FString {char* data, num, max} (narrow on this build). Copy for logs.
static std::string GuestFString(uint8_t* base, uint32_t fs)
{
    std::string out;
    const uint32_t data = fs ? PPC_LOAD_U32(fs) : 0;
    for (uint32_t s = data; s && out.size() < 200; s++)
    {
        const uint8_t c = PPC_LOAD_U8(s);
        if (!c)
            break;
        out += c < 0x80 ? char(c) : '?';
    }
    return out;
}

// UI movie wrapper load sub_82E6D748(this, FString* path, ...): this+4 = GFx movie after the call.
extern "C" PPC_FUNC(__imp__sub_82E6D748);
PPC_FUNC(sub_82E6D748)
{
    static const bool trace = getenv("SCDA_TRACE_GFX") != nullptr;
    const uint32_t self = ctx.r3.u32, lr = uint32_t(ctx.lr);
    const std::string path = trace ? GuestFString(base, ctx.r4.u32) : std::string();
    __imp__sub_82E6D748(ctx, base);
    if (trace)
    {
        const uint32_t movie = PPC_LOAD_U32(self + 4), vt = movie ? PPC_LOAD_U32(movie) : 0;
        RuntimeLog("[gfx] load '%s' this 0x%08X -> movie 0x%08X vt 0x%08X r3 0x%08X (lr 0x%08X)\n", path.c_str(), self, movie, vt,
            ctx.r3.u32, lr);
    }
}

PZ_TRACE_ONCE(8330B5E8)
PZ_TRACE_ONCE(82D12150)
PZ_TRACE_ONCE(82DAA0D0)

// GFxLoaderImpl::OpenFile sub_83377E98(this, const char* path, flags) -> GFxFile* or 0.
extern "C" PPC_FUNC(__imp__sub_83377E98);
PPC_FUNC(sub_83377E98)
{
    static const bool trace = getenv("SCDA_TRACE_GFX") != nullptr;
    std::string path;
    for (uint32_t s = ctx.r4.u32; trace && s && PPC_LOAD_U8(s) && path.size() < 260; s++)
        path += char(PPC_LOAD_U8(s));
    const uint32_t lr = uint32_t(ctx.lr), flags = ctx.r5.u32;
    __imp__sub_83377E98(ctx, base);
    if (trace)
        RuntimeLog("[gfx] OpenFile '%s' flags 0x%X -> 0x%08X (lr 0x%08X)\n", path.c_str(), flags, ctx.r3.u32, lr);
}

// Occlusion result pass sub_82EF6278(this): per query entry (this+164, 20 bytes: obj, kind @+4,
// query @+8, skip @+12) GetData spins; samples 0 -> obj+28 |= 2 (hidden), kind 1 -> obj+896 = samples.
// SCDA_TRACE_OCCL=<n>: log the entries of every n-th call.
extern "C" PPC_FUNC(__imp__sub_82EF6278);
PPC_FUNC(sub_82EF6278)
{
    static const int every = getenv("SCDA_TRACE_OCCL") ? atoi(getenv("SCDA_TRACE_OCCL")) : 0;
    static int calls = 0;
    const uint32_t self = ctx.r3.u32;
    __imp__sub_82EF6278(ctx, base);
    if (!every || (calls++ % every) != 0)
        return;
    const uint32_t list = PPC_LOAD_U32(PPC_LOAD_U32(self) + 16);
    const uint32_t sel = PPC_LOAD_U32(PPC_LOAD_U32(self + 148) + PPC_LOAD_U32(list) * 8);
    const uint32_t r28 = PPC_LOAD_U32(self + 148) + PPC_LOAD_U32(list) * 8;
    const int32_t n = int32_t(PPC_LOAD_U32(r28 + 4));
    const uint32_t first = PPC_LOAD_U32(r28), table = PPC_LOAD_U32(self + 164);
    std::string line;
    for (int32_t i = 0; i < n && i < 64; i++)
    {
        const uint32_t e = table + (first + i) * 20, obj = PPC_LOAD_U32(e);
        char buf[64];
        snprintf(buf, sizeof(buf), " [%08X k%u s%u f%X r%d]", obj, PPC_LOAD_U32(e + 4), PPC_LOAD_U8(e + 12),
            obj ? PPC_LOAD_U32(obj + 28) : 0, obj ? int32_t(PPC_LOAD_U32(obj + 896)) : 0);
        line += buf;
    }
    (void)sel;
    RuntimeLog("[occl] call %d entries %d:%s\n", calls - 1, n, line.c_str());
}

// GFx movie (vtable 0x821CB780) ctor sub_82DAA408 / dtor sub_82DAA4F0. SCDA_TRACE_GFX: log this and
// the guest call chain (stale movie pointers: HUD_Saving SetVariable crash in sub_83427D78).
static void LogMovieLife(const char* what, uint32_t self, uint8_t* base, const PPCContext& ctx)
{
    std::string chain;
    for (uint32_t a : WalkGuestStack(base, ctx.r1.u32, uint32_t(ctx.lr), 10))
    {
        char buf[16];
        snprintf(buf, sizeof(buf), " %08X", a);
        chain += buf;
    }
    RuntimeLog("[gfx] movie %s 0x%08X:%s\n", what, self, chain.c_str());
}

extern "C" PPC_FUNC(__imp__sub_82DAA408);
PPC_FUNC(sub_82DAA408)
{
    static const bool trace = getenv("SCDA_TRACE_GFX") != nullptr;
    if (trace)
        LogMovieLife("ctor", ctx.r3.u32, base, ctx);
    __imp__sub_82DAA408(ctx, base);
}

// Stale HUD_Saving fix. The HUD wrapper (vtable 0x82403000, mgr+92) borrows the loading screen's
// HUD_Saving movie through the registry lookup (no refcount). The loading-screen teardown
// (sub_82D02350 -> sub_82D11278) releases it by name and the dtor runs. Event 9 (save icon,
// sub_82E8F920 -> sub_83427D78) then calls through the freed vtable. Fix: when a movie dies, clear
// +4 of every live wrapper that points to it. sub_83427D78 null-checks +4, so the save icon is skipped.
static constexpr uint32_t kSavingWrapperVt = 0x82403000;
static uint32_t g_savingWrappers[8];

extern "C" PPC_FUNC(__imp__sub_82DAA4F0);
PPC_FUNC(sub_82DAA4F0)
{
    static const bool trace = getenv("SCDA_TRACE_GFX") != nullptr;
    const uint32_t movie = ctx.r3.u32;
    if (trace)
        LogMovieLife("dtor", movie, base, ctx);
    for (uint32_t& w : g_savingWrappers)
    {
        if (w && PPC_LOAD_U32(w) != kSavingWrapperVt)
            w = 0; // wrapper itself is gone
        if (w && PPC_LOAD_U32(w + 4) == movie)
        {
            RuntimeLog("[gfx] HUD_Saving wrapper 0x%08X: movie 0x%08X destroyed, pointer cleared (lr 0x%08X)\n", w,
                       movie, uint32_t(ctx.lr));
            if (!trace)
                LogMovieLife("dtor", movie, base, ctx); // guest chain: which release path bypassed the keep
            PPC_STORE_U32(w + 4, 0);
        }
    }
    __imp__sub_82DAA4F0(ctx, base);
}

// Release by name sub_82D0F9F0(registry, movie): the loading-screen teardown loop (sub_82D11278 with
// keep = 0, so the movie+120 keep flag does not help) queues every movie it acquired. A movie a live
// HUD_Saving wrapper still holds is skipped: it stays in the registry, the next loading screen finds
// it again by name, and the save icon keeps working. SCDA_NO_SAVING_KEEP=1: old behaviour (released,
// the dtor hook above clears the wrapper and the icon is skipped).
extern "C" PPC_FUNC(__imp__sub_82D0F9F0);
PPC_FUNC(sub_82D0F9F0)
{
    static const bool noKeep = getenv("SCDA_NO_SAVING_KEEP") != nullptr;
    const uint32_t movie = ctx.r4.u32;
    if (!noKeep && movie && uint32_t(ctx.lr) == 0x82D112E4) // only the teardown loop: other callers retry until gone
        for (uint32_t w : g_savingWrappers)
            if (w && PPC_LOAD_U32(w) == kSavingWrapperVt && PPC_LOAD_U32(w + 4) == movie)
            {
                static int logged;
                if (logged++ < 8)
                    RuntimeLog("[gfx] HUD_Saving movie 0x%08X kept (held by wrapper 0x%08X)\n", movie, w);
                return;
            }
    __imp__sub_82D0F9F0(ctx, base);
}

// HUD_Saving wrapper load sub_83427FD8(this, ...): this+4 = registry lookup sub_82D10420(sub_82D11588(),
// "Ingame\HUD_Saving.swf", 0) (raw pointer, no refcount), else a fresh load. SCDA_TRACE_GFX: log the
// result and which registry list (+76 main / +108 second) holds it.
extern "C" PPC_FUNC(__imp__sub_83427FD8);
extern "C" PPC_FUNC(__imp__sub_82D11588);
PPC_FUNC(sub_83427FD8)
{
    static const bool trace = getenv("SCDA_TRACE_GFX") != nullptr;
    const uint32_t self = ctx.r3.u32, lr = uint32_t(ctx.lr);
    __imp__sub_83427FD8(ctx, base);
    const uint32_t movie = PPC_LOAD_U32(self + 4);
    if (movie)
    {
        uint32_t* slot = nullptr;
        for (uint32_t& w : g_savingWrappers)
            if (w == self || (!slot && (!w || PPC_LOAD_U32(w) != kSavingWrapperVt)))
                slot = &w;
        if (slot)
            *slot = self;
    }
    if (!trace)
        return;
    PPCContext c = ctx;
    __imp__sub_82D11588(c, base);
    const uint32_t reg = c.r3.u32;
    int where = 0;
    for (int list = 0; list < 2 && !where; list++)
    {
        const uint32_t items = PPC_LOAD_U32(reg + (list ? 108 : 76)), n = PPC_LOAD_U32(reg + (list ? 112 : 80));
        for (uint32_t i = 0; i < n && i < 512; i++)
            if (PPC_LOAD_U32(items + 4 * i) == movie)
                where = list ? 108 : 76;
    }
    RuntimeLog("[gfx] HUD_Saving wrapper 0x%08X movie 0x%08X (reg 0x%08X list +%d) lr 0x%08X\n", self, movie, reg, where, lr);
}

// Save-result handler sub_824FB378(save, result, name, ?, ?): picks the save error dialog from the
// result. SCDA_TRACE_SAVE=1 logs each call.
extern "C" PPC_FUNC(__imp__sub_824FB378);
PPC_FUNC(sub_824FB378)
{
    static const bool trace = getenv("SCDA_TRACE_SAVE") != nullptr;
    const uint32_t r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32, r7 = ctx.r7.u32, lr = uint32_t(ctx.lr);
    __imp__sub_824FB378(ctx, base);
    if (trace)
        RuntimeLog("[save] result handler(0x%X, 0x%08X, %u, %u) lr 0x%08X -> %u\n", r4, r5, r6, r7, lr, ctx.r3.u32);
}

// GetData sub_825283A0(query r3, data r4, size r5, flags r6) -> r3 1 (S_FALSE) while the result is
// pending. sub_82EF6278 spins on it every frame until the renderer answers: a whole host core busy.
// After a run of back-to-back pending answers for one query on one thread, wait for the next
// renderer result/fence write (NotifyGpuWrite, 1 ms cap; CP-written results only via the cap)
// instead of spinning. A caller polling once a frame never builds a run (gap > 200 us resets it).
// SCDA_GETDATA_SPIN=1: old busy poll.
extern "C" PPC_FUNC(__imp__sub_825283A0);
PPC_FUNC(sub_825283A0)
{
    static const bool spin = getenv("SCDA_GETDATA_SPIN") != nullptr;
    thread_local uint32_t lastQuery = 0, run = 0;
    thread_local std::chrono::steady_clock::time_point lastPending;
    const uint32_t query = ctx.r3.u32;
    const uint32_t seq = GpuWriteSeq().load(std::memory_order_acquire);
    __imp__sub_825283A0(ctx, base);
    if (spin || ctx.r3.u32 != 1)
    {
        run = 0;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (query != lastQuery || now - lastPending > std::chrono::microseconds(200))
        run = 0;
    lastQuery = query;
    lastPending = now;
    if (++run < 16)
        return;
    uint32_t expected = seq;
    WaitOnAddress(&GpuWriteSeq(), &expected, sizeof(expected), 1);
    lastPending = std::chrono::steady_clock::now();
}

// Sleep(ms) wrapper sub_82F50BE8. The stream worker pass sub_82AB6528 calls Sleep(0) (lr 82AB660C)
// when a pass found no work, then runs the next pass: ~20k passes/s, a host core busy in its event
// set/clear. Idle passes sleep 1 ms instead. SCDA_STREAM_SPIN=1: old Sleep(0).
extern "C" PPC_FUNC(__imp__sub_82F50BE8);
PPC_FUNC(sub_82F50BE8)
{
    static const bool spin = getenv("SCDA_STREAM_SPIN") != nullptr;
    if (!spin && ctx.r3.u32 == 0 && uint32_t(ctx.lr) == 0x82AB660C)
        ctx.r3.u64 = 1;
    __imp__sub_82F50BE8(ctx, base);
}

// Sleep(seconds f1) wrapper sub_82A8F6E8. The engine frame limiter (main loop sub_824E53B0, loop at
// 824E5668, lr 824E5684) calls it with 0 until the minimum frame time has passed: ~140k host yields/s
// on the main game thread. The caller's registers are live here: f31 = minimum frame time, float at
// r23-13592 = elapsed. Those calls sleep (high-resolution timer) until 1 ms before the deadline,
// then yield as before. SCDA_LIMITER_SPIN=1: old Sleep(0).
extern "C" PPC_FUNC(__imp__sub_82A8F6E8);
PPC_FUNC(sub_82A8F6E8)
{
    static const bool spin = getenv("SCDA_LIMITER_SPIN") != nullptr;
    if (spin || uint32_t(ctx.lr) != 0x824E5684 || ctx.f1.f64 > 0.0)
    {
        __imp__sub_82A8F6E8(ctx, base);
        return;
    }
    const uint32_t elapsedBits = PPC_LOAD_U32(ctx.r23.u32 - 13592);
    float elapsed;
    memcpy(&elapsed, &elapsedBits, 4);
    const double remaining = ctx.f31.f64 - double(elapsed); // seconds
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -LONGLONG((remaining - 0.001) * 1e7); // relative, 100 ns units
    if (timer && remaining > 0.0015 && remaining < 0.1 && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(timer, 100);
    else
        SwitchToThread();
}

// D3D SetTexture sub_82511D28(dev, stage, tex, flags): user playtests (2026-11-04, 2026-11-06, both
// right after a checkpoint save; 06_WindowlessBuilding, walking from the elevator checkpoint to the
// weapon stash) crashed reading tex+48 with tex = 1 / 0x1E0. Caller sub_82EF49D0 (batch draw) loads
// tex = holder+28, holder = batch+12 or the default fill: the holder was freed and reused. Chain runs
// through sub_83425C58 -> sub_83425AC8 (same code region as the HUD_Saving wrapper). Guest objects
// live at >= 0x40000000, and tex+28 is fetch constant dword 0 (type bits 0-1, texture = 2): either
// check failing = garbage, bind nothing for that draw instead of faulting. Root cause unknown; the log
// names the object being drawn (r3 of sub_83425C58) to tell HUD_Saving from other movies.
// SCDA_COUNT_BADTEX=1: log every reject (check that real textures never fail the type test).
static thread_local uint32_t t_gfxDrawThis;
extern "C" PPC_FUNC(__imp__sub_83425C58);
PPC_FUNC(sub_83425C58)
{
    const uint32_t prev = t_gfxDrawThis;
    t_gfxDrawThis = ctx.r3.u32;
    __imp__sub_83425C58(ctx, base);
    t_gfxDrawThis = prev;
}

extern "C" PPC_FUNC(__imp__sub_82511D28);
PPC_FUNC(sub_82511D28)
{
    static const bool countAll = getenv("SCDA_COUNT_BADTEX") != nullptr;
    const uint32_t tex = ctx.r5.u32;
    if (tex && (tex < 0x40000000 || (PPC_LOAD_U32(tex + 28) & 3) != 2))
    {
        static int logged;
        if (countAll || logged++ < 8)
        {
            const uint32_t self = t_gfxDrawThis, batch = ctx.r28.u32;
            bool saving = false;
            for (uint32_t w : g_savingWrappers)
                saving |= w && w == self;
            const bool selfOk = self >= 0x40000000, batchOk = batch >= 0x40000000;
            RuntimeLog("[gfx] SetTexture stage %u bad texture 0x%08X dw0 0x%08X (lr 0x%08X): unbound; drawing 0x%08X "
                       "vt 0x%08X +4 0x%08X%s, batch 0x%08X holder 0x%08X\n",
                       ctx.r4.u32, tex, tex >= 0x40000000 ? PPC_LOAD_U32(tex + 28) : 0, uint32_t(ctx.lr), self,
                       selfOk ? PPC_LOAD_U32(self) : 0, selfOk ? PPC_LOAD_U32(self + 4) : 0,
                       saving ? " (HUD_Saving wrapper)" : "", batch, batchOk ? PPC_LOAD_U32(batch + 12) : 0);
        }
        ctx.r5.u64 = 0;
    }
    __imp__sub_82511D28(ctx, base);
}
