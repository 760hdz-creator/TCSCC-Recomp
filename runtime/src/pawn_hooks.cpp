#include <runtime.h>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <cstdio>

// Guest hooks for pawn_trace.cpp (not linked into scda_tests: needs the recompiled code).
namespace
{
constexpr uint32_t kLoc = 0x94, kRot = 0xA0;
float F(uint32_t a) { return std::bit_cast<float>(GuestLoad32(a)); }
} // namespace

// SCDA_TRACE_VIEWBLEND=<every>: camera writer sub_82C1EAC8(S) copies Location/Rotation of
// A = *(S+56) and B = *(S+60). Log both actors every n calls.
extern "C" PPC_FUNC(__imp__sub_82C1EAC8);
PPC_FUNC(sub_82C1EAC8)
{
    static const uint32_t every = getenv("SCDA_TRACE_VIEWBLEND") ? uint32_t(atoi(getenv("SCDA_TRACE_VIEWBLEND"))) : 0;
    static uint32_t calls;
    const uint32_t s = ctx.r3.u32;
    __imp__sub_82C1EAC8(ctx, base);
    if (!every || calls++ % every)
        return;
    for (int i = 0; i < 2; i++)
    {
        const uint32_t a = GuestLoad32(s + 56 + 4 * i);
        if (!a)
        {
            RuntimeLog("[vblend] call %u S %08X %c null\n", calls, s, 'A' + i);
            continue;
        }
        RuntimeLog("[vblend] call %u S %08X %c %08X vt %08X loc %.1f %.1f %.1f rot %08X %08X %08X\n", calls, s, 'A' + i,
            a, GuestLoad32(a), F(a + kLoc), F(a + kLoc + 4), F(a + kLoc + 8), GuestLoad32(a + kRot),
            GuestLoad32(a + kRot + 4), GuestLoad32(a + kRot + 8));
    }
}

// sub_82CE28A8(progress, campaign, mission): 1 when the mission is not locked (state @+260 != 1).
// Scene Select drops missions it returns 0 for (GetMissionList sub_82E7EB80).
// SCDA_UNLOCK_ALL=1 (test aid): every in-range mission counts as unlocked.
extern "C" PPC_FUNC(__imp__sub_82CE28A8);
PPC_FUNC(sub_82CE28A8)
{
    static const bool all = getenv("SCDA_UNLOCK_ALL") != nullptr;
    const uint32_t p = ctx.r3.u32;
    const int32_t c = ctx.r4.s32, m = ctx.r5.s32;
    __imp__sub_82CE28A8(ctx, base);
    if (!all || ctx.r3.u32 || c < 0 || m < 0 || c >= int32_t(GuestLoad32(p + 120)))
        return;
    const uint32_t camp = GuestLoad32(GuestLoad32(p + 116) + 4 * uint32_t(c));
    if (m < int32_t(GuestLoad32(camp + 28)))
        ctx.r3.u64 = 1;
}
