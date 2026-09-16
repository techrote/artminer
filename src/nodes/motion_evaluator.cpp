#include "nodes/motion_evaluator.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <new>
#include <numbers>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "core/graph.hpp"
#include "core/hash.hpp"
#include "core/prng.hpp"

namespace artminer::nodes {
namespace {

using core::i64;
using core::u8;
using core::u32;
using core::u64;

constexpr u64 kMaximumAnimationPixels = 1'048'576ULL;
constexpr u64 kMaximumParticleUpdates = 8'000'000ULL;
constexpr std::size_t kMaximumAnimationNodes = 256U;
constexpr double kInvU53 = 1.0 / 9007199254740992.0;

struct ScalarField final {
    u32 width{0U};
    u32 height{0U};
    std::vector<double> values;
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

struct Palette final {
    std::vector<Rgba> colours;
};

using Value = std::variant<ScalarField, ColourField, ParticleSet, Palette, Image>;
using EvaluationKey = std::pair<std::string, u64>;

[[nodiscard]] MotionError make_error(const MotionErrorCode code, std::string message) {
    return MotionError{code, std::move(message)};
}

[[nodiscard]] const core::ParameterAssignment* find_parameter(
    const core::NodeInstance& node,
    const std::string_view name) noexcept {
    const auto found = std::find_if(
        node.parameters.begin(), node.parameters.end(),
        [&](const core::ParameterAssignment& assignment) { return assignment.name == name; });
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

[[nodiscard]] u64 particle_seed(const core::Recipe& recipe, const core::NodeInstance& node) {
    std::string identity;
    identity.reserve(node.id.size() + node.type_id.size() + 1U);
    identity.append(node.id);
    identity.push_back('\n');
    identity.append(node.type_id);
    return core::derive_seed(recipe.root_seed, core::fnv1a64(identity));
}

[[nodiscard]] u64 keyed_hash(const u64 seed, const u64 tick, const u64 id, const u64 purpose) noexcept {
    return core::splitmix64(
        seed ^ core::splitmix64(tick ^ 0x9e3779b97f4a7c15ULL) ^
        core::splitmix64(id ^ 0xbf58476d1ce4e5b9ULL) ^ purpose);
}

[[nodiscard]] double unit_from_hash(const u64 value) noexcept {
    return static_cast<double>(value >> 11U) * kInvU53;
}

void normalize(double& x, double& y) noexcept {
    const double length_squared = x * x + y * y;
    if (length_squared <= 1.0e-24) {
        x = 1.0;
        y = 0.0;
        return;
    }
    const double inverse = 1.0 / std::sqrt(length_squared);
    x *= inverse;
    y *= inverse;
}

void apply_boundary(Particle& particle, const bool repeat) noexcept {
    if (repeat) {
        particle.x -= std::floor(particle.x);
        particle.y -= std::floor(particle.y);
        return;
    }
    particle.x = std::clamp(particle.x, 0.0, 1.0);
    particle.y = std::clamp(particle.y, 0.0, 1.0);
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

[[nodiscard]] Image image_from_colour(const ColourField& colours) {
    Image image;
    image.width = colours.width;
    image.height = colours.height;
    image.rgba.resize(colours.values.size() * 4U);
    for (std::size_t index = 0U; index < colours.values.size(); ++index) {
        const Rgba& colour = colours.values[index];
        image.rgba[index * 4U + 0U] = to_unorm8(colour.r);
        image.rgba[index * 4U + 1U] = to_unorm8(colour.g);
        image.rgba[index * 4U + 2U] = to_unorm8(colour.b);
        image.rgba[index * 4U + 3U] = to_unorm8(colour.a);
    }
    return image;
}

[[nodiscard]] Rgba palette_sample(const Palette& palette, const double value) noexcept {
    if (palette.colours.empty()) {
        return {};
    }
    if (palette.colours.size() == 1U) {
        return palette.colours.front();
    }
    const double scaled = clamp01(value) * static_cast<double>(palette.colours.size() - 1U);
    const std::size_t lower = static_cast<std::size_t>(std::floor(scaled));
    const std::size_t upper = (std::min)(lower + 1U, palette.colours.size() - 1U);
    const double blend = scaled - static_cast<double>(lower);
    const Rgba& a = palette.colours[lower];
    const Rgba& b = palette.colours[upper];
    return {
        a.r + (b.r - a.r) * blend,
        a.g + (b.g - a.g) * blend,
        a.b + (b.b - a.b) * blend,
        a.a + (b.a - a.a) * blend,
    };
}

[[nodiscard]] core::Result<ScalarField, MotionError> deposit_particles(
    const ParticleSet& set,
    const u32 width,
    const u32 height,
    const double radius,
    const double intensity,
    const double trail_decay) {
    ScalarField field{width, height, {}};
    try {
        field.values.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0.0);
    } catch (const std::bad_alloc&) {
        return core::Result<ScalarField, MotionError>::failure(make_error(
            MotionErrorCode::resource_limit, "particle deposition could not allocate its bounded field"));
    }

    const double minimum_dimension = static_cast<double>((std::min)(width, height));
    const int radius_pixels = static_cast<int>(std::ceil(radius * minimum_dimension));
    for (const Particle& particle : set.particles) {
        double weight = intensity;
        for (auto point = particle.trail.rbegin(); point != particle.trail.rend(); ++point) {
            const int center_x = static_cast<int>((std::min)(
                static_cast<u32>(std::floor(clamp01(point->x) * static_cast<double>(width))), width - 1U));
            const int center_y = static_cast<int>((std::min)(
                static_cast<u32>(std::floor(clamp01(point->y) * static_cast<double>(height))), height - 1U));
            if (radius_pixels == 0) {
                const std::size_t index = index_of(width, static_cast<u32>(center_x), static_cast<u32>(center_y));
                field.values[index] = (std::min)(1.0, field.values[index] + weight);
            } else {
                const int radius_squared = radius_pixels * radius_pixels;
                for (int offset_y = -radius_pixels; offset_y <= radius_pixels; ++offset_y) {
                    for (int offset_x = -radius_pixels; offset_x <= radius_pixels; ++offset_x) {
                        const int distance_squared = offset_x * offset_x + offset_y * offset_y;
                        if (distance_squared > radius_squared) {
                            continue;
                        }
                        const int x = center_x + offset_x;
                        const int y = center_y + offset_y;
                        if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) {
                            continue;
                        }
                        const double distance = std::sqrt(static_cast<double>(distance_squared));
                        const double falloff = 1.0 - distance / static_cast<double>(radius_pixels + 1);
                        const std::size_t index = index_of(width, static_cast<u32>(x), static_cast<u32>(y));
                        field.values[index] = (std::min)(1.0, field.values[index] + weight * falloff);
                    }
                }
            }
            weight *= trail_decay;
            if (weight <= 1.0e-12) {
                break;
            }
        }
    }
    return core::Result<ScalarField, MotionError>::success(std::move(field));
}

[[nodiscard]] std::string cache_key(
    const core::Recipe& recipe,
    const u64 tick,
    const std::string_view output_name) {
    return "am007-frame-v1|" + core::semantic_fingerprint(recipe) + "|" + std::string(output_name) + "|" +
        std::to_string(tick);
}

[[nodiscard]] bool has_state_boundary(const core::Recipe& recipe) {
    for (const auto& node : recipe.nodes) {
        const core::NodeMetadata* metadata = core::builtin_node_registry().find(node.type_id);
        if (metadata != nullptr && metadata->state_class == core::NodeStateClass::state_boundary) {
            return true;
        }
    }
    return false;
}

class MotionEvaluationContext final {
public:
    MotionEvaluationContext(const core::Recipe& recipe, const u32 width, const u32 height)
        : recipe_(recipe), width_(width), height_(height) {
        for (const auto& node : recipe.nodes) {
            nodes_.emplace(node.id, &node);
        }
        for (const auto& edge : recipe.edges) {
            incoming_.emplace(std::make_pair(edge.to_node, edge.to_port), &edge);
        }
    }

