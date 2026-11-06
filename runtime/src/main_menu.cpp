#include <runtime.h>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <recomp_page.h>
#include "online/xnet.h"

// Main menu (Scaleform FlashMenu). The UPLAY entry (MAINPAGE key PlayerStore, fscommand StartUPlay,
// which launches UPlayBrowser.xex = the process exits) becomes RECOMP SETTINGS: the menu's own camera
// travel to the settings spot (as SETTINGS plays it), then recomp_page.cpp draws the page; its Back
// plays the travel back to the main page.
// fscommand handlers: sub_82E7EB80 (menu pages) falls back to sub_82E91010 (shared: Localize,
// Get/SetGameSettings, ...); r4 = command (ASCII), r5 = arguments (ASCII).
extern "C" PPC_FUNC(__imp__sub_82E7EB80);
extern "C" PPC_FUNC(__imp__sub_82E91010);
extern "C" PPC_FUNC(__imp__sub_82E86A50); // 3D menu event handler (this, event id, ...)
extern "C" PPC_FUNC(__imp__sub_82E85358); // 3D menu vtbl+44 (this, command, arguments): TRAVEL_MENU
extern "C" PPC_FUNC(__imp__sub_82A8D3D8); // UE Localize(r3 section, r4 key, r5 file, r6, r7) -> r3 UTF-16BE (own 2 KB buffer)
extern "C" PPC_FUNC(__imp__sub_824B1EE8); // FString(r3 this, r4 ASCII)
extern "C" PPC_FUNC(__imp__sub_82B9FD18); // FString dtor
extern "C" PPC_FUNC(__imp__sub_824B2430); // Invoke result dtor (result + 32)
extern "C" PPC_FUNC(__imp__sub_82D00FB0); // UI event post (event, a, b, c)
extern "C" PPC_FUNC(__imp__sub_824F76A0); // profile object vt+84 (this, user): signed-in check
extern "C" PPC_FUNC(__imp__sub_82E49CB8); // -> menu movie; vtbl+52 = Invoke(result, movie, &path)

