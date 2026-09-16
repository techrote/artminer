#pragma once

#include <atomic>
#include <string_view>

#include "core/recipe.hpp"
#include "nodes/static_evaluator.hpp"

namespace artminer::nodes {

struct GrowthRenderRequest final {
    core::u64 tick{0U};
    const std::atomic_bool* cancel{nullptr};
};

[[nodiscard]] bool is_growth_node(std::string_view type_id) noexcept;

// Canonical CPU implementation for AM-006 self-contained stateful growth nodes.
// State is reconstructed from recipe + root/node seed + requested tick. No
// persistent simulation object is authoritative; snapshots may be added later as
// disposable accelerators without changing these semantics.
[[nodiscard]] core::Result<Image, EvaluationError> render_growth_node(
    const core::Recipe& recipe,
    const core::NodeInstance& node,
    const GrowthRenderRequest& request);

}  // namespace artminer::nodes
