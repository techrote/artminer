#include <atomic>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/graph.hpp"
#include "core/recipe.hpp"
#include "nodes/static_evaluator.hpp"

namespace {

using artminer::core::NodeInstance;
using artminer::core::ParameterAssignment;
using artminer::core::ParameterValue;
using artminer::core::Recipe;
using artminer::core::i64;
using artminer::core::u64;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "growth test failure: " << message << '\n';
    std::exit(1);
}

void require(const bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

[[nodiscard]] ParameterAssignment parameter(std::string name, ParameterValue value) {
    return ParameterAssignment{std::move(name), std::move(value)};
}

[[nodiscard]] Recipe make_recipe(std::string type_id, std::vector<ParameterAssignment> parameters) {
    Recipe recipe;
    recipe.root_seed = 12345U;
    recipe.render.width = 16U;
    recipe.render.height = 16U;
    recipe.render.quality = "reference";
    recipe.nodes.push_back(NodeInstance{"growth", std::move(type_id), 1U, std::move(parameters)});
    recipe.outputs.push_back(artminer::core::OutputBinding{"main", "growth", "image"});
    return recipe;
}

[[nodiscard]] Recipe make_ca(const std::string& boundary = "wrap") {
    return make_recipe(
        "core.growth.cellular_automaton",
        {
            parameter("states", i64{4}),
            parameter("birth_min", i64{3}),
            parameter("birth_max", i64{3}),
            parameter("survive_min", i64{2}),
            parameter("survive_max", i64{3}),
            parameter("initial_density", 0.35),
            parameter("boundary", boundary),
            parameter("palette", std::string{"mono"}),
        });
}

[[nodiscard]] Recipe make_reaction() {
    return make_recipe(
        "core.growth.reaction_diffusion",
        {
            parameter("diff_a", 0.16),
            parameter("diff_b", 0.08),
            parameter("feed", 0.035),
            parameter("kill", 0.062),
            parameter("dt", 0.8),
            parameter("seed_density", 0.08),
            parameter("boundary", std::string{"wrap"}),
            parameter("palette", std::string{"cyan"}),
        });
}

[[nodiscard]] Recipe make_walkers() {
    return make_recipe(
        "core.growth.walkers",
        {
            parameter("walkers", i64{24}),
            parameter("steps_per_tick", i64{2}),
            parameter("turn_chance", 0.25),
            parameter("deposit", 0.12),
            parameter("decay", 0.01),
            parameter("boundary", std::string{"wrap"}),
            parameter("palette", std::string{"forest"}),
        });
}

[[nodiscard]] Recipe make_branching() {
    return make_recipe(
        "core.growth.branching",
        {
            parameter("initial_branches", i64{4}),
            parameter("max_tips", i64{64}),
            parameter("turn_chance", 0.18),
            parameter("branch_chance", 0.08),
            parameter("boundary", std::string{"clamp"}),
            parameter("palette", std::string{"ember"}),
        });
}

[[nodiscard]] std::string render_hash(const Recipe& recipe, const u64 tick) {
    auto rendered = artminer::nodes::render_reference_at_tick(recipe, tick);
    if (rendered.is_error()) {
        fail("render at tick " + std::to_string(tick) + " failed: " + rendered.error().message);
    }
    return artminer::nodes::image_fingerprint(rendered.value());
}

void test_registry_metadata() {
    const auto& registry = artminer::core::builtin_node_registry();
    for (const std::string type : {
             "core.growth.reaction_diffusion",
             "core.growth.cellular_automaton",
             "core.growth.walkers",
             "core.growth.branching"}) {
        const auto* metadata = registry.find(type);
        require(metadata != nullptr, type + " metadata missing");
        require(metadata->semantic_version == 1U, type + " semantic version changed unexpectedly");
        require(metadata->state_class == artminer::core::NodeStateClass::stateful, type + " must be stateful");
        require(metadata->evaluators.cpu, type + " must expose canonical CPU evaluation");
        require(!metadata->evaluators.gpu, type + " must not claim unvalidated GPU equivalence");
        require(!metadata->parameters.empty(), type + " should expose mutation parameters");
        for (const auto& spec : metadata->parameters) {
            require(spec.mutation.mutable_parameter, type + "." + spec.name + " unexpectedly non-mutable");
            require(!spec.mutation.group.empty(), type + "." + spec.name + " lacks mutation group metadata");
        }
    }
}

void test_ca_goldens_and_replay() {
    const Recipe recipe = make_ca();
    require(render_hash(recipe, 0U) == "54d783432df8c584", "CA tick-0 golden mismatch");
    require(render_hash(recipe, 1U) == "ad221e4a40beb9ca", "CA tick-1 golden mismatch");
    const std::string tick8 = render_hash(recipe, 8U);
    require(tick8 == "a991ced7edb6b64d", "CA tick-8 golden mismatch");

    const std::string earlier = render_hash(recipe, 4U);
    require(earlier != tick8, "CA should evolve between tested ticks");
    require(render_hash(recipe, 8U) == tick8, "reset/replay did not reproduce CA tick 8");
    require(render_hash(recipe, 0U) == "54d783432df8c584", "reset-to-zero semantics changed after later renders");
}

void test_boundary_and_distinct_families() {
    const std::string wrap = render_hash(make_ca("wrap"), 8U);
    const std::string clamp = render_hash(make_ca("clamp"), 8U);
    require(wrap != clamp, "CA boundary condition did not affect the tested state");

    std::set<std::string> hashes;
    hashes.insert(render_hash(make_ca(), 8U));
    hashes.insert(render_hash(make_reaction(), 8U));
    hashes.insert(render_hash(make_walkers(), 8U));
    hashes.insert(render_hash(make_branching(), 8U));
    require(hashes.size() == 4U, "growth families should produce visibly distinct canonical fixtures");
}

void test_serialization_version_and_fingerprint() {
    Recipe recipe = make_walkers();
    const std::string original_fingerprint = artminer::core::semantic_fingerprint(recipe);
    const std::string text = artminer::core::serialize_recipe_canonical(recipe);
    auto parsed = artminer::core::parse_recipe(text);
    require(parsed.is_ok(), "growth recipe failed canonical round trip");
    require(artminer::core::validate_recipe(parsed.value()).empty(), "round-tripped growth recipe failed validation");
    require(artminer::core::semantic_fingerprint(parsed.value()) == original_fingerprint,
            "growth canonical round trip changed semantic fingerprint");

    Recipe mutated = recipe;
    mutated.nodes.front().parameters.front().value = i64{25};
    require(artminer::core::semantic_fingerprint(mutated) != original_fingerprint,
            "growth parameter mutation must participate in semantic fingerprints");

    Recipe future = recipe;
    future.nodes.front().semantic_version = 2U;
    require(!artminer::core::validate_recipe(future).empty(),
            "unsupported growth node semantic version should be rejected");
}

void test_limits_cancellation_and_rule_validation() {
    const Recipe walkers = make_walkers();
    auto excessive = artminer::nodes::render_reference_at_tick(walkers, 100001U);
    require(excessive.is_error(), "excessive growth tick should be rejected");
    require(excessive.error().code == artminer::nodes::EvaluationErrorCode::resource_limit,
            "excessive growth tick should report resource_limit");

    std::atomic_bool cancel{true};
    auto cancelled = artminer::nodes::render_reference_at_tick(walkers, 20U, "main", &cancel);
    require(cancelled.is_error(), "pre-cancelled growth render should stop");
    require(cancelled.error().code == artminer::nodes::EvaluationErrorCode::cancelled,
            "pre-cancelled growth render should report cancelled");

    Recipe invalid = make_ca();
    invalid.nodes.front().parameters[1].value = i64{5};
    invalid.nodes.front().parameters[2].value = i64{2};
    auto invalid_render = artminer::nodes::render_reference_at_tick(invalid, 1U);
    require(invalid_render.is_error(), "relationally invalid CA rule should fail at evaluation");
    require(invalid_render.error().code == artminer::nodes::EvaluationErrorCode::invalid_recipe,
            "invalid CA rule should report invalid_recipe");
}

}  // namespace

int main() {
    test_registry_metadata();
    test_ca_goldens_and_replay();
    test_boundary_and_distinct_families();
    test_serialization_version_and_fingerprint();
    test_limits_cancellation_and_rule_validation();
    std::cout << "ArtMiner deterministic growth tests passed\n";
    return 0;
}
