#include "nodes/static_evaluator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <numbers>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

#include "core/checked_math.hpp"
#include "core/graph.hpp"
#include "core/hash.hpp"
#include "core/prng.hpp"

namespace artminer::nodes {
namespace {

using core::i64;
using core::u8;
using core::u32;
using core::u64;

constexpr u64 kMaxReferencePixels = 4'194'304ULL;  // 2048x2048.
constexpr u64 kMaxEstimatedWorkingBytes = 768ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxReferenceNodes = 1024U;
constexpr double kInvU53 = 1.0 / 9007199254740992.0;
constexpr double kInvSqrt2 = 0.707106781186547524400844362104849039;

struct ScalarField final {
    u32 width{0U};
    u32 height{0U};
    std::vector<double> values;
};

struct Vec2 final {
    double x{0.0};
    double y{0.0};
};

struct VectorField final {
    u32 width{0U};
    u32 height{0U};
    std::vector<Vec2> values;
};

struct Rgba final {
    double r{0.0};
    double g{0.0};
    double b{0.0};
    double a{1.0};
};

struct ColourField final {
    u32 width{0U};
    u32 height{0U};
    std::vector<Rgba> values;
};

struct Mask final {
    u32 width{0U};
    u32 height{0U};
    std::vector<u8> values;
};

struct Palette final {
    std::vector<Rgba> colours;
};

using Value = std::variant<ScalarField, VectorField, ColourField, Mask, Palette, Image>;

[[nodiscard]] EvaluationError make_error(EvaluationErrorCode code, std::string message) {
    return EvaluationError{code, std::move(message)};
}

[[nodiscard]] double clamp01(const double value) noexcept {
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] u8 to_unorm8(const double value) noexcept {
    const double scaled = std::floor(clamp01(value) * 255.0 + 0.5);
    return static_cast<u8>(scaled);
}

[[nodiscard]] std::size_t index_of(const u32 width, const u32 x, const u32 y) noexcept {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
}

[[nodiscard]] const core::ParameterAssignment* find_parameter(
    const core::NodeInstance& node,
    const std::string_view name) noexcept {
    const auto found = std::find_if(
        node.parameters.begin(), node.parameters.end(),
        [&](const core::ParameterAssignment& parameter) { return parameter.name == name; });
    return found == node.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] double real_parameter(const core::NodeInstance& node, const std::string_view name) {
    return std::get<double>(find_parameter(node, name)->value);
}

[[nodiscard]] i64 integer_parameter(const core::NodeInstance& node, const std::string_view name) {
    return std::get<i64>(find_parameter(node, name)->value);
}

[[nodiscard]] const std::string& enum_parameter(const core::NodeInstance& node, const std::string_view name) {
    return std::get<std::string>(find_parameter(node, name)->value);
}

[[nodiscard]] u64 hash_cell(const u64 seed, const i64 x, const i64 y, const u64 salt) noexcept {
    const u64 hx = core::splitmix64(static_cast<u64>(x) ^ 0x9e3779b97f4a7c15ULL);
    const u64 hy = core::splitmix64(static_cast<u64>(y) ^ 0xbf58476d1ce4e5b9ULL);
    return core::splitmix64(seed ^ hx ^ hy ^ salt);
}

[[nodiscard]] double unit_from_hash(const u64 value) noexcept {
    return static_cast<double>(value >> 11U) * kInvU53;
}

[[nodiscard]] double smoothstep(const double value) noexcept {
    return value * value * (3.0 - 2.0 * value);
}

[[nodiscard]] double lerp(const double a, const double b, const double t) noexcept {
    return a + (b - a) * t;
}

[[nodiscard]] double value_noise_raw(const u64 seed, const double x, const double y) noexcept {
    const auto x0 = static_cast<i64>(std::floor(x));
    const auto y0 = static_cast<i64>(std::floor(y));
    const double tx = x - static_cast<double>(x0);
    const double ty = y - static_cast<double>(y0);
    const double sx = smoothstep(tx);
    const double sy = smoothstep(ty);

    const double v00 = unit_from_hash(hash_cell(seed, x0, y0, 0x243f6a8885a308d3ULL)) * 2.0 - 1.0;
    const double v10 = unit_from_hash(hash_cell(seed, x0 + 1, y0, 0x243f6a8885a308d3ULL)) * 2.0 - 1.0;
    const double v01 = unit_from_hash(hash_cell(seed, x0, y0 + 1, 0x243f6a8885a308d3ULL)) * 2.0 - 1.0;
    const double v11 = unit_from_hash(hash_cell(seed, x0 + 1, y0 + 1, 0x243f6a8885a308d3ULL)) * 2.0 - 1.0;
    return lerp(lerp(v00, v10, sx), lerp(v01, v11, sx), sy);
}

[[nodiscard]] Vec2 gradient_from_hash(const u64 hash) noexcept {
    switch (hash & 7ULL) {
    case 0ULL:
        return {1.0, 0.0};
    case 1ULL:
        return {-1.0, 0.0};
    case 2ULL:
        return {0.0, 1.0};
    case 3ULL:
        return {0.0, -1.0};
    case 4ULL:
        return {kInvSqrt2, kInvSqrt2};
    case 5ULL:
        return {-kInvSqrt2, kInvSqrt2};
    case 6ULL:
        return {kInvSqrt2, -kInvSqrt2};
    default:
        return {-kInvSqrt2, -kInvSqrt2};
    }
}

[[nodiscard]] double gradient_noise_raw(const u64 seed, const double x, const double y) noexcept {
    const auto x0 = static_cast<i64>(std::floor(x));
    const auto y0 = static_cast<i64>(std::floor(y));
    const double tx = x - static_cast<double>(x0);
    const double ty = y - static_cast<double>(y0);
    const double sx = smoothstep(tx);
    const double sy = smoothstep(ty);

    const Vec2 g00 = gradient_from_hash(hash_cell(seed, x0, y0, 0x13198a2e03707344ULL));
    const Vec2 g10 = gradient_from_hash(hash_cell(seed, x0 + 1, y0, 0x13198a2e03707344ULL));
    const Vec2 g01 = gradient_from_hash(hash_cell(seed, x0, y0 + 1, 0x13198a2e03707344ULL));
    const Vec2 g11 = gradient_from_hash(hash_cell(seed, x0 + 1, y0 + 1, 0x13198a2e03707344ULL));

    const double d00 = g00.x * tx + g00.y * ty;
    const double d10 = g10.x * (tx - 1.0) + g10.y * ty;
    const double d01 = g01.x * tx + g01.y * (ty - 1.0);
    const double d11 = g11.x * (tx - 1.0) + g11.y * (ty - 1.0);
    return std::clamp(lerp(lerp(d00, d10, sx), lerp(d01, d11, sx), sy) * 1.4142135623730951, -1.0, 1.0);
}

[[nodiscard]] double worley_distance(const u64 seed, const double x, const double y) noexcept {
    const auto cell_x = static_cast<i64>(std::floor(x));
    const auto cell_y = static_cast<i64>(std::floor(y));
    double best_squared = (std::numeric_limits<double>::max)();

    for (i64 offset_y = -1; offset_y <= 1; ++offset_y) {
        for (i64 offset_x = -1; offset_x <= 1; ++offset_x) {
            const i64 candidate_x = cell_x + offset_x;
            const i64 candidate_y = cell_y + offset_y;
            const u64 hash = hash_cell(seed, candidate_x, candidate_y, 0xa4093822299f31d0ULL);
            const double jitter_x = unit_from_hash(hash);
            const double jitter_y = unit_from_hash(core::splitmix64(hash));
            const double point_x = static_cast<double>(candidate_x) + jitter_x;
            const double point_y = static_cast<double>(candidate_y) + jitter_y;
            const double dx = x - point_x;
            const double dy = y - point_y;
            best_squared = (std::min)(best_squared, dx * dx + dy * dy);
        }
    }

    return std::sqrt(best_squared);
}

[[nodiscard]] u64 node_seed(const core::Recipe& recipe, const core::NodeInstance& node) {
    std::string identity;
    identity.reserve(node.id.size() + node.type_id.size() + 1U);
    identity.append(node.id);
    identity.push_back('\n');
    identity.append(node.type_id);
    return core::derive_seed(recipe.root_seed, core::fnv1a64(identity));
}

[[nodiscard]] double sample_scalar(
    const ScalarField& field,
    double u,
    double v,
    const bool repeat) noexcept {
    if (repeat) {
        u -= std::floor(u);
        v -= std::floor(v);
    } else {
        u = std::clamp(u, 0.0, 1.0);
        v = std::clamp(v, 0.0, 1.0);
    }

    const double fx = u * static_cast<double>(field.width) - 0.5;
    const double fy = v * static_cast<double>(field.height) - 0.5;
    const i64 x0 = static_cast<i64>(std::floor(fx));
    const i64 y0 = static_cast<i64>(std::floor(fy));
    const double tx = fx - static_cast<double>(x0);
    const double ty = fy - static_cast<double>(y0);

    const auto resolve = [repeat](const i64 value, const u32 extent) noexcept -> u32 {
        const i64 e = static_cast<i64>(extent);
        if (repeat) {
            const i64 wrapped = ((value % e) + e) % e;
            return static_cast<u32>(wrapped);
        }
        return static_cast<u32>(std::clamp<i64>(value, 0, e - 1));
    };

    const u32 ix0 = resolve(x0, field.width);
    const u32 ix1 = resolve(x0 + 1, field.width);
    const u32 iy0 = resolve(y0, field.height);
    const u32 iy1 = resolve(y0 + 1, field.height);
    const double a = lerp(field.values[index_of(field.width, ix0, iy0)], field.values[index_of(field.width, ix1, iy0)], tx);
    const double b = lerp(field.values[index_of(field.width, ix0, iy1)], field.values[index_of(field.width, ix1, iy1)], tx);
    return lerp(a, b, ty);
}

[[nodiscard]] Image image_from_colour(const ColourField& field) {
    Image image;
    image.width = field.width;
    image.height = field.height;
    image.rgba.resize(field.values.size() * 4U);
    for (std::size_t index = 0U; index < field.values.size(); ++index) {
        const Rgba& colour = field.values[index];
        const std::size_t byte_index = index * 4U;
        image.rgba[byte_index] = to_unorm8(colour.r);
        image.rgba[byte_index + 1U] = to_unorm8(colour.g);
        image.rgba[byte_index + 2U] = to_unorm8(colour.b);
        image.rgba[byte_index + 3U] = to_unorm8(colour.a);
    }
    return image;
}

[[nodiscard]] Rgba heat_colour(const double value) noexcept {
    const double t = clamp01(value);
    return {
        clamp01(1.5 - std::abs(4.0 * t - 3.0)),
        clamp01(1.5 - std::abs(4.0 * t - 2.0)),
        clamp01(1.5 - std::abs(4.0 * t - 1.0)),
        1.0,
    };
}

[[nodiscard]] Rgba palette_sample(const Palette& palette, const double value, const bool nearest) noexcept {
    if (palette.colours.empty()) {
        return {};
    }
    if (palette.colours.size() == 1U) {
        return palette.colours.front();
    }
    const double t = clamp01(value);
    const double scaled = t * static_cast<double>(palette.colours.size() - 1U);
    if (nearest) {
        const auto index = static_cast<std::size_t>(std::floor(scaled + 0.5));
        return palette.colours[(std::min)(index, palette.colours.size() - 1U)];
    }
    const auto lower = static_cast<std::size_t>(std::floor(scaled));
    const std::size_t upper = (std::min)(lower + 1U, palette.colours.size() - 1U);
    const double fraction = scaled - static_cast<double>(lower);
    const Rgba& a = palette.colours[lower];
    const Rgba& b = palette.colours[upper];
    return {
        lerp(a.r, b.r, fraction),
        lerp(a.g, b.g, fraction),
        lerp(a.b, b.b, fraction),
        lerp(a.a, b.a, fraction),
    };
}

[[nodiscard]] int bayer4(const u32 x, const u32 y) noexcept {
    constexpr std::array<int, 16> matrix{
        0, 8, 2, 10,
        12, 4, 14, 6,
        3, 11, 1, 9,
        15, 7, 13, 5,
    };
    return matrix[(y & 3U) * 4U + (x & 3U)];
}

[[nodiscard]] int bayer8(const u32 x, const u32 y) noexcept {
    const u32 bx = x & 7U;
    const u32 by = y & 7U;
    // Recursive Bayer definition from two-bit interleaving; equivalent to the
    // conventional 8x8 matrix while avoiding a large lookup table.
    const u32 x0 = bx & 1U;
    const u32 x1 = (bx >> 1U) & 1U;
    const u32 x2 = (bx >> 2U) & 1U;
    const u32 y0 = by & 1U;
    const u32 y1 = (by >> 1U) & 1U;
    const u32 y2 = (by >> 2U) & 1U;
    const u32 value =
        ((x0 ^ y0) << 5U) |
        (y0 << 4U) |
        ((x1 ^ y1) << 3U) |
        (y1 << 2U) |
        ((x2 ^ y2) << 1U) |
        y2;
    return static_cast<int>(value);
}

[[nodiscard]] double dither_channel(const double value, const i64 levels, const double threshold) noexcept {
    const double scaled = clamp01(value) * static_cast<double>(levels - 1);
    const double base = std::floor(scaled);
    const double fraction = scaled - base;
    const double quantized = base + (fraction > threshold ? 1.0 : 0.0);
    return std::clamp(quantized / static_cast<double>(levels - 1), 0.0, 1.0);
}

class EvaluationContext final {
public:
    explicit EvaluationContext(const core::Recipe& recipe)
        : recipe_(recipe), width_(recipe.render.width), height_(recipe.render.height) {
        for (const auto& node : recipe.nodes) {
            nodes_.emplace(node.id, &node);
        }
        for (const auto& edge : recipe.edges) {
            incoming_.emplace(std::make_pair(edge.to_node, edge.to_port), &edge);
        }
    }

