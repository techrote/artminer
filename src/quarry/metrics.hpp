#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "core/types.hpp"
#include "nodes/static_evaluator.hpp"

namespace artminer::quarry {

inline constexpr core::u32 kMetricSemanticVersion = 1U;

struct MetricValue final {
    std::string name;
    double value{0.0};
};

using MetricVector = std::vector<MetricValue>;

enum class MetricErrorCode {
    invalid_image,
    unknown_metric,
    insufficient_frames,
};

struct MetricError final {
    MetricErrorCode code{MetricErrorCode::invalid_image};
    std::string message;
};

[[nodiscard]] const std::vector<std::string>& supported_metric_names();
[[nodiscard]] bool is_supported_metric(std::string_view name) noexcept;

// Computes deterministic transparent metrics from canonical RGBA8 frames. The
// first frame owns all still-image metrics. Animation metrics consume the full
// bounded sample sequence in caller-provided tick order.
[[nodiscard]] core::Result<MetricVector, MetricError> compute_metrics(
    const std::vector<nodes::Image>& frames,
    const std::vector<std::string>& selected_metrics);

}  // namespace artminer::quarry
