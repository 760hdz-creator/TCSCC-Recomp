// Keyboard/mouse bindings: which keys press which pad control (settings.ini [Keyboard], one line per
// control, keys comma-separated, e.g. "A=E, Enter"). The keyboard drives the same virtual pad as a
// controller; the keyboard button prompts (prompts.cpp) show the first key bound to each control.
#pragma once
#include <array>
#include <filesystem>
#include <string>
#include <vector>

enum class Control
{
    A, B, X, Y, LB, RB, LT, RT, LS, RS, Back, Start, DpadUp, DpadDown, DpadLeft, DpadRight,
    MoveUp, MoveDown, MoveLeft, MoveRight, LookUp, LookDown, LookLeft, LookRight,
    Walk, // held: the left stick at walking speed (keyboard movement is full deflection otherwise)
    Count
};

// Key codes are Win32 virtual keys (mouse buttons VK_LBUTTON...VK_XBUTTON2); the wheel has two pseudo keys.
constexpr int kWheelUp = 0x1001, kWheelDown = 0x1002;

struct KeyBindings
{
    std::array<std::vector<int>, size_t(Control::Count)> keys;
    const std::vector<int>& operator[](Control c) const { return keys[size_t(c)]; }
    std::vector<int>& operator[](Control c) { return keys[size_t(c)]; }
};

KeyBindings DefaultKeyBindings();
// Defaults, then every [Keyboard] line that parses replaces that control's keys ("None" = unbound).
KeyBindings LoadKeyBindings(const std::filesystem::path& ini);
KeyBindings CurrentKeyBindings(); // the user's settings.ini, reloaded after InputSettingsChanged() (settings.h)
void SaveKeyBinding(Control c, const std::vector<int>& keys); // [Keyboard] line + InputSettingsChanged()

const char* ControlName(Control c); // the [Keyboard] key: "A", "LT", "DpadUp", "MoveUp", ...
int ParseKeyName(const std::string& name); // "E", "Space", "LMB", "WheelDown", "F5", ... -> code, 0 unknown
std::string KeyName(int code);             // code -> the name ParseKeyName reads
std::string KeyLabel(int code);            // short label for a key cap: "E", "SPACE", "ENTER", "1"
bool IsMouseKey(int code);                 // a mouse button or the wheel
