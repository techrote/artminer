#include <algorithm>
#include <array>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include "core/animation.hpp"
#include "core/graph.hpp"
#include "core/recipe.hpp"
#include "nodes/motion_evaluator.hpp"
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

[[nodiscard]] artminer::core::Recipe make_particle_recipe(
    const std::string_view mode = "flow",
    const artminer::core::u64 seed = 7001ULL) {
    artminer::core::Recipe recipe;
    recipe.root_seed = seed;
    recipe.render.width = 48U;
    recipe.render.height = 40U;
    recipe.nodes.push_back(default_node("particles", "core.motion.particles"));
    recipe.nodes.push_back(default_node("deposit", "core.particles.deposit.image"));
    expect(set_parameter(recipe.nodes[0], "count", artminer::core::i64{24}), "particle fixture count edits");
    expect(set_parameter(recipe.nodes[0], "lifetime", artminer::core::i64{64}), "particle fixture lifetime edits");
    expect(set_parameter(recipe.nodes[0], "trail_length", artminer::core::i64{8}), "particle fixture trail edits");
    expect(set_parameter(recipe.nodes[0], "mode", std::string(mode)), "particle fixture mode edits");
    expect(set_parameter(recipe.nodes[1], "radius", 0.012), "particle deposition radius edits");
    recipe.edges.push_back({"particles", "value", "deposit", "particles"});
    recipe.outputs.push_back({"main", "deposit", "value"});
    return recipe;
}

[[nodiscard]] artminer::core::Recipe make_feedback_recipe() {
    artminer::core::Recipe recipe;
    recipe.render.width = 24U;
    recipe.render.height = 20U;
    recipe.nodes.push_back(default_node("source", "core.scalar.constant"));
    recipe.nodes.push_back(default_node("current", "core.image.from_scalar"));
    recipe.nodes.push_back(default_node("history", "core.state.delay.image"));
    recipe.nodes.push_back(default_node("blend", "core.image.blend"));
    expect(set_parameter(recipe.nodes[0], "value", 0.75), "feedback source value edits");
    expect(set_parameter(recipe.nodes[3], "history_weight", 0.75), "feedback weight edits");
    recipe.edges.push_back({"source", "value", "current", "source"});
    recipe.edges.push_back({"current", "value", "blend", "current"});
    recipe.edges.push_back({"history", "previous", "blend", "history"});
    recipe.edges.push_back({"blend", "value", "history", "next"});
    recipe.outputs.push_back({"main", "blend", "value"});
    return recipe;
}

[[nodiscard]] artminer::core::Recipe make_palette_recipe() {
    artminer::core::Recipe recipe;
    recipe.render.width = 32U;
    recipe.render.height = 32U;
    recipe.nodes.push_back(default_node("radial", "core.scalar.radial"));
    recipe.nodes.push_back(default_node("palette", "core.palette.default"));
    recipe.nodes.push_back(default_node("cycle", "core.palette.cycle"));
    recipe.nodes.push_back(default_node("colour", "core.colour.from_palette"));
    recipe.nodes.push_back(default_node("image", "core.image.from_colour"));
    expect(set_parameter(recipe.nodes[1], "preset", std::string("cool")), "palette preset edits");
    recipe.edges.push_back({"palette", "value", "cycle", "palette"});
    recipe.edges.push_back({"radial", "value", "colour", "source"});
    recipe.edges.push_back({"cycle", "value", "colour", "palette"});
    recipe.edges.push_back({"colour", "value", "image", "source"});
    recipe.outputs.push_back({"main", "image", "value"});
    return recipe;
}

[[nodiscard]] std::string frame_hash(const artminer::core::Recipe& recipe, const artminer::core::u64 tick) {
    auto rendered = artminer::nodes::render_animation_reference(recipe, tick);
    if (rendered.is_error()) {
        ++g_failures;
        std::cerr << "FAIL: animation render failed: " << rendered.error().message << '\n';
        return {};
    }
    return artminer::nodes::image_fingerprint(rendered.value());
}

[[nodiscard]] bool same_particles(
    const artminer::nodes::ParticleSet& left,
    const artminer::nodes::ParticleSet& right) {
    if (left.particles.size() != right.particles.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.particles.size(); ++index) {
        const auto& a = left.particles[index];
        const auto& b = right.particles[index];
        if (a.id != b.id || a.x != b.x || a.y != b.y || a.velocity_x != b.velocity_x ||
            a.velocity_y != b.velocity_y || a.age != b.age || a.lifetime != b.lifetime ||
            a.trail.size() != b.trail.size()) {
            return false;
        }
        for (std::size_t point = 0U; point < a.trail.size(); ++point) {
            if (a.trail[point].x != b.trail[point].x || a.trail[point].y != b.trail[point].y) {
                return false;
            }
        }
    }
    return true;
}

