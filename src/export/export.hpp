#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/recipe.hpp"
#include "core/result.hpp"
#include "core/types.hpp"
#include "nodes/static_evaluator.hpp"

namespace artminer::exporting {

inline constexpr core::u32 kExporterSemanticVersion = 1U;
inline constexpr core::u64 kMaximumExportFrames = 256ULL;

enum class ExportKind {
    still,
    frame,
    sequence,
    sprite_sheet,
    palette_text,
    palette_csv,
    cube_lut,
};

enum class RasterFormat {
    png,
    bmp,
    raw_rgba,
};

struct ExportRequest final {
    ExportKind kind{ExportKind::still};
    RasterFormat raster_format{RasterFormat::png};
    std::filesystem::path destination_directory;
    std::string stem{"art"};
    std::optional<core::u64> tick;
    core::u64 start_tick{0U};
    core::u64 end_tick{0U};
    core::u32 sheet_columns{8U};
    std::string palette_node_id;
};

struct ExportedFile final {
    std::string relative_path;
    core::u64 byte_count{0U};
    std::string checksum_fnv1a64;
};

struct ExportResult final {
    std::filesystem::path directory;
    std::filesystem::path manifest_path;
    std::string recipe_fingerprint;
    std::vector<ExportedFile> files;
};

struct ExportError final {
    std::string message;
};

[[nodiscard]] std::string_view to_string(ExportKind kind) noexcept;
[[nodiscard]] std::string_view to_string(RasterFormat format) noexcept;

[[nodiscard]] std::string deterministic_frame_filename(core::u64 tick, RasterFormat format);
[[nodiscard]] std::filesystem::path staging_path_for_destination(const std::filesystem::path& destination);

// Pure deterministic sprite-sheet composition helper. Frames must have equal,
// valid RGBA8 dimensions; layout is row-major by input order.
[[nodiscard]] core::Result<nodes::Image, ExportError> compose_sprite_sheet(
    std::span<const nodes::Image> frames,
    core::u32 columns);

// Recipe palette serializers are intentionally conservative. Only current
// palette node semantics with a faithful textual representation are accepted.
[[nodiscard]] core::Result<std::string, ExportError> serialize_palette(
    const core::Recipe& recipe,
    std::string_view palette_node_id,
    bool csv);

// .cube is emitted only when the palette can be represented as an opaque,
// channel-neutral 1D transfer function without inventing colour semantics.
[[nodiscard]] core::Result<std::string, ExportError> serialize_cube_lut(
    const core::Recipe& recipe,
    std::string_view palette_node_id);

// High-level transactional export entry point shared by native UI and headless
// commands. The source recipe is accepted by const reference and never mutated.
// destination_directory is created only by an atomic same-parent staging rename;
// existing destinations are never overwritten.
[[nodiscard]] core::Result<ExportResult, ExportError> export_recipe(
    const core::Recipe& recipe,
    const ExportRequest& request);

}  // namespace artminer::exporting
