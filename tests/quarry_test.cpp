#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "core/recipe.hpp"
#include "quarry/metrics.hpp"
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
    auto recipe = std::move(parsed).value();
    const auto validation = artminer::core::validate_recipe(recipe);
    if (!validation.empty()) {
        ++g_failures;
        std::cerr << "FAIL: base recipe validation failed: " << validation.front().message << '\n';
    }
    return recipe;
}

[[nodiscard]] std::filesystem::path fresh_root() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "artminer-am010-quarry-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root, ignored);
    return root;
}

[[nodiscard]] const artminer::quarry::MetricValue* find_metric(
    const artminer::quarry::MetricVector& metrics,
    const std::string_view name) {
    for (const auto& metric : metrics) {
        if (metric.name == name) {
            return &metric;
        }
    }
    return nullptr;
}

void test_metric_reference_vectors() {
    artminer::nodes::Image transparent;
    transparent.width = 2U;
    transparent.height = 2U;
    transparent.rgba.assign(16U, 0U);
    auto empty = artminer::quarry::compute_metrics(
        {transparent}, {"entropy", "edge_density", "connected_components", "empty_space_ratio", "tile_seam_error"});
    expect(empty.is_ok(), "transparent metric vector should evaluate");
    if (empty.is_ok()) {
        expect(find_metric(empty.value(), "entropy")->value == 0.0, "transparent entropy reference");
        expect(find_metric(empty.value(), "edge_density")->value == 0.0, "transparent edge density reference");
        expect(find_metric(empty.value(), "connected_components")->value == 0.0, "transparent component reference");
        expect(find_metric(empty.value(), "empty_space_ratio")->value == 1.0, "transparent empty-space reference");
        expect(find_metric(empty.value(), "tile_seam_error")->value == 0.0, "transparent seam reference");
    }

    artminer::nodes::Image checker;
    checker.width = 2U;
    checker.height = 2U;
    checker.rgba = {
        0U, 0U, 0U, 255U, 255U, 255U, 255U, 255U,
        255U, 255U, 255U, 255U, 0U, 0U, 0U, 255U,
    };
    auto metrics = artminer::quarry::compute_metrics(
        {checker}, {"entropy", "edge_density", "connected_components", "symmetry_rotational", "tile_seam_error"});
    expect(metrics.is_ok(), "checker metric vector should evaluate");
    if (metrics.is_ok()) {
        expect(std::abs(find_metric(metrics.value(), "entropy")->value - 0.125) < 1e-12, "checker entropy reference");
        expect(find_metric(metrics.value(), "edge_density")->value == 1.0, "checker edge density reference");
        expect(find_metric(metrics.value(), "connected_components")->value == 1.0, "opaque checker component reference");
        expect(find_metric(metrics.value(), "symmetry_rotational")->value == 1.0, "checker rotational symmetry reference");
        expect(find_metric(metrics.value(), "tile_seam_error")->value == 1.0, "checker seam reference");
    }

    artminer::nodes::Image second = checker;
    for (std::size_t index = 0U; index < second.rgba.size(); index += 4U) {
        second.rgba[index] = static_cast<artminer::core::u8>(255U - second.rgba[index]);
        second.rgba[index + 1U] = static_cast<artminer::core::u8>(255U - second.rgba[index + 1U]);
        second.rgba[index + 2U] = static_cast<artminer::core::u8>(255U - second.rgba[index + 2U]);
    }
    auto temporal = artminer::quarry::compute_metrics({checker, second}, {"motion_energy", "temporal_flicker"});
    expect(temporal.is_ok(), "animation metric vector should evaluate");
    if (temporal.is_ok()) {
        expect(find_metric(temporal.value(), "motion_energy")->value == 1.0, "checker inversion motion energy reference");
        expect(find_metric(temporal.value(), "temporal_flicker")->value == 0.0, "checker inversion global flicker reference");
    }
}

void compare_results(
    const std::vector<artminer::quarry::CandidateResult>& left,
    const std::vector<artminer::quarry::CandidateResult>& right,
    const std::string_view context) {
    expect(left.size() == right.size(), std::string(context) + " result count");
    const std::size_t count = (std::min)(left.size(), right.size());
    for (std::size_t index = 0U; index < count; ++index) {
        expect(left[index].index == right[index].index, std::string(context) + " candidate index");
        expect(left[index].candidate_id == right[index].candidate_id, std::string(context) + " candidate identity");
        expect(left[index].recipe_fingerprint == right[index].recipe_fingerprint, std::string(context) + " recipe fingerprint");
        expect(left[index].metrics.size() == right[index].metrics.size(), std::string(context) + " metric count");
        const std::size_t metrics = (std::min)(left[index].metrics.size(), right[index].metrics.size());
        for (std::size_t metric = 0U; metric < metrics; ++metric) {
            expect(left[index].metrics[metric].name == right[index].metrics[metric].name, std::string(context) + " metric name");
            expect(left[index].metrics[metric].value == right[index].metrics[metric].value, std::string(context) + " metric value");
        }
    }
}

