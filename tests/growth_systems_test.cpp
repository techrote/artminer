#include <algorithm>
#include <array>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include "core/graph.hpp"
#include "core/recipe.hpp"
#include "core/specimen_browser.hpp"
#include "nodes/static_evaluator.hpp"

namespace {

int g_failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

[[nodiscard]] artminer::core::NodeInstance default_node(
    const std::string_view id,
    const std::string_view type_id) {
    const auto* metadata = artminer::core::builtin_node_registry().find(type_id);
    if (metadata == nullptr) {
        ++g_failures;
        std::cerr << "FAIL: missing metadata for " << type_id << '\n';
        return {};
    }
    artminer::core::NodeInstance node;
    node.id = std::string(id);
    node.type_id = std::string(type_id);
    node.semantic_version = metadata->semantic_version;
    for (const auto& parameter : metadata->parameters) {
        node.parameters.push_back({parameter.name, parameter.default_value});
    }
    return node;
}

[[nodiscard]] bool set_parameter(
    artminer::core::NodeInstance& node,
    const std::string_view name,
    artminer::core::ParameterValue value) {
    const auto found = std::find_if(
        node.parameters.begin(), node.parameters.end(),
        [name](const artminer::core::ParameterAssignment& parameter) { return parameter.name == name; });
    if (found == node.parameters.end()) {
        return false;
    }
    found->value = std::move(value);
    return true;
}

[[nodiscard]] artminer::core::Recipe make_growth_recipe(
    const std::string_view type_id,
    const artminer::core::i64 tick,
    const artminer::core::u64 seed = 42ULL,
    const artminer::core::u32 width = 16U,
    const artminer::core::u32 height = 12U) {
    artminer::core::Recipe recipe;
    recipe.root_seed = seed;
    recipe.render.width = width;
    recipe.render.height = height;
    recipe.render.quality = "reference";
    recipe.nodes.push_back(default_node("growth", type_id));
    recipe.nodes.push_back(default_node("image", "core.image.from_scalar"));
    expect(set_parameter(recipe.nodes.front(), "tick", tick), "growth fixture has explicit tick parameter");
    recipe.edges.push_back({"growth", "value", "image", "source"});
    recipe.outputs.push_back({"main", "image", "value"});
    return recipe;
}

[[nodiscard]] std::string render_hash(const artminer::core::Recipe& recipe) {
    auto rendered = artminer::nodes::render_reference(recipe);
    if (rendered.is_error()) {
        ++g_failures;
        std::cerr << "FAIL: render failed: " << rendered.error().message << '\n';
        return {};
    }
    return artminer::nodes::image_fingerprint(rendered.value());
}

void test_metadata_contract() {
    constexpr std::array<std::string_view, 4> types{
        "core.growth.reaction_diffusion",
        "core.growth.cellular_automaton",
        "core.growth.walkers",
        "core.growth.branching",
    };
    for (const auto type : types) {
        const auto* metadata = artminer::core::builtin_node_registry().find(type);
        expect(metadata != nullptr, "growth node is registered");
        if (metadata == nullptr) {
            continue;
        }
        expect(metadata->state_class == artminer::core::NodeStateClass::stateful, "growth node is explicitly stateful");
        expect(metadata->evaluators.cpu, "growth node has canonical CPU evaluator");
        expect(!metadata->evaluators.gpu, "growth node does not overclaim GPU equivalence");
        expect(metadata->outputs.size() == 1U, "growth node has one scalar output");
        if (!metadata->outputs.empty()) {
            expect(metadata->outputs.front().kind == artminer::core::DataKind::scalar_field, "growth output is ScalarField");
        }
        const auto tick = std::find_if(
            metadata->parameters.begin(), metadata->parameters.end(),
            [](const artminer::core::ParameterSpec& parameter) { return parameter.name == "tick"; });
        expect(tick != metadata->parameters.end(), "growth metadata declares tick");
        if (tick != metadata->parameters.end()) {
            expect(!tick->mutation.mutable_parameter, "tick is not an automatic mutation axis");
            expect(tick->mutation.scale == artminer::core::MutationScale::none, "tick mutation scale is none");
        }
    }
}

void test_reference_goldens_and_reset() {
    struct Fixture final {
        std::string_view type;
        std::string_view tick0;
        std::string_view tick5;
    };
    constexpr std::array<Fixture, 4> fixtures{{
        {"core.growth.reaction_diffusion", "cd573607b718fe87", "b073514e4d2da720"},
        {"core.growth.cellular_automaton", "29e5ed05d3d967fd", "70d5845ec3887965"},
        {"core.growth.walkers", "97f10fd16b920125", "e906747482b39bc8"},
        {"core.growth.branching", "97f10fd16b920125", "68f2feca9628882b"},
    }};

    for (const auto& fixture : fixtures) {
        auto initial = make_growth_recipe(fixture.type, 0);
        const auto validation = artminer::core::validate_recipe(initial);
        expect(validation.empty(), "growth fixture validates");
        const std::string initial_hash = render_hash(initial);
        expect(initial_hash == fixture.tick0, "tick-zero image matches canonical golden");

        auto evolved = make_growth_recipe(fixture.type, 5);
        const std::string first = render_hash(evolved);
        const std::string repeated = render_hash(evolved);
        expect(first == fixture.tick5, "evolved image matches canonical tick-five golden");
        expect(first == repeated, "same recipe seed and tick replay identically");
        expect(first != initial_hash, "tick advancement changes representative growth state");

        const std::string serialized = artminer::core::serialize_recipe_canonical(evolved);
        auto parsed = artminer::core::parse_recipe(serialized);
        expect(parsed.is_ok(), "growth recipe canonical serialization parses");
        if (parsed.is_ok()) {
            expect(
                artminer::core::semantic_fingerprint(parsed.value()) == artminer::core::semantic_fingerprint(evolved),
                "growth recipe round-trip preserves semantic fingerprint");
            expect(render_hash(parsed.value()) == first, "serialized growth recipe replays exact state");
        }

        expect(set_parameter(evolved.nodes.front(), "tick", artminer::core::i64{0}), "reset can set tick zero");
        expect(render_hash(evolved) == initial_hash, "reset to tick zero reconstructs initial state");
        expect(set_parameter(evolved.nodes.front(), "tick", artminer::core::i64{5}), "replay can restore tick five");
        expect(render_hash(evolved) == first, "reset then replay restores exact evolved state");
    }
}

void test_seed_and_boundary_semantics() {
    auto left = make_growth_recipe("core.growth.cellular_automaton", 5, 100ULL);
    auto right = make_growth_recipe("core.growth.cellular_automaton", 5, 101ULL);
    expect(render_hash(left) != render_hash(right), "root seed changes cellular initial state deterministically");

    auto repeated = make_growth_recipe("core.growth.cellular_automaton", 5, 100ULL, 7U, 5U);
    auto clamped = repeated;
    expect(set_parameter(clamped.nodes.front(), "boundary", std::string("clamp")), "CA boundary parameter is editable");
    expect(render_hash(repeated) != render_hash(clamped), "repeat and clamp boundary conditions are semantically distinct");
}

void test_mutation_safety() {
    constexpr std::array<std::string_view, 4> types{
        "core.growth.reaction_diffusion",
        "core.growth.cellular_automaton",
        "core.growth.walkers",
        "core.growth.branching",
    };
    for (const auto type : types) {
        auto parent = make_growth_recipe(type, 5);
        artminer::core::ParameterLocks locks;
        auto child = artminer::core::mutate_recipe_parameters(
            parent, 0x60060001ULL, artminer::core::kParameterMutationOperatorVersion, 0.35, locks);
        expect(child.is_ok(), "growth parameter mutation produces a valid child");
        if (child.is_error()) {
            continue;
        }
        const auto& mutated = child.value();
        const auto& tick = mutated.nodes.front().parameters.front();
        expect(tick.name == "tick", "growth tick remains first declared parameter");
        expect(std::get<artminer::core::i64>(tick.value) == 5, "automatic mutation preserves requested tick");
        expect(artminer::core::validate_recipe(mutated).empty(), "mutated growth recipe remains graph-valid");
        expect(!render_hash(mutated).empty(), "mutated growth recipe remains canonically renderable");
    }
}

void test_resource_limit() {
    auto expensive = make_growth_recipe("core.growth.reaction_diffusion", 300, 42ULL, 512U, 512U);
    auto rendered = artminer::nodes::render_reference(expensive);
    expect(rendered.is_error(), "excessive growth work is rejected before simulation");
    if (rendered.is_error()) {
        expect(
            rendered.error().code == artminer::nodes::EvaluationErrorCode::resource_limit,
            "excessive growth work reports resource_limit");
    }
}

}  // namespace

int main() {
    test_metadata_contract();
    test_reference_goldens_and_reset();
    test_seed_and_boundary_semantics();
    test_mutation_safety();
    test_resource_limit();

    if (g_failures != 0) {
        std::cerr << g_failures << " growth test(s) failed\n";
        return 1;
    }
    std::cout << "ArtMiner AM-006 growth tests passed\n";
    return 0;
}
