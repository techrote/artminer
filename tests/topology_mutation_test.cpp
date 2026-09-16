#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "core/breeding.hpp"
#include "core/topology_lineage.hpp"
#include "core/topology_mutation.hpp"
#include "core/topology_specimen.hpp"
#include "export/export.hpp"
#include "nodes/static_evaluator.hpp"
#include "quarry/quarry.hpp"
#include "quarry/topology_search.hpp"

namespace {

using artminer::core::Recipe;

Recipe load_recipe(const std::filesystem::path& path, int& failures) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    auto parsed = artminer::core::parse_recipe(text.str());
    if (!input || parsed.is_error()) {
        std::cerr << "FAIL: could not load " << path.string() << '\n';
        ++failures;
        return {};
    }
    return std::move(parsed).value();
}

std::vector<std::tuple<std::string, std::string, std::string, std::string>> incident_edges(
    const Recipe& recipe,
    const std::string& id) {
    std::vector<std::tuple<std::string, std::string, std::string, std::string>> result;
    for (const auto& edge : recipe.edges) {
        if (edge.from_node == id || edge.to_node == id) {
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
    const auto expect = [&](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    };

    const std::filesystem::path root = ARTMINER_SOURCE_DIR;
    Recipe minimal = load_recipe(root / "examples" / "am002-minimal.amr", failures);
    expect(core::validate_recipe(minimal).empty(), "minimal fixture validates");

    const auto& catalog = core::topology_operator_catalog();
    expect(catalog.size() == 5U, "five versioned topology operator families exist");
    expect(catalog[0].kind == core::TopologyOperatorKind::insert_node &&
            catalog[1].kind == core::TopologyOperatorKind::replace_node &&
            catalog[2].kind == core::TopologyOperatorKind::delete_bypass &&
            catalog[3].kind == core::TopologyOperatorKind::duplicate_branch &&
            catalog[4].kind == core::TopologyOperatorKind::rewire_edge,
        "catalog order is stable");

    core::TopologyMutationOptions options;
    options.seed = 140014U;
    options.budget = 3U;
    core::StructuralLocks no_locks;
    auto first = core::mutate_recipe_topology(minimal, options, no_locks);
    auto second = core::mutate_recipe_topology(minimal, options, no_locks);
    expect(first.is_ok() && second.is_ok(), "deterministic topology mutation succeeds");
    if (first.is_ok() && second.is_ok()) {
        expect(core::semantic_fingerprint(first.value().recipe) == core::semantic_fingerprint(second.value().recipe),
            "same parent/version/seed/budget reproduces child");
        expect(!first.value().steps.empty() && first.value().steps.size() <= options.budget,
            "accepted edits are bounded by budget");
        expect(core::validate_recipe(first.value().recipe).empty(), "accepted child passes normal validator");
        expect(first.value().recipe.nodes.size() <= options.limits.max_nodes &&
                first.value().recipe.edges.size() <= options.limits.max_edges &&
                core::topology_depth(first.value().recipe) <= options.limits.max_depth,
            "accepted child stays within structural limits");
        expect(!core::diff_recipes(minimal, first.value().recipe).semantic.empty(),
            "existing recipe diff exposes topology changes");

        auto lineage = core::topology_lineage_record_from_recipe(first.value().recipe);
        expect(lineage.is_ok(), "topology provenance converts to lineage");
        if (lineage.is_ok()) {
            auto parsed = core::parse_topology_lineage_record(core::serialize_topology_lineage_record(lineage.value()));
            expect(parsed.is_ok(), "topology lineage round-trips");
            if (parsed.is_ok()) {
                auto replay = core::replay_topology_lineage_record(parsed.value(), minimal);
                expect(replay.is_ok(), "topology lineage replays");
                if (replay.is_ok()) {
                    expect(core::semantic_fingerprint(replay.value()) == core::semantic_fingerprint(first.value().recipe),
                        "lineage replay identity matches child");
                }
            }
        }

        Recipe reordered = minimal;
        std::reverse(reordered.nodes.begin(), reordered.nodes.end());
        std::reverse(reordered.edges.begin(), reordered.edges.end());
        auto reordered_child = core::mutate_recipe_topology(reordered, options, no_locks);
        expect(reordered_child.is_ok(), "storage-order variant mutates");
        if (reordered_child.is_ok()) {
            expect(core::semantic_fingerprint(reordered_child.value().recipe) == core::semantic_fingerprint(first.value().recipe),
                "mutation is independent of node/edge storage order");
        }
    }

    core::StructuralLocks frozen;
    expect(frozen.lock_subgraph(minimal, "source"), "subgraph lock resolves root");
    expect(frozen.size() == minimal.nodes.size(), "subgraph lock freezes descendants");
    auto blocked = core::mutate_recipe_topology(minimal, options, frozen);
    expect(blocked.is_error() && blocked.error().code == core::TopologyMutationErrorCode::no_legal_mutation,
        "fully locked graph rejects mutation deterministically");

    core::TopologyMutationOptions too_small = options;
    too_small.limits.max_nodes = 1U;
    auto bounded = core::mutate_recipe_topology(minimal, too_small, no_locks);
    expect(bounded.is_error() && bounded.error().code == core::TopologyMutationErrorCode::resource_limit,
        "parent beyond requested limits is rejected before editing");

    std::size_t accepted = 0U;
    for (core::u64 seed = 0U; seed < 64U; ++seed) {
        core::TopologyMutationOptions sample = options;
        sample.seed = seed;
        sample.budget = 2U;
        auto a = core::mutate_recipe_topology(minimal, sample, no_locks);
        auto b = core::mutate_recipe_topology(minimal, sample, no_locks);
        expect(a.is_error() == b.is_error(), "property sweep repeats success/failure");
        if (a.is_ok() && b.is_ok()) {
            ++accepted;
            expect(core::validate_recipe(a.value().recipe).empty(), "property child validates");
            expect(core::semantic_fingerprint(a.value().recipe) == core::semantic_fingerprint(b.value().recipe),
                "property child is deterministic");
            expect(a.value().recipe.nodes.size() <= sample.limits.max_nodes &&
                    a.value().recipe.edges.size() <= sample.limits.max_edges &&
                    core::topology_depth(a.value().recipe) <= sample.limits.max_depth,
                "property child remains bounded");
        }
    }
    expect(accepted > 0U, "property sweep accepts legal mutations");

    auto grid = core::generate_topology_specimen_grid(
        minimal, 0x140014ULL, core::kTopologyMutationOperatorVersion, 1U, no_locks);
    expect(grid.is_ok(), "explicit topology specimen grid generates");
    if (grid.is_ok()) {
        expect(grid.value().size() == core::kSpecimenGridSize, "topology specimen grid remains 4x4");
        for (const auto& specimen : grid.value()) {
            expect(core::validate_recipe(specimen.recipe).empty(), "topology specimen validates");
            expect(specimen.fingerprint == core::semantic_fingerprint(specimen.recipe),
                "topology specimen carries canonical identity");
        }
    }

    Recipe feedback = load_recipe(root / "examples" / "am007-feedback-trails.amr", failures);
    const auto before_boundary = incident_edges(feedback, "history");
    core::TopologyMutationOptions feedback_options = options;
    feedback_options.seed = 7714U;
    feedback_options.budget = 2U;
    auto feedback_child = core::mutate_recipe_topology(feedback, feedback_options, no_locks);
    expect(feedback_child.is_ok(), "feedback fixture has legal edits outside state boundary");
    if (feedback_child.is_ok()) {
        const auto history = std::find_if(
            feedback_child.value().recipe.nodes.begin(), feedback_child.value().recipe.nodes.end(),
            [](const core::NodeInstance& node) { return node.id == "history"; });
        expect(history != feedback_child.value().recipe.nodes.end() && history->type_id == "core.state.delay.image",
            "state boundary node remains unchanged");
        expect(incident_edges(feedback_child.value().recipe, "history") == before_boundary,
            "state-boundary incident edges remain unchanged");
    }

    const std::string malformed =
        "aml-topology 1\nchild 00000000000000000000000000000000\n"
        "parent 00000000000000000000000000000000\noperator 1\nseed 1\n"
        "limits 128/256/64\nlocks -\ntrace insert@test\n";
    expect(core::parse_topology_lineage_record(malformed).is_error(),
        "lineage missing required budget fails instead of guessing");

    auto manifest = quarry::make_job_manifest(minimal, 14014U, 16U, 64U, 64U);
    expect(manifest.is_ok(), "ordinary Quarry manifest builds");
    if (manifest.is_ok()) {
        quarry::TopologySearchConfig config;
        config.budget = 1U;
        auto parsed = quarry::parse_topology_search_config(quarry::serialize_topology_search_config(config));
        expect(parsed.is_ok(), "topology Quarry config round-trips");
        if (parsed.is_ok()) {
            auto a = quarry::reconstruct_topology_candidate(manifest.value(), 3U, parsed.value());
            auto b = quarry::reconstruct_topology_candidate(manifest.value(), 3U, parsed.value());
            expect(a.is_ok() && b.is_ok(), "Quarry topology candidate reconstructs");
            if (a.is_ok() && b.is_ok()) {
                expect(a.value().candidate_id == b.value().candidate_id &&
                        a.value().recipe_fingerprint == b.value().recipe_fingerprint,
                    "Quarry topology candidate identity is deterministic");
                expect(core::validate_recipe(a.value().recipe).empty(), "Quarry topology candidate validates");
            }
        }
    }

    bool render_export_ok = false;
    const std::filesystem::path export_dir =
        std::filesystem::temp_directory_path() / "artminer-am014-topology-export";
    std::error_code ec;
    std::filesystem::remove_all(export_dir, ec);
    for (core::u64 seed = 100U; seed < 164U && !render_export_ok; ++seed) {
        core::TopologyMutationOptions render_options = options;
        render_options.seed = seed;
        render_options.budget = 1U;
        auto child = core::mutate_recipe_topology(minimal, render_options, no_locks);
        if (child.is_error() || nodes::render_reference(child.value().recipe).is_error()) {
            continue;
        }
        exporting::ExportRequest request;
        request.raster_format = exporting::RasterFormat::raw_rgba;
        request.destination_directory = export_dir;
        request.stem = "topology";
        auto exported = exporting::export_recipe(child.value().recipe, request);
        if (exported.is_ok()) {
            render_export_ok = true;
            expect(exported.value().recipe_fingerprint == core::semantic_fingerprint(child.value().recipe),
                "export provenance retains topology child identity");
        }
    }
    expect(render_export_ok, "representative topology child renders and exports canonically");
    std::filesystem::remove_all(export_dir, ec);

    if (failures == 0) {
        std::cout << "AM-014 topology mutation tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
