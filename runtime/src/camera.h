// Hor+ widescreen (camera.cpp hooks the projection): the renderer reports the window aspect each present.
#pragma once
#include <atomic>

inline std::atomic<float>& CameraHorPlusK() { static std::atomic<float> k{ 1.0f }; return k; }
// k = window aspect / (16/9) when ultrawide is on and the window is wider than 16:9, else 1.
inline void CameraSetDisplayAspect(float aspect, bool ultrawide)
{
    constexpr float k169 = 16.0f / 9.0f;
    CameraHorPlusK() = ultrawide && aspect > k169 * 1.01f ? aspect / k169 : 1.0f;
}
// The current k (1 = off: the present keeps 16:9 with bars).
inline float CameraHorPlusFactor() { return CameraHorPlusK().load(std::memory_order_relaxed); }
