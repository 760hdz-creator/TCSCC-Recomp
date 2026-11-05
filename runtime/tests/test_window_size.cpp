#include "check.h"
#include <host_window.h>

// Scale is the internal render resolution, not the window size: windowed client = 1280x720,
// shrunk to fit the work area with the 16:9 aspect kept.
TEST(window_client_size_fits_work_area)
{
    auto a = WindowClientSize(1920, 1040);
    CHECK_EQ(a.first, 1280u);
    CHECK_EQ(a.second, 720u);
    auto b = WindowClientSize(1024, 700);
    CHECK(b.first <= 1024u && b.second <= 700u);
    CHECK_EQ(b.first * 9, b.second * 16);
}

// Per-monitor DPI aware process: the default windowed size follows the monitor scale (150% -> 1920x1080).
TEST(window_client_size_follows_dpi)
{
    auto a = WindowClientSize(3840, 2100, 144);
    CHECK_EQ(a.first, 1920u);
    CHECK_EQ(a.second, 1080u);
    auto b = WindowClientSize(1900, 1000, 144);
    CHECK(b.first <= 1900u && b.second <= 1000u);
}