    [[nodiscard]] core::Result<void, MotionError> evaluate(const std::string& node_id, const u64 tick) {
        const EvaluationKey key{node_id, tick};
        if (values_.contains(key)) {
            return core::Result<void, MotionError>::success();
        }
        if (!active_.insert(key).second) {
            return core::Result<void, MotionError>::failure(make_error(
                MotionErrorCode::internal_graph_error,
                "unexpected same-tick recursion at node '" + node_id + "' tick " + std::to_string(tick)));
        }

        const auto node_found = nodes_.find(node_id);
        if (node_found == nodes_.end()) {
            active_.erase(key);
            return core::Result<void, MotionError>::failure(make_error(
                MotionErrorCode::internal_graph_error, "animation evaluator could not find node '" + node_id + "'"));
        }
        const core::NodeInstance& node = *node_found->second;
        const core::NodeMetadata* metadata = core::builtin_node_registry().find(node.type_id);
        if (metadata == nullptr) {
            active_.erase(key);
            return core::Result<void, MotionError>::failure(make_error(
                MotionErrorCode::unsupported_node, "no metadata for animation node type '" + node.type_id + "'"));
        }

        if (metadata->state_class == core::NodeStateClass::state_boundary) {
            auto boundary = evaluate_boundary(node, tick);
            active_.erase(key);
            if (boundary.is_error()) {
                return core::Result<void, MotionError>::failure(boundary.error());
            }
            values_.emplace(key, std::move(boundary).value());
            return core::Result<void, MotionError>::success();
        }
        if (!metadata->evaluators.cpu) {
            active_.erase(key);
            return core::Result<void, MotionError>::failure(make_error(
                MotionErrorCode::unsupported_node,
                "canonical animation CPU evaluator is unavailable for node '" + node.id + "' (" + node.type_id + ")"));
        }

        std::map<std::string, const Value*, std::less<>> inputs;
        for (const auto& port : metadata->inputs) {
            const auto incoming = incoming_.find(std::make_pair(node.id, port.name));
            if (incoming == incoming_.end()) {
                continue;
            }
            const core::Edge& edge = *incoming->second;
            auto source = evaluate(edge.from_node, tick);
            if (source.is_error()) {
                active_.erase(key);
                return source;
            }
            inputs.emplace(port.name, &values_.at(EvaluationKey{edge.from_node, tick}));
        }

        auto evaluated = evaluate_builtin(node, inputs, tick);
        active_.erase(key);
        if (evaluated.is_error()) {
            return core::Result<void, MotionError>::failure(evaluated.error());
        }
        values_.emplace(key, std::move(evaluated).value());
        return core::Result<void, MotionError>::success();
    }

