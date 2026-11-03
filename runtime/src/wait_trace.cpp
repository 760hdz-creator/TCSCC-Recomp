#include <windows.h>
#include <runtime.h>

// Guest GPU waits (callers of the D3D spin-wait sub_821FA428), for SCDA_TIMELINE: one 'A' event at
// return for waits over 100 us; a = function address, b = duration us.
// sub_821F33A8 waits for the GPU fence at writeback +4 (ring position), sub_821F3440 for ring space.
extern "C" PPC_FUNC(__imp__sub_821F33A8);
extern "C" PPC_FUNC(__imp__sub_821F3440);
extern "C" PPC_FUNC(__imp__sub_821F4088);
extern "C" PPC_FUNC(__imp__sub_8220A638);

static void TimedWait(PPCFunc* wait, uint32_t address, PPCContext& ctx, uint8_t* base)
{
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    wait(ctx, base);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    const uint32_t us = uint32_t((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart);
    if (us > 100)
        TimelineEvent('A', address, us);
}

PPC_FUNC(sub_821F33A8) { TimedWait(__imp__sub_821F33A8, 0x821F33A8, ctx, base); }
PPC_FUNC(sub_821F3440) { TimedWait(__imp__sub_821F3440, 0x821F3440, ctx, base); }
PPC_FUNC(sub_821F4088) { TimedWait(__imp__sub_821F4088, 0x821F4088, ctx, base); }
PPC_FUNC(sub_8220A638) { TimedWait(__imp__sub_8220A638, 0x8220A638, ctx, base); }
