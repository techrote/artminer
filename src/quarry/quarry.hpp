#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/recipe.hpp"
#include "core/result.hpp"
#include "core/specimen_browser.hpp"
#include "core/types.hpp"
#include "quarry/metrics.hpp"

namespace artminer::quarry {

inline constexpr core::u32 kQuarryManifestVersion = 1U;
inline constexpr core::u32 kQuarryCheckpointVersion = 1U;
inline constexpr core::u32 kCandidateEnumerationVersion = 1U;
inline constexpr core::u32 kThumbnailCacheVersion = 1U;
inline constexpr core::u64 kMaximumQuarryCandidates = 1'000'000ULL;
inline constexpr core::u32 kMaximumQuarryWorkers = 64U;
inline constexpr core::u32 kMaximumAnimationSamples = 16U;

struct AnimationSampling final {
    core::u64 first_tick{0U};
    core::u32 frame_count{1U};
    core::u32 tick_stride{1U};
};

struct JobManifest final {
    core::u32 version{kQuarryManifestVersion};
    core::u32 candidate_enumeration_version{kCandidateEnumerationVersion};
    core::u32 mutation_operator_version{core::kParameterMutationOperatorVersion};
    core::u32 metric_semantic_version{kMetricSemanticVersion};
    core::u32 evaluator_semantic_version{core::kEvaluatorSemanticVersion};
    core::u64 root_seed{0U};
    core::u64 first_candidate{0U};
    core::u64 candidate_count{0U};
    double mutation_strength{0.25};
    core::u32 render_width{128U};
    core::u32 render_height{128U};
    AnimationSampling animation;
    std::vector<std::string> metrics;
    core::Recipe base_recipe;
    std::string base_recipe_fingerprint;
    std::string identity;
};

struct CandidateResult final {
    core::u64 index{0U};
    std::string candidate_id;
    std::string recipe_fingerprint;
    MetricVector metrics;
};

struct JobPaths final {
    std::filesystem::path manifest;
    std::filesystem::path checkpoint;
    std::filesystem::path results;
    std::filesystem::path cache_directory;
};

struct JobProgress final {
    core::u64 committed{0U};
    core::u64 total{0U};
    core::u64 cache_hits{0U};
    bool complete{false};
    bool cancelled{false};
};

class CancellationToken final {
public:
    void cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool cancelled() const noexcept { return cancelled_.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> cancelled_{false};
};

using ProgressCallback = std::function<void(const JobProgress&)>;

enum class QuarryErrorCode {
    invalid_manifest,
    unsupported_version,
    invalid_recipe,
    invalid_job,
    mutation_failed,
    render_failed,
    metric_failed,
    io_error,
    checkpoint_corrupt,
    checkpoint_incompatible,
    results_corrupt,
    resource_limit,
};

struct QuarryError final {
    QuarryErrorCode code{QuarryErrorCode::invalid_job};
    std::string message;
};

[[nodiscard]] JobPaths derive_job_paths(const std::filesystem::path& manifest_path);
[[nodiscard]] core::Result<JobManifest, QuarryError> make_job_manifest(
    const core::Recipe& base_recipe,
    core::u64 root_seed,
    core::u64 candidate_count,
    core::u32 render_width,
    core::u32 render_height,
    double mutation_strength = 0.25,
    std::vector<std::string> metrics = default_still_metric_names(),
    AnimationSampling animation = {});
[[nodiscard]] std::string serialize_job_manifest(const JobManifest& manifest);
[[nodiscard]] core::Result<JobManifest, QuarryError> parse_job_manifest(std::string_view text);
[[nodiscard]] core::Result<void, QuarryError> write_job_manifest(
    const std::filesystem::path& path,
    const JobManifest& manifest);
[[nodiscard]] core::Result<JobManifest, QuarryError> read_job_manifest(const std::filesystem::path& path);

[[nodiscard]] core::Result<CandidateResult, QuarryError> evaluate_candidate(
    const JobManifest& manifest,
    core::u64 candidate_index,
    const std::filesystem::path& cache_directory,
    bool* cache_hit = nullptr);

// Read-through disposable thumbnail cache for the candidate's first sampled
// canonical frame. Missing, stale, malformed, or checksum-mismatched entries
// are regenerated from the manifest and replaced best-effort; cached bytes are
// never authoritative state.
[[nodiscard]] core::Result<nodes::Image, QuarryError> read_cached_thumbnail(
    const JobManifest& manifest,
    core::u64 candidate_index,
    const std::filesystem::path& cache_directory);

// Runs or resumes one manifest. The scheduler admits only 1..64 workers and
// processes one bounded batch at a time. Each worker owns at most one candidate
// evaluation; canonical evaluator limits bound that evaluation's working set.
// Completed images are discarded after metric extraction and only compact
// result rows survive, so population size cannot grow resident image memory.
[[nodiscard]] core::Result<JobProgress, QuarryError> run_job(
    const std::filesystem::path& manifest_path,
    core::u32 worker_count,
    CancellationToken* cancellation = nullptr,
    ProgressCallback progress = {});

[[nodiscard]] core::Result<JobProgress, QuarryError> inspect_job(
    const std::filesystem::path& manifest_path);
[[nodiscard]] core::Result<std::vector<CandidateResult>, QuarryError> read_results(
    const std::filesystem::path& manifest_path,
    std::size_t maximum_results = 0U);

}  // namespace artminer::quarry
