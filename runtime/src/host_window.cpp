#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <gpu/d3d11_renderer.h>
#include <gpu_state.h>
#include <gpu/draw_state.h>
#include <gpu/shader_compiler.h>
#include <host_window.h>
#include <input.h>
#include <overlay.h>
#include <runtime.h>

static std::atomic<HWND> g_hwnd;
std::atomic<HANDLE> g_titleGpuThread{ nullptr }; // the title thread that calls VdSwap ([perf] CPU time; video.cpp)
static std::atomic<uint32_t> g_resizeW, g_resizeH;
static bool g_rawMouse;    // raw mouse input registered (every run but scripted ones)
static bool g_cursorHeld;  // cursor hidden and clipped to the client area now
static int g_absX = INT_MIN, g_absY; // last absolute raw position (INT_MIN: none yet)

// Mouse look on: capture the cursor and feed raw motion to the right stick. Follows the F1 setting live.
static bool MouseLookOn() { return g_rawMouse && CurrentInputSettings().mouseLook; }
static std::atomic<bool> g_dumpKeyRequest{ false }; // F8 (SCDA_DEBUG_KEYS): dump the next frame

// Mouse look while the window is in the foreground: the cursor is hidden and kept inside the
// client area; Alt+Tab (focus loss) releases it.
static void HoldCursor(HWND hwnd, bool hold)
{
    if (hold)
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&rc), 2);
        ClipCursor(&rc);
    }
    else
        ClipCursor(nullptr);
    if (hold != g_cursorHeld)
        ShowCursor(hold ? FALSE : TRUE);
    g_cursorHeld = hold;
}

constexpr UINT kSetModeMessage = WM_APP + 3; // wp = DisplaySettings::Mode
static std::atomic<int> g_windowMode{-1};      // the mode the window is in (-1 before it exists)
static RECT g_windowedRect;                    // window rect to return to from full screen

