// Game window (own thread) and the render thread that presents frames from Frames().
#pragma once
#include <settings.h>

#include <cstdint>
#include <utility>

void HostWindowStart(const DisplaySettings& settings);
// Switches the window between windowed and borderless full screen at run time (F1 Window mode).
// Fullscreen is borderless full screen on the window's monitor too (no exclusive mode).
void HostWindowSetMode(DisplaySettings::Mode mode);

// Windowed client size: 1280x720 at 96 dpi (scaled by dpi / 96: the process is per-monitor DPI aware),
// shrunk to the work area keeping 16:9. (Scale sets the internal render resolution, not the window.)
std::pair<uint32_t, uint32_t> WindowClientSize(uint32_t workWidth, uint32_t workHeight, uint32_t dpi = 96);
