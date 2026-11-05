// In-game button prompts for the device in use (prompts.h). Icons are drawn with GDI+ at 4x and
// box-filtered down; the art is ours (shapes and text), nothing is taken from the game or Sony.
#include <windows.h>
#include <objidl.h> // GDI+ needs IStream/PROPID (WIN32_LEAN_AND_MEAN drops them)
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
namespace Gdiplus
{
using std::max;
using std::min;
}
#include <gdiplus.h>
#include <keybinds.h>
#include <prompts.h>
#include <settings.h>

namespace {

struct Icon
{
    const char* name;
    uint32_t width, height;
    uint64_t hash;
};
const Icon kIcons[] = {
#include "prompts_table.inc"
};

} // namespace

PromptSetting ParsePromptSetting(const char* value)
{
    std::string v = value ? value : "";
    for (auto& c : v)
        c = char(tolower((unsigned char)c));
    return v == "xbox" ? PromptSetting::Xbox : v == "playstation" ? PromptSetting::PlayStation
         : v == "keyboard" ? PromptSetting::Keyboard : PromptSetting::Auto;
}

static PromptStyle PadStyle(PadKind k)
{
    return k == PadKind::PS4 ? PromptStyle::PS4 : k == PadKind::PS5 ? PromptStyle::PS5 : PromptStyle::Xbox;
}

void PromptTracker::Note(PadKind pad, bool padActive, bool keyboardActive)
{
    if (pad == PadKind::PS4 || pad == PadKind::PS5)
        lastPs = pad;
    if (keyboardActive)
        last = int(PromptStyle::Keyboard);
    else if (padActive && pad != PadKind::None)
        last = int(PadStyle(pad));
}

PromptStyle PromptTracker::Style(PromptSetting setting, PadKind connected) const
{
    switch (setting)
    {
    case PromptSetting::Xbox: return PromptStyle::Xbox;
    case PromptSetting::PlayStation: return lastPs == PadKind::PS4 ? PromptStyle::PS4 : PromptStyle::PS5;
    case PromptSetting::Keyboard: return PromptStyle::Keyboard;
    default: break;
    }
    if (last >= 0)
        return PromptStyle(last);
    return connected != PadKind::None ? PadStyle(connected) : PromptStyle::Keyboard;
}

static std::mutex g_trackerMutex;
static PromptTracker g_tracker;
static PadKind g_connected = PadKind::None;
static std::atomic<int> g_style{ int(PromptStyle::Keyboard) };

void PromptNoteInput(PadKind pad, bool padActive, bool keyboardActive)
{
    const PromptSetting setting = ParsePromptSetting(CurrentInputSettings().buttonPrompts.c_str());
    std::lock_guard lock(g_trackerMutex);
    g_connected = pad;
    g_tracker.Note(pad, padActive, keyboardActive);
    g_style = int(g_tracker.Style(setting, g_connected));
}

PromptStyle CurrentPromptStyle() { return PromptStyle(g_style.load()); }

int PromptIconIndex(uint64_t blockHash, uint32_t width, uint32_t height)
{
    for (int i = 0; i < int(std::size(kIcons)); i++)
        if (kIcons[i].hash == blockHash && kIcons[i].width == width && kIcons[i].height == height)
            return i;
    return -1;
}

const char* PromptIconName(int index) { return index >= 0 && index < int(std::size(kIcons)) ? kIcons[index].name : "?"; }

// ---- drawing ---------------------------------------------------------------------------------