    [[nodiscard]] const Value* value(const std::string& node_id, const u64 tick) const noexcept {
        const auto found = values_.find(EvaluationKey{node_id, tick});
        return found == values_.end() ? nullptr : &found->second;
    }

private:
    [[nodiscard]] core::Result<Value, MotionError> evaluate_boundary(
        const core::NodeInstance& node,
        const u64 tick) {
        if (node.type_id == "core.state.delay.image") {
            if (tick == 0U) {
                Image image;
                image.width = width_;
                image.height = height_;
                image.rgba.resize(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4U);
                const std::string& initial = enum_parameter(node, "initial");
                const u8 channel = initial == "white" ? 255U : 0U;
                const u8 alpha = initial == "transparent" ? 0U : 255U;
                for (std::size_t offset = 0U; offset < image.rgba.size(); offset += 4U) {
                    image.rgba[offset + 0U] = channel;
                    image.rgba[offset + 1U] = channel;
                    image.rgba[offset + 2U] = channel;
                    image.rgba[offset + 3U] = alpha;
                }
                return core::Result<Value, MotionError>::success(std::move(image));
            }
        } else if (node.type_id == "core.state.delay.scalar") {
            if (tick == 0U) {
                ScalarField field{width_, height_, {}};
                field.values.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), 0.0);
                return core::Result<Value, MotionError>::success(std::move(field));
            }
        } else {
            return core::Result<Value, MotionError>::failure(make_error(
                MotionErrorCode::unsupported_node,
                "unsupported state-boundary node type '" + node.type_id + "'"));
        }

