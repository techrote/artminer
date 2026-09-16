#include "nodes/static_evaluator.hpp"

#include <algorithm>
#include <string>
#include <string_view>

#include "core/checked_math.hpp"
#include "core/graph.hpp"
#include "nodes/growth_systems.hpp"

namespace artminer::nodes {

// static_evaluator.cpp is compiled with its established AM-003 implementation
// exported under this implementation name. The public entry point below adds
// fixed-tick stateful dispatch while leaving the accepted static evaluator intact.
[[nodiscard]] core::Result<Image, EvaluationError> render_reference_static(
    const core::Recipe& recipe,
    std::string_view output_name);

namespace {

constexpr core::u64 kMaxReferencePixels = 4'194'304ULL;

[[nodiscard]] EvaluationError make_error(const EvaluationErrorCode code, std::string message) {
    return EvaluationError{code, std::move(message)};
}

}  // namespace

core::Result<Image, EvaluationError> render_reference(
    const core::Recipe& recipe,
    const std::string_view output_name) {
    return render_reference_at_tick(recipe, 0U, output_name, nullptr);
}

core::Result<Image, EvaluationError> render_reference_at_tick(
    const core::Recipe& recipe,
    const core::u64 tick,
    const std::string_view output_name,
    const std::atomic_bool* cancel) {
    const auto validation_errors = core::validate_recipe(recipe);
    if (!validation_errors.empty()) {
        return core::Result<Image, EvaluationError>::failure(make_error(
            EvaluationErrorCode::invalid_recipe,
            "recipe validation failed: " + validation_errors.front().message));
    }

    const auto output = std::find_if(
        recipe.outputs.begin(), recipe.outputs.end(),
        [&](const core::OutputBinding& binding) { return binding.name == output_name; });
    if (output == recipe.outputs.end()) {
        return core::Result<Image, EvaluationError>::failure(make_error(
            EvaluationErrorCode::missing_output,
            "recipe does not define output '" + std::string(output_name) + "'"));
    }

    const auto node = std::find_if(
        recipe.nodes.begin(), recipe.nodes.end(),
        [&](const core::NodeInstance& instance) { return instance.id == output->node_id; });
    if (node == recipe.nodes.end()) {
        return core::Result<Image, EvaluationError>::failure(make_error(
            EvaluationErrorCode::internal_graph_error,
            "output references a node that is unavailable"));
    }

    if (!is_growth_node(node->type_id)) {
        return render_reference_static(recipe, output_name);
    }

    const core::NodeMetadata* metadata = core::builtin_node_registry().find(node->type_id);
    if (metadata == nullptr) {
        return core::Result<Image, EvaluationError>::failure(make_error(
            EvaluationErrorCode::internal_graph_error,
            "growth output references node metadata that is unavailable"));
    }
    const auto port = std::find_if(
        metadata->outputs.begin(), metadata->outputs.end(),
        [&](const core::PortSpec& spec) { return spec.name == output->port; });
    if (port == metadata->outputs.end() || port->kind != core::DataKind::image) {
        return core::Result<Image, EvaluationError>::failure(make_error(
            EvaluationErrorCode::unsupported_output_kind,
            "canonical render output '" + std::string(output_name) + "' must have Image kind"));
    }

    auto pixels = core::checked_multiply_u64(
        static_cast<core::u64>(recipe.render.width),
        static_cast<core::u64>(recipe.render.height));
    if (pixels.is_error() || pixels.value() > kMaxReferencePixels) {
        return core::Result<Image, EvaluationError>::failure(make_error(
            EvaluationErrorCode::resource_limit,
            "reference growth render is limited to 4,194,304 pixels"));
    }

    return render_growth_node(recipe, *node, GrowthRenderRequest{tick, cancel});
}

}  // namespace artminer::nodes
