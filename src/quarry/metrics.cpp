#include "quarry/metrics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace artminer::quarry {
namespace {

constexpr core::u8 kVisibleAlphaThreshold = 8U;
constexpr int kEdgeThreshold = 32;

[[nodiscard]] MetricError make_error(const MetricErrorCode code, std::string message) {
    return MetricError{code, std::move(message)};
}

[[nodiscard]] bool valid_image(const nodes::Image& image) noexcept {
    if (image.width == 0U || image.height == 0U) {
        return false;
    }
    const std::uint64_t pixel_count = static_cast<std::uint64_t>(image.width) * image.height;
    return pixel_count <= (std::numeric_limits<std::size_t>::max)() / 4U &&
           image.rgba.size() == static_cast<std::size_t>(pixel_count * 4U);
}

[[nodiscard]] core::u8 luminance_at(const nodes::Image& image, const std::size_t pixel_index) noexcept {
    const std::size_t offset = pixel_index * 4U;
    const unsigned int red = image.rgba[offset];
    const unsigned int green = image.rgba[offset + 1U];
    const unsigned int blue = image.rgba[offset + 2U];
    const unsigned int alpha = image.rgba[offset + 3U];
    const unsigned int luma = (54U * red + 183U * green + 19U * blue + 128U) >> 8U;
    return static_cast<core::u8>((luma * alpha + 127U) / 255U);
}

[[nodiscard]] double clamp_unit(const double value) noexcept {
    return (std::max)(0.0, (std::min)(1.0, value));
}

[[nodiscard]] double entropy_metric(const nodes::Image& image) {
    std::array<std::uint64_t, 256U> histogram{};
    const std::size_t pixels = static_cast<std::size_t>(image.width) * image.height;
    for (std::size_t index = 0U; index < pixels; ++index) {
        ++histogram[luminance_at(image, index)];
    }
    double entropy = 0.0;
    const double denominator = static_cast<double>(pixels);
    for (const std::uint64_t count : histogram) {
        if (count == 0U) {
            continue;
        }
        const double probability = static_cast<double>(count) / denominator;
        entropy -= probability * std::log2(probability);
    }
    return clamp_unit(entropy / 8.0);
}

struct EdgeStats final {
    std::uint64_t horizontal_edges{0U};
    std::uint64_t vertical_edges{0U};
    std::uint64_t horizontal_pairs{0U};
    std::uint64_t vertical_pairs{0U};
};

[[nodiscard]] EdgeStats edge_stats(const nodes::Image& image) noexcept {
    EdgeStats stats;
    const std::size_t width = image.width;
    const std::size_t height = image.height;
    for (std::size_t y = 0U; y < height; ++y) {
        for (std::size_t x = 0U; x < width; ++x) {
            const std::size_t index = y * width + x;
            const int here = luminance_at(image, index);
            if (x + 1U < width) {
                const int right = luminance_at(image, index + 1U);
                ++stats.horizontal_pairs;
                if (std::abs(here - right) >= kEdgeThreshold) {
                    ++stats.horizontal_edges;
                }
            }
            if (y + 1U < height) {
                const int below = luminance_at(image, index + width);
                ++stats.vertical_pairs;
                if (std::abs(here - below) >= kEdgeThreshold) {
                    ++stats.vertical_edges;
                }
            }
        }
    }
    return stats;
}

struct ComponentStats final {
    std::uint64_t count{0U};
    std::uint64_t occupied{0U};
    std::uint64_t largest{0U};
};

[[nodiscard]] ComponentStats component_stats(const nodes::Image& image) {
    const std::size_t width = image.width;
    const std::size_t height = image.height;
    const std::size_t pixels = width * height;
    std::vector<core::u8> visited(pixels, 0U);
    ComponentStats stats;
    std::deque<std::size_t> queue;

    const auto occupied = [&](const std::size_t index) noexcept {
        return image.rgba[index * 4U + 3U] >= kVisibleAlphaThreshold;
    };

    for (std::size_t start = 0U; start < pixels; ++start) {
        if (!occupied(start)) {
            continue;
        }
        ++stats.occupied;
        if (visited[start] != 0U) {
            continue;
        }
        ++stats.count;
        std::uint64_t component_size = 0U;
        visited[start] = 1U;
        queue.push_back(start);
        while (!queue.empty()) {
            const std::size_t current = queue.front();
            queue.pop_front();
            ++component_size;
            const std::size_t x = current % width;
            const std::size_t y = current / width;
            const auto visit = [&](const std::size_t neighbour) {
                if (visited[neighbour] == 0U && occupied(neighbour)) {
                    visited[neighbour] = 1U;
                    queue.push_back(neighbour);
                }
            };
            if (x > 0U) {
                visit(current - 1U);
            }
            if (x + 1U < width) {
                visit(current + 1U);
            }
            if (y > 0U) {
                visit(current - width);
            }
            if (y + 1U < height) {
                visit(current + width);
            }
        }
        stats.largest = (std::max)(stats.largest, component_size);
    }
    return stats;
}

[[nodiscard]] double bilateral_symmetry(const nodes::Image& image) noexcept {
    const std::size_t width = image.width;
    const std::size_t height = image.height;
    if (width <= 1U) {
        return 1.0;
    }
    std::uint64_t difference = 0U;
    std::uint64_t pairs = 0U;
    for (std::size_t y = 0U; y < height; ++y) {
        for (std::size_t x = 0U; x < width / 2U; ++x) {
            const std::size_t a = y * width + x;
            const std::size_t b = y * width + (width - 1U - x);
            difference += static_cast<std::uint64_t>(std::abs(
                static_cast<int>(luminance_at(image, a)) - static_cast<int>(luminance_at(image, b))));
            ++pairs;
        }
    }
    if (pairs == 0U) {
        return 1.0;
    }
    return clamp_unit(1.0 - static_cast<double>(difference) / (255.0 * static_cast<double>(pairs)));
}

[[nodiscard]] double rotational_symmetry(const nodes::Image& image) noexcept {
    const std::size_t width = image.width;
    const std::size_t height = image.height;
    const std::size_t pixels = width * height;
    if (pixels <= 1U) {
        return 1.0;
    }
    std::uint64_t difference = 0U;
    for (std::size_t index = 0U; index < pixels; ++index) {
        const std::size_t reverse = pixels - 1U - index;
        difference += static_cast<std::uint64_t>(std::abs(
            static_cast<int>(luminance_at(image, index)) - static_cast<int>(luminance_at(image, reverse))));
    }
    return clamp_unit(1.0 - static_cast<double>(difference) / (255.0 * static_cast<double>(pixels)));
}

[[nodiscard]] double luma_variance(const nodes::Image& image) noexcept {
    const std::size_t pixels = static_cast<std::size_t>(image.width) * image.height;
    double mean = 0.0;
    for (std::size_t index = 0U; index < pixels; ++index) {
        mean += luminance_at(image, index);
    }
    mean /= static_cast<double>(pixels);
    double variance = 0.0;
    for (std::size_t index = 0U; index < pixels; ++index) {
        const double delta = static_cast<double>(luminance_at(image, index)) - mean;
        variance += delta * delta;
    }
    return variance / static_cast<double>(pixels);
}

struct PeriodicityStats final {
    double best_similarity{0.0};
    std::size_t best_shift{0U};
};

[[nodiscard]] PeriodicityStats periodicity_stats(const nodes::Image& image) noexcept {
    const std::size_t width = image.width;
    const std::size_t height = image.height;
    const std::size_t maximum_shift = (std::min<std::size_t>)(32U, (std::max)(width, height) > 1U ? (std::max)(width, height) - 1U : 0U);
    PeriodicityStats best;
    for (std::size_t shift = 1U; shift <= maximum_shift; ++shift) {
        std::uint64_t difference = 0U;
        std::uint64_t pairs = 0U;
        if (shift < width) {
            for (std::size_t y = 0U; y < height; ++y) {
                for (std::size_t x = 0U; x + shift < width; ++x) {
                    const std::size_t a = y * width + x;
                    const std::size_t b = a + shift;
                    difference += static_cast<std::uint64_t>(std::abs(
                        static_cast<int>(luminance_at(image, a)) - static_cast<int>(luminance_at(image, b))));
                    ++pairs;
                }
            }
        }
        if (shift < height) {
            for (std::size_t y = 0U; y + shift < height; ++y) {
                for (std::size_t x = 0U; x < width; ++x) {
                    const std::size_t a = y * width + x;
                    const std::size_t b = a + shift * width;
                    difference += static_cast<std::uint64_t>(std::abs(
                        static_cast<int>(luminance_at(image, a)) - static_cast<int>(luminance_at(image, b))));
                    ++pairs;
                }
            }
        }
        if (pairs == 0U) {
            continue;
        }
        const double similarity = clamp_unit(
            1.0 - static_cast<double>(difference) / (255.0 * static_cast<double>(pairs)));
        if (similarity > best.best_similarity) {
            best.best_similarity = similarity;
            best.best_shift = shift;
        }
    }
    return best;
}

[[nodiscard]] double palette_utilisation(const nodes::Image& image) {
    std::set<std::uint16_t> colours;
    const std::size_t pixels = static_cast<std::size_t>(image.width) * image.height;
    std::size_t visible = 0U;
    for (std::size_t index = 0U; index < pixels; ++index) {
        const std::size_t offset = index * 4U;
        if (image.rgba[offset + 3U] < kVisibleAlphaThreshold) {
            continue;
        }
        ++visible;
        const std::uint16_t red = static_cast<std::uint16_t>(image.rgba[offset] >> 4U);
        const std::uint16_t green = static_cast<std::uint16_t>(image.rgba[offset + 1U] >> 4U);
        const std::uint16_t blue = static_cast<std::uint16_t>(image.rgba[offset + 2U] >> 4U);
        colours.insert(static_cast<std::uint16_t>((red << 8U) | (green << 4U) | blue));
    }
    if (visible == 0U) {
        return 0.0;
    }
    const std::size_t possible = (std::min<std::size_t>)(4096U, visible);
    return static_cast<double>(colours.size()) / static_cast<double>(possible);
}

[[nodiscard]] double empty_space_ratio(const nodes::Image& image) noexcept {
    const std::size_t pixels = static_cast<std::size_t>(image.width) * image.height;
    std::size_t empty = 0U;
    for (std::size_t index = 0U; index < pixels; ++index) {
        if (image.rgba[index * 4U + 3U] < kVisibleAlphaThreshold) {
            ++empty;
        }
    }
    return static_cast<double>(empty) / static_cast<double>(pixels);
}

[[nodiscard]] double tile_seam_error(const nodes::Image& image) noexcept {
    const std::size_t width = image.width;
    const std::size_t height = image.height;
    std::uint64_t difference = 0U;
    std::uint64_t pairs = 0U;
    if (width > 1U) {
        for (std::size_t y = 0U; y < height; ++y) {
            const std::size_t left = y * width;
            const std::size_t right = left + width - 1U;
            difference += static_cast<std::uint64_t>(std::abs(
                static_cast<int>(luminance_at(image, left)) - static_cast<int>(luminance_at(image, right))));
            ++pairs;
        }
    }
    if (height > 1U) {
        for (std::size_t x = 0U; x < width; ++x) {
            const std::size_t top = x;
            const std::size_t bottom = (height - 1U) * width + x;
            difference += static_cast<std::uint64_t>(std::abs(
                static_cast<int>(luminance_at(image, top)) - static_cast<int>(luminance_at(image, bottom))));
            ++pairs;
        }
    }
    if (pairs == 0U) {
        return 0.0;
    }
    return clamp_unit(static_cast<double>(difference) / (255.0 * static_cast<double>(pairs)));
}

[[nodiscard]] double region_diversity(const nodes::Image& image) noexcept {
    constexpr std::size_t kGrid = 4U;
    std::array<double, kGrid * kGrid> means{};
    std::array<std::size_t, kGrid * kGrid> counts{};
    const std::size_t width = image.width;
    const std::size_t height = image.height;
    for (std::size_t y = 0U; y < height; ++y) {
        const std::size_t region_y = (std::min)(kGrid - 1U, y * kGrid / height);
        for (std::size_t x = 0U; x < width; ++x) {
            const std::size_t region_x = (std::min)(kGrid - 1U, x * kGrid / width);
            const std::size_t region = region_y * kGrid + region_x;
            means[region] += luminance_at(image, y * width + x);
            ++counts[region];
        }
    }
    double global_mean = 0.0;
    std::size_t populated = 0U;
    for (std::size_t region = 0U; region < means.size(); ++region) {
        if (counts[region] == 0U) {
            continue;
        }
        means[region] /= static_cast<double>(counts[region]);
        global_mean += means[region];
        ++populated;
    }
    if (populated <= 1U) {
        return 0.0;
    }
    global_mean /= static_cast<double>(populated);
    double variance = 0.0;
    for (std::size_t region = 0U; region < means.size(); ++region) {
        if (counts[region] == 0U) {
            continue;
        }
        const double delta = means[region] - global_mean;
        variance += delta * delta;
    }
    variance /= static_cast<double>(populated);
    return clamp_unit(std::sqrt(variance) / 127.5);
}

[[nodiscard]] double mean_luminance(const nodes::Image& image) noexcept {
    const std::size_t pixels = static_cast<std::size_t>(image.width) * image.height;
    double sum = 0.0;
    for (std::size_t index = 0U; index < pixels; ++index) {
        sum += luminance_at(image, index);
    }
    return sum / static_cast<double>(pixels);
}

[[nodiscard]] core::Result<std::pair<double, double>, MetricError> animation_metrics(
    const std::vector<nodes::Image>& frames) {
    if (frames.size() < 2U) {
        return core::Result<std::pair<double, double>, MetricError>::failure(make_error(
            MetricErrorCode::insufficient_frames,
            "animation metrics require at least two sampled frames"));
    }
    const std::size_t pixels = static_cast<std::size_t>(frames.front().width) * frames.front().height;
    double motion_sum = 0.0;
    double flicker_sum = 0.0;
    for (std::size_t frame = 1U; frame < frames.size(); ++frame) {
        if (frames[frame].width != frames.front().width || frames[frame].height != frames.front().height) {
            return core::Result<std::pair<double, double>, MetricError>::failure(make_error(
                MetricErrorCode::invalid_image,
                "animation metric frames must have identical dimensions"));
        }
        double frame_motion = 0.0;
        for (std::size_t index = 0U; index < pixels; ++index) {
            frame_motion += std::abs(
                static_cast<int>(luminance_at(frames[frame], index)) -
                static_cast<int>(luminance_at(frames[frame - 1U], index)));
        }
        motion_sum += frame_motion / (255.0 * static_cast<double>(pixels));
        flicker_sum += std::abs(mean_luminance(frames[frame]) - mean_luminance(frames[frame - 1U])) / 255.0;
    }
    const double transitions = static_cast<double>(frames.size() - 1U);
    return core::Result<std::pair<double, double>, MetricError>::success({
        clamp_unit(motion_sum / transitions), clamp_unit(flicker_sum / transitions)});
}

}  // namespace

