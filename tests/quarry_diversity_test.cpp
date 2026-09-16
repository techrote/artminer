#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/recipe.hpp"
#include "platform/windows/browser_store.hpp"
#include "quarry/diversity.hpp"
#include "quarry/quarry.hpp"

namespace {

int g_failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

[[nodiscard]] artminer::core::Recipe base_recipe() {
    constexpr std::string_view text = R"AMR(amr 1
evaluator 1
seed 71
render 16 16 reference
node source core.scalar.constant 1
param source value f64 0.5
node image core.image.from_scalar 1
param image palette enum grayscale
edge source value image source
output main image value
)AMR";
    auto parsed = artminer::core::parse_recipe(text);
    if (parsed.is_error()) {
        ++g_failures;
        std::cerr << "FAIL: base recipe parse failed: " << parsed.error().message << '\n';
        return {};
    }
    return std::move(parsed).value();
}

[[nodiscard]] artminer::quarry::CandidateFeatures feature(
    const artminer::core::u64 index,
    const double x,
    const double y,
    const double parameter,
    const artminer::core::u8 signature) {
    artminer::quarry::CandidateFeatures result;
    result.index = index;
    result.candidate_id = "candidate-" + std::to_string(index);
    result.recipe_fingerprint = "recipe-" + std::to_string(index);
    result.raw_metrics = {{"x", x}, {"y", y}, {"flat", 5.0}};
    result.metrics = {
        {"x", x, x, true, false},
        {"y", y, y, true, false},
        {"flat", 5.0, 0.0, true, true},
    };
    result.parameters = {parameter};
    result.thumbnail_signature.assign(64U, signature);
    return result;
}

void test_normalization_and_degenerate_handling() {
    std::vector<artminer::quarry::CandidateResult> results{
        {0U, "a", "fa", {{"x", 10.0}, {"y", 2.0}, {"flat", 5.0}}},
        {1U, "b", "fb", {{"x", 20.0}, {"flat", 5.0}}},
        {2U, "c", "fc", {{"x", 30.0}, {"y", 6.0}, {"flat", 5.0}}},
    };
    auto model = artminer::quarry::build_normalization_model(
        results, {{"x", 2.0}, {"y", 1.0}, {"flat", 1.0}});
    expect(model.is_ok(), "normalization model should accept per-candidate missing metrics");
    if (model.is_error()) {
        return;
    }
    expect(model.value().dimensions.size() == 3U, "normalization should preserve selected metric order");
    expect(model.value().dimensions[0].minimum == 10.0 && model.value().dimensions[0].maximum == 30.0,
           "normalization x min/max");
    expect(model.value().dimensions[1].minimum == 2.0 && model.value().dimensions[1].maximum == 6.0,
           "normalization y ignores missing values deterministically");
    expect(model.value().dimensions[2].degenerate, "constant metric should be explicitly degenerate");

    auto missing = artminer::quarry::build_normalization_model(results, {{"absent", 1.0}});
    expect(missing.is_error() && missing.error().code == artminer::quarry::DiversityErrorCode::missing_metric,
           "metric absent from the entire population should fail explicitly");
    auto zero_weights = artminer::quarry::build_normalization_model(results, {{"x", 0.0}});
    expect(zero_weights.is_error(), "normalization must retain at least one positive metric weight");
}