namespace
{
constexpr uint32_t kDeactivatePage = 0x821DAF00; // "_root.FlashMenu.frame.deactivateActivePage"

std::atomic<uint32_t> g_menu3d{0}; // 3D menu object (vtbl+44 = TRAVEL_MENU)

bool TraceFlash()
{
    static const bool on = getenv("SCDA_TRACE_FLASH") != nullptr;
    return on;
}

// Guest string at addr as ASCII, read as 8-bit or UTF-16BE (the first byte 0 = UTF-16BE).
std::string GuestText(uint32_t addr, size_t max = 160)
{
    std::string s;
    if (!addr)
        return "(null)";
    const bool wide = GuestLoad16(addr) < 0x100 && GuestLoad16(addr) != 0 && g_base[addr] == 0;
    for (size_t i = 0; i < max; i++)
    {
        const uint32_t c = wide ? GuestLoad16(addr + 2 * uint32_t(i)) : g_base[addr + i];
        if (!c)
            break;
        s += c < 0x80 ? char(c) : '?';
    }
    return s;
}

bool WideIs(uint32_t addr, const char* text)
{
    for (uint32_t i = 0;; i++)
    {
        const uint16_t c = GuestLoad16(addr + 2 * i);
        if (c != uint16_t(uint8_t(text[i])))
            return false;
        if (!c)
            return true;
    }
}

bool AsciiIs(uint32_t addr, const char* text) { return addr && strcmp(reinterpret_cast<const char*>(g_base + addr), text) == 0; }

// Into a Localize result (a 2 KB buffer of its own).
void StoreWide(uint32_t dst, const char* text)
{
    uint32_t i = 0;
    for (; text[i] && i < 1000; i++)
        GuestStore16(dst + 2 * i, uint16_t(uint8_t(text[i])));
    GuestStore16(dst + 2 * i, 0);
}

// Flash Invoke path() on the menu movie (vtbl+52), on the game thread.
void Invoke(const PPCContext& from, uint8_t* base, uint32_t asciiPath)
{
    static const uint32_t scratch = GuestAllocVirtual(0x100, 0x10);
    const uint32_t path = scratch, result = scratch + 0x40;
    PPCContext c = from;
    c.r3.u64 = path;
    c.r4.u64 = asciiPath;
    __imp__sub_824B1EE8(c, base);
    c = from;
    __imp__sub_82E49CB8(c, base);
    const uint32_t movie = c.r3.u32;
    c = from;
    c.r3.u64 = result;
    c.r4.u64 = movie;
    c.r5.u64 = path;
    (PPC_LOOKUP_FUNC(base, GuestLoad32(GuestLoad32(movie) + 52)))(c, base);
    c = from;
    c.r3.u64 = result + 32;
    __imp__sub_824B2430(c, base);
    c = from;
    c.r3.u64 = result + 12;
    __imp__sub_82B9FD18(c, base);
    c = from;
    c.r3.u64 = path;
    __imp__sub_82B9FD18(c, base);
}

// 3D menu vtbl+44: TRAVEL_MENU (camera travel, then the page).
void Travel(const PPCContext& from, uint8_t* base, uint32_t menu3d, const char* args)
{
    static const uint32_t scratch = GuestAllocVirtual(0x100, 0x10);
    strcpy(reinterpret_cast<char*>(base + scratch), "TRAVEL_MENU");
    strcpy(reinterpret_cast<char*>(base + scratch + 0x20), args);
    PPCContext c = from;
    c.r3.u64 = menu3d;
    c.r4.u64 = scratch;
    c.r5.u64 = scratch + 0x20;
    __imp__sub_82E85358(c, base);
}

void Log(const char* tag, PPCContext& ctx)
{
    RuntimeLog("[flash] %s %s | %s\n", tag, GuestText(ctx.r4.u32).c_str(), GuestText(ctx.r5.u32).c_str());
}
} // namespace

// Game thread, every frame (camera.cpp's view builder hook): a mission load closes the page.
void MainMenuTick(PPCContext&, uint8_t*)
{
    if (RecompPageIsOpen() && !GuestInFrontEnd())
        RecompPageClose();
}

PPC_FUNC(sub_82A8D3D8)
{
    const uint32_t sec = ctx.r3.u32, key = ctx.r4.u32, file = ctx.r5.u32;
    __imp__sub_82A8D3D8(ctx, base);
    const uint32_t text = ctx.r3.u32;
    if (TraceFlash())
        RuntimeLog("[loc] %s / %s / %s -> %08X %s\n", GuestText(sec).c_str(), GuestText(key).c_str(),
                   GuestText(file).c_str(), text, GuestText(text).c_str());
    if (!text || !sec || !key)
        return;
    if (WideIs(key, "PlayerStore") && WideIs(sec, "MAINPAGE"))
        StoreWide(text, "RECOMP SETTINGS");
    else if (WideIs(key, "PlayerStore") && WideIs(sec, "MAINPAGE_DESC"))
        StoreWide(text, "Display, frame rate and other settings of the PC version.");
    else if (WideIs(key, "MarketPage") && WideIs(sec, "PAGETITLES"))
        StoreWide(text, "RECOMP SETTINGS");
    else if (WideIs(key, "RedeemCode") && WideIs(sec, "TIPS"))
        StoreWide(text, "Reset to Defaults");
    // SCDA_PROMPT_SHOWALL=1 (icon table collection): the difficulty page's Rookie line and the Back tip show
    // every {Img.Buttons.IK_JoyN} icon from N = the value (1 = all), numbered.
    static const int showFrom = getenv("SCDA_PROMPT_SHOWALL") ? std::max(1, atoi(getenv("SCDA_PROMPT_SHOWALL"))) : 0; // first N shown
    static const bool showAll = showFrom > 0;
    if (showAll && ((WideIs(key, "NormalWithIcon") && WideIs(sec, "SPDIFFICULTYPAGE")) || (WideIs(key, "Back") && WideIs(sec, "TIPS"))))
    {
        std::string all;
        for (int i = showFrom; i <= 16; i++)
            all += std::to_string(i) + "{Img.Buttons.IK_Joy" + std::to_string(i) + "} ";
        StoreWide(text, all.c_str());
    }
}