// Borderless full screen covers the monitor the window is on; windowed restores the last windowed rect.
static void ApplyWindowMode(HWND hwnd, DisplaySettings::Mode mode)
{
    const bool full = mode != DisplaySettings::Windowed;
    if (g_windowMode.load() == int(DisplaySettings::Windowed) && full)
        GetWindowRect(hwnd, &g_windowedRect);
    g_windowMode = int(mode);
    SetWindowLongW(hwnd, GWL_STYLE, (full ? WS_POPUP : WS_OVERLAPPEDWINDOW) | WS_VISIBLE);
    RECT r = g_windowedRect;
    if (full)
    {
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        r = mi.rcMonitor;
    }
    SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

void HostWindowSetMode(DisplaySettings::Mode mode)
{
    if (HWND hwnd = g_hwnd.load(); hwnd && g_windowMode.load() != int(mode) && g_windowMode.load() >= 0)
        PostMessageW(hwnd, kSetModeMessage, WPARAM(mode), 0);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == kSetModeMessage)
    {
        if (g_windowMode.load() != int(wp))
            ApplyWindowMode(hwnd, DisplaySettings::Mode(wp));
        return 0;
    }
    // F1 or L3+R3 (kOverlayToggleMessage) opens and closes the Extra Settings menu (Esc closes it too);
    // while it is open the cursor is free and mouse/keyboard messages go to the menu, not the game.
    // In the front end (title screen, main menus) they only close it: settings are the main menu's
    // RECOMP SETTINGS page there (main_menu.cpp). SCDA_MENU_ANYWHERE=1 lifts that.
    static const bool anywhere = getenv("SCDA_MENU_ANYWHERE") != nullptr;
    if (((msg == WM_KEYDOWN && !(lp & (1 << 30)) && (wp == VK_F1 || (wp == VK_ESCAPE && OverlayOpen() && !OverlayCapturingKey()))) || msg == kOverlayToggleMessage ||
         (msg == kOverlayOpenMessage && !OverlayOpen())) &&
        (OverlayOpen() || msg == kOverlayOpenMessage || anywhere || !GuestInFrontEnd()))
    {
        OverlayToggle();
        if (g_rawMouse)
            HoldCursor(hwnd, MouseLookOn() && !OverlayWantsCursor() && GetForegroundWindow() == hwnd);
        return 0;
    }
    // SCDA_DEBUG_KEYS=1: F9 occlusion queries all visible, F11 culling off (F10 is a system key) (draw_state.h), toggled live;
    // F8 dumps the next frame (scda_frame_<n>.bmp; with SCDA_TRACE_DUMPED=1 also its full draw trace).
    static const bool debugKeys = getenv("SCDA_DEBUG_KEYS") != nullptr;
    if (debugKeys && msg == WM_KEYDOWN && !(lp & (1 << 30)) && wp == VK_F8)
    {
        g_dumpKeyRequest = true;
        RuntimeLog("[debug] F8: dumping the next frame\n");
        return 0;
    }
    if (debugKeys && msg == WM_KEYDOWN && !(lp & (1 << 30)) && (wp == VK_F9 || wp == VK_F11))
    {
        std::atomic<bool>& flag = wp == VK_F9 ? g_debugQueriesVisible : g_debugNoCull;
        flag = !flag;
        RuntimeLog("[debug] %s: %s\n", wp == VK_F9 ? "queries all visible (F9)" : "culling off (F11)", flag ? "on" : "off");
        return 0;
    }
    // The clickable update notification came or went: free the cursor for it, or hold it again.
    if (msg == kOverlayCursorMessage)
    {
        if (g_rawMouse)
            HoldCursor(hwnd, MouseLookOn() && !OverlayWantsCursor() && GetForegroundWindow() == hwnd);
        return 0;
    }
    if (OverlayWndProc(hwnd, msg, wp, lp))
        return 0;
    switch (msg)
    {
    case WM_SIZE:
        g_resizeH = HIWORD(lp);
        g_resizeW = LOWORD(lp);
        if (g_cursorHeld)
            HoldCursor(hwnd, true);
        return 0;
    case WM_MOVE:
        if (g_cursorHeld)
            HoldCursor(hwnd, true);
        return 0;
    case WM_DPICHANGED:
        // Moved to a monitor with another scale: windowed takes the size Windows suggests, borderless
        // covers the new monitor.
        if (g_windowMode.load() == int(DisplaySettings::Windowed))
        {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        else if (g_windowMode.load() >= 0)
            ApplyWindowMode(hwnd, DisplaySettings::Mode(g_windowMode.load()));
        return 0;
    case WM_ACTIVATE:
        g_absX = INT_MIN; // an absolute pointer re-enters anywhere: no jump on the first motion
        if (g_rawMouse)
            HoldCursor(hwnd, MouseLookOn() && LOWORD(wp) != WA_INACTIVE && !OverlayWantsCursor());
        break; // DefWindowProc gives the window keyboard focus
    case WM_INPUT:
    {
        RAWINPUT raw;
        UINT size = sizeof(raw);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
            raw.header.dwType == RIM_TYPEMOUSE && (raw.data.mouse.lLastX || raw.data.mouse.lLastY))
        {
            const RAWMOUSE& m = raw.data.mouse;
            const bool absolute = (m.usFlags & MOUSE_MOVE_ABSOLUTE) != 0;
            MouseEvent(absolute ? MouseEventKind::RawAbsolute : MouseEventKind::RawRelative);
            static bool logged[2];
            if (!logged[absolute])
                logged[absolute] = true, RuntimeLog("[input] mouse motion: %s\n", absolute ? "absolute (remote desktop, streaming, VM or tablet)" : "relative");
            if (absolute)
            {
                // Remote Desktop, game streaming (Parsec, Moonlight), VMs and pen tablets send positions
                // (0-65535 over the screen), not motion: the change since the last one is the motion.
                const bool virt = (m.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
                const int x = MulDiv(m.lLastX, GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN), 65535);
                const int y = MulDiv(m.lLastY, GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN), 65535);
                if (g_absX != INT_MIN)
                    MouseAccumulate(x - g_absX, y - g_absY, 0);
                g_absX = x, g_absY = y;
            }
            else
                MouseAccumulate(m.lLastX, m.lLastY, 0);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    case WM_MOUSEMOVE: // counted only: the mouse summary in tcscc.log compares it with raw input
        MouseEvent(MouseEventKind::WindowMove);
        break;
    case WM_MOUSEWHEEL:
        MouseAccumulate(0, 0, GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA);
        return 0;
    case WM_CLOSE:
        // Guest threads have no shutdown path: end the process.
        RuntimeLog("[window] closed by user\n");
        ExitProcess(0);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void WindowThread(DisplaySettings s)
{
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)) /* IDC_ARROW */;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"TCSCCRecompWindow";
    RegisterClassExW(&wc);

    DWORD style = s.mode == DisplaySettings::Windowed ? WS_OVERLAPPEDWINDOW : WS_POPUP;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT frame{ 0, 0, 0, 0 };
    AdjustWindowRect(&frame, style, FALSE);
    auto [cw, ch] = WindowClientSize(uint32_t(work.right - work.left - (frame.right - frame.left)),
                                     uint32_t(work.bottom - work.top - (frame.bottom - frame.top)), GetDpiForSystem());
    // SCDA_WINDOW_SIZE=<w>x<h>: client size of a windowed run (aspect tests, e.g. 1680x720 = 21:9).
    if (const char* ws = getenv("SCDA_WINDOW_SIZE"); ws && s.mode == DisplaySettings::Windowed)
        if (unsigned w = 0, h = 0; sscanf(ws, "%ux%u", &w, &h) == 2 && w >= 320 && h >= 180)
            cw = w, ch = h;
    RECT r{ 0, 0, LONG(cw), LONG(ch) };
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (s.mode != DisplaySettings::Windowed)
    {
        r = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        x = y = 0;
    }
    else
        AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Splinter Cell: Conviction Recomp", style, x, y,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    // SCDA_START_MINIMIZED=1 (test runs): start minimized without taking focus. The swap chain keeps
    // its created size (WM_SIZE 0x0 is ignored), so the game still renders and [perf] stays valid.
    ShowWindow(hwnd, getenv("SCDA_START_MINIMIZED") ? SW_SHOWMINNOACTIVE : SW_SHOW);
    if (s.mode == DisplaySettings::Windowed)
        GetWindowRect(hwnd, &g_windowedRect);
    else
    {
        // Windowed rect to come back to: the default windowed size, centred in the work area.
        RECT w{ 0, 0, LONG(cw), LONG(ch) };
        AdjustWindowRect(&w, WS_OVERLAPPEDWINDOW, FALSE);
        const LONG ww = w.right - w.left, wh = w.bottom - w.top;
        g_windowedRect = { work.left + (work.right - work.left - ww) / 2, work.top + (work.bottom - work.top - wh) / 2, 0, 0 };
        g_windowedRect.right = g_windowedRect.left + ww;
        g_windowedRect.bottom = g_windowedRect.top + wh;
    }
    g_windowMode = int(s.mode);
    // Scripted runs (SCDA_AUTOPRESS) never take the mouse: an unattended window must not trap the cursor.
    // Raw mouse input is always registered, so turning Mouse look on in F1 works without a restart.
    if (!getenv("SCDA_AUTOPRESS") || getenv("SCDA_AUTOPRESS_MOUSE"))
    {
        RAWINPUTDEVICE rid{ 0x01 /* generic desktop */, 0x02 /* mouse */, 0, hwnd };
        g_rawMouse = RegisterRawInputDevices(&rid, 1, sizeof(rid)) != FALSE;
        RuntimeLog("[input] mouse look %s, raw mouse input %s (error %lu)\n", CurrentInputSettings().mouseLook ? "on" : "off",
            g_rawMouse ? "registered" : "FAILED", g_rawMouse ? 0ul : GetLastError());
        if (MouseLookOn() && GetForegroundWindow() == hwnd)
            HoldCursor(hwnd, true);
    }
    g_hwnd = hwnd;
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0))
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

