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
    if (AsciiIs(ctx.r4.u32, "TRAVEL_MENU"))
        MainMenuSetIndexPage(GuestText(ctx.r5.u32).find("PAGE=IndexPage") != std::string::npos);
    __imp__sub_82E85358(ctx, base);
}

PPC_FUNC(sub_82E91010)
{
    if (TraceFlash())
        Log("common", ctx);
    __imp__sub_82E91010(ctx, base);
}