        const auto incoming = incoming_.find(std::make_pair(node.id, std::string("next")));
        if (incoming == incoming_.end()) {
            return core::Result<Value, MotionError>::failure(make_error(
                MotionErrorCode::internal_graph_error,
                "state boundary '" + node.id + "' has no next input"));
        }
        const core::Edge& edge = *incoming->second;
        auto previous = evaluate(edge.from_node, tick - 1U);
        if (previous.is_error()) {
            return core::Result<Value, MotionError>::failure(previous.error());
        }
        const Value* previous_value = value(edge.from_node, tick - 1U);
        if (previous_value == nullptr) {
            return core::Result<Value, MotionError>::failure(make_error(
                MotionErrorCode::internal_graph_error, "state-boundary previous value is unavailable"));
        }
        return core::Result<Value, MotionError>::success(*previous_value);
    }

    [[nodiscard]] core::Result<Value, MotionError> evaluate_builtin(
        const core::NodeInstance& node,
        const std::map<std::string, const Value*, std::less<>>& inputs,
        const u64 tick) const {
        const auto particle_input = [&](const std::string_view name) -> const ParticleSet& {
            return std::get<ParticleSet>(*inputs.at(std::string(name)));
        };
        const auto scalar_input = [&](const std::string_view name) -> const ScalarField& {
            return std::get<ScalarField>(*inputs.at(std::string(name)));
        };
        const auto image_input = [&](const std::string_view name) -> const Image& {
            return std::get<Image>(*inputs.at(std::string(name)));
        };
        const auto palette_input = [&](const std::string_view name) -> const Palette& {
            return std::get<Palette>(*inputs.at(std::string(name)));
        };

        if (node.type_id == "core.motion.particles") {
            auto particles = evaluate_particle_set(recipe_, node, tick);
            if (particles.is_error()) {
                return core::Result<Value, MotionError>::failure(particles.error());
            }
            return core::Result<Value, MotionError>::success(std::move(particles).value());
        }
        if (node.type_id == "core.particles.deposit.scalar" || node.type_id == "core.particles.deposit.image") {
            const ParticleSet& particles = particle_input("particles");
            auto deposited = deposit_particles(
                particles,
                width_,
                height_,
                real_parameter(node, "radius"),
                real_parameter(node, "intensity"),
                real_parameter(node, "trail_decay"));
            if (deposited.is_error()) {
                return core::Result<Value, MotionError>::failure(deposited.error());
            }
            ScalarField field = std::move(deposited).value();
            if (node.type_id == "core.particles.deposit.scalar") {
                return core::Result<Value, MotionError>::success(std::move(field));
            }
            ColourField colours{width_, height_, {}};
            colours.values.resize(field.values.size());
            const bool heat = enum_parameter(node, "palette") == "heat";
            for (std::size_t index = 0U; index < field.values.size(); ++index) {
                if (heat) {
                    colours.values[index] = heat_colour(field.values[index]);
                } else {
                    const double value = clamp01(field.values[index]);
                    colours.values[index] = {value, value, value, 1.0};
                }
            }
            return core::Result<Value, MotionError>::success(image_from_colour(colours));
        }
        if (node.type_id == "core.scalar.constant") {
            ScalarField field{width_, height_, {}};
            field.values.assign(
                static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_),
                real_parameter(node, "value"));
            return core::Result<Value, MotionError>::success(std::move(field));
        }
        if (node.type_id == "core.scalar.radial") {
            ScalarField field{width_, height_, {}};
            field.values.resize(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_));
            const double center_x = real_parameter(node, "center_x");
            const double center_y = real_parameter(node, "center_y");
            const double scale = real_parameter(node, "scale");
            for (u32 y = 0U; y < height_; ++y) {
                const double v = (static_cast<double>(y) + 0.5) / static_cast<double>(height_);
                for (u32 x = 0U; x < width_; ++x) {
                    const double u = (static_cast<double>(x) + 0.5) / static_cast<double>(width_);
                    const double dx = (u - center_x) * 2.0;
                    const double dy = (v - center_y) * 2.0;
                    field.values[index_of(width_, x, y)] = std::sqrt(dx * dx + dy * dy) * scale;
                }
            }
            return core::Result<Value, MotionError>::success(std::move(field));
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
            return core::Result<Value, MotionError>::success(std::move(palette));
        }
        if (node.type_id == "core.palette.cycle") {
            Palette palette = palette_input("palette");
            if (!palette.colours.empty()) {
                const i64 count = static_cast<i64>(palette.colours.size());
                const i64 rate = integer_parameter(node, "rate") % count;
                const i64 phase = integer_parameter(node, "phase") % count;
                const i64 tick_mod = static_cast<i64>(tick % static_cast<u64>(count));
                i64 offset = (phase + rate * tick_mod) % count;
                if (offset < 0) {
                    offset += count;
                }
                std::rotate(
                    palette.colours.begin(),
                    palette.colours.begin() + static_cast<std::ptrdiff_t>(offset),
                    palette.colours.end());
            }
            return core::Result<Value, MotionError>::success(std::move(palette));
        }
        if (node.type_id == "core.colour.from_palette") {
            const ScalarField& source = scalar_input("source");
            const Palette& palette = palette_input("palette");
            ColourField colours{width_, height_, {}};
            colours.values.resize(source.values.size());
            for (std::size_t index = 0U; index < source.values.size(); ++index) {
                colours.values[index] = palette_sample(palette, source.values[index]);
            }
            return core::Result<Value, MotionError>::success(std::move(colours));
        }
        if (node.type_id == "core.image.from_colour") {
            const ColourField& source = std::get<ColourField>(*inputs.at("source"));
            return core::Result<Value, MotionError>::success(image_from_colour(source));
        }
        if (node.type_id == "core.image.from_scalar") {
            const ScalarField& source = scalar_input("source");
            ColourField colours{width_, height_, {}};
            colours.values.resize(source.values.size());
            const bool heat = enum_parameter(node, "palette") == "heat";
            for (std::size_t index = 0U; index < source.values.size(); ++index) {
                if (heat) {
                    colours.values[index] = heat_colour(source.values[index]);
                } else {
                    const double value = clamp01(source.values[index]);
                    colours.values[index] = {value, value, value, 1.0};
                }
            }
            return core::Result<Value, MotionError>::success(image_from_colour(colours));
        }
        if (node.type_id == "core.image.blend") {
            const Image& current = image_input("current");
            const Image& history = image_input("history");
            if (current.width != history.width || current.height != history.height || current.rgba.size() != history.rgba.size()) {
                return core::Result<Value, MotionError>::failure(make_error(
                    MotionErrorCode::invalid_recipe, "image blend inputs must have matching dimensions"));
            }
            Image blended{current.width, current.height, {}};
            blended.rgba.resize(current.rgba.size());
            const u32 history_weight = static_cast<u32>(
                std::floor(clamp01(real_parameter(node, "history_weight")) * 256.0 + 0.5));
            const u32 current_weight = 256U - history_weight;
            for (std::size_t index = 0U; index < current.rgba.size(); ++index) {
                const u32 mixed = static_cast<u32>(current.rgba[index]) * current_weight +
                    static_cast<u32>(history.rgba[index]) * history_weight;
                blended.rgba[index] = static_cast<u8>((mixed + 128U) / 256U);
            }
            return core::Result<Value, MotionError>::success(std::move(blended));
        }

