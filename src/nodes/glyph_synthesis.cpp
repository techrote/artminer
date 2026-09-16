#include "nodes/glyph_synthesis.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/checked_math.hpp"
#include "core/graph.hpp"
#include "nodes/motion_evaluator.hpp"

namespace artminer::nodes {
namespace {

constexpr double kPi = std::numbers::pi_v<double>;
constexpr double kHalfPi = kPi * 0.5;
constexpr double kEdgeEpsilon = 1.0 / 1024.0;

struct GlyphPrototype final {
    char32_t codepoint{U' '};
    double density{0.0};
    bool oriented{false};
    double orientation{0.0};
    double detail{0.0};
    double corner{0.0};
};

[[nodiscard]] GlyphError make_error(std::string message) {
    return GlyphError{std::move(message)};
}

[[nodiscard]] const core::NodeInstance* find_node(
    const core::Recipe& recipe,
    const std::string_view node_id) noexcept {
    const auto found = std::find_if(
        recipe.nodes.begin(),
        recipe.nodes.end(),
        [node_id](const core::NodeInstance& node) { return node.id == node_id; });
    return found == recipe.nodes.end() ? nullptr : &*found;
}

[[nodiscard]] const core::ParameterAssignment* find_parameter(
    const core::NodeInstance& node,
    const std::string_view name) noexcept {
    const auto found = std::find_if(
        node.parameters.begin(),
        node.parameters.end(),
        [name](const core::ParameterAssignment& parameter) { return parameter.name == name; });
    return found == node.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] double normalize_axis_angle(double angle) noexcept {
    angle = std::fmod(angle, kPi);
    if (angle < 0.0) {
        angle += kPi;
    }
    return angle;
}

[[nodiscard]] double axis_distance(const double left, const double right) noexcept {
    const double a = normalize_axis_angle(left);
    const double b = normalize_axis_angle(right);
    const double direct = std::abs(a - b);
    return (std::min)(direct, kPi - direct) / kHalfPi;
}

[[nodiscard]] core::u8 composite_black(const core::u8 channel, const core::u8 alpha) noexcept {
    const core::u32 product = static_cast<core::u32>(channel) * static_cast<core::u32>(alpha);
    return static_cast<core::u8>((product + 127U) / 255U);
}

[[nodiscard]] Rgb8 pixel_rgb(
    const Image& image,
    const core::u32 x,
    const core::u32 y,
    const GlyphAlphaMode alpha_mode) noexcept {
    const std::size_t offset =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 4U;
    const core::u8 alpha = image.rgba[offset + 3U];
    if (alpha_mode == GlyphAlphaMode::ignore) {
        return {image.rgba[offset], image.rgba[offset + 1U], image.rgba[offset + 2U]};
    }
    return {
        composite_black(image.rgba[offset], alpha),
        composite_black(image.rgba[offset + 1U], alpha),
        composite_black(image.rgba[offset + 2U], alpha),
    };
}

[[nodiscard]] double luminance(const Rgb8 colour) noexcept {
    // Integer Rec.709-like weights summing to 256 keep the source-byte mapping
    // explicit before normalization: 54 R + 183 G + 19 B.
    const core::u32 weighted = 54U * static_cast<core::u32>(colour.r) +
        183U * static_cast<core::u32>(colour.g) + 19U * static_cast<core::u32>(colour.b);
    return static_cast<double>(weighted) / 65280.0;
}

[[nodiscard]] double pixel_luminance(
    const Image& image,
    const core::u32 x,
    const core::u32 y,
    const GlyphAlphaMode alpha_mode) noexcept {
    return luminance(pixel_rgb(image, x, y, alpha_mode));
}

[[nodiscard]] core::Result<core::u64, GlyphError> validate_grid_shape(
    const Image& image,
    const core::u32 cell_width,
    const core::u32 cell_height) {
    const auto byte_count = core::checked_image_byte_count(image.width, image.height, 4U);
    if (byte_count.is_error() || byte_count.value() != static_cast<core::u64>(image.rgba.size())) {
        return core::Result<core::u64, GlyphError>::failure(make_error(
            "glyph synthesis requires a valid tightly packed RGBA8 source image"));
    }
    if (cell_width == 0U || cell_height == 0U) {
        return core::Result<core::u64, GlyphError>::failure(make_error(
            "glyph cell dimensions must be greater than zero"));
    }
    const core::u64 columns =
        (static_cast<core::u64>(image.width) + static_cast<core::u64>(cell_width) - 1ULL) /
        static_cast<core::u64>(cell_width);
    const core::u64 rows =
        (static_cast<core::u64>(image.height) + static_cast<core::u64>(cell_height) - 1ULL) /
        static_cast<core::u64>(cell_height);
    auto cells = core::checked_multiply_u64(columns, rows);
    if (cells.is_error() || cells.value() == 0U || cells.value() > kMaximumGlyphCells) {
        return core::Result<core::u64, GlyphError>::failure(make_error(
            "glyph grid exceeds the canonical 262144-cell working limit"));
    }
    return core::Result<core::u64, GlyphError>::success(cells.value());
}

[[nodiscard]] std::vector<GlyphPrototype> glyph_prototypes(const GlyphSet set) {
    switch (set) {
    case GlyphSet::ascii_density:
        return {
            {U' ', 0.00, false, 0.0, 0.00, 0.00},
            {U'.', 0.10, false, 0.0, 0.10, 0.05},
            {U':', 0.18, false, 0.0, 0.18, 0.08},
            {U'-', 0.28, true, 0.0, 0.25, 0.05},
            {U'=', 0.38, true, 0.0, 0.35, 0.08},
            {U'+', 0.48, false, 0.0, 0.55, 0.65},
            {U'*', 0.58, false, 0.0, 0.70, 0.80},
            {U'#', 0.72, false, 0.0, 0.80, 0.70},
            {U'%', 0.84, false, 0.0, 0.85, 0.55},
            {U'@', 0.96, false, 0.0, 0.95, 0.65},
        };
    case GlyphSet::blocks:
        return {
            {U' ', 0.00, false, 0.0, 0.00, 0.00},
            {U'░', 0.25, false, 0.0, 0.45, 0.05},
            {U'▒', 0.50, false, 0.0, 0.55, 0.05},
            {U'▓', 0.75, false, 0.0, 0.65, 0.05},
            {U'█', 1.00, false, 0.0, 0.10, 0.00},
        };
    case GlyphSet::lines:
        return {
            {U' ', 0.00, false, 0.0, 0.00, 0.00},
            {U'·', 0.12, false, 0.0, 0.12, 0.05},
            {U'─', 0.50, true, 0.0, 0.20, 0.05},
            {U'│', 0.50, true, kHalfPi, 0.20, 0.05},
            {U'╲', 0.50, true, kPi * 0.25, 0.30, 0.08},
            {U'╱', 0.50, true, kPi * 0.75, 0.30, 0.08},
            {U'┌', 0.52, false, 0.0, 0.45, 0.95},
            {U'┐', 0.52, false, 0.0, 0.45, 0.95},
            {U'└', 0.52, false, 0.0, 0.45, 0.95},
            {U'┘', 0.52, false, 0.0, 0.45, 0.95},
            {U'┼', 0.62, false, 0.0, 0.75, 1.00},
            {U'█', 1.00, false, 0.0, 0.10, 0.00},
        };
    case GlyphSet::sparkle:
        return {
            {U' ', 0.00, false, 0.0, 0.00, 0.00},
            {U'.', 0.10, false, 0.0, 0.10, 0.05},
            {U'·', 0.20, false, 0.0, 0.20, 0.08},
            {U'+', 0.40, false, 0.0, 0.55, 0.70},
            {U'*', 0.58, false, 0.0, 0.70, 0.82},
            {U'✧', 0.72, false, 0.0, 0.80, 0.92},
            {U'✦', 0.90, false, 0.0, 0.92, 1.00},
        };
    }
    return {{U' ', 0.0, false, 0.0, 0.0, 0.0}};
}

[[nodiscard]] char32_t choose_glyph(const GlyphDescriptor& descriptor, const GlyphSettings& settings) {
    const auto prototypes = glyph_prototypes(settings.glyph_set);
    double density_weight = settings.density_weight;
    double orientation_weight = settings.orientation_weight;
    double detail_weight = settings.detail_weight;
    double corner_weight = settings.corner_weight;
    double motion_weight = settings.motion_weight;

    if (settings.choice_mode == GlyphChoiceMode::density) {
        orientation_weight = 0.0;
        detail_weight = 0.0;
        corner_weight = 0.0;
        motion_weight = 0.0;
    } else if (settings.choice_mode == GlyphChoiceMode::structure) {
        density_weight *= 0.25;
    }

    std::size_t best_index = 0U;
    double best_score = (std::numeric_limits<double>::max)();
    for (std::size_t index = 0U; index < prototypes.size(); ++index) {
        const GlyphPrototype& prototype = prototypes[index];
        const double density_delta = descriptor.density - prototype.density;
        const double detail_delta = descriptor.detail - prototype.detail;
        const double corner_delta = descriptor.corner - prototype.corner;
        double score = density_weight * density_delta * density_delta +
            detail_weight * detail_delta * detail_delta +
            corner_weight * corner_delta * corner_delta;

        if (orientation_weight > 0.0) {
            if (prototype.oriented) {
                if (descriptor.edge_strength > kEdgeEpsilon) {
                    const double distance = axis_distance(descriptor.edge_orientation, prototype.orientation);
                    score += orientation_weight * descriptor.edge_strength * distance * distance;
                } else {
                    score += orientation_weight * 0.02;
                }
            } else if (descriptor.edge_strength > kEdgeEpsilon) {
                score += orientation_weight * descriptor.edge_strength * 0.35;
            }
        }

        if (motion_weight > 0.0 && descriptor.motion_strength > kEdgeEpsilon) {
            if (prototype.oriented) {
                const double distance = axis_distance(descriptor.motion_direction, prototype.orientation);
                score += motion_weight * descriptor.motion_strength * distance * distance;
            } else {
                score += motion_weight * descriptor.motion_strength * 0.25;
            }
        }

        if (score < best_score) {
            best_score = score;
            best_index = index;
        }
    }
    return prototypes[best_index].codepoint;
}

constexpr std::array<Rgb8, 16U> kAnsi16{{
    {0U, 0U, 0U},
    {128U, 0U, 0U},
    {0U, 128U, 0U},
    {128U, 128U, 0U},
    {0U, 0U, 128U},
    {128U, 0U, 128U},
    {0U, 128U, 128U},
    {192U, 192U, 192U},
    {128U, 128U, 128U},
    {255U, 0U, 0U},
    {0U, 255U, 0U},
    {255U, 255U, 0U},
    {0U, 0U, 255U},
    {255U, 0U, 255U},
    {0U, 255U, 255U},
    {255U, 255U, 255U},
}};

[[nodiscard]] core::u32 colour_distance_squared(const Rgb8 left, const Rgb8 right) noexcept {
    const std::int32_t dr = static_cast<std::int32_t>(left.r) - static_cast<std::int32_t>(right.r);
    const std::int32_t dg = static_cast<std::int32_t>(left.g) - static_cast<std::int32_t>(right.g);
    const std::int32_t db = static_cast<std::int32_t>(left.b) - static_cast<std::int32_t>(right.b);
    return static_cast<core::u32>(dr * dr + dg * dg + db * db);
}

[[nodiscard]] TerminalColour nearest_ansi16(
    const Rgb8 colour,
    const std::span<const core::u8> allowed) noexcept {
    core::u32 best_index = static_cast<core::u32>(allowed.front());
    core::u32 best_distance = (std::numeric_limits<core::u32>::max)();
    for (const core::u8 candidate : allowed) {
        const core::u32 distance = colour_distance_squared(colour, kAnsi16[candidate]);
        if (distance < best_distance ||
            (distance == best_distance && static_cast<core::u32>(candidate) < best_index)) {
            best_distance = distance;
            best_index = candidate;
        }
    }
    return TerminalColour{TerminalColourEncoding::ansi16, best_index, kAnsi16[best_index]};
}

[[nodiscard]] Rgb8 ansi256_rgb(const core::u32 index) noexcept {
    if (index < 16U) {
        return kAnsi16[index];
    }
    if (index < 232U) {
        constexpr std::array<core::u8, 6U> levels{{0U, 95U, 135U, 175U, 215U, 255U}};
        const core::u32 offset = index - 16U;
        const core::u32 r = offset / 36U;
        const core::u32 g = (offset / 6U) % 6U;
        const core::u32 b = offset % 6U;
        return {levels[r], levels[g], levels[b]};
    }
    const core::u8 gray = static_cast<core::u8>(8U + 10U * (index - 232U));
    return {gray, gray, gray};
}

[[nodiscard]] Rgb8 average_cell_colour(
    const Image& image,
    const core::u32 x0,
    const core::u32 y0,
    const core::u32 x1,
    const core::u32 y1,
    const GlyphAlphaMode alpha_mode) noexcept {
    core::u64 sum_r = 0U;
    core::u64 sum_g = 0U;
    core::u64 sum_b = 0U;
    core::u64 count = 0U;
    for (core::u32 y = y0; y < y1; ++y) {
        for (core::u32 x = x0; x < x1; ++x) {
            const Rgb8 colour = pixel_rgb(image, x, y, alpha_mode);
            sum_r += colour.r;
            sum_g += colour.g;
            sum_b += colour.b;
            ++count;
        }
    }
    if (count == 0U) {
        return {};
    }
    return {
        static_cast<core::u8>((sum_r + count / 2U) / count),
        static_cast<core::u8>((sum_g + count / 2U) / count),
        static_cast<core::u8>((sum_b + count / 2U) / count),
    };
}

[[nodiscard]] bool valid_glyph_scalar(const char32_t codepoint) noexcept {
    const auto value = static_cast<std::uint32_t>(codepoint);
    if (value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU)) {
        return false;
    }
    if (value <= 0x1fU || (value >= 0x7fU && value <= 0x9fU)) {
        return false;
    }
    if (value >= 0xfdd0U && value <= 0xfdefU) {
        return false;
    }
    return (value & 0xffffU) != 0xfffeU && (value & 0xffffU) != 0xffffU;
}

[[nodiscard]] bool append_utf8(std::string& output, const char32_t codepoint) {
    if (!valid_glyph_scalar(codepoint)) {
        return false;
    }
    const auto value = static_cast<std::uint32_t>(codepoint);
    if (value <= 0x7fU) {
        output.push_back(static_cast<char>(value));
    } else if (value <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (value >> 6U)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    } else if (value <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (value >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (value >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    }
    return true;
}

[[nodiscard]] core::Result<void, GlyphError> validate_grid_for_serialization(const GlyphGrid& grid) {
    if (grid.columns == 0U || grid.rows == 0U || grid.cell_width == 0U || grid.cell_height == 0U) {
        return core::Result<void, GlyphError>::failure(make_error(
            "glyph grid dimensions must be non-zero"));
    }
    auto expected = core::checked_multiply_u64(grid.columns, grid.rows);
    if (expected.is_error() || expected.value() != static_cast<core::u64>(grid.cells.size()) ||
        expected.value() > kMaximumGlyphCells) {
        return core::Result<void, GlyphError>::failure(make_error(
            "glyph grid cell storage does not match its declared dimensions"));
    }
    return core::Result<void, GlyphError>::success();
}

[[nodiscard]] core::Result<GlyphSet, GlyphError> parse_glyph_set(const std::string& value) {
    if (value == "ascii-density") {
        return core::Result<GlyphSet, GlyphError>::success(GlyphSet::ascii_density);
    }
    if (value == "blocks") {
        return core::Result<GlyphSet, GlyphError>::success(GlyphSet::blocks);
    }
    if (value == "lines") {
        return core::Result<GlyphSet, GlyphError>::success(GlyphSet::lines);
    }
    if (value == "sparkle") {
        return core::Result<GlyphSet, GlyphError>::success(GlyphSet::sparkle);
    }
    return core::Result<GlyphSet, GlyphError>::failure(make_error("unknown glyph_set value"));
}

[[nodiscard]] core::Result<GlyphChoiceMode, GlyphError> parse_choice_mode(const std::string& value) {
    if (value == "density") {
        return core::Result<GlyphChoiceMode, GlyphError>::success(GlyphChoiceMode::density);
    }
    if (value == "structure") {
        return core::Result<GlyphChoiceMode, GlyphError>::success(GlyphChoiceMode::structure);
    }
    if (value == "balanced") {
        return core::Result<GlyphChoiceMode, GlyphError>::success(GlyphChoiceMode::balanced);
    }
    return core::Result<GlyphChoiceMode, GlyphError>::failure(make_error("unknown choice_mode value"));
}

[[nodiscard]] core::Result<GlyphColourMode, GlyphError> parse_colour_mode(const std::string& value) {
    if (value == "monochrome") {
        return core::Result<GlyphColourMode, GlyphError>::success(GlyphColourMode::monochrome);
    }
    if (value == "limited") {
        return core::Result<GlyphColourMode, GlyphError>::success(GlyphColourMode::limited);
    }
    if (value == "ansi16") {
        return core::Result<GlyphColourMode, GlyphError>::success(GlyphColourMode::ansi16);
    }
    if (value == "ansi256") {
        return core::Result<GlyphColourMode, GlyphError>::success(GlyphColourMode::ansi256);
    }
    if (value == "truecolor") {
        return core::Result<GlyphColourMode, GlyphError>::success(GlyphColourMode::truecolor);
    }
    return core::Result<GlyphColourMode, GlyphError>::failure(make_error("unknown colour_mode value"));
}

[[nodiscard]] core::Result<GlyphLimitedPalette, GlyphError> parse_limited_palette(const std::string& value) {
    if (value == "mono4") {
        return core::Result<GlyphLimitedPalette, GlyphError>::success(GlyphLimitedPalette::mono4);
    }
    if (value == "warm8") {
        return core::Result<GlyphLimitedPalette, GlyphError>::success(GlyphLimitedPalette::warm8);
    }
    if (value == "cool8") {
        return core::Result<GlyphLimitedPalette, GlyphError>::success(GlyphLimitedPalette::cool8);
    }
    return core::Result<GlyphLimitedPalette, GlyphError>::failure(make_error("unknown limited_palette value"));
}

[[nodiscard]] core::Result<GlyphBackgroundMode, GlyphError> parse_background_mode(const std::string& value) {
    if (value == "black") {
        return core::Result<GlyphBackgroundMode, GlyphError>::success(GlyphBackgroundMode::black);
    }
    if (value == "terminal-default") {
        return core::Result<GlyphBackgroundMode, GlyphError>::success(GlyphBackgroundMode::terminal_default);
    }
    return core::Result<GlyphBackgroundMode, GlyphError>::failure(make_error("unknown background_mode value"));
}

[[nodiscard]] core::Result<GlyphAlphaMode, GlyphError> parse_alpha_mode(const std::string& value) {
    if (value == "composite-black") {
        return core::Result<GlyphAlphaMode, GlyphError>::success(GlyphAlphaMode::composite_black);
    }
    if (value == "ignore") {
        return core::Result<GlyphAlphaMode, GlyphError>::success(GlyphAlphaMode::ignore);
    }
    return core::Result<GlyphAlphaMode, GlyphError>::failure(make_error("unknown alpha_mode value"));
}

[[nodiscard]] std::string enum_value(const core::NodeInstance& node, const std::string_view name) {
    return std::get<std::string>(find_parameter(node, name)->value);
}

[[nodiscard]] double real_value(const core::NodeInstance& node, const std::string_view name) {
    return std::get<double>(find_parameter(node, name)->value);
}

[[nodiscard]] core::i64 integer_value(const core::NodeInstance& node, const std::string_view name) {
    return std::get<core::i64>(find_parameter(node, name)->value);
}

}  // namespace

std::string_view to_string(const GlyphSet value) noexcept {
    switch (value) {
    case GlyphSet::ascii_density:
        return "ascii-density";
    case GlyphSet::blocks:
        return "blocks";
    case GlyphSet::lines:
        return "lines";
    case GlyphSet::sparkle:
        return "sparkle";
    }
    return "unknown";
}

std::string_view to_string(const GlyphChoiceMode value) noexcept {
    switch (value) {
    case GlyphChoiceMode::density:
        return "density";
    case GlyphChoiceMode::structure:
        return "structure";
    case GlyphChoiceMode::balanced:
        return "balanced";
    }
    return "unknown";
}

std::string_view to_string(const GlyphColourMode value) noexcept {
    switch (value) {
    case GlyphColourMode::monochrome:
        return "monochrome";
    case GlyphColourMode::limited:
        return "limited";
    case GlyphColourMode::ansi16:
        return "ansi16";
    case GlyphColourMode::ansi256:
        return "ansi256";
    case GlyphColourMode::truecolor:
        return "truecolor";
    }
    return "unknown";
}

std::string_view to_string(const GlyphLimitedPalette value) noexcept {
    switch (value) {
    case GlyphLimitedPalette::mono4:
        return "mono4";
    case GlyphLimitedPalette::warm8:
        return "warm8";
    case GlyphLimitedPalette::cool8:
        return "cool8";
    }
    return "unknown";
}

std::string_view to_string(const GlyphBackgroundMode value) noexcept {
    switch (value) {
    case GlyphBackgroundMode::black:
        return "black";
    case GlyphBackgroundMode::terminal_default:
        return "terminal-default";
    }
    return "unknown";
}

std::string_view to_string(const GlyphAlphaMode value) noexcept {
    switch (value) {
    case GlyphAlphaMode::composite_black:
        return "composite-black";
    case GlyphAlphaMode::ignore:
        return "ignore";
    }
    return "unknown";
}

core::Result<GlyphSettings, GlyphError> glyph_settings_from_recipe(
    const core::Recipe& recipe,
    const std::string_view settings_node_id) {
    const auto validation_errors = core::validate_recipe(recipe);
    if (!validation_errors.empty()) {
        return core::Result<GlyphSettings, GlyphError>::failure(make_error(
            "glyph settings require a valid recipe: " + validation_errors.front().message));
    }
    const core::NodeInstance* node = find_node(recipe, settings_node_id);
    if (node == nullptr || node->type_id != "core.glyph.settings") {
        return core::Result<GlyphSettings, GlyphError>::failure(make_error(
            "glyph settings node '" + std::string(settings_node_id) + "' was not found or has the wrong type"));
    }

    GlyphSettings settings;
    settings.cell_width = static_cast<core::u32>(integer_value(*node, "cell_width"));
    settings.cell_height = static_cast<core::u32>(integer_value(*node, "cell_height"));

    auto glyph_set = parse_glyph_set(enum_value(*node, "glyph_set"));
    auto choice_mode = parse_choice_mode(enum_value(*node, "choice_mode"));
    auto colour_mode = parse_colour_mode(enum_value(*node, "colour_mode"));
    auto limited_palette = parse_limited_palette(enum_value(*node, "limited_palette"));
    auto background_mode = parse_background_mode(enum_value(*node, "background_mode"));
    auto alpha_mode = parse_alpha_mode(enum_value(*node, "alpha_mode"));
    if (glyph_set.is_error() || choice_mode.is_error() || colour_mode.is_error() || limited_palette.is_error() ||
        background_mode.is_error() || alpha_mode.is_error()) {
        return core::Result<GlyphSettings, GlyphError>::failure(make_error(
            "glyph settings contain an unsupported enumeration value"));
    }
    settings.glyph_set = glyph_set.value();
    settings.choice_mode = choice_mode.value();
    settings.density_weight = real_value(*node, "density_weight");
    settings.orientation_weight = real_value(*node, "orientation_weight");
    settings.detail_weight = real_value(*node, "detail_weight");
    settings.corner_weight = real_value(*node, "corner_weight");
    settings.motion_weight = real_value(*node, "motion_weight");
    settings.colour_mode = colour_mode.value();
    settings.limited_palette = limited_palette.value();
    settings.background_mode = background_mode.value();
    settings.alpha_mode = alpha_mode.value();
    return core::Result<GlyphSettings, GlyphError>::success(std::move(settings));
}

core::Result<std::vector<GlyphDescriptor>, GlyphError> extract_glyph_descriptors(
    const Image& image,
    const core::u32 cell_width,
    const core::u32 cell_height,
    const GlyphAlphaMode alpha_mode,
    const std::span<const GlyphMotionVector> motion) {
    auto cell_count = validate_grid_shape(image, cell_width, cell_height);
    if (cell_count.is_error()) {
        return core::Result<std::vector<GlyphDescriptor>, GlyphError>::failure(cell_count.error());
    }
    const core::u64 pixel_count = static_cast<core::u64>(image.width) * static_cast<core::u64>(image.height);
    if (!motion.empty() && static_cast<core::u64>(motion.size()) != pixel_count) {
        return core::Result<std::vector<GlyphDescriptor>, GlyphError>::failure(make_error(
            "motion descriptor input must contain exactly one vector per source pixel"));
    }

    const core::u32 columns = (image.width + cell_width - 1U) / cell_width;
    const core::u32 rows = (image.height + cell_height - 1U) / cell_height;
    std::vector<GlyphDescriptor> descriptors;
    descriptors.reserve(static_cast<std::size_t>(cell_count.value()));

    for (core::u32 cell_y = 0U; cell_y < rows; ++cell_y) {
        for (core::u32 cell_x = 0U; cell_x < columns; ++cell_x) {
            const core::u32 x0 = cell_x * cell_width;
            const core::u32 y0 = cell_y * cell_height;
            const core::u32 x1 = (std::min)(image.width, x0 + cell_width);
            const core::u32 y1 = (std::min)(image.height, y0 + cell_height);
            double sum_luminance = 0.0;
            double sum_gx = 0.0;
            double sum_gy = 0.0;
            double sum_detail = 0.0;
            double sum_motion_x = 0.0;
            double sum_motion_y = 0.0;
            core::u64 samples = 0U;
            core::u64 detail_samples = 0U;

            for (core::u32 y = y0; y < y1; ++y) {
                for (core::u32 x = x0; x < x1; ++x) {
                    const core::u32 left_x = x == 0U ? 0U : x - 1U;
                    const core::u32 right_x = (std::min)(image.width - 1U, x + 1U);
                    const core::u32 up_y = y == 0U ? 0U : y - 1U;
                    const core::u32 down_y = (std::min)(image.height - 1U, y + 1U);
                    const double here = pixel_luminance(image, x, y, alpha_mode);
                    const double gx = 0.5 * (
                        pixel_luminance(image, right_x, y, alpha_mode) -
                        pixel_luminance(image, left_x, y, alpha_mode));
                    const double gy = 0.5 * (
                        pixel_luminance(image, x, down_y, alpha_mode) -
                        pixel_luminance(image, x, up_y, alpha_mode));
                    sum_luminance += here;
                    sum_gx += gx;
                    sum_gy += gy;
                    ++samples;

                    if (x + 1U < image.width) {
                        sum_detail += std::abs(pixel_luminance(image, x + 1U, y, alpha_mode) - here);
                        ++detail_samples;
                    }
                    if (y + 1U < image.height) {
                        sum_detail += std::abs(pixel_luminance(image, x, y + 1U, alpha_mode) - here);
                        ++detail_samples;
                    }
                    if (!motion.empty()) {
                        const std::size_t motion_index =
                            static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                            static_cast<std::size_t>(x);
                        sum_motion_x += motion[motion_index].x;
                        sum_motion_y += motion[motion_index].y;
                    }
                }
            }

            const double inv_samples = 1.0 / static_cast<double>(samples);
            const double average_gx = sum_gx * inv_samples;
            const double average_gy = sum_gy * inv_samples;
            GlyphDescriptor descriptor;
            descriptor.luminance = std::clamp(sum_luminance * inv_samples, 0.0, 1.0);
            descriptor.density = descriptor.luminance;
            descriptor.gradient_x = average_gx;
            descriptor.gradient_y = average_gy;
            descriptor.edge_strength = std::clamp(std::hypot(average_gx, average_gy), 0.0, 1.0);
            if (descriptor.edge_strength > kEdgeEpsilon) {
                descriptor.edge_orientation = normalize_axis_angle(std::atan2(average_gy, average_gx) + kHalfPi);
            }
            descriptor.detail = detail_samples == 0U
                ? 0.0
                : std::clamp(sum_detail / static_cast<double>(detail_samples), 0.0, 1.0);
            descriptor.corner = std::clamp(
                4.0 * (std::min)(std::abs(average_gx), std::abs(average_gy)) + descriptor.detail * 0.25,
                0.0,
                1.0);

            if (!motion.empty()) {
                const double motion_x = sum_motion_x * inv_samples;
                const double motion_y = sum_motion_y * inv_samples;
                descriptor.motion_strength = std::clamp(std::hypot(motion_x, motion_y), 0.0, 1.0);
                if (descriptor.motion_strength > kEdgeEpsilon) {
                    descriptor.motion_direction = normalize_axis_angle(std::atan2(motion_y, motion_x));
                }
            }
            descriptors.push_back(descriptor);
        }
    }
    return core::Result<std::vector<GlyphDescriptor>, GlyphError>::success(std::move(descriptors));
}

TerminalColour quantize_terminal_colour(
    const Rgb8 colour,
    const GlyphColourMode mode,
    const GlyphLimitedPalette limited_palette) noexcept {
    if (mode == GlyphColourMode::truecolor) {
        return TerminalColour{TerminalColourEncoding::truecolor, 0U, colour};
    }
    if (mode == GlyphColourMode::monochrome) {
        return TerminalColour{TerminalColourEncoding::ansi16, 15U, kAnsi16[15U]};
    }
    if (mode == GlyphColourMode::limited) {
        constexpr std::array<core::u8, 4U> mono4{{0U, 8U, 7U, 15U}};
        constexpr std::array<core::u8, 8U> warm8{{0U, 1U, 9U, 3U, 11U, 5U, 13U, 15U}};
        constexpr std::array<core::u8, 8U> cool8{{0U, 4U, 12U, 6U, 14U, 2U, 10U, 15U}};
        if (limited_palette == GlyphLimitedPalette::mono4) {
            return nearest_ansi16(colour, mono4);
        }
        if (limited_palette == GlyphLimitedPalette::warm8) {
            return nearest_ansi16(colour, warm8);
        }
        return nearest_ansi16(colour, cool8);
    }
    if (mode == GlyphColourMode::ansi16) {
        constexpr std::array<core::u8, 16U> all{{
            0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U}};
        return nearest_ansi16(colour, all);
    }

    core::u32 best_index = 0U;
    core::u32 best_distance = (std::numeric_limits<core::u32>::max)();
    for (core::u32 index = 0U; index < 256U; ++index) {
        const Rgb8 candidate = ansi256_rgb(index);
        const core::u32 distance = colour_distance_squared(colour, candidate);
        if (distance < best_distance || (distance == best_distance && index < best_index)) {
            best_distance = distance;
            best_index = index;
        }
    }
    return TerminalColour{TerminalColourEncoding::ansi256, best_index, ansi256_rgb(best_index)};
}

core::Result<GlyphGrid, GlyphError> synthesize_glyph_grid(
    const Image& image,
    const GlyphSettings& settings,
    const std::span<const GlyphMotionVector> motion) {
    auto descriptors = extract_glyph_descriptors(
        image,
        settings.cell_width,
        settings.cell_height,
        settings.alpha_mode,
        motion);
    if (descriptors.is_error()) {
        return core::Result<GlyphGrid, GlyphError>::failure(descriptors.error());
    }

    GlyphGrid grid;
    grid.source_width = image.width;
    grid.source_height = image.height;
    grid.cell_width = settings.cell_width;
    grid.cell_height = settings.cell_height;
    grid.columns = (image.width + settings.cell_width - 1U) / settings.cell_width;
    grid.rows = (image.height + settings.cell_height - 1U) / settings.cell_height;
    grid.settings = settings;
    grid.cells.reserve(descriptors.value().size());

    std::size_t descriptor_index = 0U;
    for (core::u32 cell_y = 0U; cell_y < grid.rows; ++cell_y) {
        for (core::u32 cell_x = 0U; cell_x < grid.columns; ++cell_x) {
            const core::u32 x0 = cell_x * settings.cell_width;
            const core::u32 y0 = cell_y * settings.cell_height;
            const core::u32 x1 = (std::min)(image.width, x0 + settings.cell_width);
            const core::u32 y1 = (std::min)(image.height, y0 + settings.cell_height);
            GlyphCell cell;
            cell.descriptor = descriptors.value()[descriptor_index++];
            cell.codepoint = choose_glyph(cell.descriptor, settings);
            cell.source_colour = average_cell_colour(image, x0, y0, x1, y1, settings.alpha_mode);
            cell.foreground = quantize_terminal_colour(
                cell.source_colour,
                settings.colour_mode,
                settings.limited_palette);
            grid.cells.push_back(cell);
        }
    }
    return core::Result<GlyphGrid, GlyphError>::success(std::move(grid));
}

core::Result<GlyphGrid, GlyphError> synthesize_recipe_glyph_grid(
    const core::Recipe& recipe,
    const std::string_view settings_node_id,
    const std::optional<core::u64> tick) {
    auto settings = glyph_settings_from_recipe(recipe, settings_node_id);
    if (settings.is_error()) {
        return core::Result<GlyphGrid, GlyphError>::failure(settings.error());
    }

    Image image;
    if (tick.has_value()) {
        FrameSnapshotCache cache(16U);
        auto rendered = render_animation_reference(recipe, *tick, "main", &cache);
        if (rendered.is_error()) {
            return core::Result<GlyphGrid, GlyphError>::failure(make_error(
                "glyph animation source render failed at tick " + std::to_string(*tick) + ": " +
                rendered.error().message));
        }
        image = std::move(rendered).value();
    } else {
        auto rendered = render_reference(recipe, "main");
        if (rendered.is_error()) {
            return core::Result<GlyphGrid, GlyphError>::failure(make_error(
                "glyph source render failed: " + rendered.error().message));
        }
        image = std::move(rendered).value();
    }
    return synthesize_glyph_grid(image, settings.value());
}

core::Result<std::string, GlyphError> serialize_glyph_utf8(const GlyphGrid& grid) {
    auto valid = validate_grid_for_serialization(grid);
    if (valid.is_error()) {
        return core::Result<std::string, GlyphError>::failure(valid.error());
    }
    std::string output;
    output.reserve(grid.cells.size() * 3U + static_cast<std::size_t>(grid.rows));
    for (core::u32 y = 0U; y < grid.rows; ++y) {
        for (core::u32 x = 0U; x < grid.columns; ++x) {
            const std::size_t index =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(grid.columns) + static_cast<std::size_t>(x);
            if (!append_utf8(output, grid.cells[index].codepoint)) {
                return core::Result<std::string, GlyphError>::failure(make_error(
                    "glyph grid contains an unsupported Unicode scalar/control codepoint"));
            }
        }
        output.push_back('\n');
    }
    return core::Result<std::string, GlyphError>::success(std::move(output));
}

core::Result<std::string, GlyphError> serialize_glyph_ansi(const GlyphGrid& grid) {
    auto valid = validate_grid_for_serialization(grid);
    if (valid.is_error()) {
        return core::Result<std::string, GlyphError>::failure(valid.error());
    }
    std::string output;
    output.reserve(grid.cells.size() * 24U + static_cast<std::size_t>(grid.rows) * 5U);
    for (core::u32 y = 0U; y < grid.rows; ++y) {
        for (core::u32 x = 0U; x < grid.columns; ++x) {
            const std::size_t index =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(grid.columns) + static_cast<std::size_t>(x);
            const GlyphCell& cell = grid.cells[index];
            if (!valid_glyph_scalar(cell.codepoint)) {
                return core::Result<std::string, GlyphError>::failure(make_error(
                    "glyph grid contains an unsupported Unicode scalar/control codepoint"));
            }

            output += "\x1b[";
            if (cell.foreground.encoding == TerminalColourEncoding::ansi16) {
                const core::u32 foreground = cell.foreground.index;
                if (foreground > 15U) {
                    return core::Result<std::string, GlyphError>::failure(make_error(
                        "ANSI16 glyph colour index is outside 0..15"));
                }
                const core::u32 sgr = foreground < 8U ? 30U + foreground : 90U + (foreground - 8U);
                output += std::to_string(sgr);
                if (grid.settings.background_mode == GlyphBackgroundMode::black) {
                    output += ";40";
                }
            } else if (cell.foreground.encoding == TerminalColourEncoding::ansi256) {
                if (cell.foreground.index > 255U) {
                    return core::Result<std::string, GlyphError>::failure(make_error(
                        "ANSI256 glyph colour index is outside 0..255"));
                }
                output += "38;5;";
                output += std::to_string(cell.foreground.index);
                if (grid.settings.background_mode == GlyphBackgroundMode::black) {
                    output += ";48;5;0";
                }
            } else {
                output += "38;2;";
                output += std::to_string(cell.foreground.rgb.r);
                output.push_back(';');
                output += std::to_string(cell.foreground.rgb.g);
                output.push_back(';');
                output += std::to_string(cell.foreground.rgb.b);
                if (grid.settings.background_mode == GlyphBackgroundMode::black) {
                    output += ";48;2;0;0;0";
                }
            }
            output.push_back('m');
            if (!append_utf8(output, cell.codepoint)) {
                return core::Result<std::string, GlyphError>::failure(make_error(
                    "glyph grid contains an unsupported Unicode scalar/control codepoint"));
            }
        }
        output += "\x1b[0m\n";
    }
    return core::Result<std::string, GlyphError>::success(std::move(output));
}

}  // namespace artminer::nodes
