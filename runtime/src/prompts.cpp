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
    static const PromptSetting setting = ParsePromptSetting(LoadInputSettings(SettingsPath()).buttonPrompts.c_str());
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

const Color kGreen = Rgb(0x63E39A), kKeyText = Rgb(0x1E1E1E);

// A dark round button (PS face buttons, sticks).
void Disk(Canvas& c, REAL r, uint32_t top = 0x4A4A4A, uint32_t bottom = 0x161616)
{
    const REAL u = c.U(), cx = 32 * u, cy = 32 * u;
    GraphicsPath path;
    path.AddEllipse(cx - r * u, cy - r * u, 2 * r * u, 2 * r * u);
    LinearGradientBrush fill(PointF(0, cy - r * u), PointF(0, cy + r * u), Rgb(top), Rgb(bottom));
    c.G().FillPath(&fill, &path);
    Pen ring(Rgb(0x9A9A9A), 1.6f * u);
    c.G().DrawPath(&ring, &path);
}

void PsSymbol(Canvas& c, char which, REAL cx, REAL cy, REAL r)
{
    const REAL u = c.U();
    cx *= u, cy *= u, r *= u;
    switch (which)
    {
    case 'a': { Pen p(Rgb(0x8AB4FF), 4.2f * u); p.SetStartCap(LineCapRound), p.SetEndCap(LineCapRound);
        c.G().DrawLine(&p, cx - r, cy - r, cx + r, cy + r); c.G().DrawLine(&p, cx - r, cy + r, cx + r, cy - r); break; }
    case 'b': { Pen p(Rgb(0xFF6B6B), 4.2f * u); c.G().DrawEllipse(&p, cx - r * 1.08f, cy - r * 1.08f, r * 2.16f, r * 2.16f); break; }
    case 'x': { Pen p(Rgb(0xF59BD8), 4.2f * u); p.SetLineJoin(LineJoinRound); c.G().DrawRectangle(&p, cx - r, cy - r, 2 * r, 2 * r); break; }
    default: { Pen p(Rgb(0x48D9B0), 4.2f * u); p.SetLineJoin(LineJoinRound);
        PointF t[3] = { { cx, cy - r * 1.15f }, { cx - r * 1.15f, cy + r * 0.85f }, { cx + r * 1.15f, cy + r * 0.85f } };
        c.G().DrawPolygon(&p, t, 3); break; }
    }
}

// A light key cap (keyboard) or grey shoulder/pill (PS) with a centred label.
void Cap(Canvas& c, REAL x, REAL y, REAL w, REAL h, const wchar_t* label, REAL radius = 7, bool light = true)
{
    const REAL u = c.U();
    GraphicsPath path;
    RoundRect(path, x * u, y * u, w * u, h * u, radius * u);
    LinearGradientBrush fill(PointF(0, y * u), PointF(0, (y + h) * u), light ? Rgb(0xF2F2F2) : Rgb(0x5A5A5A), light ? Rgb(0xB4B4B4) : Rgb(0x262626));
    c.G().FillPath(&fill, &path);
    Pen border(light ? Rgb(0x2E2E2E) : Rgb(0xA0A0A0), 2.0f * u);
    c.G().DrawPath(&border, &path);
    if (label)
        c.Text(label, (x + w / 2) * u, (y + h / 2) * u, (w - 8) * u, (h - 10) * u, light ? kKeyText : Rgb(0xF0F0F0));
}

// Arrow key cap: one arrow, or all four for the plain d-pad.
void ArrowCap(Canvas& c, REAL x, REAL y, REAL w, REAL h, int dirs)
{
    Cap(c, x, y, w, h, nullptr);
    const REAL u = c.U(), cx = (x + w / 2) * u, cy = (y + h / 2) * u, s = std::min(w, h) * u;
    const Color dark = kKeyText;
    if (dirs == 0)
    {
        Arrow(c, cx, cy - s * 0.32f, Up, s * 0.11f, dark);
        Arrow(c, cx, cy + s * 0.32f, Down, s * 0.11f, dark);
        Arrow(c, cx - s * 0.32f, cy, Left, s * 0.11f, dark);
        Arrow(c, cx + s * 0.32f, cy, Right, s * 0.11f, dark);
        return;
    }
    const REAL a = s * 0.2f;
    if (dirs == Up) Arrow(c, cx, cy - a, Up, a, dark);
    if (dirs == Down) Arrow(c, cx, cy + a, Down, a, dark);
    if (dirs == Left) Arrow(c, cx - a, cy, Left, a, dark);
    if (dirs == Right) Arrow(c, cx + a, cy, Right, a, dark);
}

// Two caps side by side (pairs: L1 R1, W S, ...).
template <typename F>
void Pair(F&& draw)
{
    draw(2.0f, 30.0f);
    draw(34.0f, 30.0f);
}

