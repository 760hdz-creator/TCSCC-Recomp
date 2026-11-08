#include <windows.h>
#include <runtime.h>

// Guest GPU waits (Conviction addresses). Total time goes to the [perf] line (GuestGpuWaitUsTake);
// for SCDA_TIMELINE one 'A' event at return for waits over 100 us: a = guest return address
// (caller), b = duration us.
// sub_82514FE0 = D3D BlockOnFence: spins (poll body sub_8251F7E8: db16cyc, 5 s hang check) until
// the done fence *(device +10896) reaches the fence in r4.
extern "C" PPC_FUNC(__imp__sub_82514FE0);

PPC_FUNC(sub_82514FE0)
{
    const uint32_t lr = uint32_t(ctx.lr);
    static const LONGLONG freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    __imp__sub_82514FE0(ctx, base);
    QueryPerformanceCounter(&t1);
    const uint32_t us = uint32_t((t1.QuadPart - t0.QuadPart) * 1000000 / freq);
    GuestGpuWaitAdd(us);
    if (us > 100)
        TimelineEvent('A', lr, us);
}