PPC_FUNC(sub_82E7EB80)
{
    if (TraceFlash())
        Log("menu", ctx);
    if (AsciiIs(ctx.r4.u32, "StartUPlay"))
    {
        RuntimeLog("[menu] RECOMP SETTINGS\n");
        if (const uint32_t menu3d = g_menu3d)
        {
            // As SETTINGS: the main page closes, the camera travels to the settings spot, and the game's
            // EXCLUSIVE CONTENT page (MarketPage; offline the game never opens it) comes up there with its
            // title and transition but an empty list: recomp_page.cpp draws the rows in its place.
            Invoke(ctx, base, kDeactivatePage);
            Travel(ctx, base, menu3d, "EVENT=TRAVEL_A_C PAGE=MarketPage");
            RecompPageOpen();
        }
        ctx.r3.u64 = 1;
        return;
    }
    // Online (online/xnet.h): a hosted System Link match is published as a Steam lobby while it lasts.
    if (AsciiIs(ctx.r4.u32, "CreateSession"))
        xnet::HostStarted();
    else if (AsciiIs(ctx.r4.u32, "LeaveMatch") || AsciiIs(ctx.r4.u32, "UnInitMPConnection"))
        xnet::HostStopped();
    __imp__sub_82E7EB80(ctx, base);
}

// DLC list (this, TArray<u8>* ids, TArray<u8>* status): empty while the RECOMP SETTINGS page is open.
extern "C" PPC_FUNC(__imp__sub_82DDC888);
PPC_FUNC(sub_82DDC888)
{
    if (!RecompPageIsOpen())
        __imp__sub_82DDC888(ctx, base);
}

// 3D menu event handler (this, event id, ...): only remembers the 3D menu object.
PPC_FUNC(sub_82E86A50)
{
    g_menu3d = ctx.r3.u32;
    __imp__sub_82E86A50(ctx, base);
}

