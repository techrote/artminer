#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "core/breeding.hpp"
#include "core/recipe.hpp"
#include "core/topology_lineage.hpp"
#include "core/topology_mutation.hpp"
#include "core/topology_specimen.hpp"
#include "export/export.hpp"
#include "nodes/static_evaluator.hpp"
#include "quarry/quarry.hpp"
#include "quarry/topology_search.hpp"

namespace {

using artminer::core::Recipe;

[[nodiscard]] Recipe load_recipe(const std::filesystem::path& path, int& failures) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    auto parsed = artminer::core::parse_recipe(buffer.str());
    if (!input || parsed.is_error()) {
        std::cerr << "FAIL: could not load test recipe " << path.string() << '\n';
        ++failures;
        return {};
    }
    return std::move(parsed).value();
}

[[nodiscard]] std::vector<std::tuple<std::string, std::string, std::string, std::string>> incident_edges(
    const Recipe& recipe,
    const std::string& node_id) {
    std::vector<std::tuple<std::string, std::string, std::string, std::string>> result;
    for (const auto& edge : recipe.edges) {
        if (edge.from_node == node_id || edge.to_node == node_id) {
            result.emplace_back(edge.from_node, edge.from_port, edge.to_node, edge.to_port);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

}  // namespace

int main() {
    using namespace artminer;
    int failures = 0;
    const auto expect = [&](const bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    };

    const std::filesystem::path root = ARTMINER_SOURCE_DIR;
    Recipe minimal = load_recipe(root / "examples" / "am002-minimal.amr", failures);
    expect(core::validate_recipe(minimal).empty(), "minimal fixture must validate");

    const auto& catalog = core::topology_operator_catalog();
    expect(catalog.size() == 5U, "topology catalog must contain the five AM-014 operator families");
    expect(catalog[0].kind == core::TopologyOperatorKind::insert_node, "catalog insertion operator missing");
    expect(catalog[1].kind == core::TopologyOperatorKind::replace_node, "catalog replacement operator missing");
    expect(catalog[2].kind == core::TopologyOperatorKind::delete_bypass, "catalog delete/bypass operator missing");
    expect(catalog[3].kind == core::TopologyOperatorKind::duplicate_branch, "catalog branch duplication operator missing");
    expect(catalog[4].kind == core::TopologyOperatorKind::rewire_edge, "catalog rewire operator missing");

    core::TopologyMutationOptions options;
    options.seed = 140014U;
    options.budget = 3U;
    core::StructuralLocks no_locks;
    auto first = core::mutate_recipe_topology(minimal, options, no_locks);
    auto second = core::mutate_recipe_topology(minimal, options, no_locks);
    expect(first.is_ok() && second.is_ok(), "same deterministic topology mutation should succeed twice");
    if (first.is_ok() && second.is_ok()) {
        expect(core::semantic_fingerprint(first.value().recipe) == core::semantic_fingerprint(second.value().recipe),
            "same parent/version/seed/budget must reproduce the same child");
        expect(first.value().steps.size() == second.value().steps.size(), "repeated mutation trace length must match");
        expect(!first.value().steps.empty() && first.value().steps.size() <= options.budget,
            "topology mutation must accept one to budget edits");
        expect(core::validate_recipe(first.value().recipe).empty(), "accepted topology child must pass normal graph validation");
        expect(first.value().recipe.nodes.size() <= options.limits.max_nodes &&
                first.value().recipe.edges.size() <= options.limits.max_edges &&
                core::topology_depth(first.value().recipe) <= options.limits.max_depth,
            "accepted topology child must remain inside configured bounds");

        const core::RecipeDiff diff = core::diff_recipes(minimal, first.value().recipe);
        expect(!diff.semantic.empty(), "topology mutation must appear in ordinary semantic recipe diff");

        auto lineage = core::topology_lineage_record_from_recipe(first.value().recipe);
        expect(lineage.is_ok(), "topology child must expose durable lineage");
        if (lineage.is_ok()) {
            const std::string encoded = core::serialize_topology_lineage_record(lineage.value());
            auto decoded = core::parse_topology_lineage_record(encoded);
            expect(decoded.is_ok(), "topology lineage must round-trip");
            if (decoded.is_ok()) {
                auto replayed = core::replay_topology_lineage_record(decoded.value(), minimal);
                expect(replayed.is_ok(), "topology lineage replay must succeed");
                if (replayed.is_ok()) {
                    expect(core::semantic_fingerprint(replayed.value()) == core::semantic_fingerprint(first.value().recipe),
                        "topology lineage replay must reproduce child identity");
                }
            }
        }

        Recipe reordered = minimal;
        std::reverse(reordered.nodes.begin(), reordered.nodes.end());
        std::reverse(reordered.edges.begin(), reordered.edges.end());
        auto reordered_child = core::mutate_recipe_topology(reordered, options, no_locks);
        expect(reordered_child.is_ok(), "UI/storage order-independent topology mutation should succeed");
        if (reordered_child.is_ok()) {
            expect(core::semantic_fingerprint(reordered_child.value().recipe) == core::semantic_fingerprint(first.value().recipe),
                "canonical topology mutation must be independent of node/edge storage order");
        }
    }

    // A whole-subgraph structural lock freezes every descendant and therefore
    // leaves no legal mutation in this two-node fixture.
    core::StructuralLocks frozen;
    expect(frozen.lock_subgraph(minimal, "source"), "subgraph structural lock root should resolve");
    expect(frozen.size() == minimal.nodes.size(), "subgraph lock should include all reachable descendants");
    auto blocked = core::mutate_recipe_topology(minimal, options, frozen);
    expect(blocked.is_error() && blocked.error().code == core::TopologyMutationErrorCode::no_legal_mutation,
        "fully locked graph must reject structural mutation deterministically");

    core::TopologyMutationOptions too_small = options;
    too_small.limits.max_nodes = 1U;
    auto bounded = core::mutate_recipe_topology(minimal, too_small, no_locks);
    expect(bounded.is_error() && bounded.error().code == core::TopologyMutationErrorCode::resource_limit,
        "parent beyond configured node bound must fail before mutation");

    // Property-style deterministic coverage across many seeds: every accepted
    // edit must validate and stay bounded, and repeating a seed is identical.
    std::size_t accepted = 0U;
    for (core::u64 seed = 0U; seed < 64U; ++seed) {
        core::TopologyMutationOptions sample = options;
        sample.seed = seed;
        sample.budget = 2U;
        auto left = core::mutate_recipe_topology(minimal, sample, no_locks);
        auto right = core::mutate_recipe_topology(minimal, sample, no_locks);
        expect(left.is_error() == right.is_error(), "topology mutation success/failure must be seed-repeatable");
        if (left.is_ok() && right.is_ok()) {
            ++accepted;
            expect(core::validate_recipe(left.value().recipe).empty(), "property topology child must validate");
            expect(core::semantic_fingerprint(left.value().recipe) == core::semantic_fingerprint(right.value().recipe),
                "property topology child must replay from same seed");
            expect(left.value().recipe.nodes.size() <= sample.limits.max_nodes &&
                    left.value().recipe.edges.size() <= sample.limits.max_edges &&
                    core::topology_depth(left.value().recipe) <= sample.limits.max_depth,
                "property topology child must remain bounded");
        }
    }
    expect(accepted > 0U, "property topology sweep must find accepted mutations");

    // Explicit specimen-browser integration remains separate from ordinary
    // parameter mutation and returns normal complete recipe specimens.
    auto grid = core::generate_topology_specimen_grid(
        minimal, 0x140014ULL, core::kTopologyMutationOperatorVersion, 1U, no_locks);
    expect(grid.is_ok(), "topology specimen grid must generate");
    if (grid.is_ok()) {
        expect(grid.value().size() == core::kSpecimenGridSize, "topology specimen grid must remain 4x4");
        for (const auto& specimen : grid.value()) {
            expect(core::validate_recipe(specimen.recipe).empty(), "topology specimen must validate normally");
            expect(specimen.fingerprint == core::semantic_fingerprint(specimen.recipe),
                "topology specimen fingerprint must be canonical recipe identity");
        }
    }

    // State-boundary node and every incident feedback edge are protected even
    // without an explicit user lock.
    Recipe feedback = load_recipe(root / "examples" / "am007-feedback-trails.amr", failures);
    const auto before_history_edges = incident_edges(feedback, "history");
    core::TopologyMutationOptions feedback_options = options;
    feedback_options.seed = 7714U;
    feedback_options.budget = 2U;
    auto feedback_child = core::mutate_recipe_topology(feedback, feedback_options, no_locks);
    expect(feedback_child.is_ok(), "feedback fixture should have legal edits outside its state boundary");
    if (feedback_child.is_ok()) {
        const auto* history = std::find_if(
            feedback_child.value().recipe.nodes.begin(), feedback_child.value().recipe.nodes.end(),
            [](const core::NodeInstance& node) { return node.id == "history"; });
        expect(history != feedback_child.value().recipe.nodes.end() && history->type_id == "core.state.delay.image",
            "state boundary node must survive topology mutation unchanged");
        expect(incident_edges(feedback_child.value().recipe, "history") == before_history_edges,
            "state-boundary incident edges must remain unchanged");
        expect(core::validate_recipe(feedback_child.value().recipe).empty(), "feedback topology child must validate");
    }

    // Malformed lineage/provenance is rejected rather than guessed.
    const std::string malformed_lineage =
        "aml-topology 1\nchild 00000000000000000000000000000000\n"
        "parent 00000000000000000000000000000000\noperator 1\nseed 1\n"
        "limits 128/256/64\nlocks -\ntrace insert@test\n";
    expect(core::parse_topology_lineage_record(malformed_lineage).is_error(),
        "topology lineage missing budget must fail clearly");

    // Quarry structural search is a separately serialized opt-in mode. Candidate
    // identity includes the topology config rather than masquerading as ordinary
    // numeric mutation identity.
    auto manifest_result = quarry::make_job_manifest(minimal, 14014U, 16U, 64U, 64U);
    expect(manifest_result.is_ok(), "base Quarry manifest should build");
    if (manifest_result.is_ok()) {
        quarry::TopologySearchConfig config;
        config.budget = 1U;
        const std::string encoded = quarry::serialize_topology_search_config(config);
        auto parsed_config = quarry::parse_topology_search_config(encoded);
        expect(parsed_config.is_ok(), "Quarry topology-search config must round-trip");
        if (parsed_config.is_ok()) {
            auto candidate_a = quarry::reconstruct_topology_candidate(manifest_result.value(), 3U, parsed_config.value());
            auto candidate_b = quarry::reconstruct_topology_candidate(manifest_result.value(), 3U, parsed_config.value());
            expect(candidate_a.is_ok() && candidate_b.is_ok(), "Quarry topology candidate must reconstruct");
            if (candidate_a.is_ok() && candidate_b.is_ok()) {
                expect(candidate_a.value().candidate_id == candidate_b.value().candidate_id &&
                        candidate_a.value().recipe_fingerprint == candidate_b.value().recipe_fingerprint,
                    "Quarry topology candidate identity must be deterministic");
                expect(core::validate_recipe(candidate_a.value().recipe).empty(),
                    "Quarry topology candidate must validate normally");
            }
        }
    }

    // Representative accepted topology children remain ordinary recipes for the
    // canonical renderer and transactional exporter. Search a bounded deterministic
    // seed interval because not every generic typed node belongs to this minimal
    // evaluator family.
    bool rendered_and_exported = false;
    const std::filesystem::path export_dir =
        std::filesystem::temp_directory_path() / "artminer-am014-topology-export";
    std::error_code cleanup_error;
    std::filesystem::remove_all(export_dir, cleanup_error);
    for (core::u64 seed = 100U; seed < 164U && !rendered_and_exported; ++seed) {
        core::TopologyMutationOptions render_options = options;
        render_options.seed = seed;
        render_options.budget = 1U;
        auto child = core::mutate_recipe_topology(minimal, render_options, no_locks);
        if (child.is_error()) {
            continue;
        }
        auto image = nodes::render_reference(child.value().recipe);
        if (image.is_error()) {
            continue;
        }
        exporting::ExportRequest request;
        request.kind = exporting::ExportKind::still;
        request.raster_format = exporting::RasterFormat::raw_rgba;
        request.destination_directory = export_dir;
        request.stem = "topology";
        auto exported = exporting::export_recipe(child.value().recipe, request);
        if (exported.is_ok()) {
            rendered_and_exported = true;
            expect(exported.value().recipe_fingerprint == core::semantic_fingerprint(child.value().recipe),
                "export provenance must retain topology-mutated recipe identity");
        }
    }
    expect(rendered_and_exported, "at least one deterministic topology child must render and export canonically");
    std::filesystem::remove_all(export_dir, cleanup_error);

    if (failures == 0) {
        std::cout << "AM-014 topology mutation tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