namespace {
using namespace Gdiplus;

constexpr int kSuper = 4; // drawn at 4x, box-filtered down
enum Dir { Up = 1, Down = 2, Left = 4, Right = 8 };

// What an icon shows, from its name.
struct Shape
{
    enum Kind { Face, Back, Start, Bumper, Trigger, Bumpers, Triggers, Stick, Dpad, Opsat } kind;
    char which = 0;      // face/opsat: a b x y; bumper/trigger/stick: l r
    int dirs = 0;        // stick/dpad arrows
    bool press = false;  // stick click
};

Shape ParseShape(const std::string& n)
{
    Shape s{ Shape::Face };
    auto dirsOf = [](const std::string& rest)
    {
        int d = 0;
        for (size_t i = 0; i < rest.size(); i++)
            if (rest[i] == 'u' && rest.compare(i, 2, "up") == 0) { d |= Up; i++; }
            else if (rest[i] == 'u') d |= Up;
            else if (rest[i] == 'd') d |= Down;
            else if (rest[i] == 'l') d |= Left;
            else if (rest[i] == 'r') d |= Right;
        return d;
    };
    if (n.rfind("btn_opsat_", 0) == 0)
        return { Shape::Opsat, n.back() };
    const std::string b = n.substr(7); // after "button_"
    if (b.size() == 1 && strchr("abxy", b[0]))
        return { Shape::Face, b[0] };
    if (b == "back") return { Shape::Back };
    if (b == "start") return { Shape::Start };
    if (b == "lb" || b == "rb") return { Shape::Bumper, b[0] };
    if (b == "lt" || b == "rt") return { Shape::Trigger, b[0] };
    if (b == "rb_lb") return { Shape::Bumpers };
    if (b == "rt_lt") return { Shape::Triggers };
    if (b[0] == 'd')
    {
        s = { Shape::Dpad };
        s.dirs = dirsOf(b.substr(1));
        return s;
    }
    // ls*/rs*, and al*/ar* (the same sticks: al = up/down, al_up, al_clic = click)
    s = { Shape::Stick, b[1] == 's' ? b[0] : b[1] };
    std::string rest = b.substr(2);
    s.press = rest.find("press") != std::string::npos || rest.find("clic") != std::string::npos;
    if (!s.press)
        s.dirs = b[1] == 's' ? dirsOf(rest) : rest.empty() ? (Up | Down) : dirsOf(rest);
    return s;
}

Color Rgb(uint32_t rgb, int a = 255) { return Color(BYTE(a), BYTE(rgb >> 16), BYTE(rgb >> 8), BYTE(rgb)); }

void RoundRect(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r)
{
    r = std::min({ r, w / 2, h / 2 });
    p.AddArc(x, y, 2 * r, 2 * r, 180, 90);
    p.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 90);
    p.AddArc(x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 0, 90);
    p.AddArc(x, y + h - 2 * r, 2 * r, 2 * r, 90, 90);
    p.CloseFigure();
}

class Canvas
{
public:
    explicit Canvas(int size) : m_size(size), m_bmp(size, size, PixelFormat32bppARGB), m_g(&m_bmp)
    {
        m_g.SetSmoothingMode(SmoothingModeAntiAlias);
        m_g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        m_g.Clear(Color(0, 0, 0, 0));
    }
    Graphics& G() { return m_g; }
    REAL U() const { return REAL(m_size) / 64; } // one icon unit (64 units across)
    Bitmap& Bmp() { return m_bmp; }

