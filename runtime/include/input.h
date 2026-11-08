// Controller input for XamInput*: pad 0 (pad.h: XInput, else SDL3), else the keyboard while the game window has focus,
// plus SCDA_AUTOPRESS scripted presses (unattended menu testing).
#pragma once
#include <windows.h>
#include <xinput.h>
#include <cstdint>
#include <vector>

// Writes a guest X_INPUT_STATE (packet number + XINPUT_GAMEPAD, big-endian) at addr.
void StoreInputState(uint32_t addr, uint32_t packet, const XINPUT_GAMEPAD& g);

// Keyboard/mouse -> gamepad through the [Keyboard] bindings (keybinds.h; defaults there). Mouse motion is
// the right stick (MouseToStick below); wheelUp/wheelDown = a wheel notch is being held this poll.
struct KeyBindings;
XINPUT_GAMEPAD KeyboardGamepad(bool (*down)(int vk), const KeyBindings& keys, bool wheelUp = false, bool wheelDown = false);

// Mouse look. The window thread adds raw motion and wheel notches; each XamInputGetState poll
// takes them (MouseTake zeroes the totals) and turns the motion into the right stick. MouseToStick takes
// pixels per 1/60 s: full deflection at 50 / sensitivity, +dy (down) pulls the stick down unless inverted.
// Any motion lands past the title's stick deadzone (kMouseStickDeadzone) and the title's steep stick
// response is undone (power kMouseStickCurve), so the camera turns about as fast as the mouse moves.
void MouseAccumulate(int dx, int dy, int wheel);
void MouseTake(int& dx, int& dy, int& wheel);
// Direct mouse look (MouseDirectApply): view rotation units (65536 = 360 deg) per mouse count at sensitivity 1.
constexpr float kMouseUnitsPerCount = 5.0f;
constexpr float kMouseStickDeadzone = 7849.0f / 32767.0f; // measured: 0.22 does nothing, 0.27 turns slowly
constexpr float kMouseStickCurve = 1.6f;                  // measured turn rate ~ (stick past deadzone)^1.6
void MouseToStick(float dx, float dy, float sensitivity, bool invertY, int16_t& rx, int16_t& ry);
// Mouse diagnostics for tcscc.log: the window thread counts what Windows sends (MouseEvent), each
// XamInputGetState poll counts what reaches the stick, and every 10 s a summary line is written.
enum class MouseEventKind { RawRelative, RawAbsolute, WindowMove };
void MouseEvent(MouseEventKind kind);

// The F1 menu is open: the title sees an idle pad (no pad, keyboard or mouse input reaches it).
void SetInputBlocked(bool blocked);
bool InputBlocked();
// Menu shortcut hygiene: L3+R3 held together never reaches the title (it toggles the menu), and
// after the menu closes (waitRelease) the title sees an idle pad until every button and Esc are released
// (the Start, Back, L3+R3 or Esc that closed it must not reach the game).
void FilterMenuShortcut(XINPUT_GAMEPAD& g, bool escapeDown, bool& waitRelease);

// A pad stick at rest inside the XInput deadzone (pads never rest at exactly 0): keyboard and mouse
// may drive that stick instead.
bool StickIdle(int16_t x, int16_t y, int deadzone);

struct AutoPress
{
    uint32_t frame;   // guest frame number (as SCDA_DUMP_FRAME counts)
    uint16_t buttons; // XINPUT_GAMEPAD_* bits
    uint32_t frames;  // frames held
    int axis;         // -1 none; 0-3 LX LY RX RY; 4-5 LT RT
    float value;      // axis value: sticks [-1, 1], triggers [0, 1]
};
// "<frame>:<name>[=<value>][:<frames>],..." with buttons A B X Y START BACK UP DOWN LEFT RIGHT LB RB,
// sticks LX LY RX RY (value required, [-1, 1]) and triggers LT RT ([0, 1]). Each entry is held for
// <frames> frames (default kAutoPressFrames). Bad entries are skipped.
constexpr uint32_t kAutoPressFrames = 4;
std::vector<AutoPress> ParseAutoPress(const char* text);
// Applies the script at a frame: buttons are ORed in, held axes replace the pad's.
void AutoPressApply(const std::vector<AutoPress>& script, uint32_t frame, XINPUT_GAMEPAD& g);
// Buttons the script holds at a frame.
uint16_t AutoPressButtons(const std::vector<AutoPress>& script, uint32_t frame);
