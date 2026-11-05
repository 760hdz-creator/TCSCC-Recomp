#include <runtime.h>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <settings.h>
#include "overlay.h"

// Main menu (Scaleform FlashMenu). The UPLAY entry (MAINPAGE key PlayerStore, fscommand StartUPlay,
// which launches UPlayBrowser.xex = the process exits) becomes RECOMP SETTINGS: the game's own
// EXCLUSIVE CONTENT page (MarketPage, DLC list; offline it is unreachable behind a network check)
// in recomp mode, with one row per setting ("FRAME RATE: 60 FPS"); A cycles the value.
// fscommand handlers: sub_82E7EB80 (menu pages) falls back to sub_82E91010 (shared: Localize,
// Get/SetGameSettings, ...); r4 = command (ASCII), r5 = arguments (ASCII).
// MarketPage (ActionScript): GetDLCList -> native ids/status bytes (sub_82DDC888) and per id
// Localize(DLCITEM_NAMES / DLCITEM_DESC, "ExclusiveContent<id>", Localization\DLC); A = UnlockDLCItem ITEM=<id>.
extern "C" PPC_FUNC(__imp__sub_82E7EB80);
extern "C" PPC_FUNC(__imp__sub_82E91010);
extern "C" PPC_FUNC(__imp__sub_82E86A50); // 3D menu event handler (this, event id, ...)
extern "C" PPC_FUNC(__imp__sub_82E85358); // 3D menu vtbl+44 (this, command, arguments): TRAVEL_MENU
extern "C" PPC_FUNC(__imp__sub_82A8D3D8); // UE Localize(r3 section, r4 key, r5 file, r6, r7) -> r3 UTF-16BE (own 2 KB buffer)
extern "C" PPC_FUNC(__imp__sub_824B1EE8); // FString(r3 this, r4 ASCII)
extern "C" PPC_FUNC(__imp__sub_82B9FD18); // FString dtor
extern "C" PPC_FUNC(__imp__sub_824B2430); // Invoke result dtor (result + 32)
extern "C" PPC_FUNC(__imp__sub_82E49CB8); // -> menu movie; vtbl+56 = Invoke(result, movie, &path, fmt, ...)
extern "C" PPC_FUNC(__imp__sub_82DDC888); // DLC list (this, TArray<u8>* ids, TArray<u8>* status)
extern "C" PPC_FUNC(__imp__sub_824B1A10); // TArray add (array, count, element size) -> first new index
extern "C" PPC_FUNC(__imp__sub_824B2368); // FString (20 bytes) from UTF-16BE (this, text)
extern "C" PPC_FUNC(__imp__sub_82A8C5B8); // TArray<FString 20 bytes> add copy (array, &string)
extern "C" PPC_FUNC(__imp__sub_82D067E8); // TArray<FString> destroy elements (array, first, count)
extern "C" PPC_FUNC(__imp__sub_824E6180); // TArray free (array)

