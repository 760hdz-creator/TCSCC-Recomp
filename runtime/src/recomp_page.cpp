#include <recomp_page.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <imgui.h>
#include <keybinds.h>
#include <prompts.h>
#include <runtime.h>
#include <settings.h>
#include "overlay.h"

// Layout: the game's own settings pages at 1056x576 (title top-left, rows under it, the description
// on the right, button tips bottom right), scaled to the 16:9 frame in the window.
namespace
{
struct Row
{
    const char* name;
    const char* desc;
    std::vector<const char*> values;
    int (*get)(const DisplaySettings&);
    void (*set)(DisplaySettings&, int);
    enum Kind { Setting, Key, Submenu } kind = Setting;
    Control key = Control::Count; // Key rows: the pad control the keys press
};

int g_online = -1;   // [Online] Enabled as saved (applies at the next start)
int g_language = -1; // index into the language tables ([General] Language, read at startup; saved at once)
const char* const kLanguageIni[] = { "English", "French", "German", "Italian", "Spanish" };
constexpr uint32_t kLanguageIds[] = { 1, 4, 3, 6, 5 };
constexpr uint32_t kFov[] = { 70, 80, 90, 100, 110, 120 };
constexpr uint32_t kAniso[] = { 1, 2, 4, 8, 16 };
constexpr uint32_t kSharpen[] = { 0, 25, 50, 75, 100 };
constexpr uint32_t kGamma[] = { 70, 80, 90, 100, 110, 120, 130, 140, 150 };
constexpr float kSensitivity[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f };

int StepIndex(const uint32_t* steps, int n, uint32_t v)
{
    int best = 0;
    for (int i = 0; i < n; i++)
        if (std::abs(int(steps[i]) - int(v)) < std::abs(int(steps[best]) - int(v)))
            best = i;
    return best;
}

const std::vector<Row>& Rows()
{
    static const std::vector<Row> rows = {
        { "WINDOW MODE", "Borderless and Fullscreen both fill the monitor the window is on.", { "WINDOWED", "BORDERLESS", "FULLSCREEN" },
          [](const DisplaySettings& s) { return int(s.mode); }, [](DisplaySettings& s, int v) { s.mode = DisplaySettings::Mode(v); } },
        { "FRAME RATE", "Raises the game's 30 fps cap. Game speed is unchanged. Above 60 needs a high-refresh monitor or V-Sync off.",
          { "30 FPS", "60 FPS", "120 FPS", "144 FPS", "165 FPS", "240 FPS" },
          [](const DisplaySettings& s) { return StepIndex(kFrameRates, 6, s.frameRate); }, [](DisplaySettings& s, int v) { s.frameRate = kFrameRates[v]; } },
        { "V-SYNC", "Waits for the monitor refresh: no tearing.", { "OFF", "ON" },
          [](const DisplaySettings& s) { return int(s.vsync); }, [](DisplaySettings& s, int v) { s.vsync = v != 0; } },
        { "FIELD OF VIEW", "Horizontal field of view of the player camera in degrees. 90 is the game's own. Zoomed views keep theirs.",
          { "70", "80", "90", "100", "110", "120" },
          [](const DisplaySettings& s) { return StepIndex(kFov, 6, s.fov); }, [](DisplaySettings& s, int v) { s.fov = kFov[v]; } },
        { "DRAW DISTANCE", "Small objects stay visible and models keep their detail further away (2x / 4x the console distance). Costs GPU time.",
          { "CONSOLE", "HIGH", "ULTRA", "MAXIMUM" },
          [](const DisplaySettings& s) { return int(s.drawDistance); }, [](DisplaySettings& s, int v) { s.drawDistance = uint32_t(v); } },
        { "ULTRAWIDE", "In a window wider than 16:9, shows more of the scene at the sides instead of black bars. The HUD is stretched.",
          { "OFF", "ON" },
          [](const DisplaySettings& s) { return int(s.ultrawide); }, [](DisplaySettings& s, int v) { s.ultrawide = v != 0; } },
        { "UPSCALING", "Picks the internal resolution from the window size (Native = the window's). Off uses Internal Resolution.",
          { "OFF", "NATIVE", "QUALITY", "BALANCED", "PERFORMANCE", "ULTRA PERFORMANCE" },
          [](const DisplaySettings& s) { return int(s.upscaling); }, [](DisplaySettings& s, int v) { s.upscaling = DisplaySettings::Upscaling(v); } },
        { "UPSCALER", "The filter that scales the picture up to the window when Upscaling is on. NIS (NVIDIA Image Scaling, any GPU) sharpens while scaling; up to 2x, larger steps use Lanczos.",
          { "LANCZOS", "NIS" },
          [](const DisplaySettings& s) { return int(s.upscaler); }, [](DisplaySettings& s, int v) { s.upscaler = DisplaySettings::Upscaler(v); } },
        { "INTERNAL RESOLUTION", "Renders at 1280x720 times this and filters down to the window. Used when Upscaling is Off.",
          { "1280X720", "1920X1080", "2560X1440", "3840X2160", "5120X2880" },
          [](const DisplaySettings& s) { return int(std::find(std::begin(kScales), std::end(kScales), NearestScale(s.scale)) - std::begin(kScales)); },
          [](DisplaySettings& s, int v) { s.scale = kScales[v]; } },
        { "ANTI-ALIASING", "FXAA and SMAA smooth edges after the frame is drawn (SMAA keeps fine detail sharper). The game's own 4x MSAA is always on.",
          { "OFF", "FXAA", "SMAA" },
          [](const DisplaySettings& s) { return int(s.antiAliasing); }, [](DisplaySettings& s, int v) { s.antiAliasing = DisplaySettings::AntiAliasing(v); } },
        { "SHARPENING", "Contrast-adaptive sharpening of the final image.", { "OFF", "25%", "50%", "75%", "100%" },
          [](const DisplaySettings& s) { return StepIndex(kSharpen, 5, s.sharpening); }, [](DisplaySettings& s, int v) { s.sharpening = kSharpen[v]; } },
        { "BRIGHTNESS", "Display gamma on top of the game's own brightness. 100% = unchanged; higher lifts dark areas, black stays black.",
          { "70%", "80%", "90%", "100%", "110%", "120%", "130%", "140%", "150%" },
          [](const DisplaySettings& s) { return StepIndex(kGamma, 9, s.gamma); }, [](DisplaySettings& s, int v) { s.gamma = kGamma[v]; } },
        { "TEXTURE SHARPNESS", "More detailed texture levels at a distance. Sharper can shimmer on fine patterns.",
          { "DEFAULT", "SHARP", "SHARPER" },
          [](const DisplaySettings& s) { return int(s.textureSharpness); }, [](DisplaySettings& s, int v) { s.textureSharpness = uint32_t(v); } },
        { "ANISOTROPIC FILTERING", "Sharper textures on surfaces seen at a grazing angle.", { "OFF", "2X", "4X", "8X", "16X" },
          [](const DisplaySettings& s) { return StepIndex(kAniso, 5, s.anisotropy); }, [](DisplaySettings& s, int v) { s.anisotropy = kAniso[v]; } },
        { "ONLINE", "Play online over Steam: SYSTEM LINK and XBOX LIVE PLAYER MATCH (COOP & MULTIPLAYER) find other Steam players' matches. Needs Steam running. Applies the next time the game starts.",
          { "OFF", "ON" },
          [](const DisplaySettings&) {
              if (g_online < 0)
                  g_online = OnlineEnabledSetting() ? 1 : 0;
              return g_online;
          },
          [](DisplaySettings&, int v) {
              g_online = v;
              SaveSetting(SettingsPath(), "Online", "Enabled", v ? "1" : "0");
          } },
        { "LANGUAGE", "Menu, subtitle and voice language. Applies the next time the game starts.",
          { "ENGLISH", "FRENCH", "GERMAN", "ITALIAN", "SPANISH" },
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
        { "CONTROLS", "Button icons, mouse and keyboard keys.", {}, nullptr, nullptr, Row::Submenu },
        { "BUTTON PROMPTS", "The button icons in menus and the HUD. Auto follows the device used last: a PlayStation pad shows its symbols, the keyboard the keys bound below.",
          { "AUTO", "XBOX", "PLAYSTATION", "KEYBOARD" },
          [](const DisplaySettings&) { return int(ParsePromptSetting(CurrentInputSettings().buttonPrompts.c_str())); },
          [](DisplaySettings&, int v) {
              static const char* const ini[] = { "Auto", "Xbox", "PlayStation", "Keyboard" };
              SaveSetting(SettingsPath(), "Input", "ButtonPrompts", ini[v]);
              InputSettingsChanged();
          } },
        { "MOUSE SENSITIVITY", "How fast the mouse turns the camera.",
          { "0.25", "0.5", "0.75", "1.0", "1.25", "1.5", "2.0", "2.5", "3.0", "4.0", "5.0" },
          [](const DisplaySettings&) {
              const float v = CurrentInputSettings().mouseSensitivity;
              int best = 0;
              for (int i = 0; i < 11; i++)
                  if (std::abs(kSensitivity[i] - v) < std::abs(kSensitivity[best] - v))
                      best = i;
              return best;
          },
          [](DisplaySettings&, int v) {
              char text[16];
              snprintf(text, sizeof(text), "%.2f", kSensitivity[v]);
              SaveSetting(SettingsPath(), "Input", "MouseSensitivity", text);
              InputSettingsChanged();
          } },
        { "INVERT MOUSE Y", "Mouse up looks down.", { "OFF", "ON" },
          [](const DisplaySettings&) { return CurrentInputSettings().mouseInvertY ? 1 : 0; },
          [](DisplaySettings&, int v) {
              SaveSetting(SettingsPath(), "Input", "MouseInvertY", v ? "1" : "0");
              InputSettingsChanged();
          } },
    };
    // Then one row per keyboard/mouse binding (keybinds.h) and a reset.
    static const std::vector<Row> all = [] {
        std::vector<Row> v = rows;
        static const struct { Control c; const char* name; } kKeys[] = {
            { Control::MoveUp, "MOVE FORWARD" }, { Control::MoveDown, "MOVE BACK" }, { Control::MoveLeft, "MOVE LEFT" },
            { Control::MoveRight, "MOVE RIGHT" }, { Control::Walk, "WALK (HOLD)" }, { Control::A, "ACTION / OK" },
            { Control::B, "ATTACK / BACK" }, { Control::X, "GADGET" }, { Control::Y, "EXECUTE" },
            { Control::LT, "COVER" }, { Control::RT, "SHOOT" }, { Control::RS, "AIM" }, { Control::LS, "RELOAD" },
            { Control::LB, "CROUCH" }, { Control::RB, "MARK" }, { Control::DpadUp, "SONAR GOGGLES" },
            { Control::DpadDown, "SWITCH WEAPON" }, { Control::DpadLeft, "PREVIOUS GADGET" },
            { Control::DpadRight, "NEXT GADGET" }, { Control::Back, "HELP" }, { Control::Start, "PAUSE" },
            { Control::LookUp, "LOOK UP" }, { Control::LookDown, "LOOK DOWN" }, { Control::LookLeft, "LOOK LEFT" },
            { Control::LookRight, "LOOK RIGHT" },
        };
        for (const auto& k : kKeys)
        {
            Row r{ k.name, "Change, then press the new key or mouse button within 6 seconds (Esc too). X clears the key. "
                           "The mouse wheel is set in settings.ini, [Keyboard].", {}, nullptr, nullptr };
            r.kind = Row::Key;
            r.key = k.c;
            v.push_back(r);
        }
        return v;
    }();
    return all;
}

using Clock = std::chrono::steady_clock;
std::atomic<bool> g_open{false}, g_back{false};
std::atomic<int> g_selected{0};
std::atomic<int> g_tab{0}; // 0 the main list, 1 the CONTROLS sub-page

// The CONTROLS row (end of the main list); the sub-page is every row after it.
int SubmenuRow()
{
    static const int row = [] {
        for (size_t i = 0; i < Rows().size(); i++)
            if (Rows()[i].kind == Row::Submenu)
                return int(i);
        return int(Rows().size()) - 1;
    }();
    return row;
}
int TabBegin(int tab) { return tab == 0 ? 0 : SubmenuRow() + 1; }
int TabEnd(int tab) { return tab == 0 ? SubmenuRow() + 1 : int(Rows().size()); }
std::atomic<int64_t> g_openedAt{0}; // Clock ticks
std::atomic<int64_t> g_backAt{0};   // Clock ticks of the B that closed the page

int Value(const Row& r, const DisplaySettings& s) { return r.get ? std::clamp(r.get(s), 0, int(r.values.size()) - 1) : 0; }

// Key rows: Change starts a capture; the next key or mouse button pressed (after every key is up) is bound.
std::atomic<int> g_capture{ -1 }; // row waiting for a key
std::chrono::steady_clock::time_point g_captureAt;
bool g_captureArmed = false;

std::string KeysText(Control c)
{
    std::string t;
    for (const int k : CurrentKeyBindings()[c])
        t += (t.empty() ? "" : ", ") + KeyName(k);
    return t.empty() ? "NONE" : t;
}

// Game thread, every poll. True while a capture goes on (the page ignores the pad meanwhile).
bool CaptureTick()
{
    const int row = g_capture;
    if (row < 0)
        return false;
    bool anyDown = false;
    for (int vk = 1; vk < 0xFF; vk++)
        anyDown |= (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (!g_captureArmed)
        g_captureArmed = !anyDown;
    else
        for (int vk = 1; vk < 0xFF; vk++)
        {
            // The generic Shift/Ctrl/Alt codes also report the left/right ones: bind the specific key.
            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_F1 || !(GetAsyncKeyState(vk) & 0x8000))
                continue;
            const Row& r = Rows()[size_t(row)];
            SaveKeyBinding(r.key, { vk });
            RuntimeLog("[menu] RECOMP SETTINGS: %s -> %s\n", r.name, KeyName(vk).c_str());
            g_capture = -1;
            return true;
        }
    if (std::chrono::steady_clock::now() - g_captureAt > std::chrono::seconds(6))
        g_capture = -1; // nothing pressed: unchanged
    return true;
}

void Step(int row, int dir)
{
    const Row& r = Rows()[size_t(row)];
    if (r.kind == Row::Submenu)
    {
        if (dir > 0)
        {
            g_tab = 1;
            g_selected = TabBegin(1);
            RuntimeLog("[menu] RECOMP SETTINGS: CONTROLS\n");
        }
        return;
    }
    if (r.kind == Row::Key)
    {
        if (dir > 0) // Change (A) / right: capture; left does nothing on a key
        {
            g_capture = row, g_captureAt = std::chrono::steady_clock::now(), g_captureArmed = false;
            RuntimeLog("[menu] RECOMP SETTINGS: %s waits for a key\n", r.name);
        }
        return;
    }

    DisplaySettings s = OverlaySettings();
    const int n = int(r.values.size());
    const int v = (Value(r, s) + dir + n) % n;
    r.set(s, v);
    OverlayChangeSettings(s);
    RuntimeLog("[menu] RECOMP SETTINGS: %s -> %s\n", r.name, r.values[size_t(v)]);
}

// Y: SETTINGS tab: every setting back to its default, except the window mode and the language;
// CONTROLS tab: every key.
void Reset()
{
    if (g_tab == 1)
    {
        const KeyBindings d = DefaultKeyBindings();
        for (size_t c = 0; c < size_t(Control::Count); c++)
            SaveKeyBinding(Control(c), d.keys[c]);
        RuntimeLog("[menu] RECOMP SETTINGS: keys reset\n");
        return;
    }
    DisplaySettings s;
    s.mode = OverlaySettings().mode;
    OverlayChangeSettings(s);
    RuntimeLog("[menu] RECOMP SETTINGS: defaults\n");
}

// The pad as d-pad + face buttons: the left stick counts as the d-pad.
WORD Buttons(const XINPUT_GAMEPAD& g)
{
    WORD b = g.wButtons;
    constexpr int kStick = 16000;
    if (g.sThumbLY > kStick) b |= XINPUT_GAMEPAD_DPAD_UP;
    if (g.sThumbLY < -kStick) b |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (g.sThumbLX > kStick) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (g.sThumbLX < -kStick) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    return b;
}

// Seconds since the page opened; the page shows once the camera travel has (about) ended.
constexpr float kTravelSeconds = 0.7f, kFadeSeconds = 0.25f;
float SinceOpen() { return std::chrono::duration<float>(Clock::now() - Clock::time_point(Clock::duration(g_openedAt.load()))).count(); }
} // namespace

void RecompPageOpen()
{
    g_selected = 0;
    g_tab = 0;
    g_back = false;
    g_openedAt = Clock::now().time_since_epoch().count();
    g_open = true;
    RuntimeLog("[menu] RECOMP SETTINGS open\n");
}

void RecompPageClose() { g_open = false, g_capture = -1; }
bool RecompPageIsOpen() { return g_open; }
bool RecompPageTakeBack()
{
    // The game's page turns the B into its travel within a few frames; later travels are its own.
    const auto at = Clock::time_point(Clock::duration(g_backAt.load()));
    return g_back.exchange(false) && Clock::now() - at < std::chrono::seconds(2);
}

void RecompPageInput(const XINPUT_GAMEPAD& g)
{
    static WORD last = 0xFFFF; // the A that opened the page must be released first
    static Clock::time_point repeatAt;
    const WORD now = Buttons(g);
    if (last == 0xFFFF)
    {
        last = now;
        return;
    }
    const WORD pressed = now & ~last;
    last = now;
    if (!g_open || SinceOpen() < kTravelSeconds)
        return;
    if (CaptureTick())
    {
        last = 0xFFFF; // after the capture: the bound key must be released before it acts on the page
        return;
    }
    if (g_tab == 1 && (pressed & XINPUT_GAMEPAD_B))
    {
        g_tab = 0;
        g_selected = SubmenuRow();
        RuntimeLog("[menu] RECOMP SETTINGS: CONTROLS back\n");
        return; // the page stays open: the game's page sees an idle pad
    }
    const int begin = TabBegin(g_tab), count = TabEnd(g_tab) - begin;
    // Up/down repeat while held (0.4 s, then every 0.12 s).
    constexpr WORD kVertical = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN;
    int move = 0;
    if (pressed & kVertical)
    {
        move = (pressed & XINPUT_GAMEPAD_DPAD_UP) ? -1 : 1;
        repeatAt = Clock::now() + std::chrono::milliseconds(400);
    }
    else if ((now & kVertical) && Clock::now() >= repeatAt)
    {
        move = (now & XINPUT_GAMEPAD_DPAD_UP) ? -1 : 1;
        repeatAt = Clock::now() + std::chrono::milliseconds(120);
    }
    if (move)
        g_selected = begin + (g_selected - begin + move + count) % count;
    const int row = g_selected;
    if (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_RIGHT))
        Step(row, 1);
    else if (pressed & XINPUT_GAMEPAD_DPAD_LEFT)
        Step(row, -1);
    else if ((pressed & XINPUT_GAMEPAD_X) && Rows()[size_t(row)].kind == Row::Key)
    {
        SaveKeyBinding(Rows()[size_t(row)].key, {});
        RuntimeLog("[menu] RECOMP SETTINGS: %s cleared\n", Rows()[size_t(row)].name);
    }
    else if (pressed & XINPUT_GAMEPAD_Y)
        Reset();
    else if (pressed & XINPUT_GAMEPAD_B) // only B: the game's page turns it into its Back
    {
        g_backAt = Clock::now().time_since_epoch().count();
        g_back = true;
        g_open = false; // this poll's B goes on to the game's page (its own Back)
        g_capture = -1;
        last = 0xFFFF; // next open: wait for a release again
        RuntimeLog("[menu] RECOMP SETTINGS back\n");
    }
}

