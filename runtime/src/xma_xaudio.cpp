// Statically linked XAudio's XMA hardware access (Conviction). It reads the context array address
// from register 0x7FEA1800 (sub_827F5620, into 0x836EBFC8) and kicks / locks its contexts with one
// bit per context in registers 0x7FEA1940 / 0x7FEA1A40. Voice set: +0 count, +4 flags, +8 entries
// (96 bytes each: +64 hardware context, +80 u16 context index).
// - Kick sub_827F6018(set): flushes the set's dirty context quarters into the hardware contexts,
//   kicks every context, clears flag 0x10000.
// - Lock sub_827F5EA8(): for every set in the 320-slot table 0x836EBFD0 with flag 0x4 and without
//   0x20000, locks its contexts (copying the hardware state back) and sets 0x20000.
// shortcut: hooks on those functions instead of trapping the register writes.
#include <windows.h>
#include <runtime.h>
#include <vector>

void XmaEnsureArray();
void XmaKickVoiceSet(uint32_t set);
void XmaLockVoiceSet(uint32_t set);

extern "C" PPC_FUNC(__imp__sub_827F5620);
PPC_FUNC(sub_827F5620)
{
    XmaEnsureArray();
    __imp__sub_827F5620(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_827F6018);
PPC_FUNC(sub_827F6018)
{
    const uint32_t set = ctx.r3.u32;
    __imp__sub_827F6018(ctx, base);
    XmaKickVoiceSet(set);
}

extern "C" PPC_FUNC(__imp__sub_827F5EA8);
PPC_FUNC(sub_827F5EA8)
{
    constexpr uint32_t kSets = 0x836EBFD0, kSlots = 320;
    std::vector<uint32_t> locked;
    for (uint32_t i = 0; i < kSlots; i++)
        if (const uint32_t set = GuestLoad32(kSets + 4 * i))
            if ((GuestLoad32(set + 4) & 0x20004) == 0x4)
                locked.push_back(set);
    __imp__sub_827F5EA8(ctx, base);
    for (uint32_t set : locked)
        XmaLockVoiceSet(set);
}

// SCDA_TRACE_XMA_SERVICE=1: the XMA voice service sub_83218A78(voice) hands each voice's ready output
// spans to the callback *(voice+12); log every distinct (voice, callback) once.
#include <mutex>
#include <set>
extern "C" PPC_FUNC(__imp__sub_83218A78);
PPC_FUNC(sub_83218A78)
{
    static const bool trace = getenv("SCDA_TRACE_XMA_SERVICE") != nullptr;
    if (trace)
    {
        static std::mutex m;
        static std::set<uint64_t> seen;
        const uint32_t v = ctx.r3.u32, cb = GuestLoad32(v + 12);
        std::lock_guard lock(m);
        if (seen.insert(uint64_t(v) << 32 | cb).second && seen.size() < 64)
            RuntimeLog("[xmasvc] voice %08X owner %08X cb %08X +16 %08X +20 %08X ctx %08X\n", v, GuestLoad32(v),
                cb, GuestLoad32(v + 16), GuestLoad32(v + 20), GuestLoad32(v + 8));
    }
    __imp__sub_83218A78(ctx, base);
}

// SCDA_TRACE_XMA_SERVICE=1 too: the consumer sub_8324FAB0(spans {obj, ptr1, len1, ptr2, len2, ...})
// copies the ready output into the stream object's rings; once per second per object: calls, bytes
// offered, and the object's counters (+156 shift, +228 pending, +240, +256, +380, +396 skip, +400 total).
#include <unordered_map>
extern "C" PPC_FUNC(__imp__sub_8324FAB0);
PPC_FUNC(sub_8324FAB0)
{
    static const bool trace = getenv("SCDA_TRACE_XMA_SERVICE") != nullptr;
    const uint32_t s = ctx.r3.u32;
    __imp__sub_8324FAB0(ctx, base);
    if (!trace)
        return;
    struct Stat { ULONGLONG next; uint32_t calls, bytes; };
    static std::mutex m;
    static std::unordered_map<uint32_t, Stat> stats;
    const uint32_t o = GuestLoad32(s);
    std::lock_guard lock(m);
    Stat& st = stats[o];
    st.calls++;
    st.bytes += GuestLoad32(s + 8) + GuestLoad32(s + 16);
    if (GetTickCount64() >= st.next && stats.size() < 32)
    {
        RuntimeLog("[xmasvc] t %llu obj %08X calls %u bytes %u | 156 %u 228 %u 240 %u 256 %u 380 %u 396 %u 400 %u\n",
            GetTickCount64() / 1000, o, st.calls, st.bytes, GuestLoad32(o + 156), GuestLoad32(o + 228), GuestLoad32(o + 240),
            GuestLoad32(o + 256), GuestLoad32(o + 380), GuestLoad32(o + 396), GuestLoad32(o + 400));
        st = { GetTickCount64() + 1000, 0, 0 };
    }
}
