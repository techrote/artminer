#pragma once

#include <optional>
#include <string_view>

#include "core/recipe.hpp"
#include "core/types.hpp"

namespace artminer::app {

// Shows the canonical UTF-8 cell sequence through a native Win32 text control.
// DirectWrite/font raster pixels are deliberately not part of glyph semantics.
int run_glyph_preview_application(
    const core::Recipe& recipe,
    std::string_view settings_node_id,
    std::optional<core::u64> tick = std::nullopt);

}  // namespace artminer::app