        return core::Result<Value, MotionError>::failure(make_error(
            MotionErrorCode::unsupported_node,
            "canonical animation evaluator does not implement node '" + node.id + "' (" + node.type_id + ")"));
    }

    const core::Recipe& recipe_;
    u32 width_{0U};
    u32 height_{0U};
    std::map<std::string, const core::NodeInstance*, std::less<>> nodes_;
    std::map<std::pair<std::string, std::string>, const core::Edge*> incoming_;
    std::map<EvaluationKey, Value> values_;
    std::set<EvaluationKey> active_;
};

}  // namespace

FrameSnapshotCache::FrameSnapshotCache(const std::size_t capacity) noexcept
    : capacity_(capacity) {}

void FrameSnapshotCache::clear() noexcept {
    entries_.clear();
}

std::size_t FrameSnapshotCache::size() const noexcept {
    return entries_.size();
}

core::Result<ParticleSet, MotionError> evaluate_particle_set(
    const core::Recipe& recipe,
    const core::NodeInstance& node,
    const u64 tick) {
    if (node.type_id != "core.motion.particles") {
        return core::Result<ParticleSet, MotionError>::failure(make_error(
            MotionErrorCode::unsupported_node, "particle evaluator requires core.motion.particles"));
    }
    if (tick > kMaximumAnimationTick) {
        return core::Result<ParticleSet, MotionError>::failure(make_error(
            MotionErrorCode::resource_limit,
            "requested animation tick exceeds the canonical AM-007 limit of " + std::to_string(kMaximumAnimationTick)));
    }

    const i64 count_value = integer_parameter(node, "count");
    const i64 lifetime_value = integer_parameter(node, "lifetime");
    const i64 trail_length_value = integer_parameter(node, "trail_length");
    if (count_value <= 0 || lifetime_value <= 0 || trail_length_value <= 0) {
        return core::Result<ParticleSet, MotionError>::failure(make_error(
            MotionErrorCode::invalid_recipe, "particle count, lifetime and trail_length must be positive"));
    }
    const u64 count = static_cast<u64>(count_value);
    if (count > kMaximumParticleUpdates / (tick + 1U)) {
        return core::Result<ParticleSet, MotionError>::failure(make_error(
            MotionErrorCode::resource_limit,
            "particle reconstruction exceeds the canonical fixed-update work limit"));
    }

    const std::string& spawn = enum_parameter(node, "spawn");
    const std::string& mode = enum_parameter(node, "mode");
    const bool repeat = enum_parameter(node, "boundary") == "repeat";
    const double speed = real_parameter(node, "speed");
    const double turn_strength = real_parameter(node, "turn_strength");
    const double field_scale = real_parameter(node, "field_scale");
    const double target_x = real_parameter(node, "target_x");
    const double target_y = real_parameter(node, "target_y");
    const std::size_t trail_length = static_cast<std::size_t>(trail_length_value);
    const u32 lifetime = static_cast<u32>(lifetime_value);
    const u64 seed = particle_seed(recipe, node);

    ParticleSet set;
    try {
        set.particles.reserve(static_cast<std::size_t>(count));
        const double root_phase = unit_from_hash(core::splitmix64(seed)) * 2.0 * std::numbers::pi_v<double>;
        for (u64 id = 0U; id < count; ++id) {
            Particle particle;
            particle.id = id;
            particle.lifetime = lifetime;
            const double heading = unit_from_hash(keyed_hash(seed, 0U, id, 0x243f6a8885a308d3ULL)) *
                2.0 * std::numbers::pi_v<double>;
            if (spawn == "ring") {
                const double angle = root_phase + 2.0 * std::numbers::pi_v<double> *
                    (static_cast<double>(id) / static_cast<double>(count));
                particle.x = 0.5 + std::cos(angle) * 0.28;
                particle.y = 0.5 + std::sin(angle) * 0.28;
            } else if (spawn == "seeded") {
                particle.x = unit_from_hash(keyed_hash(seed, 0U, id, 0x13198a2e03707344ULL));
                particle.y = unit_from_hash(keyed_hash(seed, 0U, id, 0xa4093822299f31d0ULL));
            } else {
                particle.x = 0.5 + std::cos(heading) * 0.012;
                particle.y = 0.5 + std::sin(heading) * 0.012;
            }
            particle.velocity_x = std::cos(heading) * speed;
            particle.velocity_y = std::sin(heading) * speed;
            particle.trail.reserve(trail_length);
            particle.trail.push_back({particle.x, particle.y});
            set.particles.push_back(std::move(particle));
        }

        for (u64 step = 1U; step <= tick; ++step) {
            std::vector<Particle> survivors;
            survivors.reserve(set.particles.size());
            for (Particle particle : set.particles) {
                if (particle.age >= particle.lifetime) {
                    continue;
                }

                double direction_x = particle.velocity_x;
                double direction_y = particle.velocity_y;
                normalize(direction_x, direction_y);
                double desired_x = direction_x;
                double desired_y = direction_y;

                if (mode == "flow") {
                    const double phase = static_cast<double>(seed & 0xffffULL) / 65535.0;
                    const double angle =
                        std::sin((particle.x + phase) * field_scale * 2.0 * std::numbers::pi_v<double>) +
                        std::cos((particle.y - phase) * field_scale * 2.0 * std::numbers::pi_v<double>);
                    desired_x = std::cos(angle * std::numbers::pi_v<double>);
                    desired_y = std::sin(angle * std::numbers::pi_v<double>);
                } else if (mode == "attract" || mode == "repel") {
                    desired_x = target_x - particle.x;
                    desired_y = target_y - particle.y;
                    if (mode == "repel") {
                        desired_x = -desired_x;
                        desired_y = -desired_y;
                    }
                    normalize(desired_x, desired_y);
                } else if (mode == "orbit") {
                    const double radial_x = particle.x - target_x;
                    const double radial_y = particle.y - target_y;
                    desired_x = -radial_y;
                    desired_y = radial_x;
                    normalize(desired_x, desired_y);
                } else {
                    const double turn =
                        (unit_from_hash(keyed_hash(seed, step, particle.id, 0x082efa98ec4e6c89ULL)) - 0.5) *
                        2.0 * std::numbers::pi_v<double> * turn_strength;
                    const double cosine = std::cos(turn);
                    const double sine = std::sin(turn);
                    desired_x = direction_x * cosine - direction_y * sine;
                    desired_y = direction_x * sine + direction_y * cosine;
                }

                if (mode != "walker") {
                    direction_x = direction_x * (1.0 - turn_strength) + desired_x * turn_strength;
                    direction_y = direction_y * (1.0 - turn_strength) + desired_y * turn_strength;
                    normalize(direction_x, direction_y);
                } else {
                    direction_x = desired_x;
                    direction_y = desired_y;
                    normalize(direction_x, direction_y);
                }
                particle.velocity_x = direction_x * speed;
                particle.velocity_y = direction_y * speed;
                particle.x += particle.velocity_x;
                particle.y += particle.velocity_y;
                apply_boundary(particle, repeat);
                ++particle.age;
                particle.trail.push_back({particle.x, particle.y});
                if (particle.trail.size() > trail_length) {
                    particle.trail.erase(particle.trail.begin());
                }
                if (particle.age < particle.lifetime) {
                    survivors.push_back(std::move(particle));
                }
            }
            set.particles = std::move(survivors);
        }
    } catch (const std::bad_alloc&) {
        return core::Result<ParticleSet, MotionError>::failure(make_error(
            MotionErrorCode::resource_limit, "particle reconstruction could not allocate its bounded state"));
    }

    return core::Result<ParticleSet, MotionError>::success(std::move(set));
}

