#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

#include "core/graph.hpp"
#include "core/recipe.hpp"
#include "core/specimen_browser.hpp"
#include "platform/windows/browser_store.hpp"
#include "platform/windows/portable_workspace.hpp"

namespace {

int g_failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

[[nodiscard]] artminer::core::Recipe fixture() {
    constexpr std::string_view text = R"AMR(amr 1
evaluator 1
seed 321
render 64 64 reference
node radial core.scalar.radial 1
param radial center_x f64 0.45
param radial center_y f64 0.55
param radial scale f64 2.5
node threshold core.scalar.threshold 1
param threshold threshold f64 0.55
param threshold low f64 0
param threshold high f64 1
node image core.image.from_scalar 1
param image palette enum heat
edge radial value threshold source
edge threshold value image source
output main image value
)AMR";
    auto parsed = artminer::core::parse_recipe(text);
    if (parsed.is_error()) {
        ++g_failures;
        std::cerr << "FAIL: fixture parse: " << parsed.error().message << '\n';
        return {};
    }
    artminer::core::Recipe recipe = std::move(parsed).value();
    const auto errors = artminer::core::validate_recipe(recipe);
    expect(errors.empty(), "fixture validates");
    return recipe;
}

[[nodiscard]] const artminer::core::ParameterValue* parameter(
    const artminer::core::Recipe& recipe,
    const std::string_view node_id,
    const std::string_view name) {
    const auto node = std::find_if(recipe.nodes.begin(), recipe.nodes.end(), [node_id](const auto& value) {
        return value.id == node_id;
    });
    if (node == recipe.nodes.end()) {
        return nullptr;
    }
    const auto assignment = std::find_if(node->parameters.begin(), node->parameters.end(), [name](const auto& value) {
        return value.name == name;
    });
    return assignment == node->parameters.end() ? nullptr : &assignment->value;
}

void test_repeatable_parameter_mutation() {
    using namespace artminer::core;
    const Recipe parent = fixture();
    ParameterLocks locks;
    auto first = mutate_recipe_parameters(parent, 123456789ULL, kParameterMutationOperatorVersion, 0.61, locks);
    auto second = mutate_recipe_parameters(parent, 123456789ULL, kParameterMutationOperatorVersion, 0.61, locks);
    expect(first.is_ok() && second.is_ok(), "repeatable mutation succeeds");
    if (first.is_ok() && second.is_ok()) {
        expect(semantic_fingerprint(first.value()) == semantic_fingerprint(second.value()), "same mutation inputs produce same child fingerprint");
        expect(serialize_recipe_semantic(first.value()) == serialize_recipe_semantic(second.value()), "same mutation inputs produce identical semantic recipe");
        expect(validate_recipe(first.value()).empty(), "mutated child remains graph/parameter valid");
    }

    auto changed_seed = mutate_recipe_parameters(parent, 123456790ULL, kParameterMutationOperatorVersion, 0.61, locks);
    expect(changed_seed.is_ok(), "alternate mutation seed succeeds");
    if (first.is_ok() && changed_seed.is_ok()) {
        expect(semantic_fingerprint(first.value()) != semantic_fingerprint(changed_seed.value()), "different mutation seeds alter child semantics for fixture");
    }

    auto invalid_strength = mutate_recipe_parameters(parent, 1ULL, kParameterMutationOperatorVersion, 1.01, locks);
    expect(invalid_strength.is_error(), "out-of-range mutation strength is rejected");
    auto invalid_version = mutate_recipe_parameters(parent, 1ULL, 99U, 0.5, locks);
    expect(invalid_version.is_error(), "unsupported mutation operator version is rejected");
}

void test_parameter_and_group_locks() {
    using namespace artminer::core;
    const Recipe parent = fixture();
    ParameterLocks locks;
    locks.set_group("radial", "position", true);
    locks.set_parameter("radial", "scale", true);
    auto mutated = mutate_recipe_parameters(parent, 99887766ULL, kParameterMutationOperatorVersion, 1.0, locks);
    expect(mutated.is_ok(), "locked mutation succeeds");
    if (mutated.is_error()) {
        return;
    }
    for (const std::string_view name : {"center_x", "center_y", "scale"}) {
        const auto* before = parameter(parent, "radial", name);
        const auto* after = parameter(mutated.value(), "radial", name);
        expect(before != nullptr && after != nullptr && *before == *after, "locked radial parameter is preserved exactly");
    }
    expect(locks.group_count() == 1U && locks.parameter_count() == 1U, "lock state tracks parameter and group separately");

    locks.toggle_group("radial", "position");
    locks.toggle_parameter("radial", "scale");
    expect(locks.group_count() == 0U && locks.parameter_count() == 0U, "lock toggles are reversible");
}

