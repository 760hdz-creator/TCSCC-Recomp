// Draw distance / LOD (Conviction), docs/draw_distance.md. sub_82C95F30() returns the l3d render
// settings struct (0x402945D0 on 01_Market) that the console handler sub_82C98B00 edits:
//   +46 byte  l3d-distance-fade (toggle, on)     +279 byte  l3d-toggle-lod (on)
//   +280 float l3d-lod-size2= (0.18)              +284 float l3d-lod-size1= (0.045)
//   +60 float small-object cull size (0.0025)     +64 float its override when nonzero (0)
// LOD pick sub_82EF31F8: projected size below size2 * r^2 -> LOD 1, below size1 * r^2 -> LOD 2 (clamped
// to the mesh's count). Cull sub_82EF2FE8: projected size below the cull size (copied per view to render
// ctx +23988 by sub_82F01E28) -> not drawn; with distance-fade on, objects fade out from 1.5x that size
// (sub_82EF30A8). Projected size ~ 1/distance^2: thresholds x k move each switch out by 1/sqrt(k).
#include <runtime.h>
#include <windows.h>
#include <bit>
#include <atomic>
#include <cstdlib>
#include <algorithm>
#include "gpu/frame_queue.h"

void WatchAdd(uint32_t addr, int maxHits);

int DrawDistance(); // settings.cpp: 0 Console, 1 High, 2 Ultra, 3 Maximum

namespace
{
struct Level { float cull, lod; bool lodOff; };
// High: switches at 2x the distance, Ultra 4x, Maximum: no small-object cull and always LOD 0.
constexpr Level kLevels[] = { { 1.0f, 1.0f, false }, { 0.25f, 0.25f, false }, { 0.0625f, 0.0625f, false }, { 0.0f, 1.0f, true } };

// One guest field we override. A value other than the one written last is the game's own (first
// call, level load, console command): it becomes the new original. written = f(original) every call,
// so a level change (or back to Console) applies at once. Racy across threads but idempotent.
struct Slot
{
    std::atomic<uint32_t> addr{0}, original{0}, written{0};
    bool Capture(uint32_t a, uint32_t now)
    {
        if (addr.load() != a || now != written.load())
        {
            addr = a;
            original = now;
            return true;
        }
        return false;
    }
};
Slot g_size2, g_size1, g_cull, g_cullOverride, g_lodToggle;

float EnvFloat(const char* name, float fallback)
{
    const char* v = getenv(name);
    return v ? float(atof(v)) : fallback;
}

void ScaleField(uint8_t* base, Slot& slot, uint32_t addr, float k)
{
    slot.Capture(addr, PPC_LOAD_U32(addr));
    const uint32_t value = std::bit_cast<uint32_t>(std::bit_cast<float>(slot.original.load()) * k);
    slot.written = value;
    PPC_STORE_U32(addr, value);
}

void SetByte(uint8_t* base, Slot& slot, uint32_t addr, bool zero)
{
    slot.Capture(addr, PPC_LOAD_U8(addr));
    const uint32_t value = zero ? 0 : slot.original.load();
    slot.written = value;
    PPC_STORE_U8(addr, uint8_t(value));
}
}

