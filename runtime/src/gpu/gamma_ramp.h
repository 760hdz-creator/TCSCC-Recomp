// Display gamma ramp: the 256-entry LUT the title writes to the display controller (in-game
// brightness / console gamma). The scan-out applies it after the frontbuffer, so the renderer
// applies it in the blit (and in presented-frame dumps).
//
// Register pair as observed (no Xenia source in external/, names are descriptive): 0x1922 = LUT
// index (written 0, then 0..255 per entry), 0x1925 = entry, 10:10:10 packed (bits 0-9, 10-19,
// 20-29); 0x1921 = 0 and 0x1927 = 7 precede each upload. SC:DA writes equal channels, so the
// channel order inside the entry is unverified.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <mutex>

struct GammaRamp
{
    std::mutex mutex;
    std::array<uint32_t, 256> entries; // 10:10:10
    std::atomic<uint32_t> version = 0;  // bumped per written entry (0 = identity, never written)
    uint32_t index = 0;

    GammaRamp()
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            const uint32_t v = (i << 2) | (i >> 6);
            entries[i] = v | v << 10 | v << 20;
        }
    }
    void SetIndex(uint32_t i)
    {
        std::lock_guard lock(mutex);
        index = i & 0xFF;
    }
    void Write(uint32_t entry)
    {
        std::lock_guard lock(mutex);
        entries[index] = entry & 0x3FFFFFFF;
        index = (index + 1) & 0xFF;
        version++;
    }
    std::array<uint32_t, 256> Copy()
    {
        std::lock_guard lock(mutex);
        return entries;
    }
};

// The user's brightness ([Display] Gamma, F1 menu) on top of the title's ramp: entry i becomes the
// title's ramp read at 255 * (i / 255)^(1 / gamma), per channel, interpolated. 1 = unchanged, above 1
// brighter midtones; black and white stay put.
inline std::array<uint32_t, 256> WithUserGamma(const std::array<uint32_t, 256>& ramp, float gamma)
{
    if (gamma == 1.0f || !(gamma > 0.0f))
        return ramp;
    std::array<uint32_t, 256> out;
    for (uint32_t i = 0; i < 256; i++)
    {
        const float pos = 255.0f * std::pow(float(i) / 255.0f, 1.0f / gamma);
        const uint32_t lo = std::min<uint32_t>(uint32_t(pos), 254);
        const float f = pos - float(lo);
        uint32_t e = 0;
        for (uint32_t c = 0; c < 3; c++)
        {
            const float a = float((ramp[lo] >> (c * 10)) & 0x3FF), b = float((ramp[lo + 1] >> (c * 10)) & 0x3FF);
            e |= std::min<uint32_t>(uint32_t(a + (b - a) * f + 0.5f), 0x3FF) << (c * 10);
        }
        out[i] = e;
    }
    return out;
}

inline GammaRamp& DisplayGammaRamp()
{
    static GammaRamp ramp;
    return ramp;
}

constexpr uint32_t kRegDcLutIndex = 0x1922;
constexpr uint32_t kRegDcLutColor = 0x1925;
