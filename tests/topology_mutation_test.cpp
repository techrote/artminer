#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <tuple>

#include "core/breeding.hpp"
#include "core/graph.hpp"
#include "core/recipe.hpp"
#include "core/specimen_browser.hpp"
#include "core/topology_mutation.hpp"
#include "export/export.hpp"
#include "nodes/static_evaluator.hpp"
#include "quarry/quarry.hpp"

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
    const auto errors = artminer::core::validate_recipe(recipe);
    expect(errors.empty(), "fixture validates");
    return recipe;
}

[[nodiscard]] artminer::core::Recipe base_recipe() {
    return parse_fixture(R"AMR(amr 1
evaluator 1
seed 314159
render 48 48 reference
node radial core.scalar.radial 1
param radial center_x f64 0.42
param radial center_y f64 0.58
param radial scale f64 3
node threshold core.scalar.threshold 1
param threshold threshold f64 0.45
param threshold low f64 0
param threshold high f64 1
node image core.image.from_scalar 1
param image palette enum heat
edge radial value threshold source
edge threshold value image source
output main image value
)AMR");
}

using EdgeKey = std::tuple<std::string, std::string, std::string, std::string>;

[[nodiscard]] std::set<EdgeKey> incident_edges(
    const artminer::core::Recipe& recipe,
    const std::string_view node_id) {
    std::set<EdgeKey> result;
    for (const auto& edge : recipe.edges) {
        if (edge.from_node == node_id || edge.to_node == node_id) {
            result.emplace(edge.from_node, edge.from_port, edge.to_node, edge.to_port);
        }
    }
    return result;
}

[[nodiscard]] const artminer::core::NodeInstance* node(
    const artminer::core::Recipe& recipe,
    const std::string_view id) {
    const auto found = std::find_if(recipe.nodes.begin(), recipe.nodes.end(), [id](const auto& item) {
        return item.id == id;
    });
    return found == recipe.nodes.end() ? nullptr : &*found;
}

void test_catalog_determinism_validity_and_bounds() {
    using namespace artminer::core;
    const Recipe parent = base_recipe();
    const auto& catalog = topology_operator_catalog();
    expect(catalog.size() == 5U, "AM-014 catalog exposes the five conservative structural operator families");

    StructuralLocks locks;
    auto first = mutate_recipe_topology(parent, 99ULL, kTopologyMutationOperatorVersion, 1.0, 6U, locks);
    auto second = mutate_recipe_topology(parent, 99ULL, kTopologyMutationOperatorVersion, 1.0, 6U, locks);
    expect(first.is_ok() && second.is_ok(), "deterministic topology mutation succeeds");
    if (first.is_error() || second.is_error()) {
        return;
    }
    expect(validate_recipe(first.value().recipe).empty(), "accepted topology child passes the normal recipe validator");
    expect(serialize_recipe_semantic(first.value().recipe) == serialize_recipe_semantic(second.value().recipe),
        "same parent/version/seed/strength/budget produces identical child semantics");
    expect(semantic_fingerprint(first.value().recipe) == semantic_fingerprint(second.value().recipe),
        "same topology operation reproduces exact semantic fingerprint");
    expect(first.value().steps.size() <= 6U, "accepted edit count never exceeds explicit budget");
    expect(first.value().recipe.nodes.size() <= kMaximumTopologyNodes, "node growth stays within explicit topology limit");
    expect(first.value().recipe.edges.size() <= kMaximumTopologyEdges, "edge growth stays within explicit topology limit");

    Recipe reordered = parent;
    std::reverse(reordered.nodes.begin(), reordered.nodes.end());
    std::reverse(reordered.edges.begin(), reordered.edges.end());
    expect(semantic_fingerprint(reordered) == semantic_fingerprint(parent),
        "fixture semantic identity is independent of in-memory/UI ordering");
    auto reordered_child = mutate_recipe_topology(
        reordered, 99ULL, kTopologyMutationOperatorVersion, 1.0, 6U, locks);
    expect(reordered_child.is_ok(), "reordered parent topology mutation succeeds");
    if (reordered_child.is_ok()) {
        expect(semantic_fingerprint(reordered_child.value().recipe) ==
                semantic_fingerprint(first.value().recipe),
            "operator enumeration is canonical rather than UI-order dependent");
    }

    std::set<TopologyOperatorKind> observed;
    for (u64 seed = 0U; seed < 1024U && observed.size() < catalog.size(); ++seed) {
        auto candidate = mutate_recipe_topology(
            parent, seed, kTopologyMutationOperatorVersion, 0.25, 1U, locks);
        if (candidate.is_ok() && !candidate.value().steps.empty()) {
            observed.insert(candidate.value().steps.front().kind);
            expect(validate_recipe(candidate.value().recipe).empty(),
                "large deterministic mutation sample contains only normally valid children");
        }
    }
    expect(observed.size() == catalog.size(),
        "bounded deterministic seed sample exercises every catalogued topology operator family");
}

