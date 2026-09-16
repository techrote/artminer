#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/recipe.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

namespace artminer::nodes {

struct Image final {
    core::u32 width{0U};
    core::u32 height{0U};
    // Canonical image storage is tightly packed, row-major RGBA8 with straight alpha.
    std::vector<core::u8> rgba;
};

enum class EvaluationErrorCode {
    invalid_recipe,
    resource_limit,
    missing_output,
    unsupported_output_kind,
    unsupported_node,
    internal_graph_error,
};

struct EvaluationError final {
    EvaluationErrorCode code{EvaluationErrorCode::internal_graph_error};
    std::string message;
};

// Canonical AM-003 CPU/reference renderer. The recipe is validated before any
// evaluation. output_name defaults to the conventional recipe output "main".
[[nodiscard]] core::Result<Image, EvaluationError> render_reference(
    const core::Recipe& recipe,
    std::string_view output_name = "main");

// Stable FNV-1a digest of canonical RGBA8 bytes, used by deterministic goldens.
[[nodiscard]] std::string image_fingerprint(const Image& image);

}  // namespace artminer::nodes