namespace
{
constexpr uint32_t kAddReturnPage = 0x821DAF38;  // "_root.FlashMenu.frame.addReturnPage"
constexpr uint32_t kDeactivatePage = 0x821DAF00; // "_root.FlashMenu.frame.deactivateActivePage"
constexpr uint32_t kRefreshDlcList = 0x821D9B90; // "_root.FlashMenu.frame.refreshDLCList"
constexpr uint32_t kRefreshDlcItem = 0x821D9A0C; // "_root.FlashMenu.frame.refreshCurrentDLCItem"
constexpr uint32_t kContentNames = 0x821D9A7C;   // "_root.FlashMenu.callbackObject.contentNames"
constexpr uint32_t kFormatWide = 0x821CB520;     // "S"
constexpr uint32_t kFormatInt = 0x8200E620;      // "i"

std::atomic<bool> g_recompPage{false};
std::atomic<uint32_t> g_menu3d{0}; // 3D menu object (vtbl+44 = TRAVEL_MENU)

bool TraceFlash()
{
    static const bool on = getenv("SCDA_TRACE_FLASH") != nullptr;
    return on;
}

// ---- the settings rows ----------------------------------------------------------------------
struct Row
{
    const char* name;
    const char* desc;
    std::vector<const char*> values;
    int (*get)(const DisplaySettings&);
    void (*set)(DisplaySettings&, int);
};

int g_language = -1; // index into kLanguages ([General] Language, read at startup; saved at once)
const char* const kLanguages[] = { "ENGLISH", "FRENCH", "GERMAN", "ITALIAN", "SPANISH" };
const char* const kLanguageIni[] = { "English", "French", "German", "Italian", "Spanish" };
constexpr uint32_t kLanguageIds[] = { 1, 4, 3, 6, 5 };
constexpr uint32_t kFov[] = { 70, 80, 90, 100, 110, 120 };
constexpr uint32_t kAniso[] = { 1, 2, 4, 8, 16 };
constexpr uint32_t kSharpen[] = { 0, 25, 50, 75, 100 };

int FovIndex(uint32_t fov)
{
    int best = 0;
    for (int i = 0; i < 6; i++)
        if (abs(int(kFov[i]) - int(fov)) < abs(int(kFov[best]) - int(fov)))
            best = i;
    return best;
}

const std::vector<Row>& Rows()
{
    static const std::vector<Row> rows = {
        { "WINDOW MODE", "Borderless and Fullscreen both fill the monitor the window is on.",
          { "WINDOWED", "BORDERLESS", "FULLSCREEN" },
          [](const DisplaySettings& s) { return int(s.mode); }, [](DisplaySettings& s, int v) { s.mode = DisplaySettings::Mode(v); } },
        { "FRAME RATE", "60 FPS unlocks the game's 30 fps cap. Game speed is unchanged. Menus may run below 60.",
          { "30 FPS", "60 FPS" },
          [](const DisplaySettings& s) { return int(s.fps60); }, [](DisplaySettings& s, int v) { s.fps60 = v != 0; } },
        { "V-SYNC", "Waits for the monitor refresh: no tearing.", { "OFF", "ON" },
          [](const DisplaySettings& s) { return int(s.vsync); }, [](DisplaySettings& s, int v) { s.vsync = v != 0; } },
        { "FIELD OF VIEW", "Horizontal field of view of the player camera in degrees. 90 is the game's own.",
          { "70", "80", "90 (DEFAULT)", "100", "110", "120" },
          [](const DisplaySettings& s) { return FovIndex(s.fov); }, [](DisplaySettings& s, int v) { s.fov = kFov[v]; } },
        { "DRAW DISTANCE", "Small objects stay visible and models keep their detail further away. Costs GPU time.",
          { "CONSOLE (DEFAULT)", "HIGH (2X)", "ULTRA (4X)", "MAXIMUM" },
          [](const DisplaySettings& s) { return int(s.drawDistance); }, [](DisplaySettings& s, int v) { s.drawDistance = uint32_t(v); } },
        { "ULTRAWIDE", "In a window wider than 16:9, shows more of the scene at the sides instead of black bars.",
          { "OFF", "ON" },
          [](const DisplaySettings& s) { return int(s.ultrawide); }, [](DisplaySettings& s, int v) { s.ultrawide = v != 0; } },
        { "UPSCALING", "Picks the internal resolution from the window size (Native = window resolution). Off uses Internal Resolution.",
          { "OFF", "NATIVE", "QUALITY (1.5X)", "BALANCED (1.7X)", "PERFORMANCE (2X)", "ULTRA PERFORMANCE (3X)" },
          [](const DisplaySettings& s) { return int(s.upscaling); }, [](DisplaySettings& s, int v) { s.upscaling = DisplaySettings::Upscaling(v); } },
        { "INTERNAL RESOLUTION", "Renders at 1280x720 times this and filters down to the window. Used when Upscaling is Off.",
          { "1280X720", "2560X1440", "3840X2160", "5120X2880" },
          [](const DisplaySettings& s) { return int(s.scale) - 1; }, [](DisplaySettings& s, int v) { s.scale = uint32_t(v + 1); } },
        { "ANTI-ALIASING", "FXAA smooths edges after the frame is drawn. The game's own 4x MSAA is always on.",
          { "OFF", "FXAA" },
          [](const DisplaySettings& s) { return int(s.antiAliasing); }, [](DisplaySettings& s, int v) { s.antiAliasing = DisplaySettings::AntiAliasing(v); } },
        { "SHARPENING", "Contrast-adaptive sharpening on the final image.", { "OFF", "25%", "50%", "75%", "100%" },
          [](const DisplaySettings& s) {
              int i = 0;
              while (i < 4 && kSharpen[i] < s.sharpening)
                  i++;
              return i;
          },
          [](DisplaySettings& s, int v) { s.sharpening = kSharpen[v]; } },
        { "ANISOTROPIC FILTERING", "Sharper textures at grazing angles.", { "OFF", "2X", "4X", "8X", "16X" },
          [](const DisplaySettings& s) {
              int i = 0;
              while (i < 4 && kAniso[i] < s.anisotropy)
                  i++;
              return i;
          },
          [](DisplaySettings& s, int v) { s.anisotropy = kAniso[v]; } },
        { "SOLID WALLS FROM INSIDE", "Draws opaque surfaces from both sides. Off = the console's culling.", { "OFF", "ON" },
          [](const DisplaySettings& s) { return int(s.drawBackFaces); }, [](DisplaySettings& s, int v) { s.drawBackFaces = v != 0; } },
        { "LANGUAGE", "Menu, subtitle and voice language. Applies the next time the game starts.",
          { kLanguages[0], kLanguages[1], kLanguages[2], kLanguages[3], kLanguages[4] },
          [](const DisplaySettings&) {
              if (g_language < 0)
              {
                  g_language = 0;
                  for (int i = 0; i < 5; i++)
                      if (kLanguageIds[i] == GameLanguage())
                          g_language = i;
              }
              return g_language;
          },
          [](DisplaySettings&, int v) {
              g_language = v;
              SaveSetting(SettingsPath(), "General", "Language", kLanguageIni[v]);
          } },
    };
    return rows;
}

std::string RowName(size_t i)
{
    const Row& r = Rows()[i];
    const int v = std::clamp(r.get(OverlaySettings()), 0, int(r.values.size()) - 1);
    return std::string(r.name) + ": " + r.values[size_t(v)];
}

// Row ids on the page: row + IdBase. Ids past the disc's DLC list have no icon (0..: weapon pictures).
int IdBase()
{
    static const int b = getenv("SCDA_RECOMP_IDBASE") ? atoi(getenv("SCDA_RECOMP_IDBASE")) : 40;
    return b;
}

// Y on the page: every setting back to its default, except the window mode and the language.
void ResetRows()
{
    const DisplaySettings now = OverlaySettings();
    DisplaySettings s;
    s.mode = now.mode;
    OverlayChangeSettings(s);
    RuntimeLog("[menu] RECOMP SETTINGS: defaults\n");
}

void CycleRow(size_t i)
{
    if (i >= Rows().size())
        return;
    const Row& r = Rows()[i];
    DisplaySettings s = OverlaySettings();
    const int v = (std::clamp(r.get(s), 0, int(r.values.size()) - 1) + 1) % int(r.values.size());
    r.set(s, v);
    OverlayChangeSettings(s);
    RuntimeLog("[menu] RECOMP SETTINGS: %s -> %s\n", r.name, r.values[size_t(v)]);
}

// ---- guest helpers --------------------------------------------------------------------------
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
const char* Ascii(uint32_t addr) { return reinterpret_cast<const char*>(g_base + addr); }

// Into a Localize result (a 2 KB buffer of its own): at most 1000 characters.
void StoreWide(uint32_t dst, const std::string& text)
{
    const size_t n = std::min<size_t>(text.size(), 1000);
    for (size_t i = 0; i < n; i++)
        GuestStore16(dst + 2 * uint32_t(i), uint16_t(uint8_t(text[i])));
    GuestStore16(dst + 2 * uint32_t(n), 0);
}

// Flash Invoke on the menu movie, on the menu's own thread (inside an fscommand): path(arg) with one
// wide-string arg (vtbl+56, format "S"), path(int) (format "i"), or path() (vtbl+52).
uint32_t Invoke(const PPCContext& from, uint8_t* base, uint32_t asciiPath, const char* arg, int intArg = -1)
{
    static const uint32_t scratch = GuestAllocVirtual(0x400, 0x10); // arg text: up to 380 characters
    const uint32_t path = scratch, result = scratch + 0x40, name = scratch + 0x100;
    PPCContext c = from;
    c.r3.u64 = path;
    c.r4.u64 = asciiPath;
    __imp__sub_824B1EE8(c, base);
    c = from;
    __imp__sub_82E49CB8(c, base);
    const uint32_t movie = c.r3.u32;
    const uint32_t vtbl = GuestLoad32(movie);
    c = from;
    c.r3.u64 = result;
    c.r4.u64 = movie;
    c.r5.u64 = path;
    if (arg)
    {
        StoreWide(name, arg);
        c.r6.u64 = kFormatWide;
        c.r7.u64 = name;
    }
    else if (intArg >= 0)
    {
        c.r6.u64 = kFormatInt;
        c.r7.u64 = uint32_t(intArg);
    }
    (PPC_LOOKUP_FUNC(base, GuestLoad32(vtbl + (arg || intArg >= 0 ? 56 : 52))))(c, base);
    const uint32_t ok = c.r3.u32;
    c = from;
    c.r3.u64 = result + 32;
    __imp__sub_824B2430(c, base);
    c = from;
    c.r3.u64 = result + 12;
    __imp__sub_82B9FD18(c, base);
    c = from;
    c.r3.u64 = path;
    __imp__sub_82B9FD18(c, base);
    return ok;
}

// Invoke of a path held here (ASCII), with an int argument.
uint32_t InvokePath(const PPCContext& from, uint8_t* base, const char* path, int intArg)
{
    static const uint32_t text = GuestAllocVirtual(0x100, 0x10);
    strncpy(reinterpret_cast<char*>(base + text), path, 0xFF);
    return Invoke(from, base, text, nullptr, intArg);
}

// callbackObject.contentNames = the rows' current names (as GetDLCList sets it: movie vtbl+140).
void SetRowNames(const PPCContext& from, uint8_t* base)
{
    static const uint32_t scratch = GuestAllocVirtual(0x800, 0x10);
    const uint32_t array = scratch, item = scratch + 0x20, path = scratch + 0x40, text = scratch + 0x80;
    memset(base + array, 0, 0x20);
    PPCContext c = from;
    for (size_t i = 0; i < Rows().size(); i++)
    {
        StoreWide(text, RowName(i));
        c = from;
        c.r3.u64 = item;
        c.r4.u64 = text;
        __imp__sub_824B2368(c, base);
        c = from;
        c.r3.u64 = array;
        c.r4.u64 = item;
        __imp__sub_82A8C5B8(c, base);
        c = from;
        c.r3.u64 = item;
        __imp__sub_824B2430(c, base);
    }
    c = from;
    c.r3.u64 = path;
    c.r4.u64 = kContentNames;
    __imp__sub_824B1EE8(c, base);
    c = from;
    __imp__sub_82E49CB8(c, base);
    const uint32_t movie = c.r3.u32;
    c = from;
    c.r3.u64 = movie;
    c.r4.u64 = path;
    c.r5.u64 = 0;
    c.r6.u64 = array;
    (PPC_LOOKUP_FUNC(base, GuestLoad32(GuestLoad32(movie) + 140)))(c, base);
    c = from;
    c.r3.u64 = path;
    __imp__sub_82B9FD18(c, base);
    c = from;
    c.r3.u64 = array;
    c.r4.u64 = 0;
    c.r5.u64 = GuestLoad32(array + 4);
    __imp__sub_82D067E8(c, base);
    c = from;
    c.r3.u64 = array;
    __imp__sub_824E6180(c, base);
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
    if (WideIs(key, "PlayerStore"))
    {
        if (WideIs(sec, "MAINPAGE"))
            StoreWide(text, "RECOMP SETTINGS");
        else if (WideIs(sec, "MAINPAGE_DESC"))
            StoreWide(text, "Display, frame rate and other settings of the PC version.");
        return;
    }
    // The EXCLUSIVE CONTENT page is the recomp page (offline the game never opens it): its title,
    // the status line of its rows and its Y tip (Redeem Code = reset the settings here).
    if ((WideIs(sec, "PAGETITLES") && WideIs(key, "MarketPage")))
    {
        StoreWide(text, "RECOMP SETTINGS");
        return;
    }
    if (WideIs(sec, "MarketPage") && WideIs(key, "AvailableLocked"))
    {
        StoreWide(text, getenv("SCDA_RECOMP_STATUS_TEXT") ? getenv("SCDA_RECOMP_STATUS_TEXT") : "");
        return;
    }
    if (WideIs(sec, "TIPS") && WideIs(key, "RedeemCode"))
    {
        StoreWide(text, "Reset to Defaults");
        return;
    }
    if (g_recompPage && file && strcmp(Ascii(file), "Localization\\DLC") == 0)
    {
        // Key "ExclusiveContent<id>", id = row.
        const std::string k = GuestText(key);
        const size_t row = size_t(atoi(k.c_str() + std::min<size_t>(k.size(), 16)) - IdBase());
        if (k.rfind("ExclusiveContent", 0) == 0 && row < Rows().size())
            StoreWide(text, WideIs(sec, "DLCITEM_NAMES") ? RowName(row) : std::string(Rows()[row].desc));
    }
}

// DLC list: in recomp mode one id per settings row.
PPC_FUNC(sub_82DDC888)
{
    if (!g_recompPage)
    {
        __imp__sub_82DDC888(ctx, base);
        return;
    }
    const uint32_t ids = ctx.r4.u32, status = ctx.r5.u32;
    for (size_t i = 0; i < Rows().size(); i++)
    {
        for (const uint32_t array : { ids, status })
        {
            PPCContext c = ctx;
            c.r3.u64 = array;
            c.r4.u64 = 1;
            c.r5.u64 = 1;
            __imp__sub_824B1A10(c, base);
            static const int st = getenv("SCDA_RECOMP_STATUS") ? atoi(getenv("SCDA_RECOMP_STATUS")) : 0;
            g_base[GuestLoad32(array) + c.r3.u32] = array == ids ? uint8_t(i + IdBase()) : uint8_t(st);
        }
    }
}

PPC_FUNC(sub_82E7EB80)
{
    if (TraceFlash())
        Log("menu", ctx);
    const uint32_t cmd = ctx.r4.u32;
    if (AsciiIs(cmd, "StartUPlay"))
    {
        RuntimeLog("[menu] RECOMP SETTINGS\n");
        if (const uint32_t menu3d = g_menu3d)
        {
            // As the menu's own page jumps (Lobby): return page, close the main page, travel.
            g_recompPage = true;
            Invoke(ctx, base, kAddReturnPage, "IndexPage");
            Invoke(ctx, base, kDeactivatePage, nullptr);
            Travel(ctx, base, menu3d, "EVENT=TRAVEL_A_C PAGE=MarketPage");
        }
        ctx.r3.u64 = 1;
        return;
    }
    if (g_recompPage)
    {
        if (AsciiIs(cmd, "UnlockDLCItem"))
        {
            const char* item = strstr(Ascii(ctx.r5.u32), "ITEM=");
            const int row = item ? atoi(item + 5) - IdBase() : 0;
            if (item)
                CycleRow(size_t(row));
            // The list is rebuilt (names are set when it is built), then the row is selected again.
            Invoke(ctx, base, kRefreshDlcList, nullptr);
            const uint32_t ok = InvokePath(ctx, base, "_root.FlashMenu.frame.activePage.menu.select", row);
            if (size_t(row) < Rows().size())
            {
                static const uint32_t path = [] {
                    const uint32_t p = GuestAllocVirtual(0x40, 0x10);
                    strcpy(reinterpret_cast<char*>(g_base + p), "_root.FlashMenu.frame.setDescription");
                    return p;
                }();
                Invoke(ctx, base, path, Rows()[size_t(row)].desc);
            }
            if (TraceFlash())
                RuntimeLog("[menu] reselect row %d: %u\n", row, ok);
            ctx.r3.u64 = 1;
            return;
        }
        if (AsciiIs(cmd, "RedeemCode"))
        {
            ResetRows();
            Invoke(ctx, base, kRefreshDlcList, nullptr);
            ctx.r3.u64 = 1;
            return;
        }
        // Marketplace / Live links do not apply to the settings page.
        if (AsciiIs(cmd, "FollowHyperlink") || AsciiIs(cmd, "ShowMarketplace"))
        {
            ctx.r3.u64 = 1;
            return;
        }
    }
    __imp__sub_82E7EB80(ctx, base);
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
    if (g_recompPage && AsciiIs(ctx.r4.u32, "TRAVEL_MENU") && !strstr(Ascii(ctx.r5.u32), "PAGE=MarketPage"))
    {
        // Back from the recomp page: camera from the settings spot to the main menu.
        static const uint32_t back = GuestAllocVirtual(0x40, 0x10);
        strcpy(reinterpret_cast<char*>(base + back), "EVENT=TRAVEL_C_A PAGE=IndexPage");
        ctx.r5.u64 = back;
        g_recompPage = false;
    }
    __imp__sub_82E85358(ctx, base);
}

PPC_FUNC(sub_82E91010)
{
    if (TraceFlash())
        Log("common", ctx);
    __imp__sub_82E91010(ctx, base);
}
