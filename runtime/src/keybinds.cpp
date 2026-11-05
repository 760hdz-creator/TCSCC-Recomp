// Keyboard/mouse bindings (keybinds.h). Defaults follow Conviction's controls (CONTROLLER LAYOUT page):
// A action, B attack, X gadget, Y execute, LT cover, RT shoot, LB crouch, RB mark, RS click zoom,
// d-pad up sonar goggles, left/right gadget, down weapon.
#include <windows.h>
#include <keybinds.h>
#include <settings.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <mutex>

namespace {

const char* const kControlNames[] = {
    "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS", "Back", "Start", "DpadUp", "DpadDown", "DpadLeft", "DpadRight",
    "MoveUp", "MoveDown", "MoveLeft", "MoveRight", "LookUp", "LookDown", "LookLeft", "LookRight", "Walk",
};
static_assert(std::size(kControlNames) == size_t(Control::Count));

struct Named
{
    const char* name;
    int code;
    const char* label;
};
const Named kKeys[] = {
    { "Space", VK_SPACE, "SPACE" }, { "Enter", VK_RETURN, "ENTER" }, { "Esc", VK_ESCAPE, "ESC" }, { "Tab", VK_TAB, "TAB" },
    { "Backspace", VK_BACK, "BKSP" }, { "Shift", VK_SHIFT, "SHIFT" }, { "LShift", VK_LSHIFT, "SHIFT" }, { "RShift", VK_RSHIFT, "R SHIFT" },
    { "Ctrl", VK_CONTROL, "CTRL" }, { "LCtrl", VK_LCONTROL, "CTRL" }, { "RCtrl", VK_RCONTROL, "R CTRL" },
    { "Alt", VK_MENU, "ALT" }, { "LAlt", VK_LMENU, "ALT" }, { "RAlt", VK_RMENU, "R ALT" }, { "CapsLock", VK_CAPITAL, "CAPS" },
    { "Up", VK_UP, "UP" }, { "Down", VK_DOWN, "DOWN" }, { "Left", VK_LEFT, "LEFT" }, { "Right", VK_RIGHT, "RIGHT" },
    { "Insert", VK_INSERT, "INS" }, { "Delete", VK_DELETE, "DEL" }, { "Home", VK_HOME, "HOME" }, { "End", VK_END, "END" },
    { "PageUp", VK_PRIOR, "PG UP" }, { "PageDown", VK_NEXT, "PG DN" },
    { "LMB", VK_LBUTTON, "LMB" }, { "RMB", VK_RBUTTON, "RMB" }, { "MMB", VK_MBUTTON, "MMB" },
    { "Mouse4", VK_XBUTTON1, "M4" }, { "Mouse5", VK_XBUTTON2, "M5" }, { "WheelUp", kWheelUp, "WHEEL" }, { "WheelDown", kWheelDown, "WHEEL" },
    { "Grave", VK_OEM_3, "`" }, { "Minus", VK_OEM_MINUS, "-" }, { "Equals", VK_OEM_PLUS, "=" }, { "LBracket", VK_OEM_4, "[" },
    { "RBracket", VK_OEM_6, "]" }, { "Backslash", VK_OEM_5, "\\" }, { "Semicolon", VK_OEM_1, ";" }, { "Apostrophe", VK_OEM_7, "'" },
    { "Comma", VK_OEM_COMMA, "," }, { "Period", VK_OEM_PERIOD, "." }, { "Slash", VK_OEM_2, "/" },
};

std::string Lower(std::string s)
{
    for (auto& c : s)
        c = char(tolower((unsigned char)c));
    return s;
}

std::string Trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

} // namespace

const char* ControlName(Control c) { return size_t(c) < size_t(Control::Count) ? kControlNames[size_t(c)] : "?"; }

int ParseKeyName(const std::string& raw)
{
    const std::string n = Lower(Trim(raw));
    if (n.size() == 1 && (isalnum((unsigned char)n[0])))
        return toupper((unsigned char)n[0]); // VK of A-Z and 0-9 = the character
    if (n.size() >= 2 && n.size() <= 3 && n[0] == 'f' && isdigit((unsigned char)n[1]))
    {
        const int k = atoi(n.c_str() + 1);
        return k >= 1 && k <= 12 ? VK_F1 + k - 1 : 0;
    }
    if (n.rfind("num", 0) == 0 && n.size() == 4 && isdigit((unsigned char)n[3]))
        return VK_NUMPAD0 + (n[3] - '0');
    for (const Named& k : kKeys)
        if (Lower(k.name) == n)
            return k.code;
    return 0;
}