void test_structural_locks_and_subgraphs() {
    using namespace artminer::core;
    const Recipe parent = base_recipe();
    StructuralLocks locks;
    locks.set_node("threshold", true);
    const auto before_edges = incident_edges(parent, "threshold");
    const NodeInstance* before_node = node(parent, "threshold");
    expect(before_node != nullptr, "locked fixture node exists");

    for (u64 seed = 0U; seed < 128U; ++seed) {
        auto mutated = mutate_recipe_topology(
            parent, seed, kTopologyMutationOperatorVersion, 1.0, 4U, locks);
        if (mutated.is_error()) {
            expect(mutated.error().code == TopologyMutationErrorCode::no_legal_operation,
                "fully constrained seed may reject with the explicit no-legal-operation diagnostic");
            continue;
        }
        const NodeInstance* after_node = node(mutated.value().recipe, "threshold");
        bool same_definition = after_node != nullptr && before_node != nullptr &&
            after_node->type_id == before_node->type_id &&
            after_node->semantic_version == before_node->semantic_version &&
            after_node->parameters.size() == before_node->parameters.size();
        if (same_definition) {
            for (std::size_t index = 0U; index < before_node->parameters.size(); ++index) {
                same_definition = same_definition &&
                    after_node->parameters[index].name == before_node->parameters[index].name &&
                    after_node->parameters[index].value == before_node->parameters[index].value;
            }
        }
        expect(same_definition, "locked node definition is preserved exactly");
        expect(incident_edges(mutated.value().recipe, "threshold") == before_edges,
            "all edges incident to a structurally locked node are preserved");
    }

    StructuralLocks subgraph;
    subgraph.set_upstream_subgraph(parent, "threshold", true);
    expect(subgraph.node_locked("threshold") && subgraph.node_locked("radial"),
        "upstream-subgraph lock includes root and dependency closure");
    expect(!subgraph.node_locked("image"), "upstream-subgraph lock does not overreach downstream");
    const std::string encoded = subgraph.serialize_canonical();
    auto parsed = parse_structural_locks(encoded);
    expect(parsed.is_ok() && parsed.value().serialize_canonical() == encoded,
        "structural lock encoding round-trips canonically");
    expect(parse_structural_locks("n/threshold;n/radial").is_error(),
        "noncanonical structural-lock order is rejected rather than silently normalized");
}

