// Field of view override (src/fov.cpp): the player controller's DefaultFOV / DesiredFOV, written every
// engine tick. Degrees; the game's own value (75) leaves the controller untouched. Thread-safe.
#pragma once

void SetFovOverride(float degrees);

