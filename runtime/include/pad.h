// Host controller behind guest pad 0: XInput pad 0 when connected (Xbox pads, Steam Input /
// DS4Windows virtual pads), else the first SDL3 gamepad (DualShock 4, DualSense, Switch Pro, ...)
// mapped to the Xbox 360 layout. Built without SDL3 (external/SDL missing): XInput only.
#pragma once
#include <windows.h>
#include <xinput.h>
#include <cstdint>

// Which pad answered: its button prompts (prompts.h).
enum class PadKind { None, Xbox, PS4, PS5 };
// Current state of the pad; false (and an idle g) when none is connected. Thread-safe.
bool PadGetState(XINPUT_GAMEPAD& g, PadKind* kind = nullptr);
// Motor speeds (0..65535) held until changed, as XInputSetState. Thread-safe.
void PadSetVibration(uint16_t low, uint16_t high);

// SDL gamepad state -> XINPUT_GAMEPAD. buttons[SDL_GAMEPAD_BUTTON_COUNT] and
// axes[SDL_GAMEPAD_AXIS_COUNT] are indexed by SDL_GamepadButton / SDL_GamepadAxis. Positional
// face buttons (south A, east B, west X, north Y); Share/Create and the touchpad click are Back,
// Options Start; stick Y flips to up-positive; triggers 0..32767 -> 0..255.
XINPUT_GAMEPAD PadFromSdl(const bool* buttons, const int16_t* axes);
