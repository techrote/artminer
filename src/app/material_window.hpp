#pragma once

#include <string>

#include "core/recipe.hpp"

namespace artminer::app {

// Native AM-013 seam-inspection workflow. The view shows a deterministic 2x2
// tiling with the measured joins crossing at the centre, plus horizontal,
// vertical and authoritative combined AM-010 seam errors. +/- changes the
// inspection threshold; R rerenders from the canonical workflow evaluator.
[[nodiscard]] int run_material_inspection_application(
    const core::Recipe& recipe,
    std::string output_name = "main");

}  // namespace artminer::app