static std::atomic<bool> g_indexPage{false};
static std::atomic<int64_t> g_indexSince{0}; // Clock ticks: the main page's travel started
void MainMenuSetIndexPage(bool on)
{
    if (on && !g_indexPage)
        g_indexSince = Clock::now().time_since_epoch().count();
    g_indexPage = on;
}
bool MainMenuOnIndexPage() { return g_indexPage.load(); }

// A (cross) or B (circle) button icon, drawn like the game's: pad letter, PlayStation symbol, or the
// first key bound to it ([Keyboard], keybinds.h).
static void PadIcon(ImDrawList* dl, ImFont* body, ImVec2 c, float k, float alpha, bool b)
{
    auto col = [&](int r, int g, int bl) { return IM_COL32(r, g, bl, int(255 * alpha)); };
    const PromptStyle style = CurrentPromptStyle();
    const bool ps = style == PromptStyle::PS4 || style == PromptStyle::PS5;
    dl->AddCircleFilled(c, 8.5f * k, col(70, 70, 70));
    dl->AddCircle(c, 8.5f * k, col(210, 210, 210), 0, 1.2f * k);
    if (ps && b) // circle
        dl->AddCircle(c, 4.2f * k, col(255, 255, 255), 0, 1.6f * k);
    else if (ps) // cross
    {
        dl->AddLine(ImVec2(c.x - 4 * k, c.y - 4 * k), ImVec2(c.x + 4 * k, c.y + 4 * k), col(255, 255, 255), 1.6f * k);
        dl->AddLine(ImVec2(c.x - 4 * k, c.y + 4 * k), ImVec2(c.x + 4 * k, c.y - 4 * k), col(255, 255, 255), 1.6f * k);
    }
    else
    {
        const std::vector<int> keys = CurrentKeyBindings()[b ? Control::B : Control::A];
        const std::string label = style == PromptStyle::Keyboard && !keys.empty() ? KeyLabel(keys[0]) : (b ? "B" : "A");
        const float size = label.size() > 1 ? 9.0f : 13.0f;
        const ImVec2 sz = body->CalcTextSizeA(size * k, FLT_MAX, 0.0f, label.c_str());
        dl->AddText(body, size * k, ImVec2(c.x - sz.x * 0.5f, c.y - sz.y * 0.5f), col(255, 255, 255), label.c_str());
    }
}