    [[nodiscard]] core::Result<void, EvaluationError> evaluate(const std::string& node_id) {
        if (values_.contains(node_id)) {
            return core::Result<void, EvaluationError>::success();
        }
        if (!active_.insert(node_id).second) {
            return core::Result<void, EvaluationError>::failure(make_error(
                EvaluationErrorCode::internal_graph_error,
                "unexpected evaluator recursion cycle at node '" + node_id + "'"));
        }

        const auto node_found = nodes_.find(node_id);
        if (node_found == nodes_.end()) {
            active_.erase(node_id);
            return core::Result<void, EvaluationError>::failure(make_error(
                EvaluationErrorCode::internal_graph_error,
                "evaluator could not find node '" + node_id + "'"));
        }
        const core::NodeInstance& node = *node_found->second;
        const core::NodeMetadata* metadata = core::builtin_node_registry().find(node.type_id);
        if (metadata == nullptr) {
            active_.erase(node_id);
            return core::Result<void, EvaluationError>::failure(make_error(
                EvaluationErrorCode::unsupported_node,
                "no metadata is available for node type '" + node.type_id + "'"));
        }
        if (!metadata->evaluators.cpu) {
            active_.erase(node_id);
            return core::Result<void, EvaluationError>::failure(make_error(
                EvaluationErrorCode::unsupported_node,
                "canonical CPU evaluator is unavailable for node '" + node.id + "' (" + node.type_id + ")"));
        }

        std::map<std::string, const Value*, std::less<>> inputs;
        for (const auto& port : metadata->inputs) {
            const auto incoming = incoming_.find(std::make_pair(node.id, port.name));
            if (incoming == incoming_.end()) {
                continue;
            }
            const core::Edge& edge = *incoming->second;
            auto source_result = evaluate(edge.from_node);
            if (source_result.is_error()) {
                active_.erase(node_id);
                return core::Result<void, EvaluationError>::failure(source_result.error());
            }
            inputs.emplace(port.name, &values_.at(edge.from_node));
        }

        auto result = evaluate_builtin(node, inputs);
        active_.erase(node_id);
        if (result.is_error()) {
            return core::Result<void, EvaluationError>::failure(result.error());
        }
        values_.emplace(node_id, std::move(result).value());
        return core::Result<void, EvaluationError>::success();
    }

