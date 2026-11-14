#include <windows.h>
#include <runtime.h>
#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <vector>

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
    // SCDA_TRACE_WAIT=1: every 30 s, the guest callers that blocked longest (which fence the title waits on).
    static const bool traceWait = getenv("SCDA_TRACE_WAIT") != nullptr;
    if (traceWait)
    {
        static std::mutex m;
        static std::map<uint32_t, std::pair<uint64_t, uint32_t>> byLr; // lr -> (us, calls)
        static ULONGLONG last = GetTickCount64();
        // Key: lr plus the guest callers one and two frames up (saved LR at -8 of each back-chain frame).
        uint32_t up[5] = {};
        for (uint32_t sp = ctx.r1.u32, i = 0; i < 5 && sp > 0x10000 && sp < 0xF0000000u; i++)
        {
            const uint32_t next = GuestLoad32(sp);
            if (next <= sp || next >= 0xF0000000u)
                break;
            up[i] = GuestLoad32(next - 8);
            sp = next;
        }
        std::lock_guard lock(m);
        static std::map<uint32_t, std::array<uint32_t, 5>> chains;
        const uint32_t key = lr ^ up[0] * 2654435761u ^ up[1] ^ up[2] * 40503u ^ up[3] * 7u;
        chains[key] = { up[0], up[1], up[2], up[3], up[4] };
        auto& e = byLr[key];
        e.first += us, e.second++;
        if (GetTickCount64() - last >= 30000)
        {
            last = GetTickCount64();
            std::vector<std::pair<uint64_t, uint32_t>> top;
            for (auto& [k, v] : byLr)
                top.push_back({ v.first, k });
            std::sort(top.rbegin(), top.rend());
            for (size_t i = 0; i < top.size() && i < 6; i++)
            {
                const auto& c = chains[top[i].second];
                RuntimeLog("[wait] callers %08X < %08X < %08X < %08X < %08X: %.0f ms over 30 s, %u calls\n", c[0], c[1], c[2], c[3], c[4],
                    top[i].first / 1000.0, byLr[top[i].second].second);
            }
            byLr.clear();
        }
    }
    if (us > 100)
        TimelineEvent('A', lr, us);
}