void RecompPageDraw(ImFont* title, ImFont* body)
{
    if (!g_open)
        return;
    const float t = SinceOpen() - kTravelSeconds;
    if (t <= 0.0f)
        return;
    const float alpha = std::min(t / kFadeSeconds, 1.0f);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // The 16:9 frame inside the window (pillar/letterbox as the game image).
    float w = vp->Size.x, h = vp->Size.y;
    if (w > h * 16.0f / 9.0f)
        w = h * 16.0f / 9.0f;
    else
        h = w * 9.0f / 16.0f;
    const float x0 = vp->Pos.x + (vp->Size.x - w) * 0.5f, y0 = vp->Pos.y + (vp->Size.y - h) * 0.5f;
    const float k = h / 576.0f;
    auto P = [&](float x, float y) { return ImVec2(x0 + x * k, y0 + y * k); };
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    auto col = [&](int r, int g, int b, float a = 1.0f) { return IM_COL32(r, g, b, int(255 * a * alpha)); };
    auto text = [&](ImFont* f, float size, ImVec2 at, ImU32 c, const char* s, float wrap = 0.0f) {
        dl->AddText(f, size * k, ImVec2(at.x + 1.5f * k, at.y + 1.5f * k), col(0, 0, 0, 0.7f), s, nullptr, wrap);
        dl->AddText(f, size * k, at, c, s, nullptr, wrap);
    };

    (void)title; // the title is the menu's own 3D page title (main_menu.cpp SetTitle)

    const DisplaySettings s = OverlaySettings();
    const int selected = g_selected;
    const int tab = g_tab, begin = TabBegin(tab), end = TabEnd(tab), count = end - begin;
    constexpr float kTabTop = 122.0f, kTabH = 24.0f, kRowH = 25.0f, kLeft = 120.0f, kRight = 540.0f, kName = 134.0f, kValue = 360.0f;
    const float kTop = tab == 1 ? kTabTop + kTabH + 6.0f : 128.0f;
    // 14 rows (13 under the sub-page header) show at once (the tips are at y ~505); the list scrolls.
    const int kVisible = tab == 1 ? 13 : 14;
    static int first = 0;
    first = std::clamp(first, std::max(begin, selected - kVisible + 1), std::min(selected, std::max(begin, end - kVisible)));
    const int shown = std::min(kVisible, count);
    // Our own panel behind the tabs and rows (the game's page panel and its scroll arrows, x ~573, are covered).
    const float panelBottom = kTop + kRowH * float(shown) + 3.0f;
    dl->AddRectFilled(P(kLeft - 4, kTop - 6 - (tab == 1 ? kTabH + 6.0f : 0.0f)), P(585, panelBottom), col(8, 8, 8, 0.96f));
    if (tab == 1)
    {
        // Sub-page header: where you are, and how to get back.
        text(body, 17.0f, P(kName, kTabTop + 2), col(255, 255, 255), "RECOMP SETTINGS  >  CONTROLS");
        const PromptStyle st = CurrentPromptStyle();
        std::string back = st == PromptStyle::PS4 || st == PromptStyle::PS5 ? "CIRCLE" : "B";
        if (st == PromptStyle::Keyboard)
        {
            const std::vector<int> keys = CurrentKeyBindings()[Control::B];
            back = keys.empty() ? "B" : KeyLabel(keys[0]);
        }
        back = "BACK: " + back;
        const float hw = body->CalcTextSizeA(14.0f * k, FLT_MAX, 0.0f, back.c_str()).x / k;
        text(body, 14.0f, P(kRight - hw, kTabTop + 4), col(170, 170, 170), back.c_str());
        dl->AddLine(P(kLeft, kTabTop + kTabH + 2), P(kRight, kTabTop + kTabH + 2), col(176, 8, 8), 1.5f * k);
    }
    if (first > begin) // more above / below
        dl->AddTriangleFilled(P(562, kTop + 2), P(556, kTop + 12), P(568, kTop + 12), col(220, 220, 220));
    if (first + shown < end)
        dl->AddTriangleFilled(P(562, panelBottom - 6), P(556, panelBottom - 16), P(568, panelBottom - 16), col(220, 220, 220));
    for (int i = first; i < first + shown; i++)
    {
        const Row& r = Rows()[size_t(i)];
        const float y = kTop + kRowH * float(i - first);
        const bool sel = i == selected;
        if (sel)
            dl->AddRectFilledMultiColor(P(kLeft, y), P(kRight, y + kRowH - 2), col(176, 8, 8, 0.95f), col(120, 0, 0, 0.0f),
                                        col(120, 0, 0, 0.0f), col(176, 8, 8, 0.95f));
        else
            dl->AddRectFilledMultiColor(P(kLeft, y), P(kRight, y + kRowH - 2), col(30, 30, 30, 0.55f), col(30, 30, 30, 0.0f),
                                        col(30, 30, 30, 0.0f), col(30, 30, 30, 0.55f));
        const ImU32 c = sel ? col(255, 255, 255) : col(205, 205, 205);
        text(body, 17.0f, P(kName, y + 3), c, r.name);
        std::string v;
        if (r.kind == Row::Key)
            v = g_capture.load() == i ? "PRESS A KEY..." : KeysText(r.key);
        else if (r.kind == Row::Setting)
            v = sel ? std::string("< ") + r.values[size_t(Value(r, s))] + " >" : r.values[size_t(Value(r, s))];
        else if (r.kind == Row::Submenu)
            v = ">";
        text(body, 17.0f, P(kValue, y + 3), c, v.c_str());
    }
    // Description of the selected row, right column.
    text(body, 15.0f, P(590, kTop + 2), col(235, 235, 235), Rows()[size_t(selected)].desc, 330.0f * k);

    // The page's own tips (Reset to Defaults Y, Back B) are the game's; A (Change) goes left of them,
    // drawn like the game's button icons.
    PadIcon(dl, body, P(548, 516), k, alpha, false);
    const float labelW = body->CalcTextSizeA(16.0f * k, FLT_MAX, 0.0f, "Change").x / k;
    text(body, 16.0f, P(548 - 12 - labelW, 507), col(255, 255, 255), "Change");
}

