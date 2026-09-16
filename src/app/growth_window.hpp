#pragma once

#include "core/recipe.hpp"

namespace artminer::app {

// Opens a small native fixed-tick inspector for stateful growth recipes. Tick
// selection is transient UI state; the supplied semantic recipe is never mutated.
[[nodiscard]] int run_growth_inspector(const core::Recipe& recipe);

}  // namespace artminer::app
