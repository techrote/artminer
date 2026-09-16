#pragma once

#include <filesystem>

#include "core/recipe.hpp"

namespace artminer::app {

// Opens the dedicated fixed-tick animation inspector. Rendering occurs off the
// Win32 message thread so representative canonical workloads cannot block
// pause/speed/step/reset controls. The portable output directory is supplied
// explicitly so UI export uses the same AM-009 subsystem as headless export.
[[nodiscard]] int run_playback_application(
    const core::Recipe& recipe,
    const std::filesystem::path& output_directory);

}  // namespace artminer::app