// Env overrides (A/B, win over the setting): SCDA_LOD_SCALE=<k> (LOD thresholds x k), SCDA_CULL_SCALE=<k>
// (cull size x k), SCDA_LOD_OFF=1 (always LOD 0), SCDA_LOD_FROM=<frame> (overrides from that frame on,
// same camera A/B in one run). SCDA_NO_DISTANCE_FADE=1: +46 = 0 (objects pop instead of fading).
// SCDA_TRACE_LOD=1: the struct once per second, LOD/cull counters every 5 s.
// SCDA_WATCH_LOD=1|2: hardware watchpoints on +44 (1 only)/+276/+280/+284 (SCDA_WATCH_RW=1 for readers).
extern "C" PPC_FUNC(__imp__sub_82C95F30);
PPC_FUNC(sub_82C95F30)
{
    __imp__sub_82C95F30(ctx, base);
    const uint32_t s = ctx.r3.u32;
    if (!s)
        return;
    static const bool watch = getenv("SCDA_WATCH_LOD") != nullptr;
    static const bool trace = getenv("SCDA_TRACE_LOD") && !watch; // reads would hit the watch
    static std::atomic<bool> watched{false};
    if (watch && !watched.exchange(true))
    {
        if (atoi(getenv("SCDA_WATCH_LOD")) != 2) // 2 = LOD fields only
            WatchAdd(s + 44, 80);
        WatchAdd(s + 276, 80);
        WatchAdd(s + 280, 80);
        WatchAdd(s + 284, 80);
    }
    static const uint64_t from = getenv("SCDA_LOD_FROM") ? strtoull(getenv("SCDA_LOD_FROM"), nullptr, 10) : 0;
    const bool envOn = !from || Frames().FramesEnded() >= from;
    Level level = kLevels[std::clamp(DrawDistance(), 0, 3)];
    if (envOn)
    {
        level.lod = EnvFloat("SCDA_LOD_SCALE", level.lod);
        level.cull = EnvFloat("SCDA_CULL_SCALE", level.cull);
        level.lodOff |= getenv("SCDA_LOD_OFF") != nullptr;
    }
    ScaleField(base, g_size2, s + 280, std::max(level.lod, 0.0f));
    ScaleField(base, g_size1, s + 284, std::max(level.lod, 0.0f));
    ScaleField(base, g_cull, s + 60, std::max(level.cull, 0.0f));
    ScaleField(base, g_cullOverride, s + 64, std::max(level.cull, 0.0f));
    SetByte(base, g_lodToggle, s + 279, level.lodOff);
    static const bool noFade = getenv("SCDA_NO_DISTANCE_FADE") != nullptr;
    if (noFade)
        PPC_STORE_U8(s + 46, 0);
    static ULONGLONG next = 0;
    if (trace && GetTickCount64() >= next)
    {
        next = GetTickCount64() + 1000;
        auto F = [&](uint32_t o) { return double(std::bit_cast<float>(PPC_LOAD_U32(s + o))); };
        RuntimeLog("[lod] settings %08X fade %u lod %u size2 %g size1 %g cull %g/%g lr %08X\n", s, PPC_LOAD_U8(s + 46),
            PPC_LOAD_U8(s + 279), F(280), F(284), F(60), F(64), uint32_t(ctx.lr));
    }
}

// SCDA_TRACE_LOD=1 counters, logged every 5 s: LOD picks (sub_82EF31F8 writes the level into object
// +36 bits 21-22) and small-object cull tests (sub_82EF2FE8 returns 1 = smaller than the cull size).
static std::atomic<uint64_t> g_lodPicks[4], g_cullTests[2];
static void LodStats()
{
    static std::atomic<ULONGLONG> next{0};
    ULONGLONG n = next.load();
    if (GetTickCount64() < n || !next.compare_exchange_strong(n, GetTickCount64() + 5000))
        return;
    RuntimeLog("[lod] picks lod0 %llu lod1 %llu lod2 %llu lod3 %llu, cull tests visible %llu culled %llu\n",
        g_lodPicks[0].exchange(0), g_lodPicks[1].exchange(0), g_lodPicks[2].exchange(0), g_lodPicks[3].exchange(0),
        g_cullTests[0].exchange(0), g_cullTests[1].exchange(0));
}
static const bool g_traceLod = getenv("SCDA_TRACE_LOD") != nullptr;

extern "C" PPC_FUNC(__imp__sub_82EF31F8);
PPC_FUNC(sub_82EF31F8)
{
    const uint32_t object = ctx.r4.u32;
    __imp__sub_82EF31F8(ctx, base);
    if (!g_traceLod)
        return;
    g_lodPicks[(PPC_LOAD_U64(object + 36) >> 21) & 3]++;
    LodStats();
}

extern "C" PPC_FUNC(__imp__sub_82EF2FE8);
PPC_FUNC(sub_82EF2FE8)
{
    __imp__sub_82EF2FE8(ctx, base);
    if (!g_traceLod)
        return;
    g_cullTests[ctx.r3.u32 & 1]++;
    LodStats();
}