core::Result<Image, MotionError> render_animation_reference(
    const core::Recipe& recipe,
    const u64 tick,
    const std::string_view output_name,
    FrameSnapshotCache* cache) {
    const auto validation = core::validate_recipe(recipe);
    if (!validation.empty()) {
        return core::Result<Image, MotionError>::failure(make_error(
            MotionErrorCode::invalid_recipe,
            "canonical animation render requires a valid recipe: " + validation.front().message));
    }
    if (tick > kMaximumAnimationTick) {
        return core::Result<Image, MotionError>::failure(make_error(
            MotionErrorCode::resource_limit,
            "requested animation tick exceeds the canonical AM-007 limit of " + std::to_string(kMaximumAnimationTick)));
    }
    if (has_state_boundary(recipe) && tick > kMaximumFeedbackTick) {
        return core::Result<Image, MotionError>::failure(make_error(
            MotionErrorCode::resource_limit,
            "feedback reconstruction exceeds the canonical AM-007 boundary limit of " + std::to_string(kMaximumFeedbackTick)));
    }
    const u64 pixels = static_cast<u64>(recipe.render.width) * static_cast<u64>(recipe.render.height);
    if (pixels == 0U || pixels > kMaximumAnimationPixels || recipe.nodes.size() > kMaximumAnimationNodes) {
        return core::Result<Image, MotionError>::failure(make_error(
            MotionErrorCode::resource_limit,
            "canonical animation render exceeds its pixel/node working-set limit"));
    }

    const std::string key = cache_key(recipe, tick, output_name);
    if (cache != nullptr) {
        const auto found = std::find_if(
            cache->entries_.begin(), cache->entries_.end(),
            [&](const FrameSnapshotCache::Entry& entry) { return entry.key == key; });
        if (found != cache->entries_.end()) {
            return core::Result<Image, MotionError>::success(found->image);
        }
    }

    const auto output = std::find_if(
        recipe.outputs.begin(), recipe.outputs.end(),
        [&](const core::OutputBinding& binding) { return binding.name == output_name; });
    if (output == recipe.outputs.end()) {
        return core::Result<Image, MotionError>::failure(make_error(
            MotionErrorCode::missing_output,
            "animation recipe does not define output '" + std::string(output_name) + "'"));
    }
    const auto output_node = std::find_if(
        recipe.nodes.begin(), recipe.nodes.end(),
        [&](const core::NodeInstance& node) { return node.id == output->node_id; });
    if (output_node == recipe.nodes.end()) {
        return core::Result<Image, MotionError>::failure(make_error(
            MotionErrorCode::internal_graph_error, "animation output references a missing node"));
    }
    const core::NodeMetadata* output_metadata = core::builtin_node_registry().find(output_node->type_id);
    if (output_metadata == nullptr) {
        return core::Result<Image, MotionError>::failure(make_error(
            MotionErrorCode::internal_graph_error, "animation output metadata is unavailable"));
    }
    const auto output_port = std::find_if(
        output_metadata->outputs.begin(), output_metadata->outputs.end(),
        [&](const core::PortSpec& port) { return port.name == output->port; });
    if (output_port == output_metadata->outputs.end() || output_port->kind != core::DataKind::image) {
        return core::Result<Image, MotionError>::failure(make_error(
            MotionErrorCode::unsupported_output_kind, "canonical animation output must have Image kind"));
    }

    try {
        MotionEvaluationContext context(recipe, recipe.render.width, recipe.render.height);
        auto evaluated = context.evaluate(output->node_id, tick);
        if (evaluated.is_error()) {
            return core::Result<Image, MotionError>::failure(evaluated.error());
        }
        const Value* value = context.value(output->node_id, tick);
        if (value == nullptr || !std::holds_alternative<Image>(*value)) {
            return core::Result<Image, MotionError>::failure(make_error(
                MotionErrorCode::internal_graph_error, "animation output node did not produce an Image"));
        }
        Image image = std::get<Image>(*value);
        if (cache != nullptr && cache->capacity_ != 0U) {
            if (cache->entries_.size() >= cache->capacity_) {
                cache->entries_.erase(cache->entries_.begin());
            }
            cache->entries_.push_back(FrameSnapshotCache::Entry{key, image});
        }
        return core::Result<Image, MotionError>::success(std::move(image));
    } catch (const std::bad_alloc&) {
        return core::Result<Image, MotionError>::failure(make_error(
            MotionErrorCode::resource_limit, "canonical animation render could not allocate its bounded working set"));
    }
}

}  // namespace artminer::nodes
