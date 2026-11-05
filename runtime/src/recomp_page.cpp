#include <recomp_page.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>
#include <imgui.h>
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
};

int g_online = -1;   // [Online] Enabled as saved (applies at the next start)
int g_language = -1; // index into the language tables ([General] Language, read at startup; saved at once)
const char* const kLanguageIni[] = { "English", "French", "German", "Italian", "Spanish" };
constexpr uint32_t kLanguageIds[] = { 1, 4, 3, 6, 5 };
constexpr uint32_t kFov[] = { 70, 80, 90, 100, 110, 120 };
constexpr uint32_t kAniso[] = { 1, 2, 4, 8, 16 };
constexpr uint32_t kSharpen[] = { 0, 25, 50, 75, 100 };
constexpr uint32_t kGamma[] = { 70, 80, 90, 100, 110, 120, 130, 140, 150 };

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
        { "INTERNAL RESOLUTION", "Renders at 1280x720 times this and filters down to the window. Used when Upscaling is Off.",
          { "1280X720", "2560X1440", "3840X2160", "5120X2880" },
          [](const DisplaySettings& s) { return int(s.scale) - 1; }, [](DisplaySettings& s, int v) { s.scale = uint32_t(v + 1); } },
        { "ANTI-ALIASING", "FXAA smooths edges after the frame is drawn. The game's own 4x MSAA is always on.", { "OFF", "FXAA" },
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
    };
    return rows;
}

using Clock = std::chrono::steady_clock;
std::atomic<bool> g_open{false}, g_back{false};
std::atomic<int> g_selected{0};
std::atomic<int64_t> g_openedAt{0}; // Clock ticks
std::atomic<int64_t> g_backAt{0};   // Clock ticks of the B that closed the page

int Value(const Row& r, const DisplaySettings& s) { return std::clamp(r.get(s), 0, int(r.values.size()) - 1); }

void Step(int row, int dir)
{
    const Row& r = Rows()[size_t(row)];
    DisplaySettings s = OverlaySettings();
    const int n = int(r.values.size());
    const int v = (Value(r, s) + dir + n) % n;
    r.set(s, v);
    OverlayChangeSettings(s);
    RuntimeLog("[menu] RECOMP SETTINGS: %s -> %s\n", r.name, r.values[size_t(v)]);
}

// Y: every setting back to its default, except the window mode and the language.
void Reset()
{
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
    g_back = false;
    g_openedAt = Clock::now().time_since_epoch().count();
    g_open = true;
    RuntimeLog("[menu] RECOMP SETTINGS open\n");
}

void RecompPageClose() { g_open = false; }
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
    const int count = int(Rows().size());
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
        g_selected = (g_selected + move + count) % count;
    const int row = g_selected;
    if (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_RIGHT))
        Step(row, 1);
    else if (pressed & XINPUT_GAMEPAD_DPAD_LEFT)
        Step(row, -1);
    else if (pressed & XINPUT_GAMEPAD_Y)
        Reset();
    else if (pressed & XINPUT_GAMEPAD_B) // only B: the game's page turns it into its Back
    {
        g_backAt = Clock::now().time_since_epoch().count();
        g_back = true;
        g_open = false; // this poll's B goes on to the game's page (its own Back)
        last = 0xFFFF; // next open: wait for a release again
        RuntimeLog("[menu] RECOMP SETTINGS back\n");
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
    constexpr float kTop = 128.0f, kRowH = 25.0f, kLeft = 120.0f, kRight = 540.0f, kName = 134.0f, kValue = 360.0f;
    for (size_t i = 0; i < Rows().size(); i++)
    {
        const Row& r = Rows()[i];
        const float y = kTop + kRowH * float(i);
        const bool sel = int(i) == selected;
        if (sel)
            dl->AddRectFilledMultiColor(P(kLeft, y), P(kRight, y + kRowH - 2), col(176, 8, 8, 0.95f), col(120, 0, 0, 0.0f),
                                        col(120, 0, 0, 0.0f), col(176, 8, 8, 0.95f));
        else
            dl->AddRectFilledMultiColor(P(kLeft, y), P(kRight, y + kRowH - 2), col(30, 30, 30, 0.55f), col(30, 30, 30, 0.0f),
                                        col(30, 30, 30, 0.0f), col(30, 30, 30, 0.55f));
        const ImU32 c = sel ? col(255, 255, 255) : col(205, 205, 205);
        text(body, 17.0f, P(kName, y + 3), c, r.name);
        const std::string v = sel ? std::string("< ") + r.values[size_t(Value(r, s))] + " >" : r.values[size_t(Value(r, s))];
        text(body, 17.0f, P(kValue, y + 3), c, v.c_str());
    }
    // Description of the selected row, right column.
    text(body, 15.0f, P(590, kTop + 2), col(235, 235, 235), Rows()[size_t(selected)].desc, 330.0f * k);

    // The page's own tips (Reset to Defaults Y, Back B) are the game's; A (Change) goes left of them,
    // drawn like the game's button icons.
    const PromptStyle style = CurrentPromptStyle();
    const bool ps = style == PromptStyle::PS4 || style == PromptStyle::PS5;
    const ImVec2 c = P(548, 516);
    dl->AddCircleFilled(c, 8.5f * k, col(70, 70, 70));
    dl->AddCircle(c, 8.5f * k, col(210, 210, 210), 0, 1.2f * k);
    if (ps) // cross
    {
        dl->AddLine(ImVec2(c.x - 4 * k, c.y - 4 * k), ImVec2(c.x + 4 * k, c.y + 4 * k), col(255, 255, 255), 1.6f * k);
        dl->AddLine(ImVec2(c.x - 4 * k, c.y + 4 * k), ImVec2(c.x + 4 * k, c.y - 4 * k), col(255, 255, 255), 1.6f * k);
    }
    else
    {
        const ImVec2 sz = body->CalcTextSizeA(13.0f * k, FLT_MAX, 0.0f, "A");
        dl->AddText(body, 13.0f * k, ImVec2(c.x - sz.x * 0.5f, c.y - sz.y * 0.5f), col(255, 255, 255), "A");
    }
    const float labelW = body->CalcTextSizeA(16.0f * k, FLT_MAX, 0.0f, "Change").x / k;
    text(body, 16.0f, P(548 - 12 - labelW, 507), col(255, 255, 255), "Change");
}
