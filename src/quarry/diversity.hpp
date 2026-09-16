#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/recipe.hpp"
#include "core/result.hpp"
#include "core/types.hpp"
#include "nodes/static_evaluator.hpp"
#include "quarry/quarry.hpp"

namespace artminer::quarry {

inline constexpr core::u32 kDiversitySemanticVersion = 1U;

struct MetricWeight final {
    std::string name;
    double weight{1.0};
};

struct NormalizedMetric final {
    std::string name;
    double raw{0.0};
    double normalized{0.0};
    bool available{false};
    bool degenerate{false};
};

struct CandidateFeatures final {
    core::u64 index{0U};
    std::string candidate_id;
    std::string recipe_fingerprint;
    MetricVector raw_metrics;
    std::vector<NormalizedMetric> metrics;
    std::vector<double> parameters;
    std::vector<core::u8> thumbnail_signature;
};

struct NormalizationDimension final {
    std::string name;
    double minimum{0.0};
    double maximum{0.0};
    double weight{1.0};
    bool degenerate{false};
};

struct NormalizationModel final {
    std::vector<NormalizationDimension> dimensions;
};

struct DedupeSettings final {
    double metric_distance_threshold{0.025};
    double image_mean_absolute_threshold{0.020};
};

struct DedupeGroup final {
    core::u64 representative{0U};
    std::vector<core::u64> members;
};

struct Cluster final {
    std::size_t cluster_id{0U};
    core::u64 representative{0U};
    std::vector<core::u64> members;
    std::vector<double> centroid;
};

enum class UnusualBasis {
    population,
    cluster,
};

struct MetricContribution final {
    std::string name;
    double contribution{0.0};
};

struct UnusualScore final {
    core::u64 index{0U};
    std::size_t cluster_id{0U};
    double distance{0.0};
    std::vector<MetricContribution> contributions;
};

enum class NeighbourMode {
    metric,
    parameter,
    combined,
};

struct NeighbourSettings final {
    NeighbourMode mode{NeighbourMode::metric};
    double combined_metric_weight{0.5};
    double minimum_metric_diversity{0.0};
    std::size_t maximum_results{32U};
};

struct NeighbourResult final {
    core::u64 index{0U};
    double distance{0.0};
    double metric_distance{0.0};
    double parameter_distance{0.0};
};

struct MetricRangeFilter final {
    std::string name;
    std::optional<double> minimum;
    std::optional<double> maximum;
};

enum class StableSortKind {
    candidate_index,
    raw_metric,
};

struct StableSort final {
    StableSortKind kind{StableSortKind::candidate_index};
    std::string metric;
    bool descending{false};
};

struct ProjectionPoint final {
    core::u64 index{0U};
    double x{0.0};
    double y{0.0};
};

enum class DiversityErrorCode {
    invalid_settings,
    missing_metric,
    incompatible_recipe,
    candidate_missing,
    quarry_error,
};

struct DiversityError final {
    DiversityErrorCode code{DiversityErrorCode::invalid_settings};
    std::string message;
};

[[nodiscard]] core::Result<NormalizationModel, DiversityError> build_normalization_model(
    const std::vector<CandidateResult>& results,
    const std::vector<MetricWeight>& weights = {});

[[nodiscard]] core::Result<std::vector<CandidateFeatures>, DiversityError> build_candidate_features(
    const JobManifest& manifest,
    const std::vector<CandidateResult>& results,
    const std::filesystem::path& cache_directory,
    const NormalizationModel& normalization);

[[nodiscard]] double metric_distance(
    const CandidateFeatures& left,
    const CandidateFeatures& right,
    const NormalizationModel& normalization) noexcept;

[[nodiscard]] double parameter_distance(
    const CandidateFeatures& left,
    const CandidateFeatures& right) noexcept;

[[nodiscard]] core::Result<std::vector<DedupeGroup>, DiversityError> deduplicate_candidates(
    const std::vector<CandidateFeatures>& candidates,
    const NormalizationModel& normalization,
    const DedupeSettings& settings = {});

[[nodiscard]] core::Result<std::vector<Cluster>, DiversityError> cluster_candidates(
    const std::vector<CandidateFeatures>& candidates,
    const NormalizationModel& normalization,
    std::size_t cluster_count,
    const std::vector<DedupeGroup>* dedupe = nullptr);

[[nodiscard]] core::Result<std::vector<UnusualScore>, DiversityError> rank_unusual(
    const std::vector<CandidateFeatures>& candidates,
    const NormalizationModel& normalization,
    const std::vector<Cluster>& clusters,
    UnusualBasis basis);

[[nodiscard]] core::Result<std::vector<NeighbourResult>, DiversityError> nearest_neighbours(
    const std::vector<CandidateFeatures>& candidates,
    const NormalizationModel& normalization,
    core::u64 selected_index,
    const NeighbourSettings& settings);

[[nodiscard]] core::Result<std::vector<core::u64>, DiversityError> filter_and_sort_candidates(
    const std::vector<CandidateFeatures>& candidates,
    const std::vector<MetricRangeFilter>& filters,
    const StableSort& sort);

[[nodiscard]] core::Result<std::vector<ProjectionPoint>, DiversityError> project_metric_axes(
    const std::vector<CandidateFeatures>& candidates,
    std::string_view x_metric,
    std::string_view y_metric);

[[nodiscard]] core::Result<core::Recipe, DiversityError> materialize_candidate_recipe(
    const JobManifest& manifest,
    core::u64 candidate_index);

[[nodiscard]] std::vector<core::u8> make_thumbnail_signature(const nodes::Image& image);
[[nodiscard]] double thumbnail_signature_distance(
    const std::vector<core::u8>& left,
    const std::vector<core::u8>& right) noexcept;

}  // namespace artminer::quarry