// EXIT GAME (recomp_page.h). State: 0 off, 1 confirm box open, 2 cancelled (pad idle to the game until
// every button is released, so the B does not also go back to PRESS START).
static std::atomic<bool> g_hoverSettings{false};
static std::atomic<int> g_exitState{0};
void MainMenuHoverSettings(bool on) { g_hoverSettings = on; }

static float IndexSeconds()
{
    const auto since = Clock::time_point(Clock::duration(g_indexSince.load()));
    return std::chrono::duration<float>(Clock::now() - since).count();
}

static bool ExitRowShown() { return MainMenuOnIndexPage() && GuestInFrontEnd() && !RecompPageIsOpen() && IndexSeconds() > kTravelSeconds; }

void MainMenuExitInput(XINPUT_GAMEPAD& g)
{
    static WORD last = 0;
    const WORD now = Buttons(g), pressed = now & ~last;
    last = now;
    if (!ExitRowShown())
    {
        g_exitState = 0;
        return;
    }
    switch (g_exitState.load())
    {
    case 0:
        if (g_hoverSettings && (pressed & XINPUT_GAMEPAD_DPAD_DOWN))
        {
            g_exitState = 1;
            g = XINPUT_GAMEPAD{};
            RuntimeLog("[menu] EXIT GAME: confirm\n");
        }
        break;
    case 1:
        if (pressed & XINPUT_GAMEPAD_A)
        {
            RuntimeLog("[menu] EXIT GAME: exit\n");
            ExitProcess(0); // as closing the window (host_window.cpp WM_CLOSE): guest threads have no shutdown path
        }
        if (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_DPAD_UP))
        {
            g_exitState = 2;
            RuntimeLog("[menu] EXIT GAME: cancelled\n");
        }
        g = XINPUT_GAMEPAD{};
        break;
    default:
        if (!now)
            g_exitState = 0;
        g = XINPUT_GAMEPAD{};
        break;
    }
}

