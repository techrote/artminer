#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/recipe.hpp"
#include "core/result.hpp"
#include "core/types.hpp"

namespace artminer::nodes {

struct GrowthField final {
    core::u32 width{0U};
    core::u32 height{0U};
    std::vector<double> values;
};

enum class GrowthErrorCode {
    invalid_parameter,
    resource_limit,
    unsupported_node,
};

struct GrowthError final {
    GrowthErrorCode code{GrowthErrorCode::unsupported_node};
    std::string message;
};

[[nodiscard]] bool is_growth_node(std::string_view type_id) noexcept;

// Canonical AM-006 fixed-tick evaluator. Stateful growth nodes are reconstructed
// from initial state on every call; no hidden mutable simulation state or frame
// cadence participates in the result. The node's explicit `tick` parameter is
// the requested state index and tick zero is its reset/initial state.
[[nodiscard]] core::Result<GrowthField, GrowthError> evaluate_growth_node(
    const core::Recipe& recipe,
    const core::NodeInstance& node,
    core::u32 width,
    core::u32 height);

}  // namespace artminer::nodes