// 3D menu vtbl+44 (this, command, arguments; ASCII): TRAVEL_MENU "EVENT=TRAVEL_x_y PAGE=<page>".
PPC_FUNC(sub_82E85358)
{
    g_menu3d = ctx.r3.u32;
    if (TraceFlash())
        RuntimeLog("[flash] 3d %08X %s | %s\n", ctx.r3.u32, GuestText(ctx.r4.u32).c_str(), GuestText(ctx.r5.u32).c_str());
    // The page's Back (the game's MarketPage handles the B): camera from the settings spot to the main menu.
    if (AsciiIs(ctx.r4.u32, "TRAVEL_MENU") && RecompPageTakeBack())
    {
        static const uint32_t back = GuestAllocVirtual(0x40, 0x10);
        strcpy(reinterpret_cast<char*>(base + back), "EVENT=TRAVEL_C_A PAGE=IndexPage");
        ctx.r5.u64 = back;
    }
    const bool toMain = AsciiIs(ctx.r4.u32, "TRAVEL_MENU") && GuestText(ctx.r5.u32).find("EVENT=TO_MAIN") != std::string::npos;
    if (AsciiIs(ctx.r4.u32, "TRAVEL_MENU"))
        MainMenuSetIndexPage(GuestText(ctx.r5.u32).find("PAGE=IndexPage") != std::string::npos);
    const PPCContext saved = ctx;
    __imp__sub_82E85358(ctx, base);
    // Back from a mission (EXIT TO MAIN MENU, TO_MAIN): the menu movie is made again (FlashMenu.init) and
    // shows its main page items only once it has the profile (handleProfileChange, the callbackObject
    // settings: UI events 60/74/81/79). On first boot the profile object posts them after PRESS START
    // (storage message 4, sub_824F9420); after a mission nothing did, and the items stayed at alpha 0
    // (invisible, still usable). Post the same sequence.
    // EXPERIMENT, off by default: does not make the items visible yet (STATE.md). SCDA_MENU_REFRESH=1 on.
    static const bool noRefresh = getenv("SCDA_MENU_REFRESH") == nullptr;
    const uint32_t online = GuestLoad32(0x836E70E4); // profile/online object (vt 82005150)
    static bool refreshing = false; // event 60's handler travels TO_MAIN itself: post once, not again from there
    if (toMain && !noRefresh && online && !refreshing)
    {
        refreshing = true;
        constexpr uint32_t user = 0;
        PPCContext c = saved;
        c.r3.u64 = online, c.r4.u64 = user;
        __imp__sub_824F76A0(c, base); // vt+84: the user's sign-in check, event 60's first argument
        const uint32_t signedIn = c.r3.u32;
        auto post = [&](uint32_t ev, uint32_t a, uint32_t b) {
            PPCContext p = saved;
            p.r3.u64 = ev, p.r4.u64 = a, p.r5.u64 = b, p.r6.u64 = 0;
            __imp__sub_82D00FB0(p, base);
        };
        post(60, signedIn, user); // handleProfileChange + the callbackObject settings
        static const bool all = getenv("SCDA_MENU_REFRESH_ALL") != nullptr; // also 74/81/79
        if (all)
            post(74, user, 0), post(81, 0, 0), post(79, 0, 0);
        RuntimeLog("[menu] back from a mission: profile events re-posted (signed in %u)\n", signedIn);
        refreshing = false;
    }
}

PPC_FUNC(sub_82E91010)
{
    if (TraceFlash())
        Log("common", ctx);
    if (TraceFlash())
    {
        // The menu movie's vtable (Invoke +52, SetVariable +104): logged when it changes.
        PPCContext c = ctx;
        __imp__sub_82E49CB8(c, base);
        static uint32_t lastVt = 0;
        const uint32_t movie = c.r3.u32, vt = movie ? GuestLoad32(movie) : 0;
        if (vt && vt != lastVt)
        {
            lastVt = vt;
            RuntimeLog("[flash] movie %08X vt %08X invoke %08X setvar %08X\n", movie, vt, GuestLoad32(vt + 52), GuestLoad32(vt + 104));
        }
    }
    __imp__sub_82E91010(ctx, base);
}

// Inline image tag builder (out FString*, key object, src FString*, attributes FString*): "{Img.Group.Key}"
// tokens in UI text become "<img src='%s' %s %s>". SCDA_TRACE_FLASH: log what each token turns into.
extern "C" PPC_FUNC(__imp__sub_82E79558);
PPC_FUNC(sub_82E79558)
{
    const uint32_t out = ctx.r3.u32, src = ctx.r5.u32, attrs = ctx.r6.u32;
    const std::string s = src && GuestLoad32(src) ? GuestText(GuestLoad32(src)) : "", a = attrs && GuestLoad32(attrs) ? GuestText(GuestLoad32(attrs)) : "";
    __imp__sub_82E79558(ctx, base);
    if (TraceFlash())
        RuntimeLog("[img] src '%s' attrs '%s' -> %s\n", s.c_str(), a.c_str(), GuestLoad32(out) ? GuestText(GuestLoad32(out), 300).c_str() : "");
}