std::string KeyName(int code)
{
    if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9'))
        return std::string(1, char(code));
    if (code >= VK_F1 && code <= VK_F12)
        return "F" + std::to_string(code - VK_F1 + 1);
    if (code >= VK_NUMPAD0 && code <= VK_NUMPAD9)
        return "Num" + std::to_string(code - VK_NUMPAD0);
    for (const Named& k : kKeys)
        if (k.code == code)
            return k.name;
    return "None";
}

std::string KeyLabel(int code)
{
    for (const Named& k : kKeys)
        if (k.code == code)
            return k.label;
    if (code >= VK_NUMPAD0 && code <= VK_NUMPAD9)
        return "NUM " + std::to_string(code - VK_NUMPAD0);
    return KeyName(code);
}

bool IsMouseKey(int code)
{
    return code == VK_LBUTTON || code == VK_RBUTTON || code == VK_MBUTTON || code == VK_XBUTTON1 || code == VK_XBUTTON2 ||
           code == kWheelUp || code == kWheelDown;
}

KeyBindings DefaultKeyBindings()
{
    KeyBindings b;
    auto set = [&](Control c, std::initializer_list<int> keys) { b[c] = keys; };
    set(Control::A, { 'E', VK_RETURN });
    set(Control::B, { 'F', VK_BACK });
    set(Control::X, { 'G' });
    set(Control::Y, { 'Q' });
    set(Control::LB, { 'C', VK_LCONTROL });
    set(Control::RB, { 'Z', VK_MBUTTON });
    set(Control::LT, { VK_SPACE });
    set(Control::RT, { VK_LBUTTON });
    set(Control::LS, { 'V' });
    set(Control::RS, { VK_RBUTTON });
    set(Control::Back, { VK_TAB });
    set(Control::Start, { VK_ESCAPE });
    set(Control::DpadUp, { 'T', VK_UP });
    set(Control::DpadDown, { 'R', VK_DOWN, kWheelUp, kWheelDown });
    set(Control::DpadLeft, { '1', VK_LEFT });
    set(Control::DpadRight, { '2', VK_RIGHT });
    set(Control::MoveUp, { 'W' });
    set(Control::MoveDown, { 'S' });
    set(Control::MoveLeft, { 'A' });
    set(Control::MoveRight, { 'D' });
    set(Control::LookUp, { 'I' });
    set(Control::LookDown, { 'K' });
    set(Control::LookLeft, { 'J' });
    set(Control::LookRight, { 'L' });
    set(Control::Walk, { VK_LSHIFT });
    return b;
}

KeyBindings LoadKeyBindings(const std::filesystem::path& ini)
{
    KeyBindings b = DefaultKeyBindings();
    std::ifstream in(ini);
    std::string line, section;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() > 2 && line.front() == '[')
        {
            section = Lower(line.substr(1, line.find(']') - 1));
            continue;
        }
        const size_t eq = line.find('=');
        if (section != "keyboard" || eq == std::string::npos)
            continue;
        const std::string key = Lower(Trim(line.substr(0, eq)));
        for (size_t c = 0; c < size_t(Control::Count); c++)
        {
            if (Lower(kControlNames[c]) != key)
                continue;
            std::vector<int> keys;
            bool ok = true;
            std::string list = line.substr(eq + 1);
            for (size_t start = 0; start <= list.size();)
            {
                const size_t comma = std::min(list.find(',', start), list.size());
                const std::string item = Trim(list.substr(start, comma - start));
                start = comma + 1;
                if (item.empty() || Lower(item) == "none")
                    continue;
                if (const int code = ParseKeyName(item))
                    keys.push_back(code);
                else
                    ok = false;
            }
            if (ok)
                b.keys[c] = keys;
        }
    }
    return b;
}

KeyBindings CurrentKeyBindings()
{
    static std::mutex m;
    static KeyBindings cached;
    static uint32_t loaded = 0;
    std::lock_guard l(m);
    if (const uint32_t g = InputSettingsGeneration(); g != loaded)
        cached = LoadKeyBindings(SettingsPath()), loaded = g;
    return cached;
}

void SaveKeyBinding(Control c, const std::vector<int>& keys)
{
    std::string list;
    for (const int k : keys)
        list += (list.empty() ? "" : ", ") + KeyName(k);
    SaveSetting(SettingsPath(), "Keyboard", ControlName(c), list.empty() ? "None" : list);
    InputSettingsChanged();
}
