#pragma once

#include <filesystem>

#include "core/recipe.hpp"

namespace artminer::app {

// Native AM-009 export control surface. It delegates all semantic work to the
// same export_recipe() implementation used by headless commands.
[[nodiscard]] int run_export_application(
    const core::Recipe& recipe,
    const std::filesystem::path& default_output_parent);

}  // namespace artminer::app
