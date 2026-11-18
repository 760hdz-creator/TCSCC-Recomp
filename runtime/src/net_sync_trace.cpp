// Co-op lockstep clock log (forge #3 "Out of sync"). Conviction's co-op simulation is a Quazal
// turn clock: object *0x837B177C (state +164: 3 = running, 4 = stop requested by the peer,
// 5/6 = ended; turn +152, stop turn +160, turns per second +184; the main loop's frame time is
// 1 / rate while state == 3). Advance sub_8310C0F0(clock, now) returns 0 = stepped, 1 = too early,
// 2 = waiting for the peer's turn, 3 = session check failed (-> state 6), 5 = abort flag +2749
// (-> state 6), 6 = already ended. Stop sub_83109388 sets state 4 (stop at turn +156 + 1).
// Logged: every state change and every 10 s of a running clock (waits, rate), always on (a few
// lines per match) so player logs show why a match ended.
#include <windows.h>
#include <runtime.h>
#include <cstring>

extern "C" PPC_FUNC(__imp__sub_8310C0F0);
extern "C" PPC_FUNC(__imp__sub_83109388);

namespace
{
float LoadF(uint32_t a)
{
    const uint32_t b = GuestLoad32(a);
    float v;
    memcpy(&v, &b, 4);
    return v;
}

struct Stats
{
    uint32_t calls = 0, codes[8] = {}, lastState = ~0u;
    uint64_t windowStart = 0, waitStart = 0, waitMs = 0, longestWaitMs = 0;
} g_sync;

void LogClock(const char* what, uint32_t clock, uint32_t code)
{
    RuntimeLog("[sync] %s: clock %08X state %u turn %u/%u stop %u rate %.2f missed %u flags %u/%u/%u code %u\n", what,
               clock, GuestLoad32(clock + 164), GuestLoad32(clock + 152), GuestLoad32(clock + 156),
               GuestLoad32(clock + 160), LoadF(clock + 184), GuestLoad32(clock + 2752),
               g_base[clock + 2748], g_base[clock + 2749], g_base[clock + 2750], code);
}
} // namespace

PPC_FUNC(sub_8310C0F0)
{
    const uint32_t clock = ctx.r3.u32;
    const uint32_t before = GuestLoad32(clock + 164);
    __imp__sub_8310C0F0(ctx, base);
    const uint32_t code = ctx.r3.u32, after = GuestLoad32(clock + 164);
    Stats& s = g_sync;
    const uint64_t now = GetTickCount64();
    if (after != before || after != s.lastState)
    {
        LogClock(after == 6 && code == 3 ? "ENDED: session check failed" : after == 6 && code == 5 ? "ENDED: abort flag"
                 : after == 5 ? "ended at stop turn" : "state change", clock, code);
        s.lastState = after;
    }
    // Waiting time for the peer's turns (code 2 runs).
    if (code == 2 && !s.waitStart)
        s.waitStart = now;
    if (code != 2 && s.waitStart)
    {
        const uint64_t w = now - s.waitStart;
        s.waitMs += w, s.longestWaitMs = w > s.longestWaitMs ? w : s.longestWaitMs, s.waitStart = 0;
    }
    s.calls++, s.codes[code < 8 ? code : 7]++;
    if (!s.windowStart)
        s.windowStart = now;
    if (now - s.windowStart >= 10000)
    {
        RuntimeLog("[sync] 10 s: turn %u rate %.2f | stepped %u early %u waiting %u (%llu ms, longest %llu ms) other %u\n",
                   GuestLoad32(clock + 152), LoadF(clock + 184), s.codes[0], s.codes[1], s.codes[2],
                   (unsigned long long)s.waitMs, (unsigned long long)s.longestWaitMs,
                   s.calls - s.codes[0] - s.codes[1] - s.codes[2]);
        const uint32_t keep = s.lastState;
        s = Stats{};
        s.lastState = keep, s.windowStart = now;
    }
}

PPC_FUNC(sub_83109388)
{
    const uint32_t clock = ctx.r3.u32, lr = uint32_t(ctx.lr);
    __imp__sub_83109388(ctx, base);
    if (ctx.r3.u32)
    {
        LogClock("STOP requested", clock, 0);
        uint64_t t = (uint64_t(GuestLoad32(0x837B1820)) << 32) | GuestLoad32(0x837B1824); double timeout; memcpy(&timeout, &t, 8);
        RuntimeLog("[sync] stop caller lr %08X (peer-wait timeout %.3f s)\n", lr, timeout);
    }
}
