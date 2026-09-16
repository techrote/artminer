#include "quarry/seam.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "core/checked_math.hpp"

namespace artminer::quarry {
namespace {

[[nodiscard]] bool valid_image(const nodes::Image& image) noexcept {
    if (image.width == 0U || image.height == 0U) {
        return false;
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(image.width) * image.height;
    return pixels <= static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()) / 4ULL &&
        image.rgba.size() == static_cast<std::size_t>(pixels * 4ULL);
}

[[nodiscard]] core::u8 luminance_at(const nodes::Image& image, const std::size_t pixel) noexcept {
    const std::size_t offset = pixel * 4U;
    const unsigned int red = image.rgba[offset];
    const unsigned int green = image.rgba[offset + 1U];
    const unsigned int blue = image.rgba[offset + 2U];
    const unsigned int alpha = image.rgba[offset + 3U];
    const unsigned int luma = (54U * red + 183U * green + 19U * blue + 128U) >> 8U;
    return static_cast<core::u8>((luma * alpha + 127U) / 255U);
}

[[nodiscard]] MetricError invalid_image_error() {
    return MetricError{MetricErrorCode::invalid_image, "tile seam diagnostics require a valid canonical RGBA8 image"};
}

}  // namespace

core::Result<TileSeamDiagnostics, MetricError> compute_tile_seam_diagnostics(const nodes::Image& image) {
    if (!valid_image(image)) {
        return core::Result<TileSeamDiagnostics, MetricError>::failure(invalid_image_error());
    }

    auto combined = compute_metrics({image}, {"tile_seam_error"});
    if (combined.is_error()) {
        return core::Result<TileSeamDiagnostics, MetricError>::failure(combined.error());
    }

    std::uint64_t horizontal_difference = 0U;
    std::uint64_t vertical_difference = 0U;
    if (image.width > 1U) {
        for (std::size_t y = 0U; y < image.height; ++y) {
            const std::size_t left = y * image.width;
            const std::size_t right = left + image.width - 1U;
            horizontal_difference += static_cast<std::uint64_t>(std::abs(
                static_cast<int>(luminance_at(image, left)) - static_cast<int>(luminance_at(image, right))));
        }
    }
    if (image.height > 1U) {
        for (std::size_t x = 0U; x < image.width; ++x) {
            const std::size_t top = x;
            const std::size_t bottom = (static_cast<std::size_t>(image.height) - 1U) * image.width + x;
            vertical_difference += static_cast<std::uint64_t>(std::abs(
                static_cast<int>(luminance_at(image, top)) - static_cast<int>(luminance_at(image, bottom))));
        }
    }

    const double horizontal = image.width <= 1U ? 0.0 :
        static_cast<double>(horizontal_difference) /
            (255.0 * static_cast<double>(image.height));
    const double vertical = image.height <= 1U ? 0.0 :
        static_cast<double>(vertical_difference) /
            (255.0 * static_cast<double>(image.width));
    const double authoritative = combined.value().empty() ? 0.0 : combined.value().front().value;
    return core::Result<TileSeamDiagnostics, MetricError>::success(
        TileSeamDiagnostics{horizontal, vertical, authoritative});
}

core::Result<nodes::Image, MetricError> make_tile_seam_inspection(const nodes::Image& image) {
    if (!valid_image(image)) {
        return core::Result<nodes::Image, MetricError>::failure(invalid_image_error());
    }
    const std::uint64_t doubled_width = static_cast<std::uint64_t>(image.width) * 2ULL;
    const std::uint64_t doubled_height = static_cast<std::uint64_t>(image.height) * 2ULL;
    if (doubled_width > static_cast<std::uint64_t>((std::numeric_limits<core::u32>::max)()) ||
        doubled_height > static_cast<std::uint64_t>((std::numeric_limits<core::u32>::max)())) {
        return core::Result<nodes::Image, MetricError>::failure(invalid_image_error());
    }
    auto bytes = core::checked_image_byte_count(
        static_cast<core::u32>(doubled_width), static_cast<core::u32>(doubled_height), 4U);
    if (bytes.is_error() || bytes.value() > static_cast<core::u64>((std::numeric_limits<std::size_t>::max)())) {
        return core::Result<nodes::Image, MetricError>::failure(invalid_image_error());
    }

    nodes::Image inspection;
    inspection.width = static_cast<core::u32>(doubled_width);
    inspection.height = static_cast<core::u32>(doubled_height);
    inspection.rgba.resize(static_cast<std::size_t>(bytes.value()));
    for (core::u32 y = 0U; y < inspection.height; ++y) {
        const core::u32 source_y = y % image.height;
        for (core::u32 x = 0U; x < inspection.width; ++x) {
            const core::u32 source_x = x % image.width;
            const std::size_t source =
                (static_cast<std::size_t>(source_y) * image.width + source_x) * 4U;
            const std::size_t destination =
                (static_cast<std::size_t>(y) * inspection.width + x) * 4U;
            std::copy_n(image.rgba.begin() + static_cast<std::ptrdiff_t>(source), 4U,
                        inspection.rgba.begin() + static_cast<std::ptrdiff_t>(destination));
        }
    }
    return core::Result<nodes::Image, MetricError>::success(std::move(inspection));
}

}  // namespace artminer::quarry