    // Text centred in a box, shrunk until it fits.
    void Text(const wchar_t* text, REAL cx, REAL cy, REAL maxW, REAL maxH, Color color, bool bold = true)
    {
        FontFamily family(L"Arial");
        for (REAL size = maxH; size > 2; size *= 0.92f)
        {
            Font font(&family, size, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
            RectF box;
            m_g.MeasureString(text, -1, &font, PointF(0, 0), StringFormat::GenericTypographic(), &box);
            if (box.Width <= maxW && box.Height <= maxH * 1.05f)
            {
                SolidBrush brush(color);
                m_g.DrawString(text, -1, &font, PointF(cx - box.Width / 2, cy - box.Height / 2), StringFormat::GenericTypographic(), &brush);
                return;
            }
        }
    }

private:
    int m_size;
    Bitmap m_bmp;
    Graphics m_g;
};

// Small filled triangle pointing in `dir`, tip at (x, y).
void Arrow(Canvas& c, REAL x, REAL y, int dir, REAL size, Color color)
{
    PointF p[3];
    const REAL s = size, h = size * 0.9f;
    switch (dir)
    {
    case Up: p[0] = { x, y }, p[1] = { x - s, y + h }, p[2] = { x + s, y + h }; break;
    case Down: p[0] = { x, y }, p[1] = { x - s, y - h }, p[2] = { x + s, y - h }; break;
    case Left: p[0] = { x, y }, p[1] = { x + h, y - s }, p[2] = { x + h, y + s }; break;
    default: p[0] = { x, y }, p[1] = { x - h, y - s }, p[2] = { x - h, y + s }; break;
    }
    SolidBrush b(color);
    c.G().FillPolygon(&b, p, 3);
}

// Conviction's icon look (its 36x36 button textures): flat mid-grey shapes, a light rim, white symbols.
const Color kFill = Rgb(0x969696), kRim = Rgb(0xE2E2E2), kInk = Rgb(0xF7F7F7), kDim = Rgb(0x6E6E6E);
constexpr REAL kRimW = 3.6f; // the rim, in icon units (2 px of 36)

void Shape2(Canvas& c, GraphicsPath& path, Color fill = kFill)
{
    SolidBrush f(fill);
    c.G().FillPath(&f, &path);
    Pen rim(kRim, kRimW * c.U());
    rim.SetLineJoin(LineJoinRound);
    c.G().DrawPath(&rim, &path);
}

// A round button (face buttons, sticks): the disc fills the icon like the game's.
void Disk(Canvas& c, REAL r = 29)
{
    const REAL u = c.U();
    GraphicsPath path;
    path.AddEllipse((32 - r) * u, (32 - r) * u, 2 * r * u, 2 * r * u);
    Shape2(c, path);
}

void PsSymbol(Canvas& c, char which, REAL cx, REAL cy, REAL r)
{
    const REAL u = c.U();
    cx *= u, cy *= u, r *= u;
    Pen p(kInk, 5.0f * u);
    p.SetStartCap(LineCapRound), p.SetEndCap(LineCapRound), p.SetLineJoin(LineJoinRound);
    switch (which)
    {
    case 'a': c.G().DrawLine(&p, cx - r, cy - r, cx + r, cy + r); c.G().DrawLine(&p, cx - r, cy + r, cx + r, cy - r); break; // cross
    case 'b': c.G().DrawEllipse(&p, cx - r * 1.08f, cy - r * 1.08f, r * 2.16f, r * 2.16f); break;                              // circle
    case 'x': c.G().DrawRectangle(&p, cx - r, cy - r, 2 * r, 2 * r); break;                                                     // square
    default: { PointF t[3] = { { cx, cy - r * 1.2f }, { cx - r * 1.2f, cy + r * 0.85f }, { cx + r * 1.2f, cy + r * 0.85f } };  // triangle
        c.G().DrawPolygon(&p, t, 3); break; }
    }
}

// A rounded cap with a centred label (PS shoulders/triggers, keyboard keys).
void Cap(Canvas& c, REAL x, REAL y, REAL w, REAL h, const wchar_t* label, REAL radius = 7)
{
    const REAL u = c.U();
    GraphicsPath path;
    RoundRect(path, x * u, y * u, w * u, h * u, radius * u);
    Shape2(c, path);
    if (label && *label)
        c.Text(label, (x + w / 2) * u, (y + h / 2) * u, (w - 9) * u, (h - 11) * u, kInk);
}

// Arrow key cap: one arrow, or all four.
void ArrowCap(Canvas& c, REAL x, REAL y, REAL w, REAL h, int dirs)
{
    Cap(c, x, y, w, h, nullptr);
    const REAL u = c.U(), cx = (x + w / 2) * u, cy = (y + h / 2) * u, s = std::min(w, h) * u;
    if (dirs == 0)
    {
        Arrow(c, cx, cy - s * 0.3f, Up, s * 0.11f, kInk), Arrow(c, cx, cy + s * 0.3f, Down, s * 0.11f, kInk);
        Arrow(c, cx - s * 0.3f, cy, Left, s * 0.11f, kInk), Arrow(c, cx + s * 0.3f, cy, Right, s * 0.11f, kInk);
        return;
    }
    const REAL a = s * 0.2f;
    if (dirs == Up) Arrow(c, cx, cy - a, Up, a, kInk);
    if (dirs == Down) Arrow(c, cx, cy + a, Down, a, kInk);
    if (dirs == Left) Arrow(c, cx - a, cy, Left, a, kInk);
    if (dirs == Right) Arrow(c, cx + a, cy, Right, a, kInk);
}

// Two caps side by side.
template <typename F>
void Pair(F&& draw)
{
    draw(2.0f, 30.0f);
    draw(34.0f, 30.0f);
}

// A mouse: the part in `lit` (1 left, 2 right, 4 wheel) white, optional move arrows around it.
void Mouse(Canvas& c, int lit, int dirs)
{
    const REAL u = c.U(), x = dirs ? 20 : 15, y = dirs ? 14 : 5, w = dirs ? 24 : 34, h = dirs ? 36 : 54;
    GraphicsPath body;
    RoundRect(body, x * u, y * u, w * u, h * u, (w / 2) * u);
    SolidBrush fill(kFill);
    c.G().FillPath(&fill, &body);
    const REAL split = y + h * 0.42f;
    Region clip(&body);
    c.G().SetClip(&clip);
    SolidBrush on(kInk);
    if (lit & 1) c.G().FillRectangle(&on, x * u, y * u, (w / 2) * u, (split - y) * u);
    if (lit & 2) c.G().FillRectangle(&on, (x + w / 2) * u, y * u, (w / 2) * u, (split - y) * u);
    c.G().ResetClip();
    Pen line(kRim, 2.4f * u);
    c.G().DrawLine(&line, x * u, split * u, (x + w) * u, split * u);
    c.G().DrawLine(&line, (x + w / 2) * u, y * u, (x + w / 2) * u, split * u);
    GraphicsPath wheel;
    RoundRect(wheel, (x + w / 2 - 3.5f) * u, (y + h * 0.1f) * u, 7 * u, h * 0.22f * u, 3.5f * u);
    SolidBrush wheelFill(lit & 4 ? kInk : kDim);
    c.G().FillPath(&wheelFill, &wheel);
    Pen rim(kRim, kRimW * u);
    c.G().DrawPath(&rim, &body);
    const REAL cx = 32 * u, cy = (y + h / 2) * u, a = 5 * u;
    if (dirs & Up) Arrow(c, cx, (y - 2) * u, Up, a, kInk);
    if (dirs & Down) Arrow(c, cx, (y + h + 2) * u, Down, a, kInk);
    if (dirs & Left) Arrow(c, (x - 3) * u, cy, Left, a, kInk);
    if (dirs & Right) Arrow(c, (x + w + 3) * u, cy, Right, a, kInk);
}

void PsStick(Canvas& c, const Shape& s)
{
    Disk(c);
    const REAL u = c.U();
    Pen inner(kRim, 2.0f * u);
    c.G().DrawEllipse(&inner, 14 * u, 14 * u, 36 * u, 36 * u);
    // The game's stick icons stand for the stick (move / camera); a click shows L3 / R3.
    c.Text(s.which == 'l' ? (s.press ? L"L3" : L"L") : (s.press ? L"R3" : L"R"), 32 * u, 32.5f * u, 26 * u, 22 * u, kInk);
    const REAL a = 5.5f * u;
    if (s.dirs & Up) Arrow(c, 32 * u, 7 * u, Up, a, kInk);
    if (s.dirs & Down) Arrow(c, 32 * u, 57 * u, Down, a, kInk);
    if (s.dirs & Left) Arrow(c, 7 * u, 32 * u, Left, a, kInk);
    if (s.dirs & Right) Arrow(c, 57 * u, 32 * u, Right, a, kInk);
}

// PlayStation d-pad: four separate arrow-shaped keys, the named directions white.
void PsDpad(Canvas& c, int dirs)
{
    const REAL u = c.U();
    auto key = [&](int dir)
    {
        PointF p[5] = { { -8, -7 }, { 8, -7 }, { 8, -23 }, { 0, -30 }, { -8, -23 } };
        for (auto& q : p)
        {
            REAL x = q.X, y = q.Y;
            if (dir == Down) y = -y;
            if (dir == Left) std::swap(x, y);
            if (dir == Right) std::swap(x, y), x = -x;
            q = { (32 + x) * u, (32 + y) * u };
        }
        GraphicsPath path;
        path.AddPolygon(p, 5);
        Shape2(c, path, (dirs & dir) ? kInk : kDim);
    };
    key(Up), key(Down), key(Left), key(Right);
}

void DrawPs(Canvas& c, const Shape& s, bool ps4)
{
    const REAL u = c.U();
    switch (s.kind)
    {
    case Shape::Face:
    case Shape::Opsat: Disk(c); PsSymbol(c, s.which, 32, 32, 12); break;
    case Shape::Back:
        // The touchpad (its click is Back here; SHARE / CREATE are too small to read at 36 px).
        Cap(c, 6, 14, 52, 36, nullptr, 9);
        if (!ps4)
        {
            Pen line(kInk, 2.4f * u);
            c.G().DrawLine(&line, 32 * u, 20 * u, 32 * u, 44 * u);
        }
        break;
    case Shape::Start:
    {
        // OPTIONS: a pill with three lines.
        Cap(c, 8, 16, 48, 32, nullptr, 16);
        Pen line(kInk, 3.2f * u);
        line.SetStartCap(LineCapRound), line.SetEndCap(LineCapRound);
        for (int i = -1; i <= 1; i++)
            c.G().DrawLine(&line, 23 * u, (32 + 6.5f * i) * u, 41 * u, (32 + 6.5f * i) * u);
        break;
    }
    case Shape::Bumper: Cap(c, 5, 15, 54, 34, s.which == 'l' ? L"L1" : L"R1", 10); break;
    case Shape::Trigger: Cap(c, 9, 4, 46, 56, s.which == 'l' ? L"L2" : L"R2", 15); break;
    case Shape::Bumpers: Pair([&](REAL x, REAL w) { Cap(c, x, 18, w - 2, 28, x < 10 ? L"L1" : L"R1", 8); }); break;
    case Shape::Triggers: Pair([&](REAL x, REAL w) { Cap(c, x, 11, w - 2, 42, x < 10 ? L"L2" : L"R2", 10); }); break;
    case Shape::Stick: PsStick(c, s); break;
    case Shape::Dpad: PsDpad(c, s.dirs); break;
    }
}

// The first key bound to `c` ([Keyboard], keybinds.h): a mouse button or the wheel draws the mouse with
// that part lit, any other key a cap with its label. Unbound: an empty cap.
int FirstKey(Control c)
{
    const std::vector<int> keys = CurrentKeyBindings()[c];
    return keys.empty() ? 0 : keys[0];
}

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

void KeyIcon(Canvas& c, int code, REAL x = 10, REAL y = 10, REAL w = 44, REAL h = 44)
{
    if (code == VK_LBUTTON || code == VK_RBUTTON || code == VK_MBUTTON || code == kWheelUp || code == kWheelDown)
        return Mouse(c, code == VK_LBUTTON ? 1 : code == VK_RBUTTON ? 2 : 4, 0);
    if (code == VK_UP || code == VK_DOWN || code == VK_LEFT || code == VK_RIGHT)
        return ArrowCap(c, x, y, w, h, code == VK_UP ? Up : code == VK_DOWN ? Down : code == VK_LEFT ? Left : Right);
    const std::wstring label = code ? Wide(KeyLabel(code)) : L"";
    if (label.size() > 2 && w < 50) // long names (SPACE, ENTER, SHIFT) get a wide cap
        x = 2, y = 17, w = 60, h = 30;
    Cap(c, x, y, w, h, label.c_str());
}

void DrawKeyboard(Canvas& c, const Shape& s)
{
    auto dirs = [&](int d, Control up, Control down, Control left, Control right)
    {
        const int n = !!(d & Up) + !!(d & Down) + !!(d & Left) + !!(d & Right);
        if (n == 1)
            return KeyIcon(c, FirstKey(d == Up ? up : d == Down ? down : d == Left ? left : right));
        // Several directions: two caps (up/left and down/right), or all four as a 2x2 block.
        if (n == 4 || n == 0)
        {
            const Control q[4] = { up, left, down, right };
            const REAL pos[4][2] = { { 22, 4 }, { 1, 33 }, { 22, 33 }, { 43, 33 } };
            for (int i = 0; i < 4; i++)
                Cap(c, pos[i][0], pos[i][1], 20, 27, Wide(KeyLabel(FirstKey(q[i]))).substr(0, 1).c_str(), 4);
            return;
        }
        const Control a = d & Up ? up : left, b = d & Down ? down : right;
        Pair([&](REAL x, REAL w) { Cap(c, x, 17, w - 2, 30, Wide(KeyLabel(FirstKey(x < 10 ? a : b))).c_str()); });
    };
    switch (s.kind)
    {
    case Shape::Face:
    case Shape::Opsat:
        KeyIcon(c, FirstKey(s.which == 'a' ? Control::A : s.which == 'b' ? Control::B : s.which == 'x' ? Control::X : Control::Y));
        break;
    case Shape::Back: KeyIcon(c, FirstKey(Control::Back)); break;
    case Shape::Start: KeyIcon(c, FirstKey(Control::Start)); break;
    case Shape::Bumper: KeyIcon(c, FirstKey(s.which == 'l' ? Control::LB : Control::RB)); break;
    case Shape::Trigger: KeyIcon(c, FirstKey(s.which == 'l' ? Control::LT : Control::RT)); break;
    case Shape::Bumpers: Pair([&](REAL x, REAL w) { Cap(c, x, 17, w - 2, 30, Wide(KeyLabel(FirstKey(x < 10 ? Control::LB : Control::RB))).c_str()); }); break;
    case Shape::Triggers: Pair([&](REAL x, REAL w) { Cap(c, x, 17, w - 2, 30, Wide(KeyLabel(FirstKey(x < 10 ? Control::LT : Control::RT))).c_str()); }); break;
    case Shape::Stick:
        if (s.press)
            KeyIcon(c, FirstKey(s.which == 'l' ? Control::LS : Control::RS));
        else if (s.which == 'l')
            dirs(s.dirs, Control::MoveUp, Control::MoveDown, Control::MoveLeft, Control::MoveRight);
        else
            Mouse(c, 0, s.dirs ? s.dirs : Up | Down | Left | Right); // the camera is the mouse
        break;
    case Shape::Dpad: dirs(s.dirs, Control::DpadUp, Control::DpadDown, Control::DpadLeft, Control::DpadRight); break;
    }
}

void StartGdiplus()
{
    static std::once_flag once;
    std::call_once(once, []
    {
        GdiplusStartupInput input;
        ULONG_PTR token;
        GdiplusStartup(&token, &input, nullptr); // kept for the process
    });
}

} // namespace

