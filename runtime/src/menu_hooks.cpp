// Extra Settings inside the game's own OPTIONS page (docs/re_options_menu.md). The page's TV CALIBRATION
// button (B_Adjust_display) becomes EXTRA SETTINGS: its label is rewritten where the localizer hands
// it out, and a click on it opens the F1 menu instead of the calibration page (BRIGHTNESS and CONTRAST
// stay on the OPTIONS page itself). SCDA_NATIVE_CALIBRATION=1 keeps the game's own button.
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <runtime.h>
#include "overlay.h"

extern "C" PPC_FUNC(__imp__sub_8258C230); // Localize(section, key, package, ...) -> UTF-16BE temp buffer
extern "C" PPC_FUNC(__imp__sub_82A88E60); // options pages' click handler (this, event)
extern "C" PPC_FUNC(__imp__sub_82786B48); // menu: is the current page <name> (ASCII)
extern "C" PPC_FUNC(__imp__sub_82A80E50); // event: came from the widget <name> (ASCII), 0 = no

namespace
{
constexpr uint32_t kPageOptions = 0x8212D2C4;      // "P_Options"
constexpr uint32_t kButtonCalibration = 0x8212D34C; // "B_Adjust_display"

bool Enabled()
{
    static const bool on = getenv("SCDA_NATIVE_CALIBRATION") == nullptr;
    return on;
}

// Guest UTF-16BE string at addr equals text (ASCII).
bool WideEquals(uint8_t* base, uint32_t addr, const char* text)
{
    if (!addr)
        return false;
    for (size_t i = 0;; i++)
    {
        const uint16_t c = PPC_LOAD_U16(addr + 2 * uint32_t(i));
        if (c != uint16_t(uint8_t(text[i])))
            return false;
        if (!c)
            return true;
    }
}
} // namespace

PPC_FUNC(sub_8258C230)
{
    const uint32_t key = ctx.r4.u32;
    __imp__sub_8258C230(ctx, base);
    // Same length (14), written over the localizer's temporary buffer: other languages keep their text
    // (the button still opens Extra Settings).
    const uint32_t text = ctx.r3.u32;
    if (Enabled() && WideEquals(base, key, "Calibrate") && WideEquals(base, text, "TV CALIBRATION"))
    {
        const char* label = "EXTRA SETTINGS";
        for (uint32_t i = 0; label[i]; i++)
            PPC_STORE_U16(text + 2 * i, uint16_t(label[i]));
    }
}

PPC_FUNC(sub_82A88E60)
{
    if (Enabled())
    {
        const uint32_t self = ctx.r3.u32, event = ctx.r4.u32;
        PPCContext saved = ctx;
        ctx.r3.u64 = PPC_LOAD_U32(self + 16);
        ctx.r4.u64 = kPageOptions;
        __imp__sub_82786B48(ctx, base);
        bool calibration = (ctx.r3.u32 & 0xFF) != 0;
        if (calibration)
        {
            ctx = saved;
            ctx.r3.u64 = event;
            ctx.r4.u64 = kButtonCalibration;
            __imp__sub_82A80E50(ctx, base);
            calibration = ctx.r3.s32 != 0;
        }
        ctx = saved;
        if (calibration)
        {
            RuntimeLog("[menu] OPTIONS > EXTRA SETTINGS\n");
            OverlayRequestOpen();
            PPC_STORE_U8(PPC_LOAD_U32(self + 16) + 24, 0); // as the handler's own exits
            ctx.r3.u64 = 0;
            return;
        }
    }
    __imp__sub_82A88E60(ctx, base);
}

// SCDA_TRACE_RUMBLE=1: the title's vibration setter sub_821DFFD8(pad, f1 low, f2 high) and its
// per-pad enable test sub_821C6660(manager, pad) (0 = the profile turned vibration off).
extern "C" PPC_FUNC(__imp__sub_821DFFD8);
extern "C" PPC_FUNC(__imp__sub_821C6660);
static bool TraceRumble()
{
    static const bool on = getenv("SCDA_TRACE_RUMBLE") != nullptr;
    return on;
}

PPC_FUNC(sub_821DFFD8)
{
    if (TraceRumble())
        RuntimeLog("[rumble] set %.3f %.3f\n", ctx.f1.f64, ctx.f2.f64);
    __imp__sub_821DFFD8(ctx, base);
}

PPC_FUNC(sub_821C6660)
{
    const uint32_t pad = ctx.r4.u32;
    __imp__sub_821C6660(ctx, base);
    if (TraceRumble())
        RuntimeLog("[rumble] enabled(pad %u) = %u\n", pad, ctx.r3.u32);
}

// Script rumble request sub_8299AA28(pawn, f1 left, f2 right, f3 time): plays only if the CanRumble
// event returns true, then sub_8266EC98 drives the pad.
extern "C" PPC_FUNC(__imp__sub_8299AA28);
extern "C" PPC_FUNC(__imp__sub_8266EC98);
PPC_FUNC(sub_8299AA28)
{
    if (TraceRumble())
    {
        const uint32_t prefs = PPC_LOAD_U32(ctx.r3.u32 + 1888); // +44 bit 2 (0x20000000) = vibration on
        RuntimeLog("[rumble] request %.3f %.3f %.3f prefs %08X flags %08X\n", ctx.f1.f64, ctx.f2.f64, ctx.f3.f64,
            prefs, prefs ? PPC_LOAD_U32(prefs + 44) : 0);
    }
    __imp__sub_8299AA28(ctx, base);
}

PPC_FUNC(sub_8266EC98)
{
    if (TraceRumble())
        RuntimeLog("[rumble] play %.3f %.3f %.3f\n", ctx.f1.f64, ctx.f2.f64, ctx.f3.f64);
    __imp__sub_8266EC98(ctx, base);
}

// Rumble tick sub_8266EE18(state, f1 dt, r5 float* shake, r6 float* vibrate).
extern "C" PPC_FUNC(__imp__sub_8266EE18);
PPC_FUNC(sub_8266EE18)
{
    const uint32_t shake = ctx.r5.u32, vibrate = ctx.r6.u32;
    __imp__sub_8266EE18(ctx, base);
    if (TraceRumble())
    {
        const uint32_t s = PPC_LOAD_U32(shake), v = PPC_LOAD_U32(vibrate);
        float fs, fv;
        memcpy(&fs, &s, 4);
        memcpy(&fv, &v, 4);
        static uint32_t calls;
        if (fs != 0 || fv != 0 || (++calls & 1023) == 1)
            RuntimeLog("[rumble] tick #%u shake %.3f vibrate %.3f\n", calls, fs, fv);
    }
}

// Rumble tick gates: sub_82615478(obj) = event CheckMissionStatus, sub_82615410(obj) = CanRumble.
extern "C" PPC_FUNC(__imp__sub_82615478);
extern "C" PPC_FUNC(__imp__sub_82615410);
PPC_FUNC(sub_82615478)
{
    __imp__sub_82615478(ctx, base);
    static uint32_t last = ~0u;
    if (TraceRumble() && ctx.r3.u32 != last)
        RuntimeLog("[rumble] gate A = %u\n", last = ctx.r3.u32);
}

PPC_FUNC(sub_82615410)
{
    __imp__sub_82615410(ctx, base);
    static uint32_t last = ~0u;
    if (TraceRumble() && ctx.r3.u32 != last)
        RuntimeLog("[rumble] gate CanRumble = %u\n", last = ctx.r3.u32);
}
