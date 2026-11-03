#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <runtime.h>

// SCDA_TIMELINE=<start s>:<ms>: records pipeline events (CP waits/idle, frame hand-overs, renderer
// presents, fences) with microsecond times for that window, then logs them as [tl] lines.
struct TimelineRecord
{
    LONGLONG t;
    char kind;
    uint32_t a, b;
};
static TimelineRecord g_tl[1 << 18];
static std::atomic<uint32_t> g_tlCount;
static std::atomic<bool> g_tlOn;
static LONGLONG g_tlStart, g_tlFreq;

void TimelineEvent(char kind, uint32_t a, uint32_t b)
{
    if (!g_tlOn.load(std::memory_order_relaxed))
        return;
    const uint32_t i = g_tlCount.fetch_add(1);
    if (i >= std::size(g_tl))
        return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_tl[i] = { now.QuadPart, kind, a, b };
}

void TimelineStart()
{
    const char* spec = getenv("SCDA_TIMELINE");
    if (!spec)
        return;
    const unsigned start = unsigned(atoi(spec));
    const unsigned ms = strchr(spec, ':') ? unsigned(atoi(strchr(spec, ':') + 1)) : 200;
    std::thread([start, ms]
    {
        Sleep(start * 1000);
        LARGE_INTEGER f, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t);
        g_tlFreq = f.QuadPart;
        g_tlStart = t.QuadPart;
        g_tlOn = true;
        Sleep(ms);
        g_tlOn = false;
        Sleep(50);
        const uint32_t n = std::min<uint32_t>(g_tlCount.load(), uint32_t(std::size(g_tl)));
        std::vector<TimelineRecord> v(g_tl, g_tl + n);
        std::sort(v.begin(), v.end(), [](auto& x, auto& y) { return x.t < y.t; });
        for (auto& e : v)
            RuntimeLog("[tl] %9.3f %c %08X %u\n", 1000.0 * double(e.t - g_tlStart) / double(g_tlFreq), e.kind, e.a, e.b);
    }).detach();
}
