// User settings (%LOCALAPPDATA%\TCSCCRecomp\settings.ini).
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

struct DisplaySettings
{
    uint32_t scale = 1; // internal resolution = 1280x720 * scale (1..4)
    enum Mode { Windowed, Borderless, Fullscreen } mode = Windowed;
    bool vsync = true;
    uint32_t anisotropy = 1; // minimum anisotropic filtering for linear-filtered textures (1 = off .. 16)
    // Quality of life, off by default (authentic): goggle vision modes without the scanline overlay
    // and without the thermal/EMF noise displacement ([Display] CleanGoggles=1).
    bool cleanGoggles = false;
    // Opaque surfaces drawn from both sides ([Display] DrawBackFaces=1). Default off = the console's
    // culling: the Iceland shack holes were the BSP index-offset bug, and drawing both sides put the
    // back of the mirror wall into the Hotel bathroom mirror's reflection pass.
    bool drawBackFaces = false;
    // Horizontal field of view of the player camera in degrees ([Display] FOV=70..120). 90 is the
    // game's own (nothing is changed); zooms (under 60) keep their own value (camera.cpp).
    uint32_t fov = 90;
    // 60 fps ([Display] FPS60=1): the guest vblank runs at 120 Hz, so the title's every-2nd-vblank flip
    // caps at 60 instead of 30. The game clock is wall time (0x401AAB48 tracks it 1:1 at both rates,
    // movement matches, 01_Market 2026-11-04). Default off (authentic).
    bool fps60 = false;
    // Ultrawide ([Display] Ultrawide=1): in windows wider than 16:9 the camera widens (Hor+) and the
    // frame fills the window; the HUD and menus are stretched (markers stay on target). Off = 16:9 bars.
    bool ultrawide = false;
    // Draw distance ([Display] DrawDistance=Console|High|Ultra|Maximum): small objects are culled and
    // meshes switch to coarser LODs 2x (High) / 4x (Ultra) further away; Maximum = no small-object cull,
    // always the finest LOD. Console (default) changes nothing. docs/draw_distance.md.
    uint32_t drawDistance = 0;
    // Post-processing on the presented frame (F1 menu, Graphics). Defaults leave the image as the
    // console made it. The game's own 4x MSAA is always on.
    enum AntiAliasing { AaOff, AaFxaa } antiAliasing = AaOff; // [Display] AntiAliasing=Off|FXAA
    uint32_t sharpening = 0;                                    // [Display] Sharpening=0..100 (percent)
    // [Display] Gamma=50..200 (percent): display gamma on top of the game's own brightness (the title's
    // gamma ramp is remapped, gamma_ramp.h WithUserGamma). 100 = unchanged, higher = brighter midtones.
    uint32_t gamma = 100;
    // [Display] Upscaling=Off|Native|Quality|Balanced|Performance|UltraPerformance: Off uses Scale;
    // the others pick Scale from the window height (UpscalingScale) and upscale to the window.
    enum Upscaling { UpOff, UpNative, UpQuality, UpBalanced, UpPerformance, UpUltraPerformance } upscaling = UpOff;
};

// Internal resolution scale for an upscaling mode: the window height divided by the mode's ratio
// (Native 1, Quality 1.5, Balanced 1.7, Performance 2, Ultra Performance 3), in 720-row steps,
// rounded, 1..4. UpOff returns manualScale.
uint32_t UpscalingScale(DisplaySettings::Upscaling mode, uint32_t windowHeight, uint32_t manualScale);
const char* UpscalingName(DisplaySettings::Upscaling mode);     // ini value ("UltraPerformance")
const char* AntiAliasingName(DisplaySettings::AntiAliasing aa); // ini value ("FXAA")
const char* DrawDistanceName(uint32_t level);                   // ini value ("Console" .. "Maximum")
void SetDrawDistance(int level);                                // lod.cpp applies it on the next guest read
int DrawDistance();

struct InputSettings
{
    bool mouseLook = true;         // raw mouse motion drives the right stick while the window has focus
    float mouseSensitivity = 1.0f; // full stick deflection at 50 / sensitivity pixels per poll (0.1..10)
    bool mouseInvertY = false;
    bool sdlGamepads = true;       // [Input] PlayStationNative: SDL3 gamepads (PlayStation, Switch) when no XInput pad
    std::string buttonPrompts = "Auto"; // [Input] ButtonPrompts: Auto|Xbox|PlayStation|Keyboard (prompts.h)
};

struct UpdateSettings
{
    bool checkOnStart = true; // [Update] CheckOnStart: ask GitHub for the latest release at startup
    std::string repo;         // [Update] Repo=owner/name: overrides the build's repository (testing)
};

DisplaySettings LoadDisplaySettings(const std::filesystem::path& ini); // defaults if missing/invalid
InputSettings LoadInputSettings(const std::filesystem::path& ini);     // [Input] MouseLook, MouseSensitivity, MouseInvertY, PlayStationNative, ButtonPrompts
UpdateSettings LoadUpdateSettings(const std::filesystem::path& ini);
// [General] Language=English|French|German|Spanish|Italian (or the X_LANGUAGE number):
// the console language the title is told (XGetLanguage, XCONFIG_USER_LANGUAGE). Default English (1).
// Applies on the next start. Env SCDA_LANGUAGE overrides (tests).
uint32_t GameLanguage();
const char* GameLanguageName(uint32_t id); // "English" .. ; nullptr if not one the disc can carry
// Writes key=value into [section], replacing the key's line (any case) or adding it (and the
// section). Other lines are kept as they are. Written to a temporary file, then renamed over.
bool SaveSetting(const std::filesystem::path& ini, const std::string& section, const std::string& key, const std::string& value);
std::filesystem::path SettingsPath();
// [Online] Enabled=1: System Link over Steam (online/xnet.cpp), read at startup. Default off.
bool OnlineEnabledSetting();
