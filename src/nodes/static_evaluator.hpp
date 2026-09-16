#pragma once

#include <atomic>
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
    cancelled,
    missing_output,
    unsupported_output_kind,
    unsupported_node,
    internal_graph_error,
};

struct EvaluationError final {
    EvaluationErrorCode code{EvaluationErrorCode::internal_graph_error};
    std::string message;
};

// Canonical CPU/reference renderer. For stateless recipes tick is irrelevant.
// Stateful AM-006 nodes reconstruct state from recipe + seed + requested tick;
// display frame rate and prior render calls never participate in semantics.
[[nodiscard]] core::Result<Image, EvaluationError> render_reference(
    const core::Recipe& recipe,
    std::string_view output_name = "main");

[[nodiscard]] core::Result<Image, EvaluationError> render_reference_at_tick(
    const core::Recipe& recipe,
    core::u64 tick,
    std::string_view output_name = "main",
    const std::atomic_bool* cancel = nullptr);

// Stable FNV-1a digest of canonical RGBA8 bytes, used by deterministic goldens.
[[nodiscard]] std::string image_fingerprint(const Image& image);

}  // namespace artminer::nodes