bool PromptRedraw(int index, PromptStyle style, std::vector<uint32_t>& texels, uint32_t width, uint32_t height)
{
    if (style == PromptStyle::Xbox || index < 0 || index >= int(std::size(kIcons)) || width != height || width == 0 ||
        texels.size() != size_t(width) * height)
        return false;
    StartGdiplus();
    const Shape shape = ParseShape(kIcons[index].name);
    const int size = int(width) * kSuper;
    {
        Canvas canvas(size);
        if (style == PromptStyle::Keyboard)
            DrawKeyboard(canvas, shape);
        else
            DrawPs(canvas, shape, style == PromptStyle::PS4);
        canvas.G().Flush();
        BitmapData data{};
        Rect all(0, 0, size, size);
        if (canvas.Bmp().LockBits(&all, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok)
            return false;
        // Box filter kSuper x kSuper with alpha weighting (straight alpha out).
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++)
            {
                uint32_t r = 0, g = 0, b = 0, a = 0;
                for (int sy = 0; sy < kSuper; sy++)
                {
                    const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(data.Scan0) + (y * kSuper + sy) * data.Stride);
                    for (int sx = 0; sx < kSuper; sx++)
                    {
                        const uint32_t p = row[x * kSuper + sx], pa = p >> 24;
                        a += pa, r += ((p >> 16) & 0xFF) * pa, g += ((p >> 8) & 0xFF) * pa, b += (p & 0xFF) * pa;
                    }
                }
                const uint32_t n = kSuper * kSuper;
                uint32_t out = 0;
                if (a)
                    out = (r / a) | ((g / a) << 8) | ((b / a) << 16) | ((a / n) << 24);
                texels[size_t(y) * width + x] = out;
            }
        canvas.Bmp().UnlockBits(&data);
    }
    return true;
}