    [[nodiscard]] const Value* value(const std::string& node_id) const noexcept {
        const auto found = values_.find(node_id);
        return found == values_.end() ? nullptr : &found->second;
    }

private:
    template <typename Generator>
    [[nodiscard]] ScalarField make_scalar(Generator&& generator) const {
        ScalarField field{width_, height_, {}};
        field.values.resize(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_));
        for (u32 y = 0U; y < height_; ++y) {
            const double v = (static_cast<double>(y) + 0.5) / static_cast<double>(height_);
            for (u32 x = 0U; x < width_; ++x) {
                const double u = (static_cast<double>(x) + 0.5) / static_cast<double>(width_);
                field.values[index_of(width_, x, y)] = generator(u, v, x, y);
            }
        }
        return field;
    }

    [[nodiscard]] core::Result<Value, EvaluationError> evaluate_builtin(
        const core::NodeInstance& node,
        const std::map<std::string, const Value*, std::less<>>& inputs) const {
        const auto scalar_input = [&](const std::string_view name) -> const ScalarField& {
            return std::get<ScalarField>(*inputs.at(std::string(name)));
        };
        const auto colour_input = [&](const std::string_view name) -> const ColourField& {
            return std::get<ColourField>(*inputs.at(std::string(name)));
        };
        const auto palette_input = [&](const std::string_view name) -> const Palette& {
            return std::get<Palette>(*inputs.at(std::string(name)));
        };

        if (node.type_id == "core.scalar.constant") {
            const double value = real_parameter(node, "value");
            return core::Result<Value, EvaluationError>::success(make_scalar(
                [value](double, double, u32, u32) noexcept { return value; }));
        }
        if (node.type_id == "core.scalar.pass") {
            return core::Result<Value, EvaluationError>::success(scalar_input("source"));
        }
        if (node.type_id == "core.scalar.coord_x") {
            return core::Result<Value, EvaluationError>::success(make_scalar(
                [](const double u, double, u32, u32) noexcept { return u; }));
        }
        if (node.type_id == "core.scalar.coord_y") {
            return core::Result<Value, EvaluationError>::success(make_scalar(
                [](double, const double v, u32, u32) noexcept { return v; }));
        }
        if (node.type_id == "core.scalar.radial") {
            const double cx = real_parameter(node, "center_x");
            const double cy = real_parameter(node, "center_y");
            const double scale = real_parameter(node, "scale");
            return core::Result<Value, EvaluationError>::success(make_scalar(
                [cx, cy, scale](const double u, const double v, u32, u32) noexcept {
                    const double dx = (u - cx) * 2.0;
                    const double dy = (v - cy) * 2.0;
                    return std::sqrt(dx * dx + dy * dy) * scale;
                }));
        }
        if (node.type_id == "core.scalar.angular") {
            const double cx = real_parameter(node, "center_x");
            const double cy = real_parameter(node, "center_y");
            const double turns = real_parameter(node, "turns");
            const double phase = real_parameter(node, "phase");
            return core::Result<Value, EvaluationError>::success(make_scalar(
                [cx, cy, turns, phase](const double u, const double v, u32, u32) noexcept {
                    const double angle = std::atan2(v - cy, u - cx) / (2.0 * std::numbers::pi_v<double>) + 0.5;
                    const double value = angle * turns + phase;
                    return value - std::floor(value);
                }));
        }
        if (node.type_id == "core.noise.value") {
            const double frequency = real_parameter(node, "frequency");
            const double ox = real_parameter(node, "offset_x");
            const double oy = real_parameter(node, "offset_y");
            const u64 seed = node_seed(recipe_, node);
            return core::Result<Value, EvaluationError>::success(make_scalar(
                [seed, frequency, ox, oy](const double u, const double v, u32, u32) noexcept {
                    return clamp01(value_noise_raw(seed, u * frequency + ox, v * frequency + oy) * 0.5 + 0.5);
                }));
        }
        if (node.type_id == "core.noise.gradient") {
            const double frequency = real_parameter(node, "frequency");
            const double ox = real_parameter(node, "offset_x");
            const double oy = real_parameter(node, "offset_y");
            const u64 seed = node_seed(recipe_, node);
            return core::Result<Value, EvaluationError>::success(make_scalar(
                [seed, frequency, ox, oy](const double u, const double v, u32, u32) noexcept {
                    return clamp01(gradient_noise_raw(seed, u * frequency + ox, v * frequency + oy) * 0.5 + 0.5);
                }));
        }
        if (node.type_id == "core.noise.worley") {
            const double frequency = real_parameter(node, "frequency");
            const double scale = real_parameter(node, "distance_scale");
            const u64 seed = node_seed(recipe_, node);
            return core::Result<Value, EvaluationError>::success(make_scalar(
                [seed, frequency, scale](const double u, const double v, u32, u32) noexcept {
                    return clamp01(worley_distance(seed, u * frequency, v * frequency) * scale);
                }));
        }
        if (node.type_id == "core.noise.fbm") {
            const std::string& basis = enum_parameter(node, "basis");
            const double initial_frequency = real_parameter(node, "frequency");
            const i64 octaves = integer_parameter(node, "octaves");
            const double lacunarity = real_parameter(node, "lacunarity");
            const double gain = real_parameter(node, "gain");
            const u64 base_seed = node_seed(recipe_, node);
            return core::Result<Value, EvaluationError>::success(make_scalar(
                [basis, initial_frequency, octaves, lacunarity, gain, base_seed](
                    const double u, const double v, u32, u32) noexcept {
                    double frequency = initial_frequency;
                    double amplitude = 1.0;
                    double total = 0.0;
                    double amplitude_sum = 0.0;
                    for (i64 octave = 0; octave < octaves; ++octave) {
                        const u64 seed = core::derive_seed(base_seed, static_cast<u64>(octave));
                        const double sample = basis == "value"
                            ? value_noise_raw(seed, u * frequency, v * frequency)
                            : gradient_noise_raw(seed, u * frequency, v * frequency);
                        total += sample * amplitude;
                        amplitude_sum += amplitude;
                        frequency *= lacunarity;
                        amplitude *= gain;
                    }
                    if (amplitude_sum == 0.0) {
                        return 0.5;
                    }
                    return clamp01((total / amplitude_sum) * 0.5 + 0.5);
                }));
        }
        if (node.type_id == "core.scalar.warp") {
            const ScalarField& source = scalar_input("source");
            const ScalarField& x_offset = scalar_input("x_offset");
            const ScalarField& y_offset = scalar_input("y_offset");
            const double strength = real_parameter(node, "strength");
            const bool repeat = enum_parameter(node, "wrap") == "repeat";
            return core::Result<Value, EvaluationError>::success(make_scalar(
                [&source, &x_offset, &y_offset, strength, repeat, this](
                    const double u, const double v, const u32 x, const u32 y) noexcept {
                    const std::size_t index = index_of(width_, x, y);
                    const double du = (x_offset.values[index] - 0.5) * strength;
                    const double dv = (y_offset.values[index] - 0.5) * strength;
                    return sample_scalar(source, u + du, v + dv, repeat);
                }));
        }
        if (node.type_id == "core.sdf.circle") {
            const double cx = real_parameter(node, "center_x");
            const double cy = real_parameter(node, "center_y");
            const double radius = real_parameter(node, "radius");
            return core::Result<Value, EvaluationError>::success(make_scalar(
                [cx, cy, radius](const double u, const double v, u32, u32) noexcept {
                    const double dx = u - cx;
                    const double dy = v - cy;
                    return std::sqrt(dx * dx + dy * dy) - radius;
                }));
        }
        if (node.type_id == "core.sdf.box") {
            const double cx = real_parameter(node, "center_x");
            const double cy = real_parameter(node, "center_y");
            const double half_width = real_parameter(node, "half_width");
            const double half_height = real_parameter(node, "half_height");
            return core::Result<Value, EvaluationError>::success(make_scalar(
                [cx, cy, half_width, half_height](const double u, const double v, u32, u32) noexcept {
                    const double qx = std::abs(u - cx) - half_width;
                    const double qy = std::abs(v - cy) - half_height;
                    const double outside_x = (std::max)(qx, 0.0);
                    const double outside_y = (std::max)(qy, 0.0);
                    const double outside = std::sqrt(outside_x * outside_x + outside_y * outside_y);
                    const double inside = (std::min)((std::max)(qx, qy), 0.0);
                    return outside + inside;
                }));
        }
        if (node.type_id == "core.scalar.minimum" || node.type_id == "core.scalar.maximum") {
            const ScalarField& a = scalar_input("a");
            const ScalarField& b = scalar_input("b");
            const bool minimum = node.type_id == "core.scalar.minimum";
            return core::Result<Value, EvaluationError>::success(make_scalar(
                [&a, &b, minimum, this](double, double, const u32 x, const u32 y) noexcept {
                    const std::size_t index = index_of(width_, x, y);
                    return minimum ? (std::min)(a.values[index], b.values[index]) : (std::max)(a.values[index], b.values[index]);
                }));
        }
        if (node.type_id == "core.scalar.threshold") {
            const ScalarField& source = scalar_input("source");
            const double threshold = real_parameter(node, "threshold");
            const double low = real_parameter(node, "low");
            const double high = real_parameter(node, "high");
            return core::Result<Value, EvaluationError>::success(make_scalar(
                [&source, threshold, low, high, this](double, double, const u32 x, const u32 y) noexcept {
                    return source.values[index_of(width_, x, y)] >= threshold ? high : low;
                }));
        }
        if (node.type_id == "core.scalar.quantize") {
            const ScalarField& source = scalar_input("source");
            const i64 levels = integer_parameter(node, "levels");
            const double minimum = real_parameter(node, "minimum");
            const double maximum = real_parameter(node, "maximum");
            if (maximum <= minimum) {
                return core::Result<Value, EvaluationError>::failure(make_error(
                    EvaluationErrorCode::invalid_recipe,
                    "node '" + node.id + "' quantize maximum must be greater than minimum"));
            }
            return core::Result<Value, EvaluationError>::success(make_scalar(
                [&source, levels, minimum, maximum, this](double, double, const u32 x, const u32 y) noexcept {
                    const double value = source.values[index_of(width_, x, y)];
                    const double normalized = clamp01((value - minimum) / (maximum - minimum));
                    const double step = std::floor(normalized * static_cast<double>(levels - 1) + 0.5);
                    return minimum + (step / static_cast<double>(levels - 1)) * (maximum - minimum);
                }));
        }
        if (node.type_id == "core.scalar.transform.repeat") {
            const ScalarField& source = scalar_input("source");
            const i64 x_count = integer_parameter(node, "x_count");
            const i64 y_count = integer_parameter(node, "y_count");
            return core::Result<Value, EvaluationError>::success(make_scalar(
                [&source, x_count, y_count](const double u, const double v, u32, u32) noexcept {
                    return sample_scalar(
                        source,
                        u * static_cast<double>(x_count),
                        v * static_cast<double>(y_count),
                        true);
                }));
        }
        if (node.type_id == "core.scalar.transform.symmetry") {
            const ScalarField& source = scalar_input("source");
            const std::string& mode = enum_parameter(node, "mode");
            const bool mirror_x = mode == "x" || mode == "xy";
            const bool mirror_y = mode == "y" || mode == "xy";
            return core::Result<Value, EvaluationError>::success(make_scalar(
                [&source, mirror_x, mirror_y](const double u, const double v, u32, u32) noexcept {
                    const double su = mirror_x ? std::abs(2.0 * u - 1.0) : u;
                    const double sv = mirror_y ? std::abs(2.0 * v - 1.0) : v;
                    return sample_scalar(source, su, sv, false);
                }));
        }
        if (node.type_id == "core.vector.compose") {
            const ScalarField& x_field = scalar_input("x");
            const ScalarField& y_field = scalar_input("y");
            VectorField field{width_, height_, {}};
            field.values.resize(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_));
            for (std::size_t index = 0U; index < field.values.size(); ++index) {
                field.values[index] = {x_field.values[index], y_field.values[index]};
            }
            return core::Result<Value, EvaluationError>::success(std::move(field));
        }
        if (node.type_id == "core.colour.compose") {
            const ScalarField& r = scalar_input("r");
            const ScalarField& g = scalar_input("g");
            const ScalarField& b = scalar_input("b");
            const ScalarField& a = scalar_input("a");
            ColourField field{width_, height_, {}};
            field.values.resize(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_));
            for (std::size_t index = 0U; index < field.values.size(); ++index) {
                field.values[index] = {r.values[index], g.values[index], b.values[index], a.values[index]};
            }
            return core::Result<Value, EvaluationError>::success(std::move(field));
        }
        if (node.type_id == "core.scalar.from_colour") {
            const ColourField& source = colour_input("source");
            const std::string& channel = enum_parameter(node, "channel");
            return core::Result<Value, EvaluationError>::success(make_scalar(
                [&source, channel, this](double, double, const u32 x, const u32 y) noexcept {
                    const Rgba& colour = source.values[index_of(width_, x, y)];
                    if (channel == "r") {
                        return colour.r;
                    }
                    if (channel == "g") {
                        return colour.g;
                    }
                    if (channel == "b") {
                        return colour.b;
                    }
                    if (channel == "a") {
                        return colour.a;
                    }
                    return colour.r * 0.2126 + colour.g * 0.7152 + colour.b * 0.0722;
                }));
        }
        if (node.type_id == "core.mask.from_scalar") {
            const ScalarField& source = scalar_input("source");
            Mask mask{width_, height_, {}};
            mask.values.resize(source.values.size());
            for (std::size_t index = 0U; index < source.values.size(); ++index) {
                mask.values[index] = to_unorm8(source.values[index]);
            }
            return core::Result<Value, EvaluationError>::success(std::move(mask));
        }
        if (node.type_id == "core.palette.default") {
            Palette palette;
            const std::string& preset = enum_parameter(node, "preset");
            if (preset == "mono") {
                palette.colours = {{0.0, 0.0, 0.0, 1.0}, {1.0, 1.0, 1.0, 1.0}};
            } else if (preset == "warm") {
                palette.colours = {
                    {0.055, 0.016, 0.10, 1.0},
                    {0.45, 0.06, 0.08, 1.0},
                    {0.90, 0.32, 0.08, 1.0},
                    {1.0, 0.82, 0.28, 1.0},
                };
            } else {
                palette.colours = {
                    {0.01, 0.04, 0.12, 1.0},
                    {0.02, 0.22, 0.42, 1.0},
                    {0.08, 0.62, 0.72, 1.0},
                    {0.72, 0.95, 0.98, 1.0},
                };
            }
            return core::Result<Value, EvaluationError>::success(std::move(palette));
        }
        if (node.type_id == "core.palette.gradient2") {
            Palette palette;
            palette.colours = {
                {
                    real_parameter(node, "r0"), real_parameter(node, "g0"),
                    real_parameter(node, "b0"), real_parameter(node, "a0"),
                },
                {
                    real_parameter(node, "r1"), real_parameter(node, "g1"),
                    real_parameter(node, "b1"), real_parameter(node, "a1"),
                },
            };
            return core::Result<Value, EvaluationError>::success(std::move(palette));
        }
        if (node.type_id == "core.colour.from_palette") {
            const ScalarField& source = scalar_input("source");
            const Palette& palette = palette_input("palette");
            const bool nearest = enum_parameter(node, "mode") == "nearest";
            ColourField field{width_, height_, {}};
            field.values.resize(source.values.size());
            for (std::size_t index = 0U; index < source.values.size(); ++index) {
                field.values[index] = palette_sample(palette, source.values[index], nearest);
            }
            return core::Result<Value, EvaluationError>::success(std::move(field));
        }
        if (node.type_id == "core.image.from_colour") {
            return core::Result<Value, EvaluationError>::success(image_from_colour(colour_input("source")));
        }
        if (node.type_id == "core.image.from_scalar") {
            const ScalarField& source = scalar_input("source");
            const bool heat = enum_parameter(node, "palette") == "heat";
            ColourField colours{width_, height_, {}};
            colours.values.resize(source.values.size());
            for (std::size_t index = 0U; index < source.values.size(); ++index) {
                if (heat) {
                    colours.values[index] = heat_colour(source.values[index]);
                } else {
                    const double value = clamp01(source.values[index]);
                    colours.values[index] = {value, value, value, 1.0};
                }
            }
            return core::Result<Value, EvaluationError>::success(image_from_colour(colours));
        }
        if (node.type_id == "core.image.ordered_dither") {
            const ColourField& source = colour_input("source");
            const i64 levels = integer_parameter(node, "levels");
            const bool use_bayer8 = enum_parameter(node, "matrix") == "bayer8";
            ColourField dithered{width_, height_, {}};
            dithered.values.resize(source.values.size());
            for (u32 y = 0U; y < height_; ++y) {
                for (u32 x = 0U; x < width_; ++x) {
                    const std::size_t index = index_of(width_, x, y);
                    const int entry = use_bayer8 ? bayer8(x, y) : bayer4(x, y);
                    const double divisor = use_bayer8 ? 64.0 : 16.0;
                    const double threshold = (static_cast<double>(entry) + 0.5) / divisor;
                    const Rgba& colour = source.values[index];
                    dithered.values[index] = {
                        dither_channel(colour.r, levels, threshold),
                        dither_channel(colour.g, levels, threshold),
                        dither_channel(colour.b, levels, threshold),
                        clamp01(colour.a),
                    };
                }
            }
            return core::Result<Value, EvaluationError>::success(image_from_colour(dithered));
        }