void MainMenuExitDraw(ImFont* body)
{
    if (!ExitRowShown())
        return;
    const float alpha = std::min((IndexSeconds() - kTravelSeconds) / kFadeSeconds, 1.0f);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float w = vp->Size.x, h = vp->Size.y;
    if (w > h * 16.0f / 9.0f)
        w = h * 16.0f / 9.0f;
    else
        h = w * 9.0f / 16.0f;
    const float x0 = vp->Pos.x + (vp->Size.x - w) * 0.5f, y0 = vp->Pos.y + (vp->Size.y - h) * 0.5f;
    const float k = h / 576.0f, kx = w / 1040.0f; // the game image: 1040x576 guest pixels
    auto P = [&](float x, float y) { return ImVec2(x0 + x * kx, y0 + y * k); };
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    auto col = [&](int r, int g, int b, float a = 1.0f) { return IM_COL32(r, g, b, int(255 * a * alpha)); };
    const bool confirm = g_exitState == 1;
    // The row: one item step under SETTINGS (main page items ~22.5 guest px apart).
    constexpr float kRowX = 187.0f, kRowY = 307.0f, kRowH = 21.0f, kSize = 21.0f;
    if (confirm)
        dl->AddRectFilledMultiColor(P(kRowX - 4, kRowY), P(kRowX + 250, kRowY + kRowH), col(150, 10, 10), col(150, 10, 10, 0.0f),
            col(150, 10, 10, 0.0f), col(150, 10, 10));
    const ImVec2 at = P(kRowX, kRowY);
    dl->AddText(body, kSize * k, ImVec2(at.x + 1.5f * k, at.y + 1.5f * k), col(0, 0, 0, 0.6f), "EXIT GAME");
    dl->AddText(body, kSize * k, at, col(255, 255, 255), "EXIT GAME");
    if (!confirm)
        return;
    // Confirm box: the menu dimmed (the game's own SETTINGS highlight stays under it), a dark panel.
    dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), col(0, 0, 0, 0.55f));
    const ImVec2 a = P(430, 220), b = P(810, 356);
    dl->AddRectFilled(a, b, col(10, 10, 10, 0.97f));
    dl->AddLine(ImVec2(a.x, a.y + 34 * k), ImVec2(b.x, a.y + 34 * k), col(176, 8, 8), 1.5f * k);
    dl->AddText(body, 20.0f * k, P(446, 226), col(255, 255, 255), "EXIT GAME");
    dl->AddText(body, 16.0f * k, P(446, 268), col(235, 235, 235), "Quit to the desktop?");
    const float exitW = body->CalcTextSizeA(16.0f * k, FLT_MAX, 0.0f, "Exit").x / kx;
    const float cancelW = body->CalcTextSizeA(16.0f * k, FLT_MAX, 0.0f, "Cancel").x / kx;
    const float cx = 794 - cancelW, ex = cx - 40 - exitW;
    PadIcon(dl, body, P(cx - 14, 334), k, alpha, true);
    dl->AddText(body, 16.0f * k, P(cx, 325), col(255, 255, 255), "Cancel");
    PadIcon(dl, body, P(ex - 14, 334), k, alpha, false);
    dl->AddText(body, 16.0f * k, P(ex, 325), col(255, 255, 255), "Exit");
}