// A mouse: buttons lit per mask (1 left, 2 right, 4 middle), optional move arrows around it.
void Mouse(Canvas& c, int lit, int dirs)
{
    const REAL u = c.U(), x = dirs ? 20 : 16, y = dirs ? 14 : 6, w = dirs ? 24 : 32, h = dirs ? 36 : 52;
    GraphicsPath body;
    RoundRect(body, x * u, y * u, w * u, h * u, (w / 2) * u);
    LinearGradientBrush fill(PointF(0, y * u), PointF(0, (y + h) * u), Rgb(0xF2F2F2), Rgb(0xB4B4B4));
    c.G().FillPath(&fill, &body);
    const REAL split = y + h * 0.42f;
    Region clip(&body);
    c.G().SetClip(&clip);
    SolidBrush on(kGreen);
    if (lit & 1) c.G().FillRectangle(&on, x * u, y * u, (w / 2) * u, (split - y) * u);
    if (lit & 2) c.G().FillRectangle(&on, (x + w / 2) * u, y * u, (w / 2) * u, (split - y) * u);
    c.G().ResetClip();
    Pen line(Rgb(0x2E2E2E), 1.8f * u);
    c.G().DrawLine(&line, x * u, split * u, (x + w) * u, split * u);
    c.G().DrawLine(&line, (x + w / 2) * u, y * u, (x + w / 2) * u, split * u);
    GraphicsPath wheel;
    RoundRect(wheel, (x + w / 2 - 3) * u, (y + h * 0.12f) * u, 6 * u, h * 0.2f * u, 3 * u);
    SolidBrush wheelFill(lit & 4 ? kGreen : Rgb(0x5A5A5A));
    c.G().FillPath(&wheelFill, &wheel);
    Pen border(Rgb(0x2E2E2E), 2.0f * u);
    c.G().DrawPath(&border, &body);
    const REAL cx = 32 * u, cy = (y + h / 2) * u, a = 5 * u;
    if (dirs & Up) Arrow(c, cx, (y - 2) * u, Up, a, kGreen);
    if (dirs & Down) Arrow(c, cx, (y + h + 2) * u, Down, a, kGreen);
    if (dirs & Left) Arrow(c, (x - 3) * u, cy, Left, a, kGreen);
    if (dirs & Right) Arrow(c, (x + w + 3) * u, cy, Right, a, kGreen);
}

void PsStick(Canvas& c, const Shape& s)
{
    Disk(c, 21, 0x6A6A6A, 0x2A2A2A);
    const REAL u = c.U();
    Pen inner(Rgb(0x8A8A8A), 1.2f * u);
    c.G().DrawEllipse(&inner, 17 * u, 17 * u, 30 * u, 30 * u);
    const wchar_t* label = s.which == 'l' ? (s.press ? L"L3" : L"L") : (s.press ? L"R3" : L"R");
    c.Text(label, 32 * u, 32.5f * u, 22 * u, 20 * u, Rgb(0xF0F0F0));
    const REAL a = 5.5f * u;
    if (s.press)
    {
        Arrow(c, 32 * u, 16 * u, Down, a, kGreen), Arrow(c, 32 * u, 48 * u, Up, a, kGreen);
        Arrow(c, 16 * u, 32 * u, Right, a, kGreen), Arrow(c, 48 * u, 32 * u, Left, a, kGreen);
        return;
    }
    if (s.dirs & Up) Arrow(c, 32 * u, 5 * u, Up, a, kGreen);
    if (s.dirs & Down) Arrow(c, 32 * u, 59 * u, Down, a, kGreen);
    if (s.dirs & Left) Arrow(c, 5 * u, 32 * u, Left, a, kGreen);
    if (s.dirs & Right) Arrow(c, 59 * u, 32 * u, Right, a, kGreen);
}

// PlayStation d-pad: four separate arrow-shaped keys, the named directions lit.
void PsDpad(Canvas& c, int dirs)
{
    const REAL u = c.U();
    auto key = [&](int dir)
    {
        // An arrow key pointing outward: a 14-wide stem from the centre with a pointed tip.
        PointF p[5] = { { -7, -9 }, { 7, -9 }, { 7, -24 }, { 0, -30 }, { -7, -24 } };
        for (auto& q : p)
        {
            REAL x = q.X, y = q.Y;
            if (dir == Down) y = -y;
            if (dir == Left) std::swap(x, y);
            if (dir == Right) std::swap(x, y), x = -x;
            q = { (32 + x) * u, (32 + y) * u };
        }
        LinearGradientBrush fill(PointF(0, 2 * u), PointF(0, 62 * u), (dirs & dir) ? kGreen : Rgb(0x6A6A6A), (dirs & dir) ? Rgb(0x2E9A5E) : Rgb(0x2A2A2A));
        c.G().FillPolygon(&fill, p, 5);
        Pen border(Rgb(0xA0A0A0), 1.6f * u);
        border.SetLineJoin(LineJoinRound);
        c.G().DrawPolygon(&border, p, 5);
    };
    key(Up), key(Down), key(Left), key(Right);
}