static void RenderThread(DisplaySettings s)
{
    RuntimeLog("[thread] renderer host %lu\n", GetCurrentThreadId());
    while (!g_hwnd.load())
        Sleep(1);
    HWND hwnd = g_hwnd.load();
    RECT rc;
    GetClientRect(hwnd, &rc);
    // Effective display settings, so a user's log shows what they ran with.
    static const char* kModes[] = { "windowed", "borderless", "fullscreen" };
    RuntimeLog("[settings] window %ldx%ld %s, FrameRate %u, VSync %d, Scale %g, Upscaling %s, AA %s, DrawDistance %s, Anisotropy %u\n",
        rc.right, rc.bottom, kModes[s.mode], s.frameRate, s.vsync ? 1 : 0, double(s.scale), UpscalingName(s.upscaling),
        AntiAliasingName(s.antiAliasing), DrawDistanceName(s.drawDistance), s.anisotropy);
    D3D11Renderer renderer;
    renderer.SetCleanGoggles(s.cleanGoggles);
    renderer.SetUltrawide(s.ultrawide);
    if (!renderer.Init(hwnd, rc.right, rc.bottom, s.vsync, UpscalingScale(s.upscaling, uint32_t(rc.bottom), s.scale), s.anisotropy))
    {
        RuntimeLog("[window] no Direct3D 11 device\n");
        return;
    }
    OverlayAttach(renderer, hwnd, s);
    Frame frame;
    DWORD lastTitle = 0;
    for (;;)
    {
        if (uint32_t w = g_resizeW.exchange(0))
            renderer.Resize(w, g_resizeH.load());
        if (!Frames().PopFrame(frame))
        {
            // The title may be waiting for a query result before it swaps: run the queries it has
            // issued so far (partial frame), then collect results.
            const bool flushed = Frames().FlushOpenIfWaiting();
            if (!flushed)
                renderer.WaitMemExport();
            renderer.PollQueries();
            if (flushed)
                continue; // the partial is queued: run it now
            // SCDA_IDLE_SLEEP=1: old idle loop (Sleep(1) between polls).
            static const bool oldIdle = getenv("SCDA_IDLE_SLEEP") != nullptr;
            if (oldIdle)
                Sleep(1);
            else if (renderer.HasPendingFences())
                SwitchToThread(); // a fence waits on host query results: collect them as soon as they land
            // shortcut: spins a core until the query answers (a lost one: up to the 2 s fallback).
            else
                Frames().WaitForFrame(std::chrono::microseconds(500));
            continue;
        }
        // SCDA_TRACE_FRAME=<n>: log every draw and resolve of the first frame >= n (renderer development).
        static const long long traceFrame = getenv("SCDA_TRACE_FRAME") ? atoll(getenv("SCDA_TRACE_FRAME")) : -1;
        // First frame at or after n: the renderer may drop frame n itself.
        // Every Frame of that guest frame: its partial Frames hold most draws (each replays its log on
        // the renderer's state after the previous Frame).
        static long long tracedFrame = -1;
        if (tracedFrame < 0 && traceFrame >= 0 && (long long)frame.stats.frame >= traceFrame)
            tracedFrame = (long long)frame.stats.frame;
        const bool tracing = tracedFrame >= 0 && (long long)frame.stats.frame == tracedFrame;
        if (tracing)
        {
            // Register state per draw: the renderer's (end of the last frame) plus this frame's log.
            RegisterFile regs = renderer.Registers();
            size_t applied = 0;
            auto replay = [&](uint32_t end)
            {
                for (; applied < end && applied < frame.regWrites.size(); applied++)
                    if (frame.regWrites[applied].index == kRegClearAll)
                        regs.Clear();
                    else
                        regs.Set(frame.regWrites[applied].index, frame.regWrites[applied].value);
            };
            for (auto& d : frame.draws)
            {
                replay(d.regWritesEnd);
                RuntimeLog("[frame] draw prim %u src %u idx32 %d count %u base 0x%08X vs %016llx ps %016llx rt 0x%08X/0x%08X mode %u zctl 0x%08X zinfo 0x%08X\n",
                    d.primType, uint32_t(d.source), d.index32, d.indexCount, d.indexBase,
                    (unsigned long long)(d.vs ? d.vs->hash : 0), (unsigned long long)(d.ps ? d.ps->hash : 0),
                    regs[kRegRbSurfaceInfo], regs[kRegRbColorInfo], regs[kRegRbModeControl] & 7, regs[0x2200], regs[kRegRbDepthInfo]);
            }
            regs = renderer.Registers();
            applied = 0;
            for (auto& d : frame.draws)
            {
                replay(d.regWritesEnd);
                const RegisterFile& r = regs;
                RuntimeLog("[frame]  vte 0x%08X vp %08X %08X %08X %08X %08X %08X winoff 0x%08X scis 0x%08X 0x%08X blend 0x%08X mask 0x%08X\n",
                    r[0x2206], r[0x210F], r[0x2110], r[0x2111], r[0x2112], r[0x2113], r[0x2114], r[0x2080], r[0x2081], r[0x2082], r[0x2201], r[0x2104]);
                for (uint32_t i = 0; i < 18; i += 6) // fetch dwords 0-17: texture slots 0-2 / vertex slots 0-8
                    RuntimeLog("[frame]  fetch %2u: %08X %08X %08X %08X %08X %08X\n", i, r[0x4800 + i], r[0x4801 + i], r[0x4802 + i],
                        r[0x4803 + i], r[0x4804 + i], r[0x4805 + i]);
                if (d.vs)
                {
                    std::bitset<96> slots = ShaderVertexFetchSlots(*d.vs);
                    for (uint32_t slot = 0; slot < 96; slot++)
                    {
                        MemoryRange m;
                        if (slots[slot] && VertexFetchRange(r, slot, m))
                            RuntimeLog("[frame]  vertex slot %u: 0x%08X + %u\n", slot, m.physical, m.bytes);
                    }
                    for (uint32_t s : d.memory)
                        RuntimeLog("[frame]  snapshot %u\n", s);
                }
                for (uint32_t c = 20; c < 30; c++) // VS constants of the UI shaders (glyph rects)
                {
                    auto f = [&](uint32_t i) { return std::bit_cast<float>(r[0x4000 + c * 4 + i]); };
                    RuntimeLog("[frame]  c%u: %g %g %g %g\n", c, f(0), f(1), f(2), f(3));
                }
            }
            for (auto& r : frame.resolves)
                RuntimeLog("[frame] resolve @draw %u ctrl 0x%08X dest 0x%08X pitch 0x%08X info 0x%08X surf 0x%08X color 0x%08X depth 0x%08X clear 0x%08X zclear 0x%08X rect %d %d %d %d (%d)\n",
                    r.drawIndex, r.copyControl, r.destBase, r.destPitch, r.destInfo, r.surfaceInfo, r.colorInfo, r.depthInfo, r.colorClear, r.depthClear,
                    r.rect[0], r.rect[1], r.rect[2], r.rect[3], int(r.hasRect));
            RuntimeLog("[frame] frontbuffer 0x%08X\n", frame.frontbuffer);
        }
        // SCDA_DUMP_FRAME=<n>[,<n>...] (ascending): write the presented image of the first frame at or
        // after each n (the renderer may drop frames) to scda_frame_<frame>.bmp.
        static std::vector<uint64_t> dumpFrames = []
        {
            std::vector<uint64_t> v;
            for (const char* p = getenv("SCDA_DUMP_FRAME"); p && *p; p += strcspn(p, ",") + (p[strcspn(p, ",")] ? 1 : 0))
                v.push_back(strtoull(p, nullptr, 10));
            return v;
        }();
        static size_t nextDump = 0;
        // Partial Frames of the dumped guest frame arm the dump too (their resolves and draws feed
        // SCDA_DUMP_RESOLVES / SCDA_DUMP_DRAWS); the image is written by the rest of the frame.
        bool dumping = (nextDump < dumpFrames.size() && frame.stats.frame >= dumpFrames[nextDump]) || g_dumpKeyRequest;
        if (dumping)
        {
            renderer.DumpNextFrame("scda_frame_" + std::to_string(frame.stats.frame) + ".bmp");
            // SCDA_DUMP_BACKBUFFER=1: also the window image (post-processing, menu): scda_window_<n>.bmp.
            static const bool backBuffer = getenv("SCDA_DUMP_BACKBUFFER") != nullptr;
            if (backBuffer && !frame.partial)
                renderer.DumpNextBackBuffer("scda_window_" + std::to_string(frame.stats.frame) + ".bmp");
            if (!frame.partial)
                g_dumpKeyRequest = false;
            if (!frame.partial)
                while (nextDump < dumpFrames.size() && dumpFrames[nextDump] <= frame.stats.frame)
                    nextDump++;
        }
        dumping &= !frame.partial;
        // SCDA_DUMP_RAM=<n>[,<n>...] (ascending): at the first frame at or after each n, write every committed
        // guest page range to ramdump_<frame>.bin as records {u32 guest addr, u32 size (LE), bytes}. Racy
        // (guest threads keep running); good enough for value scans.
        static std::vector<uint64_t> ramFrames = []
        {
            std::vector<uint64_t> v;
            for (const char* p = getenv("SCDA_DUMP_RAM"); p && *p; p += strcspn(p, ",") + (p[strcspn(p, ",")] ? 1 : 0))
                v.push_back(strtoull(p, nullptr, 10));
            return v;
        }();
        static size_t nextRam = 0;
        if (!frame.partial && nextRam < ramFrames.size() && frame.stats.frame >= ramFrames[nextRam])
        {
            while (nextRam < ramFrames.size() && ramFrames[nextRam] <= frame.stats.frame)
                nextRam++;
            const std::string name = "ramdump_" + std::to_string(frame.stats.frame) + ".bin";
            if (FILE* f = fopen(name.c_str(), "wb"))
            {
                uint64_t total = 0;
                for (uint64_t a = 0; a < 0x100000000ull;)
                {
                    MEMORY_BASIC_INFORMATION mbi;
                    if (!VirtualQuery(g_base + a, &mbi, sizeof(mbi)))
                        break;
                    const uint64_t start = uint64_t((uint8_t*)mbi.BaseAddress - g_base);
                    uint64_t end = start + mbi.RegionSize;
                    if (end > 0x100000000ull)
                        end = 0x100000000ull;
                    if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                        for (uint64_t s = start; s < end; s += 0x1000000) // 16 MB records
                        {
                            const uint32_t addr = uint32_t(s), size = uint32_t(std::min<uint64_t>(end - s, 0x1000000));
                            fwrite(&addr, 4, 1, f);
                            fwrite(&size, 4, 1, f);
                            fwrite(g_base + s, 1, size, f);
                            total += size;
                        }
                    a = end;
                }
                fclose(f);
                RuntimeLog("[ramdump] %s: %llu MB\n", name.c_str(), (unsigned long long)(total >> 20));
            }
        }
        // Renderer busy time (Present = execute + present), reported on [frame] lines since the last one.
        static LARGE_INTEGER qpf = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
        static uint64_t busyTicks = 0, busyDraws = 0, busyFrames = 0;
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        TimelineEvent('R', frame.partial, uint32_t(frame.draws.size()));
        renderer.Present(frame);
        TimelineEvent('r', frame.partial);
        QueryPerformanceCounter(&t1);
        busyTicks += t1.QuadPart - t0.QuadPart;
        // A frame (or partial) that took over 100 ms to render: log it with the shader counters, so a
        // reported stutter can be matched to compiles (or ruled out).
        if (const double ms = 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(qpf.QuadPart); ms > 100.0)
            RuntimeLog("[hitch] frame %llu render %.0f ms: shaders compiled %u failed %u pending %u, draws %u\n",
                (unsigned long long)frame.stats.frame, ms, ShaderCompilerCompileCount(), ShaderCompilerFailedCount(),
                ShaderCompilerPendingCount(), frame.stats.draws);
        busyDraws += renderer.LastDrawn();
        // [perf] renderer time over every Frame, partials included (most draws of a guest frame run
        // in its partials: the whole-frame "render" figure misses them).
        static LONGLONG perfAllTicks = 0;
        static uint32_t perfPartials = 0;
        perfAllTicks += t1.QuadPart - t0.QuadPart;
        perfPartials += frame.partial;
        if (frame.partial)
            continue;
        busyFrames++;
        static uint64_t presents = 0;
        static const ULONGLONG startTick = GetTickCount64();
        presents++;
        // [perf] every 30 s (always on): fps, gaps between presents (a 30 fps cap that misses a guest
        // vblank shows as 50 ms gaps), renderer time per frame, guest time blocked on GPU fences.
        {
            static LONGLONG perfStart = t1.QuadPart, lastPresent = 0, renderTicks = 0, renderMax = 0;
            static uint32_t frames = 0, gapMaxUs = 0, gaps40 = 0, gaps60 = 0;
            renderTicks += t1.QuadPart - t0.QuadPart; // whole frame only (partials: "renderer busy")
            renderMax = std::max<LONGLONG>(renderMax, t1.QuadPart - t0.QuadPart);
            if (lastPresent)
            {
                const uint32_t gapUs = uint32_t((t1.QuadPart - lastPresent) * 1000000 / qpf.QuadPart);
                gapMaxUs = std::max(gapMaxUs, gapUs);
                gaps40 += gapUs > 40000;
                gaps60 += gapUs > 60000;
            }
            lastPresent = t1.QuadPart;
            frames++;
            if (const double secs = double(t1.QuadPart - perfStart) / double(qpf.QuadPart); secs >= 30.0)
            {
                const double tickMs = 1000.0 / double(qpf.QuadPart);
                RuntimeLog("[perf] %.1f fps, frames over 40 ms %u, over 60 ms %u, longest %.1f ms; render %.1f ms/frame (max %.1f); guest GPU wait %.0f ms/s\n",
                    frames / secs, gaps40, gaps60, gapMaxUs / 1000.0, double(renderTicks) * tickMs / frames, double(renderMax) * tickMs,
                    double(GuestGpuWaitUsTake()) / 1000.0 / secs);
                RuntimeLog("[perf] renderer busy %.0f ms/s (partials included), partials %.1f/frame, GPU readback stalls %.0f ms/s, guest query wait %.0f ms/s\n",
                    double(perfAllTicks) * tickMs / secs, double(perfPartials) / frames, double(renderer.ReadbackUsTake()) / 1000.0 / secs,
                    double(GuestQueryWaitUsTake()) / 1000.0 / secs);
                {
                    // Where the renderer's time goes (ms per frame): all draws, of which state bind / vertex
                    // upload / draw calls; resolves; swap-chain Present.
                    auto ms = [&](D3D11Renderer::Phase p) { return double(renderer.PhaseTicksTake(p)) * tickMs / frames; };
                    const double draws = ms(D3D11Renderer::kPhaseDraws), bind = ms(D3D11Renderer::kPhaseBind), vertex = ms(D3D11Renderer::kPhaseVertex),
                                 call = ms(D3D11Renderer::kPhaseDrawCall), resolve = ms(D3D11Renderer::kPhaseResolve), present = ms(D3D11Renderer::kPhasePresent);
                    RuntimeLog("[perf] renderer ms/frame: draws %.2f (bind %.2f, vertex upload %.2f, draw calls %.2f), resolves %.2f, present %.2f\n",
                        draws, bind, vertex, call, resolve, present);
                    RuntimeLog("[perf] bind ms/frame: targets+state %.2f, textures %.2f, constants %.2f, SRVs %.2f, samplers %.2f\n",
                        ms(D3D11Renderer::kPhaseBindTargets), ms(D3D11Renderer::kPhaseBindTextures), ms(D3D11Renderer::kPhaseBindConstants),
                        ms(D3D11Renderer::kPhaseBindSrvs), ms(D3D11Renderer::kPhaseBindSamplers));
                }
                // CPU actually used (kernel + user) vs wall time: a renderer near 1000 ms/s with little
                // readback stall is CPU-bound; a title D3D thread near 1000 ms/s limits the frame rate.
                {
                    auto cpuMs = [](HANDLE h, bool process)
                    {
                        FILETIME c, e, k, u;
                        if (!h || !(process ? GetProcessTimes(h, &c, &e, &k, &u) : GetThreadTimes(h, &c, &e, &k, &u)))
                            return 0.0;
                        return double((uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime)) / 10000.0;
                    };
                    static double lastRenderer = 0, lastMain = 0, lastProcess = 0;
                    const double rendererMs = cpuMs(GetCurrentThread(), false), mainMs = cpuMs(g_titleGpuThread.load(), false),
                                 processMs = cpuMs(GetCurrentProcess(), true);
                    if (lastProcess > 0)
                        RuntimeLog("[perf] CPU time: renderer thread %.0f ms/s, title D3D thread %.0f ms/s, whole process %.0f ms/s\n",
                            (rendererMs - lastRenderer) / secs, (mainMs - lastMain) / secs, (processMs - lastProcess) / secs);
                    lastRenderer = rendererMs;
                    lastMain = mainMs;
                    lastProcess = processMs;
                }
                perfAllTicks = 0;
                perfPartials = 0;
                perfStart = t1.QuadPart;
                renderTicks = renderMax = 0;
                frames = gapMaxUs = gaps40 = gaps60 = 0;
            }
        }
        // SCDA_TRACE_PRESENT=1: one line per present (frame, draws, frontbuffer address, fallback).
        static const bool tracePresent = getenv("SCDA_TRACE_PRESENT") != nullptr;
        if (tracePresent)
            RuntimeLog("[present] %llu draws %u drawn %u skipped %u resolves %u fb %08X %s\n", (unsigned long long)frame.stats.frame, frame.stats.draws,
                renderer.LastDrawn(), renderer.LastSkipped(), frame.stats.resolves, frame.frontbuffer,
                renderer.LastPresentedFrontbuffer() ? "frontbuffer" : "target");
        if (dumping || tracing)
        {
            const double busyMs = 1000.0 * double(busyTicks) / double(qpf.QuadPart);
            RuntimeLog("[frame] %llu: drawn %u skipped %u resolved %u presented %s (present #%llu, %.1f s) render %.2f ms/frame %.2f us/draw\n",
                (unsigned long long)frame.stats.frame, renderer.LastDrawn(), renderer.LastSkipped(), renderer.LastResolved(),
                renderer.LastPresentedFrontbuffer() ? "frontbuffer" : "target", (unsigned long long)presents, (GetTickCount64() - startTick) / 1000.0,
                busyFrames ? busyMs / double(busyFrames) : 0.0, busyDraws ? 1000.0 * busyMs / double(busyDraws) : 0.0);
            busyTicks = busyDraws = busyFrames = 0;
        }
        if (GetTickCount() - lastTitle > 500)
        {
            lastTitle = GetTickCount();
            // SCDA_TRACE_GAMMA=1: display-controller registers (0x1800-0x1FFF, CPU MMIO writes land in
            // the register file in guest memory) logged when they change.
            static const bool traceGamma = getenv("SCDA_TRACE_GAMMA") != nullptr;
            if (traceGamma)
            {
                static uint32_t last[0x800];
                for (uint32_t i = 0; i < 0x800; i++)
                    if (uint32_t v = GuestLoad32(kGpuRegBase + (0x1800 + i) * 4); v != last[i])
                    {
                        RuntimeLog("[gamma] frame %llu dc reg %04X = %08X (was %08X)\n", (unsigned long long)frame.stats.frame, 0x1800 + i, v, last[i]);
                        last[i] = v;
                    }
            }
            wchar_t title[200];
            swprintf(title, 200, L"Splinter Cell: Conviction Recomp - frame %llu draws %u (drawn %u skipped %u) resolves %u shaders %u compiled %u failed %u",
                (unsigned long long)frame.stats.frame, frame.stats.draws, renderer.LastDrawn(), renderer.LastSkipped(),
                frame.stats.resolves, frame.stats.shaders,
                ShaderCompilerReadyCount(), ShaderCompilerFailedCount());
            SetWindowTextW(hwnd, title);
            // Leak watch: host memory and handles once a minute.
            static ULONGLONG lastMem = 0;
            if (GetTickCount64() - lastMem > 60000)
            {
                lastMem = GetTickCount64();
                PROCESS_MEMORY_COUNTERS_EX pmc{ sizeof(pmc) };
                DWORD handles = 0;
                GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
                GetProcessHandleCount(GetCurrentProcess(), &handles);
                RuntimeLog("[mem] %.0f s: working set %zu MB, private %zu MB, handles %lu\n", (GetTickCount64() - startTick) / 1000.0,
                    size_t(pmc.WorkingSetSize >> 20), size_t(pmc.PrivateUsage >> 20), handles);
            }
        }
    }
}

std::pair<uint32_t, uint32_t> WindowClientSize(uint32_t workW, uint32_t workH, uint32_t dpi)
{
    uint32_t w = std::max<uint32_t>(dpi, 96) * 1280 / 96 / 16 * 16, h = w * 9 / 16;
    if (w > workW || h > workH)
    {
        w = std::min(workW, workH * 16 / 9) / 16 * 16;
        h = w * 9 / 16;
    }
    return { w, h };
}

void HostWindowStart(const DisplaySettings& s)
{
    std::thread(WindowThread, s).detach();
    std::thread(RenderThread, s).detach();
}
