// Field of view override (F1 menu, [Display] FOV). Degrees, horizontal; Conviction applies it in the
// player camera's view builder (camera.cpp, sub_82DA8B30 hook). src/fov.cpp also holds the SC:DA
// controller-property path (FovTick, not built for Conviction). Thread-safe.
#pragma once

void SetFovOverride(float degrees);
float FovOverride(); // the user's value (0 before the settings are applied)
