#include <runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <watchdog.h>

// Conviction: Scaleform GFx log sink sub_832FA860(log + 20, fmt, ...) (GFxLoader "Error: ..." strings
// at 0x823EAEF8+). Copies each message to scda.log as [gfx], then runs the original.
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
            RuntimeLog("[gfx] HUD_Saving wrapper 0x%08X: movie 0x%08X destroyed, pointer cleared\n", w, movie);
            PPC_STORE_U32(w + 4, 0);
        }
    }
    __imp__sub_82DAA4F0(ctx, base);
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
