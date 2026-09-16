#pragma once

#include "core/result.hpp"
#include "nodes/static_evaluator.hpp"
#include "quarry/metrics.hpp"

namespace artminer::quarry {

struct TileSeamDiagnostics final {
    double horizontal_error{0.0};
    double vertical_error{0.0};
    double combined_error{0.0};
};

// Decomposes the authoritative AM-010 tile_seam_error metric into horizontal
// and vertical terms. combined_error is obtained through compute_metrics(), so
// there is one canonical combined seam metric implementation.
[[nodiscard]] core::Result<TileSeamDiagnostics, MetricError> compute_tile_seam_diagnostics(
    const nodes::Image& image);

// Produces a deterministic 2x2 tiled inspection image. The central vertical and
// horizontal joins expose exactly the same wrap boundaries measured above.
[[nodiscard]] core::Result<nodes::Image, MetricError> make_tile_seam_inspection(
    const nodes::Image& image);

}  // namespace artminer::quarry