// Screens\<lang>\Continue.bmp (loading screen "A Continue" strip, 1280x95 24-bit bottom-up, the
// same in every language): the A button is baked in, a 35 px green disc centred at (101, 35)
// top-down. Erase the disc (each row's background from just left of it) and composite the
// redrawn button_a, sized so its disc matches.
bool PromptPatchContinueBmp(std::vector<uint8_t>& file, PromptStyle style)
{
    if (style == PromptStyle::Xbox || file.size() < 54 || file[0] != 'B' || file[1] != 'M')
        return false;
    auto u32 = [&](size_t o) { uint32_t v; memcpy(&v, &file[o], 4); return v; };
    const uint32_t offset = u32(10), width = u32(18);
    const int32_t height = int32_t(u32(22));
    const uint16_t bpp = uint16_t(file[28] | (file[29] << 8));
    if (width != 1280 || height != 95 || bpp != 24 || u32(30) != 0)
        return false;
    const size_t stride = (size_t(width) * 3 + 3) & ~size_t(3);
    if (offset + stride * height > file.size())
        return false;
    auto px = [&](int x, int y) { return &file[offset + stride * size_t(height - 1 - y) + size_t(x) * 3]; }; // BGR, y top-down
    constexpr int kCx = 101, kCy = 35, kErase = 21, kSize = 48; // disc ~52/64 of an icon
    const uint8_t* c = px(kCx + 8, kCy - 8); // on the disc, off the letter
    if (!(c[1] > c[2] + 40 && c[1] > 110))
        return false; // not the layout we know
    int index = -1;
    for (int i = 0; i < int(std::size(kIcons)); i++)
        if (!strcmp(kIcons[i].name, "button_a"))
            index = i;
    std::vector<uint32_t> icon(size_t(kSize) * kSize);
    if (!PromptRedraw(index, style, icon, kSize, kSize))
        return false;
    for (int y = kCy - kErase; y <= kCy + kErase; y++)
    {
        const uint8_t* bg = px(kCx - kErase - 1, y);
        for (int x = kCx - kErase; x <= kCx + kErase; x++)
            if ((x - kCx) * (x - kCx) + (y - kCy) * (y - kCy) <= kErase * kErase)
                memcpy(px(x, y), bg, 3);
    }
    const int x0 = kCx - kSize / 2, y0 = kCy - kSize / 2;
    for (int y = 0; y < kSize; y++)
        for (int x = 0; x < kSize; x++)
        {
            const uint32_t s = icon[size_t(y) * kSize + x], a = s >> 24;
            uint8_t* d = px(x0 + x, y0 + y);
            const uint32_t rgb[3] = { (s >> 16) & 0xFF, (s >> 8) & 0xFF, s & 0xFF }; // B G R
            for (int k = 0; k < 3; k++)
                d[k] = uint8_t((rgb[k] * a + d[k] * (255 - a) + 127) / 255);
        }
    return true;
}