void test_feedback_boundary_is_untouchable() {
    using namespace artminer::core;
    Recipe parent = parse_fixture(R"AMR(amr 1
evaluator 1
seed 1
render 32 32 reference
node delay core.state.delay.scalar 1
node pass core.scalar.pass 1
node radial core.scalar.radial 1
param radial center_x f64 0.5
param radial center_y f64 0.5
param radial scale f64 2
node image core.image.from_scalar 1
param image palette enum grayscale
edge delay previous pass source
edge pass value delay next
edge radial value image source
output main image value
output memory delay previous
)AMR");
    const auto delay_edges = incident_edges(parent, "delay");

    StructuralLocks locks;
    for (u64 seed = 10U; seed < 90U; ++seed) {
        auto mutated = mutate_recipe_topology(
            parent, seed, kTopologyMutationOperatorVersion, 1.0, 3U, locks);
        expect(mutated.is_ok(), "independent mutable branch permits topology mutation beside feedback boundary");
        if (mutated.is_ok()) {
            const NodeInstance* delay = node(mutated.value().recipe, "delay");
            expect(delay != nullptr && delay->type_id == "core.state.delay.scalar",
                "state-boundary node survives topology mutation exactly");
            expect(incident_edges(mutated.value().recipe, "delay") == delay_edges,
                "edges crossing explicit feedback boundary are never inserted/deleted/rewired");
            expect(validate_recipe(mutated.value().recipe).empty(),
                "feedback-bearing topology child remains valid");
        }
    }

    Recipe boundary_only = parse_fixture(R"AMR(amr 1
evaluator 1
seed 2
render 16 16 reference
node delay core.state.delay.scalar 1
node pass core.scalar.pass 1
edge delay previous pass source
edge pass value delay next
output memory delay previous
)AMR");
    auto rejected = mutate_recipe_topology(
        boundary_only, 1ULL, kTopologyMutationOperatorVersion, 1.0, 1U, locks);
    expect(rejected.is_error() && rejected.error().code == TopologyMutationErrorCode::no_legal_operation,
        "recipe whose only structure crosses a state boundary rejects deterministically");
}

void test_lineage_diff_specimens_and_malformed_provenance() {
    using namespace artminer::core;
    const Recipe parent = base_recipe();
    StructuralLocks locks;
    locks.set_node("image", true);
    auto mutation = mutate_recipe_topology(
        parent, 424242ULL, kTopologyMutationOperatorVersion, 0.75, 4U, locks);
    expect(mutation.is_ok(), "topology lineage fixture mutation succeeds");
    if (mutation.is_error()) {
        return;
    }

    auto embedded = lineage_record_from_recipe(mutation.value().recipe);
    expect(embedded.is_ok() && embedded.value().kind == LineageOperationKind::topology_mutation,
        "topology mutation exposes immediate lineage through the common lineage API");
    if (embedded.is_ok()) {
        const std::string encoded = serialize_lineage_record(embedded.value());
        auto decoded = parse_lineage_record(encoded);
        expect(decoded.is_ok(), "topology lineage AML v2 serialization parses");
        if (decoded.is_ok()) {
            expect(serialize_lineage_record(decoded.value()) == encoded,
                "topology lineage canonical serialization round-trips");
            auto replayed = replay_lineage_record(decoded.value(), parent);
            expect(replayed.is_ok(), "topology lineage replay succeeds");
            if (replayed.is_ok()) {
                expect(semantic_fingerprint(replayed.value()) == semantic_fingerprint(mutation.value().recipe),
                    "topology lineage replay reproduces exact child fingerprint");
            }
        }
    }

    const RecipeDiff diff = diff_recipes(parent, mutation.value().recipe);
    expect(!diff.semantic.empty(), "structural mutation is visible in stable semantic recipe diff");
    expect(!diff.provenance.empty(), "topology operation evidence is visible in provenance diff");

    auto grid = generate_topology_specimen_grid(
        parent, 123ULL, kTopologyMutationOperatorVersion, 0.5, 2U, locks);
    expect(grid.is_ok() && grid.value().size() == kSpecimenGridSize,
        "specimen browser core produces a deterministic 4x4 topology grid");
    if (grid.is_ok()) {
        auto again = generate_topology_specimen_grid(
            parent, 123ULL, kTopologyMutationOperatorVersion, 0.5, 2U, locks);
        expect(again.is_ok(), "topology specimen grid replay succeeds");
        if (again.is_ok()) {
            for (std::size_t index = 0U; index < grid.value().size(); ++index) {
                expect(grid.value()[index].fingerprint == again.value()[index].fingerprint,
                    "topology specimen ordering and identity are deterministic");
            }
        }
    }

    LineageRecord malformed = make_topology_mutation_lineage_record(
        mutation.value().recipe,
        parent,
        424242ULL,
        kTopologyMutationOperatorVersion,
        0.75,
        4U,
        locks);
    malformed.structural_locks = "n/image;";
    auto malformed_replay = replay_lineage_record(malformed, parent);
    expect(malformed_replay.is_error(), "malformed structural-lock provenance fails closed");
}