        return core::Result<Value, EvaluationError>::failure(make_error(
            EvaluationErrorCode::unsupported_node,
            "canonical CPU evaluator dispatch is missing for node '" + node.id + "' (" + node.type_id + ")"));
    }

    const core::Recipe& recipe_;
    u32 width_{0U};
    u32 height_{0U};
    std::map<std::string, const core::NodeInstance*, std::less<>> nodes_;
    std::map<std::pair<std::string, std::string>, const core::Edge*> incoming_;
    std::map<std::string, Value, std::less<>> values_;
    std::set<std::string, std::less<>> active_;
};

[[nodiscard]] core::Result<void, EvaluationError> preflight_reference_render(const core::Recipe& recipe) {
    const auto validation_errors = core::validate_recipe(recipe);
    if (!validation_errors.empty()) {
        return core::Result<void, EvaluationError>::failure(make_error(
            EvaluationErrorCode::invalid_recipe,
            "recipe validation failed: " + validation_errors.front().message));
    }
    if (recipe.nodes.size() > kMaxReferenceNodes) {
        return core::Result<void, EvaluationError>::failure(make_error(
            EvaluationErrorCode::resource_limit,
            "reference render rejects recipes with more than 1024 nodes"));
    }

    auto pixels_result = core::checked_multiply_u64(
        static_cast<u64>(recipe.render.width), static_cast<u64>(recipe.render.height));
    if (pixels_result.is_error() || pixels_result.value() > kMaxReferencePixels) {
        return core::Result<void, EvaluationError>::failure(make_error(
            EvaluationErrorCode::resource_limit,
            "reference render is limited to 4,194,304 pixels"));
    }
    auto per_node_result = core::checked_multiply_u64(pixels_result.value(), 32ULL);
    if (per_node_result.is_error()) {
        return core::Result<void, EvaluationError>::failure(make_error(
            EvaluationErrorCode::resource_limit,
            "reference render working-set estimate overflowed"));
    }
    auto working_result = core::checked_multiply_u64(
        per_node_result.value(), static_cast<u64>((std::max)(recipe.nodes.size(), std::size_t{1U})));
    if (working_result.is_error() || working_result.value() > kMaxEstimatedWorkingBytes) {
        return core::Result<void, EvaluationError>::failure(make_error(
            EvaluationErrorCode::resource_limit,
            "reference render estimated working set exceeds 768 MiB"));
    }
    return core::Result<void, EvaluationError>::success();
}

}  // namespace

