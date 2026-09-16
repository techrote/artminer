#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/recipe.hpp"
#include "core/result.hpp"
#include "core/types.hpp"
#include "nodes/static_evaluator.hpp"

namespace artminer::nodes {

inline constexpr core::u64 kMaximumGlyphCells = 262144ULL;

enum class GlyphSet {
    ascii_density,
    blocks,
    lines,
    sparkle,
};

enum class GlyphChoiceMode {
    density,
    structure,
    balanced,
};

enum class GlyphColourMode {
    monochrome,
    limited,
    ansi16,
    ansi256,
    truecolor,
};

enum class GlyphLimitedPalette {
    mono4,
    warm8,
    cool8,
};

enum class GlyphBackgroundMode {
    black,
    terminal_default,
};

enum class GlyphAlphaMode {
    composite_black,
    ignore,
};

struct GlyphSettings final {
    core::u32 cell_width{8U};
    core::u32 cell_height{16U};
    GlyphSet glyph_set{GlyphSet::lines};
    GlyphChoiceMode choice_mode{GlyphChoiceMode::balanced};
    double density_weight{1.0};
    double orientation_weight{2.0};
    double detail_weight{0.75};
    double corner_weight{0.5};
    double motion_weight{0.0};
    GlyphColourMode colour_mode{GlyphColourMode::ansi256};
    GlyphLimitedPalette limited_palette{GlyphLimitedPalette::cool8};
    GlyphBackgroundMode background_mode{GlyphBackgroundMode::black};
    GlyphAlphaMode alpha_mode{GlyphAlphaMode::composite_black};
};

struct GlyphMotionVector final {
    double x{0.0};
    double y{0.0};
};

struct GlyphDescriptor final {
    double luminance{0.0};
    double density{0.0};
    double edge_strength{0.0};
    double edge_orientation{0.0};
    double gradient_x{0.0};
    double gradient_y{0.0};
    double detail{0.0};
    double corner{0.0};
    double motion_strength{0.0};
    double motion_direction{0.0};
};

struct Rgb8 final {
    core::u8 r{0U};
    core::u8 g{0U};
    core::u8 b{0U};
};

enum class TerminalColourEncoding {
    ansi16,
    ansi256,
    truecolor,
};

struct TerminalColour final {
    TerminalColourEncoding encoding{TerminalColourEncoding::ansi16};
    core::u32 index{15U};
    Rgb8 rgb{255U, 255U, 255U};
};

struct GlyphCell final {
    char32_t codepoint{U' '};
    GlyphDescriptor descriptor;
    Rgb8 source_colour;
    TerminalColour foreground;
};

struct GlyphGrid final {
    core::u32 source_width{0U};
    core::u32 source_height{0U};
    core::u32 cell_width{0U};
    core::u32 cell_height{0U};
    core::u32 columns{0U};
    core::u32 rows{0U};
    GlyphSettings settings;
    std::vector<GlyphCell> cells;
};

struct GlyphError final {
    std::string message;
};

[[nodiscard]] std::string_view to_string(GlyphSet value) noexcept;
[[nodiscard]] std::string_view to_string(GlyphChoiceMode value) noexcept;
[[nodiscard]] std::string_view to_string(GlyphColourMode value) noexcept;
[[nodiscard]] std::string_view to_string(GlyphLimitedPalette value) noexcept;
[[nodiscard]] std::string_view to_string(GlyphBackgroundMode value) noexcept;
[[nodiscard]] std::string_view to_string(GlyphAlphaMode value) noexcept;

// Reads the disconnected core.glyph.settings node. Because it is ordinary
// semantic recipe data, its values automatically participate in recipe
// fingerprinting, parameter mutation, locks, crossover and lineage.
[[nodiscard]] core::Result<GlyphSettings, GlyphError> glyph_settings_from_recipe(
    const core::Recipe& recipe,
    std::string_view settings_node_id);

// Extract descriptors in row-major glyph-cell order. Partial right/bottom cells
// sample only pixels that actually exist. `motion` is optional; when supplied it
// must contain one vector per source pixel and contributes canonical per-cell
// motion strength/direction without requiring font rasterization.
[[nodiscard]] core::Result<std::vector<GlyphDescriptor>, GlyphError> extract_glyph_descriptors(
    const Image& image,
    core::u32 cell_width,
    core::u32 cell_height,
    GlyphAlphaMode alpha_mode,
    std::span<const GlyphMotionVector> motion = {});

[[nodiscard]] TerminalColour quantize_terminal_colour(
    Rgb8 colour,
    GlyphColourMode mode,
    GlyphLimitedPalette limited_palette) noexcept;

[[nodiscard]] core::Result<GlyphGrid, GlyphError> synthesize_glyph_grid(
    const Image& image,
    const GlyphSettings& settings,
    std::span<const GlyphMotionVector> motion = {});

// Renders recipe output "main" through the canonical CPU path, then performs
// canonical cell synthesis. A supplied tick selects the fixed-tick animation
// renderer; absence of a tick selects the static reference renderer.
[[nodiscard]] core::Result<GlyphGrid, GlyphError> synthesize_recipe_glyph_grid(
    const core::Recipe& recipe,
    std::string_view settings_node_id,
    std::optional<core::u64> tick = std::nullopt);

// Canonical textual forms. Both use UTF-8 without BOM and LF row terminators.
// ANSI output emits deterministic SGR bytes per cell and resets at every row.
[[nodiscard]] core::Result<std::string, GlyphError> serialize_glyph_utf8(const GlyphGrid& grid);
[[nodiscard]] core::Result<std::string, GlyphError> serialize_glyph_ansi(const GlyphGrid& grid);

}  // namespace artminer::nodes
