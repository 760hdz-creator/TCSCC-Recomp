// Host controller behind guest pad 0 (pad.h): XInput pad 0, else the first SDL3 gamepad.
#include <algorithm>
#include <mutex>
#include <pad.h>
#include <runtime.h>
#include <settings.h>
#ifdef SCDA_HAVE_SDL3
#include <SDL3/SDL.h>
#endif

namespace {
std::mutex g_mutex;
uint16_t g_low, g_high; // last vibration the title set
constexpr uint32_t kRumbleMs = 0xFFFF; // SDL's longest rumble (SDL_MAX_RUMBLE_DURATION_MS, internal)
#ifdef SCDA_HAVE_SDL3
bool g_sdl;             // SDL gamepad subsystem up
SDL_Gamepad* g_pad;     // open SDL gamepad, or null

// SDL is started on the first poll. Its own thread handles device notifications; the game window
// is not an SDL window, so the pads keep reporting while it is in the background (as XInput does).
void SdlInit()
{
    static bool tried;
    if (tried)
        return;
    tried = true;
    if (!LoadInputSettings(SettingsPath()).sdlGamepads)
    {
        RuntimeLog("[input] [Input] PlayStationNative=0: XInput only\n");
        return;
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD))
    {
        RuntimeLog("[input] SDL_Init failed (%s): XInput only\n", SDL_GetError());
        return;
    }
    g_sdl = true;
    const int v = SDL_GetVersion();
    RuntimeLog("[input] SDL %d.%d.%d gamepads\n", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
}

// Polls SDL (devices and state) and keeps the first gamepad open. Caller holds g_mutex.
SDL_Gamepad* SdlPad()
{
    SdlInit();
    if (!g_sdl)
        return nullptr;
    SDL_UpdateGamepads();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST); // nothing reads SDL's queue
    if (g_pad && !SDL_GamepadConnected(g_pad))
    {
        RuntimeLog("[input] SDL gamepad disconnected\n");
        SDL_CloseGamepad(g_pad);
        g_pad = nullptr;
    }
    if (!g_pad)
    {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (ids && count > 0 && (g_pad = SDL_OpenGamepad(ids[0])) != nullptr)
        {
            const char* name = SDL_GetGamepadName(g_pad);
            RuntimeLog("[input] SDL gamepad: %s type %d\n", name ? name : "?", int(SDL_GetGamepadType(g_pad)));
            SDL_RumbleGamepad(g_pad, g_low, g_high, kRumbleMs);
        }
        SDL_free(ids);
    }
    return g_pad;
}
#endif
} // namespace

#ifdef SCDA_HAVE_SDL3
XINPUT_GAMEPAD PadFromSdl(const bool* buttons, const int16_t* axes)
{
    static const struct { int sdl; uint16_t button; } kButtons[] = {
        { SDL_GAMEPAD_BUTTON_SOUTH, XINPUT_GAMEPAD_A }, { SDL_GAMEPAD_BUTTON_EAST, XINPUT_GAMEPAD_B },
        { SDL_GAMEPAD_BUTTON_WEST, XINPUT_GAMEPAD_X }, { SDL_GAMEPAD_BUTTON_NORTH, XINPUT_GAMEPAD_Y },
        { SDL_GAMEPAD_BUTTON_BACK, XINPUT_GAMEPAD_BACK }, { SDL_GAMEPAD_BUTTON_TOUCHPAD, XINPUT_GAMEPAD_BACK },
        { SDL_GAMEPAD_BUTTON_START, XINPUT_GAMEPAD_START },
        { SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_GAMEPAD_LEFT_THUMB }, { SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_GAMEPAD_RIGHT_THUMB },
        { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER }, { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER },
        { SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN },
        { SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT },
    };
    XINPUT_GAMEPAD g{};
    for (auto& b : kButtons)
        if (buttons[b.sdl])
            g.wButtons |= b.button;
    auto flip = [](int16_t v) { return int16_t(std::min(-int(v), 32767)); };
    auto trigger = [](int16_t v) { return uint8_t((std::clamp(int(v), 0, 32767) * 255 + 16383) / 32767); };
    g.sThumbLX = axes[SDL_GAMEPAD_AXIS_LEFTX];
    g.sThumbLY = flip(axes[SDL_GAMEPAD_AXIS_LEFTY]);
    g.sThumbRX = axes[SDL_GAMEPAD_AXIS_RIGHTX];
    g.sThumbRY = flip(axes[SDL_GAMEPAD_AXIS_RIGHTY]);
    g.bLeftTrigger = trigger(axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER]);
    g.bRightTrigger = trigger(axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER]);
    return g;
}
#endif

bool PadGetState(XINPUT_GAMEPAD& g, PadKind* kind)
{
    g = XINPUT_GAMEPAD{};
    if (kind)
        *kind = PadKind::None;
    XINPUT_STATE x{};
    if (XInputGetState(0, &x) == ERROR_SUCCESS)
    {
        g = x.Gamepad;
        if (kind)
            *kind = PadKind::Xbox;
        return true;
    }
#ifdef SCDA_HAVE_SDL3
    std::lock_guard lock(g_mutex);
    if (SDL_Gamepad* pad = SdlPad())
    {
        bool buttons[SDL_GAMEPAD_BUTTON_COUNT];
        int16_t axes[SDL_GAMEPAD_AXIS_COUNT];
        for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
            buttons[i] = SDL_GetGamepadButton(pad, SDL_GamepadButton(i));
        for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++)
            axes[i] = SDL_GetGamepadAxis(pad, SDL_GamepadAxis(i));
        g = PadFromSdl(buttons, axes);
        if (kind)
        {
            const SDL_GamepadType type = SDL_GetGamepadType(pad);
            *kind = type == SDL_GAMEPAD_TYPE_PS4 ? PadKind::PS4 : type == SDL_GAMEPAD_TYPE_PS5 ? PadKind::PS5 : PadKind::Xbox;
        }
        return true;
    }
#endif
    return false;
}

void PadSetVibration(uint16_t low, uint16_t high)
{
    XINPUT_VIBRATION v{ low, high };
    XInputSetState(0, &v);
    std::lock_guard lock(g_mutex);
    g_low = low;
    g_high = high;
#ifdef SCDA_HAVE_SDL3
    if (g_pad)
        // Not 0 ("until changed"): that also stops SDL resending it, and a DualShock 4 drops rumble
        // after ~5 s. Repeated calls with the same speeds only extend it.
        SDL_RumbleGamepad(g_pad, low, high, kRumbleMs);
#endif
}
