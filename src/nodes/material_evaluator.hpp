#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/recipe.hpp"
#include "core/result.hpp"
#include "core/types.hpp"
#include "nodes/static_evaluator.hpp"

namespace artminer::nodes {

inline constexpr double kDefaultLoopContinuityTolerance = 1.0 / 255.0;

struct WorkflowError final {
    std::string message;
};

struct LoopValidation final {
    core::u64 loop_length{0U};
    bool validated{false};
    bool contains_stateful_nodes{false};
    double endpoint_error{0.0};
    double transition_error{0.0};
    double tolerance{kDefaultLoopContinuityTolerance};
    std::string reason;
};

struct MaterialOutputDescription final {
    std::string semantic{"image"};
    std::vector<std::pair<std::string, std::string>> mappings;
    std::vector<std::pair<std::string, std::string>> settings;
};

// Renders either an ordinary AM-003 image output or an AM-013 material/loop
// workflow output. Material HeightField semantics are enforced internally: an
// ordinary RGB image cannot reach a normal-map operation without an explicit
// core.material.height_from_image node and visible conversion parameters.
[[nodiscard]] core::Result<Image, WorkflowError> render_workflow_reference(
    const core::Recipe& recipe,
    std::string_view output_name = "main");

// Fixed-tick counterpart. Ordinary animation outputs delegate to the AM-007
// canonical renderer; AM-013 loop primitives are evaluated analytically from
// the integer tick and never depend on display cadence.
[[nodiscard]] core::Result<Image, WorkflowError> render_workflow_tick_reference(
    const core::Recipe& recipe,
    core::u64 tick,
    std::string_view output_name = "main");

// Validates both endpoint image continuity and the first transition across the
// loop boundary. Stateful nodes other than core.loop.phase are deliberately not
// claimed as closed loops because AM-013 does not implement a generic state
// closure solver.
[[nodiscard]] core::Result<LoopValidation, WorkflowError> validate_workflow_loop(
    const core::Recipe& recipe,
    core::u64 loop_length,
    std::string_view output_name = "main",
    double tolerance = kDefaultLoopContinuityTolerance);

// Deterministic material/loop semantic description used by export provenance.
[[nodiscard]] core::Result<MaterialOutputDescription, WorkflowError> describe_workflow_output(
    const core::Recipe& recipe,
    std::string_view output_name = "main");

}  // namespace artminer::nodes