const std::vector<std::string>& supported_metric_names() {
    static const std::vector<std::string> names{
        "entropy",
        "edge_density",
        "connected_components",
        "component_mean",
        "component_max",
        "symmetry_bilateral",
        "symmetry_rotational",
        "dominant_frequency",
        "palette_utilisation",
        "empty_space_ratio",
        "repetition",
        "tile_seam_error",
        "directional_bias",
        "region_diversity",
        "motion_energy",
        "temporal_flicker",
    };
    return names;
}

bool is_supported_metric(const std::string_view name) noexcept {
    const auto& names = supported_metric_names();
    return std::find(names.begin(), names.end(), name) != names.end();
}

core::Result<MetricVector, MetricError> compute_metrics(
    const std::vector<nodes::Image>& frames,
    const std::vector<std::string>& selected_metrics) {
    if (frames.empty() || !valid_image(frames.front())) {
        return core::Result<MetricVector, MetricError>::failure(make_error(
            MetricErrorCode::invalid_image, "metrics require a non-empty canonical RGBA8 image"));
    }
    for (const auto& frame : frames) {
        if (!valid_image(frame)) {
            return core::Result<MetricVector, MetricError>::failure(make_error(
                MetricErrorCode::invalid_image, "metric frame has invalid dimensions or RGBA8 byte count"));
        }
    }
    for (const std::string& name : selected_metrics) {
        if (!is_supported_metric(name)) {
            return core::Result<MetricVector, MetricError>::failure(make_error(
                MetricErrorCode::unknown_metric, "unsupported Quarry metric: " + name));
        }
    }

    const nodes::Image& image = frames.front();
    const std::size_t pixels = static_cast<std::size_t>(image.width) * image.height;
    const EdgeStats edges = edge_stats(image);
    const ComponentStats components = component_stats(image);
    const PeriodicityStats periodicity = periodicity_stats(image);
    const std::uint64_t edge_count = edges.horizontal_edges + edges.vertical_edges;
    const std::uint64_t edge_pairs = edges.horizontal_pairs + edges.vertical_pairs;
    const double edge_density = edge_pairs == 0U ? 0.0 :
        static_cast<double>(edge_count) / static_cast<double>(edge_pairs);
    const double component_mean = components.count == 0U ? 0.0 :
        static_cast<double>(components.occupied) /
        (static_cast<double>(components.count) * static_cast<double>(pixels));
    const double component_max = pixels == 0U ? 0.0 :
        static_cast<double>(components.largest) / static_cast<double>(pixels);
    const double horizontal_density = edges.horizontal_pairs == 0U ? 0.0 :
        static_cast<double>(edges.horizontal_edges) / static_cast<double>(edges.horizontal_pairs);
    const double vertical_density = edges.vertical_pairs == 0U ? 0.0 :
        static_cast<double>(edges.vertical_edges) / static_cast<double>(edges.vertical_pairs);
    const double directional_denominator = horizontal_density + vertical_density;
    const double directional_bias = directional_denominator == 0.0 ? 0.0 :
        std::abs(horizontal_density - vertical_density) / directional_denominator;
    const double dominant_frequency = luma_variance(image) < 1.0 || periodicity.best_shift == 0U ? 0.0 :
        1.0 / static_cast<double>(periodicity.best_shift);

    std::optional<std::pair<double, double>> temporal;
    MetricVector output;
    output.reserve(selected_metrics.size());
    for (const std::string& name : selected_metrics) {
        double value = 0.0;
        if (name == "entropy") {
            value = entropy_metric(image);
        } else if (name == "edge_density") {
            value = edge_density;
        } else if (name == "connected_components") {
            value = static_cast<double>(components.count);
        } else if (name == "component_mean") {
            value = component_mean;
        } else if (name == "component_max") {
            value = component_max;
        } else if (name == "symmetry_bilateral") {
            value = bilateral_symmetry(image);
        } else if (name == "symmetry_rotational") {
            value = rotational_symmetry(image);
        } else if (name == "dominant_frequency") {
            value = dominant_frequency;
        } else if (name == "palette_utilisation") {
            value = palette_utilisation(image);
        } else if (name == "empty_space_ratio") {
            value = empty_space_ratio(image);
        } else if (name == "repetition") {
            value = periodicity.best_similarity;
        } else if (name == "tile_seam_error") {
            value = tile_seam_error(image);
        } else if (name == "directional_bias") {
            value = clamp_unit(directional_bias);
        } else if (name == "region_diversity") {
            value = region_diversity(image);
        } else if (name == "motion_energy" || name == "temporal_flicker") {
            if (!temporal.has_value()) {
                auto calculated = animation_metrics(frames);
                if (calculated.is_error()) {
                    return core::Result<MetricVector, MetricError>::failure(calculated.error());
                }
                temporal = calculated.value();
            }
            value = name == "motion_energy" ? temporal->first : temporal->second;
        }
        output.push_back(MetricValue{name, value});
    }
    return core::Result<MetricVector, MetricError>::success(std::move(output));
}

}  // namespace artminer::quarry
