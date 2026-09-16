#pragma once

#include "core/recipe.hpp"

namespace artminer::app {

// Opens the dedicated AM-007 fixed-tick animation inspector. Rendering occurs
// off the Win32 message thread so representative canonical workloads cannot
// block pause/speed/step/reset controls.
[[nodiscard]] int run_playback_application(const core::Recipe& recipe);

}  // namespace artminer::app