void test_domains_and_metadata_driven_scales() {
    using namespace artminer::core;
    Recipe parent = fixture();
    ParameterLocks locks;
    for (u64 seed = 0U; seed < 256U; ++seed) {
        auto mutated = mutate_recipe_parameters(parent, seed, kParameterMutationOperatorVersion, 1.0, locks);
        expect(mutated.is_ok(), "domain stress mutation succeeds");
        if (mutated.is_error()) {
            return;
        }
        expect(validate_recipe(mutated.value()).empty(), "domain stress child validates");
        const auto* x = parameter(mutated.value(), "radial", "center_x");
        const auto* scale = parameter(mutated.value(), "radial", "scale");
        expect(x != nullptr && std::holds_alternative<double>(*x), "linear parameter preserves real type");
        expect(scale != nullptr && std::holds_alternative<double>(*scale), "log parameter preserves real type");
        if (x != nullptr && std::holds_alternative<double>(*x)) {
            const double value = std::get<double>(*x);
            expect(value >= -2.0 && value <= 3.0, "linear mutation stays inside declared domain");
        }
        if (scale != nullptr && std::holds_alternative<double>(*scale)) {
            const double value = std::get<double>(*scale);
            expect(value >= 0.001 && value <= 100.0, "log mutation stays inside declared domain");
        }
    }

    constexpr std::string_view angular_text = R"AMR(amr 1
evaluator 1
seed 4
render 16 16 reference
node angle core.scalar.angular 1
param angle center_x f64 0.5
param angle center_y f64 0.5
param angle turns f64 1
param angle phase f64 63.9
node image core.image.from_scalar 1
param image palette enum grayscale
edge angle value image source
output main image value
)AMR";
    auto parsed = parse_recipe(angular_text);
    expect(parsed.is_ok(), "periodic fixture parses");
    if (parsed.is_ok()) {
        for (u64 seed = 0U; seed < 64U; ++seed) {
            auto mutated = mutate_recipe_parameters(parsed.value(), seed, kParameterMutationOperatorVersion, 1.0, locks);
            expect(mutated.is_ok(), "periodic mutation succeeds");
            if (mutated.is_ok()) {
                const auto* phase = parameter(mutated.value(), "angle", "phase");
                expect(phase != nullptr && std::holds_alternative<double>(*phase), "periodic parameter remains real");
                if (phase != nullptr && std::holds_alternative<double>(*phase)) {
                    const double value = std::get<double>(*phase);
                    expect(value >= -64.0 && value <= 64.0, "periodic mutation wraps inside declared domain");
                }
            }
        }
    }
}

void test_seed_only_variation() {
    using namespace artminer::core;
    const Recipe parent = fixture();
    auto variant = make_seed_variant(parent, 7654321ULL);
    auto repeated = make_seed_variant(parent, 7654321ULL);
    expect(variant.is_ok() && repeated.is_ok(), "seed-only variation succeeds");
    if (variant.is_ok() && repeated.is_ok()) {
        expect(variant.value().root_seed != parent.root_seed, "seed-only variation changes root seed");
        expect(variant.value().root_seed == repeated.value().root_seed, "seed-only variation is repeatable");
        Recipe normalized = variant.value();
        normalized.root_seed = parent.root_seed;
        expect(serialize_recipe_semantic(normalized) == serialize_recipe_semantic(parent), "seed-only variation changes no parameter or topology semantics");
    }
}

void test_grid_order_is_stable() {
    using namespace artminer::core;
    const Recipe parent = fixture();
    ParameterLocks locks;
    auto first = generate_specimen_grid(parent, 42ULL, kParameterMutationOperatorVersion, 0.4, SpecimenGenerationMode::parameter_mutation, locks);
    auto second = generate_specimen_grid(parent, 42ULL, kParameterMutationOperatorVersion, 0.4, SpecimenGenerationMode::parameter_mutation, locks);
    expect(first.is_ok() && second.is_ok(), "deterministic 4x4 grid generation succeeds");
    if (first.is_ok() && second.is_ok()) {
        expect(first.value().size() == kSpecimenGridSize && second.value().size() == kSpecimenGridSize, "grid contains exactly sixteen specimens");
        for (std::size_t index = 0U; index < kSpecimenGridSize; ++index) {
            expect(first.value()[index].index == index, "slot identity follows stable row-major index");
            expect(first.value()[index].operation_seed == second.value()[index].operation_seed, "slot operation seed is stable");
            expect(first.value()[index].fingerprint == second.value()[index].fingerprint, "slot fingerprint is stable");
        }
    }

    auto seeds = generate_specimen_grid(parent, 42ULL, kParameterMutationOperatorVersion, 0.4, SpecimenGenerationMode::seed_only, locks);
    expect(seeds.is_ok() && seeds.value().size() == kSpecimenGridSize, "seed-only grid contains sixteen specimens");
    if (seeds.is_ok()) {
        std::set<u64> root_seeds;
        for (const auto& specimen : seeds.value()) {
            root_seeds.insert(specimen.recipe.root_seed);
        }
        expect(root_seeds.size() == kSpecimenGridSize, "seed-only grid has distinct deterministic root seeds for fixture");
    }
}

