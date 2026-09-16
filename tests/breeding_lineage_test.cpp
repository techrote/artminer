#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "core/breeding.hpp"
#include "core/graph.hpp"
#include "core/recipe.hpp"
#include "core/specimen_browser.hpp"
#include "nodes/static_evaluator.hpp"
#include "platform/windows/lineage_store.hpp"

namespace {

int g_failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

[[nodiscard]] artminer::core::Recipe parse_fixture(const std::string_view text) {
    auto parsed = artminer::core::parse_recipe(text);
    if (parsed.is_error()) {
        ++g_failures;
        std::cerr << "FAIL: fixture parse: " << parsed.error().message << '\n';
        return {};
    }
    artminer::core::Recipe recipe = std::move(parsed).value();
    expect(artminer::core::validate_recipe(recipe).empty(), "fixture validates");
    return recipe;
}

[[nodiscard]] artminer::core::Recipe parent_a() {
    return parse_fixture(R"AMR(amr 1
evaluator 1
seed 321
render 64 64 reference
node radial core.scalar.radial 1
param radial center_x f64 0.2
param radial center_y f64 0.3
param radial scale f64 2
node threshold core.scalar.threshold 1
param threshold threshold f64 0.4
param threshold low f64 0
param threshold high f64 1
node image core.image.from_scalar 1
param image palette enum heat
edge radial value threshold source
edge threshold value image source
output main image value
)AMR");
}

[[nodiscard]] artminer::core::Recipe parent_b() {
    return parse_fixture(R"AMR(amr 1
evaluator 1
seed 654
render 64 64 reference
node radial core.scalar.radial 1
param radial center_x f64 0.8
param radial center_y f64 0.7
param radial scale f64 6
node threshold core.scalar.threshold 1
param threshold threshold f64 0.72
param threshold low f64 0.1
param threshold high f64 0.9
node image core.image.from_scalar 1
param image palette enum grayscale
edge radial value threshold source
edge threshold value image source
output main image value
)AMR");
}

[[nodiscard]] const artminer::core::ParameterValue* parameter(
    const artminer::core::Recipe& recipe,
    const std::string_view node_id,
    const std::string_view name) {
    const auto node = std::find_if(recipe.nodes.begin(), recipe.nodes.end(), [node_id](const auto& item) {
        return item.id == node_id;
    });
    if (node == recipe.nodes.end()) {
        return nullptr;
    }
    const auto assignment = std::find_if(node->parameters.begin(), node->parameters.end(), [name](const auto& item) {
        return item.name == name;
    });
    return assignment == node->parameters.end() ? nullptr : &assignment->value;
}

void test_deterministic_crossover_and_locks() {
    using namespace artminer::core;
    const Recipe a = parent_a();
    const Recipe b = parent_b();
    ParameterLocks locks;
    locks.set_group("radial", "position", true);
    locks.set_parameter("threshold", "low", true);

    auto first = crossover_recipes(a, b, 123456789ULL, kCrossoverOperatorVersion, locks);
    auto second = crossover_recipes(a, b, 123456789ULL, kCrossoverOperatorVersion, locks);
    expect(first.is_ok() && second.is_ok(), "compatible two-parent crossover succeeds");
    if (first.is_error() || second.is_error()) {
        return;
    }
    expect(validate_recipe(first.value()).empty(), "crossover child remains graph-valid");
    expect(serialize_recipe_semantic(first.value()) == serialize_recipe_semantic(second.value()),
        "same ordered parents seed operator and locks reproduce identical child semantics");
    expect(semantic_fingerprint(first.value()) == semantic_fingerprint(second.value()),
        "same crossover inputs reproduce identical child fingerprint");
    expect(first.value().root_seed != a.root_seed && first.value().root_seed != b.root_seed,
        "crossover derives a deterministic child seed from ordered ancestry");

    for (const std::string_view name : {"center_x", "center_y"}) {
        const auto* before = parameter(a, "radial", name);
        const auto* after = parameter(first.value(), "radial", name);
        expect(before != nullptr && after != nullptr && *before == *after,
            "locked logical group is inherited exactly from parent A");
    }
    const auto* low_before = parameter(a, "threshold", "low");
    const auto* low_after = parameter(first.value(), "threshold", "low");
    expect(low_before != nullptr && low_after != nullptr && *low_before == *low_after,
        "locked individual parameter is inherited exactly from parent A");

    ParameterLocks unlocked;
    auto grouped = crossover_recipes(a, b, 77ULL, kCrossoverOperatorVersion, unlocked);
    expect(grouped.is_ok(), "unlocked crossover succeeds");
    if (grouped.is_ok()) {
        const bool x_from_a = *parameter(grouped.value(), "radial", "center_x") == *parameter(a, "radial", "center_x");
        const bool y_from_a = *parameter(grouped.value(), "radial", "center_y") == *parameter(a, "radial", "center_y");
        expect(x_from_a == y_from_a, "parameters in one mutation group inherit from the same parent role");
    }

    auto reversed = crossover_recipes(b, a, 123456789ULL, kCrossoverOperatorVersion, locks);
    expect(reversed.is_ok(), "reversed ordered parents are also compatible");
    if (reversed.is_ok()) {
        expect(semantic_fingerprint(reversed.value()) != semantic_fingerprint(first.value()),
            "parent A/B roles are ordered crossover semantics");
    }
}

void test_incompatible_parents_rejected() {
    using namespace artminer::core;
    Recipe a = parent_a();
    Recipe b = parent_b();
    b.render.width = 96U;
    ParameterLocks locks;
    auto result = crossover_recipes(a, b, 1ULL, kCrossoverOperatorVersion, locks);
    expect(result.is_error(), "structurally incompatible parent pair is rejected");
    if (result.is_error()) {
        expect(result.error().code == CrossoverErrorCode::incompatible_parents,
            "incompatible valid parents report the crossover compatibility error");
    }
}

void test_provenance_is_nonsemantic() {
    using namespace artminer::core;
    using namespace artminer::nodes;
    const Recipe a = parent_a();
    const Recipe b = parent_b();
    ParameterLocks locks;
    auto crossed = crossover_recipes(a, b, 99ULL, kCrossoverOperatorVersion, locks);
    expect(crossed.is_ok(), "provenance fixture crossover succeeds");
    if (crossed.is_error()) {
        return;
    }
    Recipe stripped = crossed.value();
    std::erase_if(stripped.metadata, [](const RecipeMetadata& item) {
        return item.key.starts_with("artminer.crossover.");
    });
    expect(semantic_fingerprint(stripped) == semantic_fingerprint(crossed.value()),
        "removing crossover provenance does not change semantic fingerprint");
    auto image_a = render_reference(crossed.value());
    auto image_b = render_reference(stripped);
    expect(image_a.is_ok() && image_b.is_ok(), "provenance and stripped recipes both render");
    if (image_a.is_ok() && image_b.is_ok()) {
        expect(image_fingerprint(image_a.value()) == image_fingerprint(image_b.value()),
            "removing lineage metadata does not change canonical pixels");
    }
}

void test_lineage_roundtrip_and_replay() {
    using namespace artminer::core;
    const Recipe a = parent_a();
    const Recipe b = parent_b();
    ParameterLocks locks;
    locks.set_parameter("threshold", "high", true);
    auto crossed = crossover_recipes(a, b, 424242ULL, kCrossoverOperatorVersion, locks);
    expect(crossed.is_ok(), "lineage fixture crossover succeeds");
    if (crossed.is_error()) {
        return;
    }

    const LineageRecord record = make_crossover_lineage_record(
        crossed.value(), a, b, 424242ULL, kCrossoverOperatorVersion, locks);
    const std::string encoded = serialize_lineage_record(record);
    auto parsed = parse_lineage_record(encoded);
    expect(parsed.is_ok(), "lineage record has deterministic parseable serialization");
    if (parsed.is_ok()) {
        expect(serialize_lineage_record(parsed.value()) == encoded, "lineage record canonical round-trip is stable");
        auto replayed = replay_lineage_record(parsed.value(), a, &b);
        expect(replayed.is_ok(), "crossover lineage record replays when both parents are available");
        if (replayed.is_ok()) {
            expect(semantic_fingerprint(replayed.value()) == semantic_fingerprint(crossed.value()),
                "replayed crossover reproduces exact child semantics");
        }
    }

    auto embedded = lineage_record_from_recipe(crossed.value());
    expect(embedded.is_ok(), "crossover recipe exposes non-semantic immediate lineage hints");
    if (embedded.is_ok()) {
        auto replayed = replay_lineage_record(embedded.value(), a, &b);
        expect(replayed.is_ok(), "embedded crossover lineage hints are sufficient for replay");
    }
}

void test_subsequent_mutation_record_replays_locks() {
    using namespace artminer::core;
    const Recipe a = parent_a();
    const Recipe b = parent_b();
    ParameterLocks cross_locks;
    auto crossed = crossover_recipes(a, b, 777ULL, kCrossoverOperatorVersion, cross_locks);
    expect(crossed.is_ok(), "mutation ancestry crossover succeeds");
    if (crossed.is_error()) {
        return;
    }

    ParameterLocks mutation_locks;
    mutation_locks.set_group("radial", "position", true);
    mutation_locks.set_parameter("threshold", "threshold", true);
    auto mutated = mutate_recipe_parameters(
        crossed.value(), 888ULL, kParameterMutationOperatorVersion, 0.9, mutation_locks);
    expect(mutated.is_ok(), "post-crossover deterministic mutation succeeds");
    if (mutated.is_error()) {
        return;
    }
    const LineageRecord mutation_record = make_parameter_mutation_lineage_record(
        mutated.value(), crossed.value(), 888ULL, kParameterMutationOperatorVersion, 0.9, mutation_locks);
    auto replayed = replay_lineage_record(mutation_record, crossed.value());
    expect(replayed.is_ok(), "mutation lineage record includes locks needed for replay");
    if (replayed.is_ok()) {
        expect(semantic_fingerprint(replayed.value()) == semantic_fingerprint(mutated.value()),
            "post-crossover mutation replay reproduces exact child");
    }
}

void test_recipe_diff_is_stable_and_separates_provenance() {
    using namespace artminer::core;
    Recipe a = parent_a();
    Recipe provenance = a;
    provenance.metadata.push_back({"example.note", "lineage-only"});
    const RecipeDiff provenance_diff = diff_recipes(a, provenance);
    expect(provenance_diff.semantic.empty(), "provenance-only edit has no semantic diff entries");
    expect(!provenance_diff.provenance.empty(), "provenance-only edit is visible in provenance diff section");

    auto edited = set_parameter_from_text(a, "radial", "scale", "9");
    expect(edited.is_ok(), "semantic diff fixture edit succeeds");
    if (edited.is_ok()) {
        const RecipeDiff first = diff_recipes(a, edited.value());
        const RecipeDiff second = diff_recipes(a, edited.value());
        expect(!first.semantic.empty(), "semantic edit produces semantic diff entries");
        expect(format_recipe_diff(first) == format_recipe_diff(second), "formatted recipe diff is deterministic");
        expect(first.semantic.front().path <= first.semantic.back().path, "semantic diff is stable path-ordered");
    }
}

void test_portable_store_survives_missing_parent() {
    using namespace artminer::core;
    using namespace artminer::platform::windows;
    using namespace artminer::nodes;
    const Recipe a = parent_a();
    const Recipe b = parent_b();
    ParameterLocks locks;
    auto crossed = crossover_recipes(a, b, 2026ULL, kCrossoverOperatorVersion, locks);
    expect(crossed.is_ok(), "persistence crossover succeeds");
    if (crossed.is_error()) {
        return;
    }
    const LineageRecord record = make_crossover_lineage_record(
        crossed.value(), a, b, 2026ULL, kCrossoverOperatorVersion, locks);

    const std::filesystem::path root = std::filesystem::temp_directory_path() / L"artminer-am008-lineage-test";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    auto saved_a = save_lineage_specimen(root, a);
    auto saved_b = save_lineage_specimen(root, b);
    auto saved_child = save_lineage_specimen(root, crossed.value());
    auto saved_record = save_lineage_record(root, record);
    expect(saved_a.is_ok() && saved_b.is_ok() && saved_child.is_ok() && saved_record.is_ok(),
        "portable lineage store persists parents child and operation record");

    auto records = load_lineage_records(root);
    auto specimens = load_lineage_specimens(root);
    expect(records.is_ok() && records.value().size() == 1U, "portable lineage record reload succeeds");
    expect(specimens.is_ok() && specimens.value().size() == 3U, "portable lineage specimen reload succeeds");

    if (saved_a.is_ok()) {
        std::filesystem::remove(saved_a.value(), cleanup_error);
    }
    auto child_after_loss = load_lineage_specimen(root, semantic_fingerprint(crossed.value()));
    auto record_after_loss = load_lineage_records(root);
    expect(child_after_loss.is_ok(), "missing parent snapshot does not prevent child recipe loading");
    expect(record_after_loss.is_ok() && record_after_loss.value().size() == 1U,
        "missing parent snapshot leaves durable lineage record inspectable");
    if (child_after_loss.is_ok()) {
        auto rendered = render_reference(child_after_loss.value().recipe);
        expect(rendered.is_ok(), "child remains independently renderable when an ancestor snapshot is missing");
    }
    auto missing_parent = load_lineage_specimen(root, semantic_fingerprint(a));
    expect(missing_parent.is_error() && missing_parent.error().code == LineageStoreErrorCode::not_found,
        "missing ancestry degrades to an explicit unavailable provenance reference");
    std::filesystem::remove_all(root, cleanup_error);
}

}  // namespace

int main() {
    test_deterministic_crossover_and_locks();
    test_incompatible_parents_rejected();
    test_provenance_is_nonsemantic();
    test_lineage_roundtrip_and_replay();
    test_subsequent_mutation_record_replays_locks();
    test_recipe_diff_is_stable_and_separates_provenance();
    test_portable_store_survives_missing_parent();
    if (g_failures != 0) {
        std::cerr << g_failures << " AM-008 breeding/lineage test(s) failed\n";
        return 1;
    }
    std::cout << "AM-008 breeding/lineage tests passed\n";
    return 0;
}