void test_worker_order_resume_cache_and_corruption() {
    constexpr artminer::core::u64 kPopulation = 64U;
    const std::filesystem::path root = fresh_root();
    const std::vector<std::string> selected_metrics{
        "entropy", "edge_density", "symmetry_bilateral", "palette_utilisation", "region_diversity"};
    auto manifest_result = artminer::quarry::make_job_manifest(
        base_recipe(), 0x123456789abcdef0ULL, kPopulation, 8U, 8U, 0.65, selected_metrics);
    expect(manifest_result.is_ok(), "Quarry manifest should be constructible");
    if (manifest_result.is_error()) {
        return;
    }
    const auto manifest = manifest_result.value();
    const std::filesystem::path one = root / "workers-1.amq";
    const std::filesystem::path four = root / "workers-4.amq";
    expect(artminer::quarry::write_job_manifest(one, manifest).is_ok(), "write worker-1 manifest");
    expect(artminer::quarry::write_job_manifest(four, manifest).is_ok(), "write worker-4 manifest");
    auto run_one = artminer::quarry::run_job(one, 1U);
    auto run_four = artminer::quarry::run_job(four, 4U);
    expect(run_one.is_ok() && run_one.value().complete, "single-worker Quarry should complete");
    expect(run_four.is_ok() && run_four.value().complete, "four-worker Quarry should complete");
    auto one_results = artminer::quarry::read_results(one);
    auto four_results = artminer::quarry::read_results(four);
    expect(one_results.is_ok() && four_results.is_ok(), "worker-count results should read");
    if (one_results.is_ok() && four_results.is_ok()) {
        expect(one_results.value().size() == kPopulation, "substantial test population should be fully committed");
        compare_results(one_results.value(), four_results.value(), "worker-count invariance");
    }

    const std::filesystem::path cancelled = root / "cancelled.amq";
    const std::filesystem::path uninterrupted = root / "uninterrupted.amq";
    expect(artminer::quarry::write_job_manifest(cancelled, manifest).is_ok(), "write cancellation manifest");
    expect(artminer::quarry::write_job_manifest(uninterrupted, manifest).is_ok(), "write uninterrupted manifest");
    artminer::quarry::CancellationToken token;
    auto partial = artminer::quarry::run_job(
        cancelled, 2U, &token,
        [&](const artminer::quarry::JobProgress& progress) {
            if (progress.committed >= 2U && !progress.complete) {
                token.cancel();
            }
        });
    expect(partial.is_ok() && partial.value().cancelled && !partial.value().complete,
           "cancelled Quarry should leave a resumable committed prefix");
    expect(partial.is_ok() && partial.value().committed == 2U,
           "cancellation should stop at the completed bounded two-worker batch");
    auto resumed = artminer::quarry::run_job(cancelled, 3U);
    auto baseline = artminer::quarry::run_job(uninterrupted, 4U);
    expect(resumed.is_ok() && resumed.value().complete, "cancelled Quarry should resume to completion");
    expect(baseline.is_ok() && baseline.value().complete, "uninterrupted comparison Quarry should complete");
    auto resumed_results = artminer::quarry::read_results(cancelled);
    auto baseline_results = artminer::quarry::read_results(uninterrupted);
    expect(resumed_results.is_ok() && baseline_results.is_ok(), "resume comparison results should read");
    if (resumed_results.is_ok() && baseline_results.is_ok()) {
        compare_results(resumed_results.value(), baseline_results.value(), "cancel/resume equivalence");
    }

    const auto paths = artminer::quarry::derive_job_paths(one);
    std::error_code ignored;
    std::filesystem::remove_all(paths.cache_directory, ignored);
    std::filesystem::create_directories(paths.cache_directory, ignored);
    bool first_hit = true;
    bool second_hit = false;
    auto first_candidate = artminer::quarry::evaluate_candidate(manifest, 0U, paths.cache_directory, &first_hit);
    auto second_candidate = artminer::quarry::evaluate_candidate(manifest, 0U, paths.cache_directory, &second_hit);
    expect(first_candidate.is_ok() && !first_hit, "first candidate evaluation after cache deletion should miss");
    expect(second_candidate.is_ok() && second_hit, "second candidate evaluation should hit disposable cache");

    std::filesystem::path cache_entry;
    for (const auto& entry : std::filesystem::directory_iterator(paths.cache_directory)) {
        if (entry.is_regular_file()) {
            cache_entry = entry.path();
            break;
        }
    }
    expect(!cache_entry.empty(), "candidate evaluation should create a disposable cache entry");
    if (!cache_entry.empty()) {
        std::ofstream stale(cache_entry, std::ios::binary | std::ios::trunc);
        stale << "stale incompatible cache\n";
    }
    bool stale_hit = true;
    auto after_stale = artminer::quarry::evaluate_candidate(manifest, 0U, paths.cache_directory, &stale_hit);
    expect(after_stale.is_ok() && !stale_hit, "malformed or stale cache entry should be a safe miss");
    if (second_candidate.is_ok() && after_stale.is_ok()) {
        expect(second_candidate.value().candidate_id == after_stale.value().candidate_id,
               "stale cache must not change candidate identity");
        expect(second_candidate.value().metrics.size() == after_stale.value().metrics.size(),
               "stale cache metric count");
        for (std::size_t index = 0U; index < second_candidate.value().metrics.size(); ++index) {
            expect(second_candidate.value().metrics[index].value == after_stale.value().metrics[index].value,
                   "stale cache must not change metric values");
        }
    }

    std::filesystem::remove_all(paths.cache_directory, ignored);
    std::filesystem::create_directories(paths.cache_directory, ignored);
    bool after_delete_hit = true;
    auto after_delete = artminer::quarry::evaluate_candidate(manifest, 0U, paths.cache_directory, &after_delete_hit);
    expect(after_delete.is_ok() && !after_delete_hit, "cache deletion should cause a safe miss");
    if (second_candidate.is_ok() && after_delete.is_ok()) {
        expect(second_candidate.value().candidate_id == after_delete.value().candidate_id,
               "cache deletion must not change candidate identity");
        expect(second_candidate.value().metrics.size() == after_delete.value().metrics.size(),
               "cache deletion metric count");
        for (std::size_t index = 0U; index < second_candidate.value().metrics.size(); ++index) {
            expect(second_candidate.value().metrics[index].value == after_delete.value().metrics[index].value,
                   "cache deletion must not change metric values");
        }
    }

    auto bounded = artminer::quarry::read_results(one, 3U);
    expect(bounded.is_ok() && bounded.value().size() == 3U,
           "result browsing should support an explicit bounded prefix rather than requiring the whole population");

    const std::filesystem::path incompatible = root / "incompatible.amq";
    auto incompatible_manifest = artminer::quarry::make_job_manifest(
        base_recipe(), 0x123456789abcdef1ULL, kPopulation, 8U, 8U, 0.65, selected_metrics);
    expect(incompatible_manifest.is_ok(), "incompatible comparison manifest should be constructible");
    if (incompatible_manifest.is_ok()) {
        expect(artminer::quarry::write_job_manifest(incompatible, incompatible_manifest.value()).is_ok(),
               "write incompatible comparison manifest");
        const auto incompatible_paths = artminer::quarry::derive_job_paths(incompatible);
        std::filesystem::copy_file(paths.checkpoint, incompatible_paths.checkpoint,
                                   std::filesystem::copy_options::overwrite_existing, ignored);
        std::filesystem::copy_file(paths.results, incompatible_paths.results,
                                   std::filesystem::copy_options::overwrite_existing, ignored);
        auto mismatched = artminer::quarry::inspect_job(incompatible);
        expect(mismatched.is_error() &&
                   mismatched.error().code == artminer::quarry::QuarryErrorCode::checkpoint_incompatible,
               "checkpoint from a different manifest identity should fail explicitly");
    }

    {
        std::ofstream corrupt(paths.checkpoint, std::ios::binary | std::ios::trunc);
        corrupt << "not a checkpoint\n";
    }
    auto inspected = artminer::quarry::inspect_job(one);
    expect(inspected.is_error() && inspected.error().code == artminer::quarry::QuarryErrorCode::checkpoint_corrupt,
           "corrupted checkpoint should fail safely with a specific diagnostic class");
}

}  // namespace

int main() {
    test_metric_reference_vectors();
    test_worker_order_resume_cache_and_corruption();
    if (g_failures != 0) {
        std::cerr << g_failures << " Quarry test(s) failed\n";
        return 1;
    }
    std::cout << "ArtMiner Quarry tests passed\n";
    return 0;
}