// SCDA_TRACE_FLASH: engine -> menu movie calls. Invoke (result, movie, FString* path, ...) and
// SetVariable (movie, FString* path, value...): the path and the first argument words.
extern "C" PPC_FUNC(__imp__sub_82E6D9D0);
PPC_FUNC(sub_82E6D9D0)
{
    if (TraceFlash() && ctx.r5.u32 && GuestLoad32(ctx.r5.u32))
        RuntimeLog("[flash>] invoke %s (%08X %08X)\n", GuestText(GuestLoad32(ctx.r5.u32)).c_str(), ctx.r6.u32, ctx.r7.u32);
    __imp__sub_82E6D9D0(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6D430);
PPC_FUNC(sub_82E6D430)
{
    if (TraceFlash() && ctx.r4.u32 && GuestLoad32(ctx.r4.u32))
        RuntimeLog("[flash>] setvar %s (%08X %08X)\n", GuestText(GuestLoad32(ctx.r4.u32)).c_str(), ctx.r5.u32, ctx.r6.u32);
    __imp__sub_82E6D430(ctx, base);
}


// SCDA_TRACE_FLASH: the other movie vtable slots (Invoke / SetVariable variants), path from r4 or r5 (FString*).
static std::string FlashPathArg(uint32_t p)
{
    if (p < 0x10000 || p >= 0xF0000000u) return "";
    const uint32_t s = GuestLoad32(p);
    if (s < 0x10000 || s >= 0xF0000000u) return "";
    std::string t = GuestText(s, 120);
    return t.rfind("_root", 0) == 0 || t.rfind("_global", 0) == 0 ? t : "";
}
extern "C" PPC_FUNC(__imp__sub_82E6DA78);
PPC_FUNC(sub_82E6DA78)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+56 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6DA78(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6E088);
PPC_FUNC(sub_82E6E088)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+60 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6E088(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6DBE8);
PPC_FUNC(sub_82E6DBE8)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+64 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6DBE8(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6DC90);
PPC_FUNC(sub_82E6DC90)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+68 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6DC90(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6DB40);
PPC_FUNC(sub_82E6DB40)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+72 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6DB40(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6D3A8);
PPC_FUNC(sub_82E6D3A8)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+76 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6D3A8(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6DD58);
PPC_FUNC(sub_82E6DD58)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+80 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6DD58(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6D3D0);
PPC_FUNC(sub_82E6D3D0)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+84 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6D3D0(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6D400);
PPC_FUNC(sub_82E6D400)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+88 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6D400(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6DE00);
PPC_FUNC(sub_82E6DE00)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+92 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6DE00(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6DE80);
PPC_FUNC(sub_82E6DE80)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+96 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6DE80(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6DF00);
PPC_FUNC(sub_82E6DF00)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+100 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6DF00(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6D458);
PPC_FUNC(sub_82E6D458)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+108 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6D458(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6D480);
PPC_FUNC(sub_82E6D480)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+112 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6D480(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6D4A8);
PPC_FUNC(sub_82E6D4A8)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+116 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6D4A8(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6D4D0);
PPC_FUNC(sub_82E6D4D0)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+120 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6D4D0(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6D500);
PPC_FUNC(sub_82E6D500)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+124 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6D500(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6D530);
PPC_FUNC(sub_82E6D530)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+128 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6D530(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6D560);
PPC_FUNC(sub_82E6D560)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+132 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6D560(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6D590);
PPC_FUNC(sub_82E6D590)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+136 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6D590(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6D5C0);
PPC_FUNC(sub_82E6D5C0)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+140 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6D5C0(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6D5F0);
PPC_FUNC(sub_82E6D5F0)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+144 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6D5F0(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82E6D620);
PPC_FUNC(sub_82E6D620)
{
    if (TraceFlash())
    {
        std::string p = FlashPathArg(ctx.r5.u32);
        if (p.empty()) p = FlashPathArg(ctx.r4.u32);
        if (!p.empty()) RuntimeLog("[flash>] vt+148 %s (%08X %08X)\n", p.c_str(), ctx.r6.u32, ctx.r7.u32);
    }
    __imp__sub_82E6D620(ctx, base);
}
