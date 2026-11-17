#include <windows.h>
#include <runtime.h>
#include "gpu_state.h"
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
    // The title's lock of its dynamic sprite vertex buffer (sub_82514168 lr 825142B8 < 82514570 <
    // 8290E718) waits once a frame for the GPU to EXECUTE the fence after the draws that read it:
    // the game thread then idles while the renderer works (~290 ms/s at 85 fps). The draws' vertex
    // and index data are copied when the command processor PARSES them, so this one wait ends when
    // the fence is parsed. Same kick as the original when the fence is not submitted yet.
    // EXPERIMENT, default off (no measured gain before the interrupt-latency fix; sprite correctness
    // not verified): SCDA_VB_WAIT_PARSED=1 enables it.
    static const bool waitExecuted = getenv("SCDA_VB_WAIT_PARSED") == nullptr;
    bool early = false;
    if (!waitExecuted && lr == 0x825142B8 && ctx.r4.u32)
    {
        const uint32_t sp1 = GuestLoad32(ctx.r1.u32), sp2 = sp1 > 0x10000 && sp1 < 0xF0000000u ? GuestLoad32(sp1) : 0;
        if (sp2 > 0x10000 && sp2 < 0xF0000000u && GuestLoad32(sp1 - 8) == 0x82514570 && GuestLoad32(sp2 - 8) == 0x8290E718)
        {
            const uint32_t device = ctx.r3.u32, target = ctx.r4.u32;
            const uint32_t fenceVa = GuestLoad32(device + 10896);
            const uint32_t physical = GuestPhysical(fenceVa & ~3u);
            if (target == GuestLoad32(device + 10908) && !(ctx.r7.u32 & 0xFF) && !GuestLoad32(device + 13408))
            {
                PPCContext saved = ctx; // kick: the fence is not in the ring yet (as sub_82514FE0 does)
                ctx.r3.u64 = device;
                ctx.lr = 0x82515040;
                sub_82515DE0(ctx, base);
                ctx = saved;
            }
            CommandProcessorWake();
            const ULONGLONG start = GetTickCount64();
            for (uint32_t spins = 0;; spins++)
            {
                uint32_t parsed = 0;
                const uint32_t cur = GuestLoad32(device + 10908), done = GuestLoad32(fenceVa);
                if ((cur - target) >= (cur - done) || (CpFenceParsedValue(physical, parsed) && (cur - target) >= (cur - parsed)))
                {
                    early = true;
                    break;
                }
                if (GetTickCount64() - start > 100)
                    break; // fall back to the real wait
                if (spins > 64)
                    SwitchToThread();
            }
        }
    }
    if (!early)
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
