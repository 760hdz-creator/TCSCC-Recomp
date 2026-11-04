// Statically linked XAudio's XMA hardware access (Conviction). It reads the context array address
// from register 0x7FEA1800 (sub_827F5620, into 0x836EBFC8) and kicks / locks its contexts with one
// bit per context in registers 0x7FEA1940 / 0x7FEA1A40. Voice set: +0 count, +4 flags, +8 entries
// (96 bytes each: +64 hardware context, +80 u16 context index).
// - Kick sub_827F6018(set): flushes the set's dirty context quarters into the hardware contexts,
//   kicks every context, clears flag 0x10000.
// - Lock sub_827F5EA8(): for every set in the 320-slot table 0x836EBFD0 with flag 0x4 and without
//   0x20000, locks its contexts (copying the hardware state back) and sets 0x20000.
// shortcut: hooks on those functions instead of trapping the register writes.
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