void test_dedupe_clustering_ranking_and_projection() {
    const artminer::quarry::NormalizationModel model{{
        {"x", 0.0, 1.0, 2.0, false},
        {"y", 0.0, 1.0, 1.0, false},
        {"flat", 5.0, 5.0, 1.0, true},
    }};

    auto a = feature(0U, 0.0, 0.0, 0.0, 20U);
    auto near = feature(1U, 0.01, 0.01, 0.1, 21U);
    auto b = feature(2U, 1.0, 0.0, 0.9, 220U);
    auto c = feature(3U, 1.0, 1.0, 1.0, 240U);
    std::vector<artminer::quarry::CandidateFeatures> candidates{a, near, b, c};

    const double expected_distance = std::sqrt(3.0 / 3.0);
    expect(std::abs(artminer::quarry::metric_distance(a, c, model) - expected_distance) < 1e-12,
           "metric distance should be transparent weighted RMS and ignore degenerate dimensions");

    artminer::quarry::DedupeSettings dedupe_settings;
    dedupe_settings.metric_distance_threshold = 0.02;
    dedupe_settings.image_mean_absolute_threshold = 0.01;
    auto dedupe = artminer::quarry::deduplicate_candidates(candidates, model, dedupe_settings);
    auto dedupe_repeat = artminer::quarry::deduplicate_candidates(candidates, model, dedupe_settings);
    expect(dedupe.is_ok() && dedupe_repeat.is_ok(), "dedupe should evaluate repeatedly");
    if (dedupe.is_ok() && dedupe_repeat.is_ok()) {
        expect(dedupe.value().size() == 3U, "near duplicate should be hidden from representative browsing only");
        expect(dedupe.value()[0].representative == 0U && dedupe.value()[0].members.size() == 2U &&
                   dedupe.value()[0].members[1] == 1U,
               "dedupe representative and member ordering should be stable by candidate index");
        expect(dedupe.value()[0].members == dedupe_repeat.value()[0].members,
               "same dedupe settings should reproduce the same grouping");
    }

    std::vector<artminer::quarry::CandidateFeatures> corners{
        feature(0U, 0.0, 0.0, 0.0, 0U),
        feature(1U, 0.0, 1.0, 0.25, 64U),
        feature(2U, 1.0, 0.0, 0.75, 128U),
        feature(3U, 1.0, 1.0, 1.0, 255U),
    };
    auto clusters = artminer::quarry::cluster_candidates(corners, model, 2U);
    auto clusters_repeat = artminer::quarry::cluster_candidates(corners, model, 2U);
    expect(clusters.is_ok() && clusters_repeat.is_ok(), "deterministic clustering should evaluate repeatedly");
    if (clusters.is_ok() && clusters_repeat.is_ok()) {
        expect(clusters.value().size() == 2U && clusters.value()[0].representative == 0U &&
                   clusters.value()[1].representative == 2U,
               "weighted farthest-first clusters and centroid representatives should use stable tie-breaking");
        expect(clusters.value()[0].members == clusters_repeat.value()[0].members &&
                   clusters.value()[1].members == clusters_repeat.value()[1].members,
               "repeated clustering should be byte-order stable at the semantic level");

        auto unusual = artminer::quarry::rank_unusual(
            corners, model, clusters.value(), artminer::quarry::UnusualBasis::population);
        expect(unusual.is_ok() && unusual.value().size() == corners.size(), "population unusual ranking should evaluate");
        if (unusual.is_ok()) {
            expect(unusual.value()[0].index == 0U,
                   "equal-distance unusual candidates should tie-break by ascending candidate index");
            expect(!unusual.value()[0].contributions.empty() &&
                       unusual.value()[0].contributions.front().name == "x",
                   "unusual ranking should expose weighted contributing metric dimensions");
        }
    }

    auto projection = artminer::quarry::project_metric_axes(corners, "x", "y");
    auto projection_repeat = artminer::quarry::project_metric_axes(corners, "x", "y");
    expect(projection.is_ok() && projection_repeat.is_ok() && projection.value().size() == 4U,
           "explicit metric-axis projection should evaluate");
    if (projection.is_ok() && projection_repeat.is_ok()) {
        expect(projection.value()[2].index == 2U && projection.value()[2].x == 1.0 && projection.value()[2].y == 0.0,
               "projection must be literal selected normalized axes, not an opaque embedding");
        expect(projection.value()[2].x == projection_repeat.value()[2].x &&
                   projection.value()[2].y == projection_repeat.value()[2].y,
               "projection should be stable across repeated runs");
    }
}

