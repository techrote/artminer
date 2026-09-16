#include "nodes/growth_evaluator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "core/checked_math.hpp"
#include "core/hash.hpp"
#include "core/prng.hpp"

namespace artminer::nodes {
namespace {

using core::i64;
using core::u8;
using core::u32;
using core::u64;

constexpr u64 kMaxCellTickUpdates = 67'108'864ULL;
constexpr u64 kMaxWalkerUpdates = 16'777'216ULL;
constexpr u64 kMaxBranchTipUpdates = 16'777'216ULL;
constexpr double kInvU53 = 1.0 / 9007199254740992.0;

struct Tip final {
    u64 id{0U};
    i64 x{0};
    i64 y{0};
    int direction{0};
};

[[nodiscard]] GrowthError make_error(const GrowthErrorCode code, std::string message) {
    return GrowthError{code, std::move(message)};
}

[[nodiscard]] const core::ParameterAssignment* find_parameter(
    const core::NodeInstance& node,
    const std::string_view name) noexcept {
    const auto found = std::find_if(
        node.parameters.begin(), node.parameters.end(),
        [name](const core::ParameterAssignment& parameter) { return parameter.name == name; });
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

[[nodiscard]] std::size_t index_of(const u32 width, const u32 x, const u32 y) noexcept {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
}

[[nodiscard]] u64 node_seed(const core::Recipe& recipe, const core::NodeInstance& node) {
    std::string identity;
    identity.reserve(node.id.size() + node.type_id.size() + 1U);
    identity.append(node.id);
    identity.push_back('\n');
    identity.append(node.type_id);
    return core::derive_seed(recipe.root_seed, core::fnv1a64(identity));
}

[[nodiscard]] u64 hash_cell(const u64 seed, const i64 x, const i64 y, const u64 salt) noexcept {
    const u64 hx = core::splitmix64(static_cast<u64>(x) ^ 0x9e3779b97f4a7c15ULL);
    const u64 hy = core::splitmix64(static_cast<u64>(y) ^ 0xbf58476d1ce4e5b9ULL);
    return core::splitmix64(seed ^ hx ^ hy ^ salt);
}

[[nodiscard]] double unit_from_hash(const u64 value) noexcept {
    return static_cast<double>(value >> 11U) * kInvU53;
}

[[nodiscard]] double clamp01(const double value) noexcept {
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] u32 resolve_coordinate(const i64 value, const u32 extent, const bool repeat) noexcept {
    const i64 signed_extent = static_cast<i64>(extent);
    if (repeat) {
        return static_cast<u32>(((value % signed_extent) + signed_extent) % signed_extent);
    }
    return static_cast<u32>(std::clamp<i64>(value, 0, signed_extent - 1));
}

[[nodiscard]] core::Result<u64, GrowthError> checked_updates(
    const u32 width,
    const u32 height,
    const i64 tick,
    const u64 limit,
    const std::string_view family) {
    auto pixels = core::checked_multiply_u64(static_cast<u64>(width), static_cast<u64>(height));
    if (pixels.is_error()) {
        return core::Result<u64, GrowthError>::failure(make_error(
            GrowthErrorCode::resource_limit, std::string(family) + " pixel count overflowed"));
    }
    auto work = core::checked_multiply_u64(pixels.value(), static_cast<u64>(tick));
    if (work.is_error() || work.value() > limit) {
        return core::Result<u64, GrowthError>::failure(make_error(
            GrowthErrorCode::resource_limit,
            std::string(family) + " requested tick and dimensions exceed the canonical work limit"));
    }
    return core::Result<u64, GrowthError>::success(work.value());
}

[[nodiscard]] core::Result<GrowthField, GrowthError> evaluate_reaction_diffusion(
    const core::Recipe& recipe,
    const core::NodeInstance& node,
    const u32 width,
    const u32 height) {
    const i64 tick = integer_parameter(node, "tick");
    auto work = checked_updates(width, height, tick, kMaxCellTickUpdates, "reaction-diffusion");
    if (work.is_error()) {
        return core::Result<GrowthField, GrowthError>::failure(work.error());
    }

    const double feed = real_parameter(node, "feed");
    const double kill = real_parameter(node, "kill");
    const double diffusion_a = real_parameter(node, "diffusion_a");
    const double diffusion_b = real_parameter(node, "diffusion_b");
    const double dt = real_parameter(node, "dt");
    const double seed_radius = real_parameter(node, "seed_radius");
    const double seed_noise = real_parameter(node, "seed_noise");
    const bool repeat = enum_parameter(node, "boundary") == "repeat";
    const u64 seed = node_seed(recipe, node);
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    std::vector<double> a(count, 1.0);
    std::vector<double> b(count, 0.0);
    std::vector<double> next_a(count, 0.0);
    std::vector<double> next_b(count, 0.0);

    for (u32 y = 0U; y < height; ++y) {
        const double v = (static_cast<double>(y) + 0.5) / static_cast<double>(height);
        for (u32 x = 0U; x < width; ++x) {
            const double u = (static_cast<double>(x) + 0.5) / static_cast<double>(width);
            const double dx = u - 0.5;
            const double dy = v - 0.5;
            const std::size_t index = index_of(width, x, y);
            const double jitter = unit_from_hash(hash_cell(seed, x, y, 0x5244494e495431ULL));
            const bool central_seed = std::sqrt(dx * dx + dy * dy) <= seed_radius * (0.85 + jitter * 0.30);
            const double sparse = unit_from_hash(hash_cell(seed, x, y, 0x5244494e495432ULL));
            if (central_seed || sparse < seed_noise) {
                b[index] = 0.55 + 0.45 * jitter;
                a[index] = 1.0 - b[index] * 0.5;
            }
        }
    }

    const auto sample = [width, height, repeat](const std::vector<double>& field, const i64 x, const i64 y) noexcept {
        const u32 sx = resolve_coordinate(x, width, repeat);
        const u32 sy = resolve_coordinate(y, height, repeat);
        return field[index_of(width, sx, sy)];
    };

    for (i64 step = 0; step < tick; ++step) {
        for (u32 y = 0U; y < height; ++y) {
            for (u32 x = 0U; x < width; ++x) {
                const i64 sx = static_cast<i64>(x);
                const i64 sy = static_cast<i64>(y);
                const std::size_t index = index_of(width, x, y);
                const double av = a[index];
                const double bv = b[index];
                const double lap_a =
                    -av +
                    0.20 * (sample(a, sx - 1, sy) + sample(a, sx + 1, sy) +
                            sample(a, sx, sy - 1) + sample(a, sx, sy + 1)) +
                    0.05 * (sample(a, sx - 1, sy - 1) + sample(a, sx + 1, sy - 1) +
                            sample(a, sx - 1, sy + 1) + sample(a, sx + 1, sy + 1));
                const double lap_b =
                    -bv +
                    0.20 * (sample(b, sx - 1, sy) + sample(b, sx + 1, sy) +
                            sample(b, sx, sy - 1) + sample(b, sx, sy + 1)) +
                    0.05 * (sample(b, sx - 1, sy - 1) + sample(b, sx + 1, sy - 1) +
                            sample(b, sx - 1, sy + 1) + sample(b, sx + 1, sy + 1));
                const double reaction = av * bv * bv;
                next_a[index] = clamp01(av + (diffusion_a * lap_a - reaction + feed * (1.0 - av)) * dt);
                next_b[index] = clamp01(bv + (diffusion_b * lap_b + reaction - (kill + feed) * bv) * dt);
            }
        }
        a.swap(next_a);
        b.swap(next_b);
    }

    return core::Result<GrowthField, GrowthError>::success(GrowthField{width, height, std::move(b)});
}

[[nodiscard]] core::Result<GrowthField, GrowthError> evaluate_cellular_automaton(
    const core::Recipe& recipe,
    const core::NodeInstance& node,
    const u32 width,
    const u32 height) {
    const i64 tick = integer_parameter(node, "tick");
    auto work = checked_updates(width, height, tick, kMaxCellTickUpdates, "cellular automaton");
    if (work.is_error()) {
        return core::Result<GrowthField, GrowthError>::failure(work.error());
    }

    const i64 states = integer_parameter(node, "states");
    const double initial_fill = real_parameter(node, "initial_fill");
    const i64 birth_min = integer_parameter(node, "birth_min");
    const i64 birth_max = (std::min)(i64{8}, birth_min + integer_parameter(node, "birth_span"));
    const i64 survive_min = integer_parameter(node, "survive_min");
    const i64 survive_max = (std::min)(i64{8}, survive_min + integer_parameter(node, "survive_span"));
    const bool moore = enum_parameter(node, "neighbourhood") == "moore";
    const bool repeat = enum_parameter(node, "boundary") == "repeat";
    const u64 seed = node_seed(recipe, node);
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const u8 maximum_state = static_cast<u8>(states - 1);
    std::vector<u8> current(count, 0U);
    std::vector<u8> next(count, 0U);

    for (u32 y = 0U; y < height; ++y) {
        for (u32 x = 0U; x < width; ++x) {
            if (unit_from_hash(hash_cell(seed, x, y, 0x4341494e495431ULL)) < initial_fill) {
                current[index_of(width, x, y)] = maximum_state;
            }
        }
    }

    for (i64 step = 0; step < tick; ++step) {
        for (u32 y = 0U; y < height; ++y) {
            for (u32 x = 0U; x < width; ++x) {
                int neighbours = 0;
                for (i64 oy = -1; oy <= 1; ++oy) {
                    for (i64 ox = -1; ox <= 1; ++ox) {
                        if ((ox == 0 && oy == 0) || (!moore && std::abs(ox) + std::abs(oy) != 1)) {
                            continue;
                        }
                        const u32 nx = resolve_coordinate(static_cast<i64>(x) + ox, width, repeat);
                        const u32 ny = resolve_coordinate(static_cast<i64>(y) + oy, height, repeat);
                        if (current[index_of(width, nx, ny)] != 0U) {
                            ++neighbours;
                        }
                    }
                }
                const std::size_t index = index_of(width, x, y);
                const u8 state = current[index];
                if (state == 0U) {
                    next[index] = neighbours >= birth_min && neighbours <= birth_max ? maximum_state : 0U;
                } else if (neighbours >= survive_min && neighbours <= survive_max) {
                    next[index] = maximum_state;
                } else {
                    next[index] = static_cast<u8>(state - 1U);
                }
            }
        }
        current.swap(next);
    }

    GrowthField result{width, height, {}};
    result.values.resize(count);
    for (std::size_t index = 0U; index < count; ++index) {
        result.values[index] = static_cast<double>(current[index]) / static_cast<double>(maximum_state);
    }
    return core::Result<GrowthField, GrowthError>::success(std::move(result));
}

[[nodiscard]] core::Result<GrowthField, GrowthError> evaluate_walkers(
    const core::Recipe& recipe,
    const core::NodeInstance& node,
    const u32 width,
    const u32 height) {
    const i64 tick = integer_parameter(node, "tick");
    const i64 walker_count = integer_parameter(node, "walkers");
    auto work = core::checked_multiply_u64(static_cast<u64>(tick), static_cast<u64>(walker_count));
    if (work.is_error() || work.value() > kMaxWalkerUpdates) {
        return core::Result<GrowthField, GrowthError>::failure(make_error(
            GrowthErrorCode::resource_limit,
            "walkers requested tick and population exceed the canonical work limit"));
    }

    const double deposit = real_parameter(node, "deposit");
    const bool seeded_spawn = enum_parameter(node, "spawn") == "seeded";
    const bool repeat = enum_parameter(node, "boundary") == "repeat";
    const u64 seed = node_seed(recipe, node);
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    GrowthField result{width, height, std::vector<double>(count, 0.0)};
    std::vector<i64> xs(static_cast<std::size_t>(walker_count), static_cast<i64>(width / 2U));
    std::vector<i64> ys(static_cast<std::size_t>(walker_count), static_cast<i64>(height / 2U));

    if (seeded_spawn) {
        for (i64 walker = 0; walker < walker_count; ++walker) {
            const u64 h = core::derive_seed(seed, static_cast<u64>(walker));
            xs[static_cast<std::size_t>(walker)] = static_cast<i64>(h % width);
            ys[static_cast<std::size_t>(walker)] = static_cast<i64>(core::splitmix64(h) % height);
        }
    }

    constexpr std::array<int, 8> dx{-1, 0, 1, 1, 1, 0, -1, -1};
    constexpr std::array<int, 8> dy{-1, -1, -1, 0, 1, 1, 1, 0};
    for (i64 step = 0; step < tick; ++step) {
        for (i64 walker = 0; walker < walker_count; ++walker) {
            const std::size_t wi = static_cast<std::size_t>(walker);
            const u32 px = resolve_coordinate(xs[wi], width, repeat);
            const u32 py = resolve_coordinate(ys[wi], height, repeat);
            const std::size_t pixel = index_of(width, px, py);
            result.values[pixel] = clamp01(result.values[pixel] + deposit);

            const u64 step_key = core::splitmix64(
                static_cast<u64>(step) * 0x9e3779b97f4a7c15ULL ^ static_cast<u64>(walker));
            const u64 random = core::derive_seed(seed, step_key);
            const std::size_t direction = static_cast<std::size_t>(random & 7ULL);
            xs[wi] = static_cast<i64>(px) + dx[direction];
            ys[wi] = static_cast<i64>(py) + dy[direction];
            xs[wi] = static_cast<i64>(resolve_coordinate(xs[wi], width, repeat));
            ys[wi] = static_cast<i64>(resolve_coordinate(ys[wi], height, repeat));
        }
    }
    return core::Result<GrowthField, GrowthError>::success(std::move(result));
}

[[nodiscard]] core::Result<GrowthField, GrowthError> evaluate_branching(
    const core::Recipe& recipe,
    const core::NodeInstance& node,
    const u32 width,
    const u32 height) {
    const i64 tick = integer_parameter(node, "tick");
    const i64 initial_tips = integer_parameter(node, "initial_tips");
    const i64 max_tips = integer_parameter(node, "max_tips");
    auto work = core::checked_multiply_u64(static_cast<u64>(tick), static_cast<u64>(max_tips));
    if (work.is_error() || work.value() > kMaxBranchTipUpdates) {
        return core::Result<GrowthField, GrowthError>::failure(make_error(
            GrowthErrorCode::resource_limit,
            "branching requested tick and tip limit exceed the canonical work limit"));
    }

    const double branch_probability = real_parameter(node, "branch_probability");
    const double turn_probability = real_parameter(node, "turn_probability");
    const double deposit = real_parameter(node, "deposit");
    const bool repeat = enum_parameter(node, "boundary") == "repeat";
    const u64 seed = node_seed(recipe, node);
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    GrowthField result{width, height, std::vector<double>(count, 0.0)};
    std::vector<Tip> tips;
    tips.reserve(static_cast<std::size_t>(max_tips));
    u64 next_id = 0U;
    for (i64 index = 0; index < initial_tips; ++index) {
        const int direction = static_cast<int>((index * 8) / initial_tips) & 7;
        tips.push_back(Tip{next_id++, static_cast<i64>(width / 2U), static_cast<i64>(height / 2U), direction});
    }

    constexpr std::array<int, 8> dx{-1, 0, 1, 1, 1, 0, -1, -1};
    constexpr std::array<int, 8> dy{-1, -1, -1, 0, 1, 1, 1, 0};
    for (i64 step = 0; step < tick; ++step) {
        const std::size_t active_count = tips.size();
        std::vector<Tip> children;
        children.reserve((std::min)(active_count, static_cast<std::size_t>(max_tips) - active_count));
        for (std::size_t index = 0U; index < active_count; ++index) {
            Tip& tip = tips[index];
            const u32 px = resolve_coordinate(tip.x, width, repeat);
            const u32 py = resolve_coordinate(tip.y, height, repeat);
            const std::size_t pixel = index_of(width, px, py);
            result.values[pixel] = clamp01(result.values[pixel] + deposit);

            const u64 event_key = core::splitmix64(
                tip.id ^ (static_cast<u64>(step) * 0xd1b54a32d192ed03ULL));
            const u64 random = core::derive_seed(seed, event_key);
            if (unit_from_hash(random) < turn_probability) {
                tip.direction = (tip.direction + ((core::splitmix64(random) & 1ULL) == 0ULL ? 7 : 1)) & 7;
            }

            tip.x = static_cast<i64>(px) + dx[static_cast<std::size_t>(tip.direction)];
            tip.y = static_cast<i64>(py) + dy[static_cast<std::size_t>(tip.direction)];
            tip.x = static_cast<i64>(resolve_coordinate(tip.x, width, repeat));
            tip.y = static_cast<i64>(resolve_coordinate(tip.y, height, repeat));

            if (active_count + children.size() < static_cast<std::size_t>(max_tips) &&
                unit_from_hash(core::splitmix64(core::splitmix64(random))) < branch_probability) {
                const int branch_turn = (core::splitmix64(random ^ 0xa5a5a5a5a5a5a5a5ULL) & 1ULL) == 0ULL ? 2 : 6;
                children.push_back(Tip{next_id++, tip.x, tip.y, (tip.direction + branch_turn) & 7});
            }
        }
        tips.insert(tips.end(), children.begin(), children.end());
    }
    return core::Result<GrowthField, GrowthError>::success(std::move(result));
}

}  // namespace

bool is_growth_node(const std::string_view type_id) noexcept {
    return type_id == "core.growth.reaction_diffusion" ||
           type_id == "core.growth.cellular_automaton" ||
           type_id == "core.growth.walkers" ||
           type_id == "core.growth.branching";
}

core::Result<GrowthField, GrowthError> evaluate_growth_node(
    const core::Recipe& recipe,
    const core::NodeInstance& node,
    const u32 width,
    const u32 height) {
    try {
        if (node.type_id == "core.growth.reaction_diffusion") {
            return evaluate_reaction_diffusion(recipe, node, width, height);
        }
        if (node.type_id == "core.growth.cellular_automaton") {
            return evaluate_cellular_automaton(recipe, node, width, height);
        }
        if (node.type_id == "core.growth.walkers") {
            return evaluate_walkers(recipe, node, width, height);
        }
        if (node.type_id == "core.growth.branching") {
            return evaluate_branching(recipe, node, width, height);
        }
        return core::Result<GrowthField, GrowthError>::failure(make_error(
            GrowthErrorCode::unsupported_node,
            "growth evaluator does not support node type '" + node.type_id + "'"));
    } catch (const std::bad_alloc&) {
        return core::Result<GrowthField, GrowthError>::failure(make_error(
            GrowthErrorCode::resource_limit,
            "growth evaluator could not allocate its bounded working set"));
    }
}

}  // namespace artminer::nodes