void test_boundary_validation_contract() {
    auto legal = make_feedback_recipe();
    expect(artminer::core::validate_recipe(legal).empty(), "cycle crossing explicit state boundary is legal");
    const auto* delay = artminer::core::builtin_node_registry().find("core.state.delay.image");
    expect(delay != nullptr, "image delay boundary is registered");
    if (delay != nullptr) {
        expect(delay->state_class == artminer::core::NodeStateClass::state_boundary, "image delay is state_boundary");
        expect(delay->evaluators.cpu, "image delay advertises canonical CPU semantics");
    }

    artminer::core::Recipe illegal;
    illegal.render.width = 8U;
    illegal.render.height = 8U;
    illegal.nodes.push_back(default_node("a", "core.scalar.pass"));
    illegal.nodes.push_back(default_node("b", "core.scalar.pass"));
    illegal.edges.push_back({"a", "value", "b", "source"});
    illegal.edges.push_back({"b", "value", "a", "source"});
    illegal.outputs.push_back({"main", "a", "value"});
    const auto errors = artminer::core::validate_recipe(illegal);
    expect(
        std::any_of(errors.begin(), errors.end(), [](const artminer::core::ValidationError& error) {
            return error.code == artminer::core::ValidationErrorCode::cycle_detected;
        }),
        "ordinary same-tick graph cycle remains rejected");
}

void test_particle_order_lifecycle_and_modes() {
    constexpr std::array<std::string_view, 5> modes{"flow", "attract", "repel", "orbit", "walker"};
    for (const auto mode : modes) {
        auto recipe = make_particle_recipe(mode);
        const auto& node = recipe.nodes.front();
        auto first = artminer::nodes::evaluate_particle_set(recipe, node, 7U);
        auto second = artminer::nodes::evaluate_particle_set(recipe, node, 7U);
        expect(first.is_ok() && second.is_ok(), "all AM-007 particle motion modes evaluate");
        if (first.is_error() || second.is_error()) {
            continue;
        }
        expect(same_particles(first.value(), second.value()), "particle replay is bit-stable within canonical evaluator");
        artminer::core::u64 previous = 0U;
        bool first_particle = true;
        for (const auto& particle : first.value().particles) {
            expect(particle.age == 7U, "particle age equals executed fixed updates");
            if (!first_particle) {
                expect(particle.id > previous, "ParticleSet canonical ordering is ascending stable id");
            }
            previous = particle.id;
            first_particle = false;
        }
    }

    auto short_lived = make_particle_recipe("walker");
    expect(set_parameter(short_lived.nodes.front(), "lifetime", artminer::core::i64{4}), "lifetime can be shortened");
    auto dead = artminer::nodes::evaluate_particle_set(short_lived, short_lived.nodes.front(), 4U);
    expect(dead.is_ok(), "death-boundary tick evaluates");
    if (dead.is_ok()) {
        expect(dead.value().particles.empty(), "particles die deterministically when age reaches lifetime");
    }
}

void test_frame_replay_reset_feedback_and_palette() {
    auto particle = make_particle_recipe("flow", 9001ULL);
    const std::string tick0 = frame_hash(particle, 0U);
    const std::string tick7 = frame_hash(particle, 7U);
    const std::string tick14 = frame_hash(particle, 14U);
    expect(!tick0.empty() && !tick7.empty() && !tick14.empty(), "representative motion ticks render");
    expect(tick0 != tick7 && tick7 != tick14, "motion frames evolve across fixed ticks");
    expect(frame_hash(particle, 7U) == tick7, "same recipe and tick reproduce identical particle frame");
    expect(frame_hash(particle, 0U) == tick0, "reset to tick zero reconstructs exact initial frame");

    auto feedback = make_feedback_recipe();
    const std::string feedback0 = frame_hash(feedback, 0U);
    const std::string feedback4 = frame_hash(feedback, 4U);
    expect(feedback0 != feedback4, "explicit previous-frame boundary evolves image history");
    expect(frame_hash(feedback, 4U) == feedback4, "feedback replay at the same tick is deterministic");

    auto palette = make_palette_recipe();
    const std::string palette0 = frame_hash(palette, 0U);
    const std::string palette1 = frame_hash(palette, 1U);
    expect(palette0 != palette1, "palette cycling composes with a static field by fixed tick");
    expect(frame_hash(palette, 1U) == palette1, "palette-cycle replay is deterministic");
}