void test_source_order_cannot_change_mutation() {
    using namespace artminer::core;
    Recipe first = fixture();
    Recipe reordered = first;
    std::reverse(reordered.nodes.begin(), reordered.nodes.end());
    std::reverse(reordered.edges.begin(), reordered.edges.end());
    expect(semantic_fingerprint(first) == semantic_fingerprint(reordered), "canonical parent fingerprint ignores source ordering");
    ParameterLocks locks;
    auto left = mutate_recipe_parameters(first, 919191ULL, kParameterMutationOperatorVersion, 0.8, locks);
    auto right = mutate_recipe_parameters(reordered, 919191ULL, kParameterMutationOperatorVersion, 0.8, locks);
    expect(left.is_ok() && right.is_ok(), "reordered parents both mutate");
    if (left.is_ok() && right.is_ok()) {
        expect(semantic_fingerprint(left.value()) == semantic_fingerprint(right.value()), "mutation values do not depend on node/parameter traversal ordering");
    }
}

void test_history_navigation_preserves_recipes() {
    using namespace artminer::core;
    Recipe initial = fixture();
    RecipeHistory history(initial);
    const std::string initial_text = serialize_recipe_semantic(history.current());
    auto child_one = make_seed_variant(initial, 11ULL);
    auto child_two = make_seed_variant(initial, 22ULL);
    expect(child_one.is_ok() && child_two.is_ok(), "history fixtures generate");
    if (child_one.is_error() || child_two.is_error()) {
        return;
    }
    const std::string one_text = serialize_recipe_semantic(child_one.value());
    history.push(child_one.value());
    history.push(child_two.value());
    expect(history.size() == 3U && history.position() == 2U, "history push advances cursor");
    expect(history.back(), "history back from latest succeeds");
    expect(serialize_recipe_semantic(history.current()) == one_text, "history back restores exact recipe semantics");
    expect(history.back(), "history back to initial succeeds");
    expect(serialize_recipe_semantic(history.current()) == initial_text, "history returns exact initial semantics");
    expect(history.forward(), "history forward succeeds");
    history.push(initial);
    expect(!history.can_forward(), "new branch truncates stale forward history");
}

void test_parameter_edit_validation() {
    using namespace artminer::core;
    const Recipe parent = fixture();
    auto edited = set_parameter_from_text(parent, "radial", "center_x", "1.25");
    expect(edited.is_ok(), "valid textual parameter edit succeeds");
    if (edited.is_ok()) {
        const auto* value = parameter(edited.value(), "radial", "center_x");
        expect(value != nullptr && std::holds_alternative<double>(*value) && std::get<double>(*value) == 1.25, "parameter edit stores requested value");
        expect(validate_recipe(edited.value()).empty(), "edited recipe remains valid");
    }
    expect(set_parameter_from_text(parent, "radial", "center_x", "500").is_error(), "out-of-domain edit is rejected");
    expect(set_parameter_from_text(parent, "radial", "missing", "1").is_error(), "unknown parameter edit is rejected");
}

void test_workspace_persistence() {
    using namespace artminer::platform::windows;
    const auto root = std::filesystem::temp_directory_path() / L"artminer-am005-persistence-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    auto opened = PortableWorkspace::open(root);
    expect(opened.is_ok(), "AM-005 test workspace opens");
    if (opened.is_error()) {
        return;
    }
    PortableWorkspace workspace = std::move(opened).value();
    expect(workspace.ensure_layout().is_ok(), "AM-005 test workspace layout exists");
    const auto& layout = workspace.layout();
    const artminer::core::Recipe recipe = fixture();
    const std::string fingerprint = artminer::core::semantic_fingerprint(recipe);

    auto saved = save_recipe_copy(layout.recipes, recipe);
    auto favourite = save_favourite(layout.recipes, recipe);
    expect(saved.is_ok() && favourite.is_ok(), "selected and favourite recipes persist atomically");
    if (saved.is_ok()) {
        expect(std::filesystem::is_regular_file(saved.value()), "saved recipe survives as workspace file");
    }
    auto loaded = load_favourites(layout.recipes);
    expect(loaded.is_ok(), "persisted favourites reload");
    if (loaded.is_ok()) {
        expect(loaded.value().size() == 1U, "one favourite reloads after simulated restart");
        if (!loaded.value().empty()) {
            expect(loaded.value().front().fingerprint == fingerprint, "reloaded favourite retains semantic identity");
            expect(artminer::core::semantic_fingerprint(loaded.value().front().recipe) == fingerprint, "reloaded favourite retains exact recipe semantics");
        }
    }
    expect(remove_favourite(layout.recipes, fingerprint).is_ok(), "favourite removal persists");
    auto empty = load_favourites(layout.recipes);
    expect(empty.is_ok() && empty.value().empty(), "removed favourite does not reappear after reload");
    std::filesystem::remove_all(root, error);
}

}  // namespace

int main() {
    test_repeatable_parameter_mutation();
    test_parameter_and_group_locks();
    test_domains_and_metadata_driven_scales();
    test_seed_only_variation();
    test_grid_order_is_stable();
    test_source_order_cannot_change_mutation();
    test_history_navigation_preserves_recipes();
    test_parameter_edit_validation();
    test_workspace_persistence();

    if (g_failures != 0) {
        std::cerr << g_failures << " AM-005 assertion(s) failed.\n";
        return 1;
    }
    std::cout << "ArtMiner AM-005 specimen-browser tests passed.\n";
    return 0;
}