void DrawPs(Canvas& c, const Shape& s, bool ps4)
{
    switch (s.kind)
    {
    case Shape::Face: Disk(c, 23); PsSymbol(c, s.which, 32, 32, 9); break;
    case Shape::Back: Cap(c, 4, 19, 56, 26, ps4 ? L"SHARE" : L"CREATE", 13, false); break;
    case Shape::Start: Cap(c, 4, 19, 56, 26, L"OPTIONS", 13, false); break;
    case Shape::Bumper: Cap(c, 6, 17, 52, 30, s.which == 'l' ? L"L1" : L"R1", 10, false); break;
    case Shape::Trigger: Cap(c, 10, 6, 44, 52, s.which == 'l' ? L"L2" : L"R2", 14, false); break;
    case Shape::Bumpers: Pair([&](REAL x, REAL w) { Cap(c, x, 19, w - 2, 26, x < 10 ? L"L1" : L"R1", 9, false); }); break;
    case Shape::Triggers: Pair([&](REAL x, REAL w) { Cap(c, x, 12, w - 2, 40, x < 10 ? L"L2" : L"R2", 11, false); }); break;
    case Shape::Stick: PsStick(c, s); break;
    case Shape::Dpad: PsDpad(c, s.dirs); break;
    case Shape::Opsat:
    {
        // The OPSAT screen's own look: dark green disk, green ring and symbol.
        const REAL u = c.U();
        SolidBrush fill(Rgb(0x14261E));
        c.G().FillEllipse(&fill, 3 * u, 3 * u, 58 * u, 58 * u);
        Pen ring(Rgb(0x6CD39A), 4 * u);
        c.G().DrawEllipse(&ring, 4 * u, 4 * u, 56 * u, 56 * u);
        PsSymbol(c, s.which, 32, 32, 13);
        break;
    }
    }
}

void DrawKeyboard(Canvas& c, const Shape& s)
{
    auto letters = [&](int dirs, const wchar_t* u, const wchar_t* d, const wchar_t* l, const wchar_t* r, const wchar_t* all)
    {
        const int n = !!(dirs & Up) + !!(dirs & Down) + !!(dirs & Left) + !!(dirs & Right);
        if (n == 0)
            Cap(c, 2, 16, 60, 32, all);
        else if (n == 1)
            Cap(c, 10, 10, 44, 44, dirs == Up ? u : dirs == Down ? d : dirs == Left ? l : r);
        else
        {
            const wchar_t* a = dirs & Up ? u : l;
            const wchar_t* b = dirs & Down ? d : r;
            Pair([&](REAL x, REAL w) { Cap(c, x, 17, w - 2, 30, x < 10 ? a : b); });
        }
    };
    switch (s.kind)
    {
    case Shape::Face:
        if (s.which == 'a') Cap(c, 1, 17, 62, 30, L"SPACE");
        else if (s.which == 'b') Cap(c, 4, 17, 56, 30, L"CTRL");
        else Cap(c, 10, 10, 44, 44, s.which == 'x' ? L"E" : L"Q");
        break;
    case Shape::Back: Cap(c, 6, 17, 52, 30, L"TAB"); break;
    case Shape::Start: Cap(c, 6, 17, 52, 30, L"ESC"); break;
    case Shape::Bumper: Cap(c, 10, 10, 44, 44, s.which == 'l' ? L"1" : L"3"); break;
    case Shape::Bumpers: Pair([&](REAL x, REAL w) { Cap(c, x, 17, w - 2, 30, x < 10 ? L"1" : L"3"); }); break;
    case Shape::Trigger: Mouse(c, s.which == 'l' ? 2 : 1, 0); break;
    case Shape::Triggers: Mouse(c, 3, 0); break;
    case Shape::Stick:
        if (s.which == 'l')
        {
            if (s.press)
                Cap(c, 10, 10, 44, 44, L"F");
            else
                letters(s.dirs, L"W", L"S", L"A", L"D", L"WASD");
        }
        else
            Mouse(c, s.press ? 4 : 0, s.press ? 0 : (s.dirs ? s.dirs : Up | Down | Left | Right));
        break;
    case Shape::Dpad:
    {
        const int n = !!(s.dirs & Up) + !!(s.dirs & Down) + !!(s.dirs & Left) + !!(s.dirs & Right);
        if (n <= 1)
            ArrowCap(c, 10, 10, 44, 44, s.dirs);
        else
        {
            const int a = s.dirs & Up ? Up : Left, b = s.dirs & Down ? Down : Right;
            Pair([&](REAL x, REAL w) { ArrowCap(c, x, 17, w - 2, 30, x < 10 ? a : b); });
        }
        break;
    }
    case Shape::Opsat:
    {
        const REAL u = c.U();
        SolidBrush fill(Rgb(0x14261E));
        c.G().FillEllipse(&fill, 3 * u, 3 * u, 58 * u, 58 * u);
        Pen ring(Rgb(0x6CD39A), 4 * u);
        c.G().DrawEllipse(&ring, 4 * u, 4 * u, 56 * u, 56 * u);
        c.Text(s.which == 'a' ? L"SPACE" : L"CTRL", 32 * u, 32 * u, 46 * u, 24 * u, Rgb(0x8DE0B0));
        break;
    }
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