core::Result<Image, EvaluationError> render_reference(
    const core::Recipe& recipe,
    const std::string_view output_name) {
    auto preflight = preflight_reference_render(recipe);
    if (preflight.is_error()) {
        return core::Result<Image, EvaluationError>::failure(preflight.error());
    }

    const auto output = std::find_if(
        recipe.outputs.begin(), recipe.outputs.end(),
        [&](const core::OutputBinding& binding) { return binding.name == output_name; });
    if (output == recipe.outputs.end()) {
        return core::Result<Image, EvaluationError>::failure(make_error(
            EvaluationErrorCode::missing_output,
            "recipe does not define output '" + std::string(output_name) + "'"));
    }

    const core::NodeMetadata* metadata = nullptr;
    const auto node = std::find_if(
        recipe.nodes.begin(), recipe.nodes.end(),
        [&](const core::NodeInstance& instance) { return instance.id == output->node_id; });
    if (node != recipe.nodes.end()) {
        metadata = core::builtin_node_registry().find(node->type_id);
    }
    if (metadata == nullptr) {
        return core::Result<Image, EvaluationError>::failure(make_error(
            EvaluationErrorCode::internal_graph_error,
            "output references node metadata that is unavailable"));
    }
    const auto port = std::find_if(
        metadata->outputs.begin(), metadata->outputs.end(),
        [&](const core::PortSpec& spec) { return spec.name == output->port; });
    if (port == metadata->outputs.end() || port->kind != core::DataKind::image) {
        return core::Result<Image, EvaluationError>::failure(make_error(
            EvaluationErrorCode::unsupported_output_kind,
            "canonical render output '" + std::string(output_name) + "' must have Image kind"));
    }

    try {
        EvaluationContext context(recipe);
        auto evaluated = context.evaluate(output->node_id);
        if (evaluated.is_error()) {
            return core::Result<Image, EvaluationError>::failure(evaluated.error());
        }
        const Value* value = context.value(output->node_id);
        if (value == nullptr || !std::holds_alternative<Image>(*value)) {
            return core::Result<Image, EvaluationError>::failure(make_error(
                EvaluationErrorCode::internal_graph_error,
                "output node did not produce an Image value"));
        }
        return core::Result<Image, EvaluationError>::success(std::get<Image>(*value));
    } catch (const std::bad_alloc&) {
        return core::Result<Image, EvaluationError>::failure(make_error(
            EvaluationErrorCode::resource_limit,
            "reference render could not allocate its bounded working set"));
    }
}

std::string image_fingerprint(const Image& image) {
    return core::hex_u64(core::fnv1a64(std::span<const u8>(image.rgba.data(), image.rgba.size())));
}

}  // namespace artminer::nodes