void test_render_export_and_quarry_identity() {
    using namespace artminer;
    const core::Recipe parent = base_recipe();
    core::StructuralLocks locks;
    auto mutation = core::mutate_recipe_topology(
        parent, 98765ULL, core::kTopologyMutationOperatorVersion, 0.5, 2U, locks);
    expect(mutation.is_ok(), "render/export fixture topology mutation succeeds");
    if (mutation.is_error()) {
        return;
    }
    auto rendered = nodes::render_reference(mutation.value().recipe);
    expect(rendered.is_ok(), "topology-mutated specimen renders through canonical evaluator");

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / L"artminer-am014-topology-export";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    exporting::ExportRequest request;
    request.kind = exporting::ExportKind::still;
    request.raster_format = exporting::RasterFormat::raw_rgba;
    request.destination_directory = root;
    request.stem = "topology";
    auto exported = exporting::export_recipe(mutation.value().recipe, request);
    expect(exported.is_ok(), "topology-mutated specimen uses the normal transactional export path");
    if (exported.is_ok()) {
        expect(exported.value().recipe_fingerprint == core::semantic_fingerprint(mutation.value().recipe),
            "export provenance carries topology child's semantic fingerprint");
    }
    std::filesystem::remove_all(root, error);

    auto manifest = quarry::make_job_manifest(parent, 555ULL, 32ULL, 32U, 32U, 0.6);
    expect(manifest.is_ok(), "Quarry manifest fixture succeeds");
    if (manifest.is_error()) {
        return;
    }
    quarry::CandidateGenerationSettings settings;
    settings.mode = quarry::CandidateMutationMode::topology;
    settings.topology_budget = 3U;
    settings.structural_locks.set_node("image", true);
    auto q1 = quarry::reconstruct_candidate_recipe(manifest.value(), 7ULL, settings);
    auto q2 = quarry::reconstruct_candidate_recipe(manifest.value(), 7ULL, settings);
    expect(q1.is_ok() && q2.is_ok(), "Quarry opt-in topology candidate reconstruction succeeds");
    if (q1.is_ok() && q2.is_ok()) {
        expect(core::semantic_fingerprint(q1.value()) == core::semantic_fingerprint(q2.value()),
            "Quarry topology candidate identity is deterministic");
        expect(core::validate_recipe(q1.value()).empty(), "Quarry topology candidate remains valid");
    }
    quarry::CandidateGenerationSettings parameter_settings;
    expect(quarry::candidate_generation_identity(manifest.value(), settings) !=
            quarry::candidate_generation_identity(manifest.value(), parameter_settings),
        "Quarry generation identity separates topology search from parameter mutation");
}

void test_resource_limit_rejection() {
    using namespace artminer::core;
    Recipe huge;
    huge.root_seed = 1U;
    huge.render.width = 8U;
    huge.render.height = 8U;
    for (std::size_t index = 0U; index < kMaximumTopologyNodes + 1U; ++index) {
        NodeInstance instance;
        instance.id = "n" + std::to_string(index);
        instance.type_id = "core.scalar.constant";
        instance.parameters.push_back(ParameterAssignment{"value", 0.0});
        huge.nodes.push_back(std::move(instance));
    }
    huge.outputs.push_back(OutputBinding{"main", "n0", "value"});
    expect(validate_recipe(huge).empty(), "oversized topology fixture is otherwise a valid recipe");
    StructuralLocks locks;
    auto rejected = mutate_recipe_topology(
        huge, 1ULL, kTopologyMutationOperatorVersion, 1.0, 1U, locks);
    expect(rejected.is_error() && rejected.error().code == TopologyMutationErrorCode::resource_limit,
        "valid parent beyond explicit graph-size safety bound is rejected with resource-limit diagnostic");
}

}  // namespace

int main() {
    test_catalog_determinism_validity_and_bounds();
    test_structural_locks_and_subgraphs();
    test_feedback_boundary_is_untouchable();
    test_lineage_diff_specimens_and_malformed_provenance();
    test_render_export_and_quarry_identity();
    test_resource_limit_rejection();
    if (g_failures != 0) {
        std::cerr << g_failures << " AM-014 topology mutation test(s) failed\n";
        return 1;
    }
    std::cout << "AM-014 topology mutation tests passed\n";
    return 0;
}