void test_play_step_and_speed_neutrality() {
    artminer::core::FixedTickPlayback played(10U);
    played.set_playing(true);
    expect(played.advance_wall_time(1000U) == 10U, "one second at 10 tick/s advances ten ticks");

    artminer::core::FixedTickPlayback stepped(10U);
    for (int index = 0; index < 10; ++index) {
        stepped.step();
    }
    expect(played.tick() == stepped.tick(), "play-to-tick and repeated single-step reach same integer tick");

    auto recipe = make_particle_recipe("walker", 9010ULL);
    expect(
        frame_hash(recipe, played.tick()) == frame_hash(recipe, stepped.tick()),
        "play-to-tick and single-step produce the same canonical frame");

    artminer::core::FixedTickPlayback slow(5U);
    artminer::core::FixedTickPlayback fast(20U);
    slow.set_playing(true);
    fast.set_playing(true);
    (void)slow.advance_wall_time(2000U);
    (void)fast.advance_wall_time(500U);
    expect(slow.tick() == 10U && fast.tick() == 10U, "different preview speeds can reach the same semantic tick");
    expect(
        frame_hash(recipe, slow.tick()) == frame_hash(recipe, fast.tick()),
        "preview speed is semantically neutral once requested tick is equal");
}

void test_snapshot_cache_is_disposable_and_keyed() {
    auto recipe = make_particle_recipe("flow", 9100ULL);
    artminer::nodes::FrameSnapshotCache cache(4U);
    auto first = artminer::nodes::render_animation_reference(recipe, 6U, "main", &cache);
    expect(first.is_ok(), "cached render succeeds");
    expect(cache.size() == 1U, "first frame inserts one snapshot");
    auto repeated = artminer::nodes::render_animation_reference(recipe, 6U, "main", &cache);
    expect(repeated.is_ok(), "cached frame lookup succeeds");
    expect(cache.size() == 1U, "repeated identical seek reuses snapshot key");
    if (first.is_ok() && repeated.is_ok()) {
        expect(
            artminer::nodes::image_fingerprint(first.value()) == artminer::nodes::image_fingerprint(repeated.value()),
            "cached frame bytes equal uncached result");
    }

    auto changed = recipe;
    ++changed.root_seed;
    auto changed_frame = artminer::nodes::render_animation_reference(changed, 6U, "main", &cache);
    expect(changed_frame.is_ok(), "changed recipe renders through same cache object");
    expect(cache.size() == 2U, "semantic recipe change invalidates snapshot key");
    if (first.is_ok() && changed_frame.is_ok()) {
        expect(
            artminer::nodes::image_fingerprint(first.value()) != artminer::nodes::image_fingerprint(changed_frame.value()),
            "changed seed does not receive stale cached pixels");
    }

    const std::string expected = first.is_ok() ? artminer::nodes::image_fingerprint(first.value()) : std::string{};
    cache.clear();
    expect(cache.size() == 0U, "snapshot cache can be removed completely");
    auto uncached = artminer::nodes::render_animation_reference(recipe, 6U, "main", nullptr);
    expect(uncached.is_ok(), "render succeeds without any snapshot cache");
    if (uncached.is_ok()) {
        expect(
            artminer::nodes::image_fingerprint(uncached.value()) == expected,
            "cache removal affects performance only, never rendered state");
    }
}

void test_resource_limits() {
    auto feedback = make_feedback_recipe();
    auto too_far = artminer::nodes::render_animation_reference(feedback, artminer::nodes::kMaximumFeedbackTick + 1U);
    expect(too_far.is_error(), "feedback tick above explicit reconstruction limit is rejected");
    if (too_far.is_error()) {
        expect(
            too_far.error().code == artminer::nodes::MotionErrorCode::resource_limit,
            "feedback limit reports resource_limit");
    }
}

}  // namespace

int main() {
    test_boundary_validation_contract();
    test_particle_order_lifecycle_and_modes();
    test_frame_replay_reset_feedback_and_palette();
    test_play_step_and_speed_neutrality();
    test_snapshot_cache_is_disposable_and_keyed();
    test_resource_limits();

    if (g_failures != 0) {
        std::cerr << g_failures << " AM-007 motion test(s) failed\n";
        return 1;
    }
    std::cout << "ArtMiner AM-007 motion/feedback tests passed\n";
    return 0;
}