void test_neighbours_filters_and_thresholds() {
    const artminer::quarry::NormalizationModel model{{
        {"x", 0.0, 1.0, 1.0, false},
        {"y", 0.0, 1.0, 1.0, false},
        {"flat", 5.0, 5.0, 1.0, true},
    }};
    std::vector<artminer::quarry::CandidateFeatures> candidates{
        feature(0U, 0.0, 0.0, 0.0, 0U),
        feature(1U, 0.1, 0.0, 0.9, 25U),
        feature(2U, 0.8, 0.0, 0.1, 200U),
        feature(3U, 1.0, 1.0, 0.5, 255U),
    };

    artminer::quarry::NeighbourSettings settings;
    settings.mode = artminer::quarry::NeighbourMode::metric;
    auto metric_neighbours = artminer::quarry::nearest_neighbours(candidates, model, 0U, settings);
    expect(metric_neighbours.is_ok() && metric_neighbours.value().front().index == 1U,
           "metric neighbours should prefer metric proximity");

    settings.mode = artminer::quarry::NeighbourMode::parameter;
    auto parameter_neighbours = artminer::quarry::nearest_neighbours(candidates, model, 0U, settings);
    expect(parameter_neighbours.is_ok() && parameter_neighbours.value().front().index == 2U,
           "parameter neighbours should prefer normalized recipe-parameter proximity");

    settings.mode = artminer::quarry::NeighbourMode::combined;
    settings.combined_metric_weight = 0.5;
    settings.minimum_metric_diversity = 0.05;
    auto combined = artminer::quarry::nearest_neighbours(candidates, model, 0U, settings);
    expect(combined.is_ok() && !combined.value().empty(), "combined nearby-but-diverse query should evaluate");
    if (combined.is_ok()) {
        expect(combined.value().front().metric_distance >= 0.05,
               "nearby-but-diverse query should enforce its explicit metric-diversity floor");
    }

    auto filtered = artminer::quarry::filter_and_sort_candidates(
        candidates,
        {{"x", 0.05, 0.9}},
        {artminer::quarry::StableSortKind::raw_metric, "x", true});
    expect(filtered.is_ok() && filtered.value().size() == 2U && filtered.value()[0] == 2U && filtered.value()[1] == 1U,
           "metric range filtering and sorting should be stable and reproducible");

    artminer::nodes::Image black;
    black.width = 8U;
    black.height = 8U;
    black.rgba.resize(8U * 8U * 4U, 255U);
    for (std::size_t index = 0U; index < black.rgba.size(); index += 4U) {
        black.rgba[index] = 0U;
        black.rgba[index + 1U] = 0U;
        black.rgba[index + 2U] = 0U;
    }
    artminer::nodes::Image white = black;
    for (std::size_t index = 0U; index < white.rgba.size(); index += 4U) {
        white.rgba[index] = 255U;
        white.rgba[index + 1U] = 255U;
        white.rgba[index + 2U] = 255U;
    }
    const auto black_signature = artminer::quarry::make_thumbnail_signature(black);
    const auto white_signature = artminer::quarry::make_thumbnail_signature(white);
    expect(artminer::quarry::thumbnail_signature_distance(black_signature, black_signature) == 0.0,
           "identical thumbnail signatures should have zero image distance");
    expect(artminer::quarry::thumbnail_signature_distance(black_signature, white_signature) == 1.0,
           "black/white thumbnail signatures should have normalized maximum image distance");
}

void test_candidate_materialization_and_favourite_identity() {
    auto manifest = artminer::quarry::make_job_manifest(base_recipe(), 0xabc123ULL, 4U, 16U, 16U, 0.5, {"entropy"});
    expect(manifest.is_ok(), "materialization test manifest should construct");
    if (manifest.is_error()) {
        return;
    }
    auto reconstructed = artminer::quarry::reconstruct_candidate_recipe(manifest.value(), 2U);
    auto materialized = artminer::quarry::materialize_candidate_recipe(manifest.value(), 2U);
    expect(reconstructed.is_ok() && materialized.is_ok(), "Quarry discovery should materialize as a normal recipe");
    if (reconstructed.is_error() || materialized.is_error()) {
        return;
    }
    const std::string expected_fingerprint = artminer::core::semantic_fingerprint(reconstructed.value());
    expect(artminer::core::semantic_fingerprint(materialized.value()) == expected_fingerprint,
           "Quarry provenance metadata must not change the discovery's semantic fingerprint");

    bool found_job = false;
    bool found_index = false;
    for (const auto& metadata : materialized.value().metadata) {
        found_job = found_job || (metadata.key == "quarry.job_identity" && metadata.value == manifest.value().identity);
        found_index = found_index || (metadata.key == "quarry.candidate_index" && metadata.value == "2");
    }
    expect(found_job && found_index, "materialized recipe should carry inspectable Quarry provenance metadata");

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "artminer-am011-diversity-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root, ignored);
    auto saved = artminer::platform::windows::save_favourite(root, materialized.value());
    expect(saved.is_ok(), "materialized Quarry discovery should save through the normal favourite store");
    auto loaded = artminer::platform::windows::load_favourites(root);
    expect(loaded.is_ok() && loaded.value().size() == 1U,
           "saved Quarry favourite should load through the normal specimen workflow");
    if (loaded.is_ok() && loaded.value().size() == 1U) {
        expect(loaded.value()[0].fingerprint == expected_fingerprint,
               "saved Quarry favourite must preserve semantic recipe identity");
        bool loaded_provenance = false;
        for (const auto& metadata : loaded.value()[0].recipe.metadata) {
            loaded_provenance = loaded_provenance ||
                (metadata.key == "quarry.job_identity" && metadata.value == manifest.value().identity);
        }
        expect(loaded_provenance, "saved Quarry favourite should preserve provenance metadata");
    }
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main() {
    test_normalization_and_degenerate_handling();
    test_dedupe_clustering_ranking_and_projection();
    test_neighbours_filters_and_thresholds();
    test_candidate_materialization_and_favourite_identity();
    if (g_failures != 0) {
        std::cerr << g_failures << " Quarry diversity test(s) failed\n";
        return 1;
    }
    std::cout << "ArtMiner Quarry diversity tests passed\n";
    return 0;
}
