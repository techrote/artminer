#include "nodes/growth_systems.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>
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

constexpr u64 kMaxGrowthTick = 100'000ULL;
constexpr u64 kMaxGridTickWork = 300'000'000ULL;
constexpr u64 kMaxAgentWork = 120'000'000ULL;
constexpr double kInvU53 = 1.0 / 9007199254740992.0;

[[nodiscard]] EvaluationError make_error(const EvaluationErrorCode code, std::string message) {
    return EvaluationError{code, std::move(message)};
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

[[nodiscard]] u64 cell_hash(const u64 seed, const u32 x, const u32 y, const u64 salt) noexcept {
    const u64 hx = core::splitmix64(static_cast<u64>(x) ^ 0x9e3779b97f4a7c15ULL);
    const u64 hy = core::splitmix64(static_cast<u64>(y) ^ 0xbf58476d1ce4e5b9ULL);
    return core::splitmix64(seed ^ hx ^ hy ^ salt);
}

[[nodiscard]] u64 event_hash(
    const u64 seed,
    const u64 id,
    const u64 tick,
    const u64 substep,
    const u64 salt) noexcept {
    u64 value = core::derive_seed(seed, id ^ salt);
    value = core::derive_seed(value, tick);
    return core::derive_seed(value, substep);
}

[[nodiscard]] double unit_from_hash(const u64 value) noexcept {
    return static_cast<double>(value >> 11U) * kInvU53;
}

[[nodiscard]] bool cancelled(const GrowthRenderRequest& request) noexcept {
    return request.cancel != nullptr && request.cancel->load(std::memory_order_relaxed);
}

[[nodiscard]] core::Result<void, EvaluationError> validate_tick_work(
    const GrowthRenderRequest& request,
    const u64 units_per_tick,
    const u64 limit,
    const std::string_view system_name) {
    if (request.tick > kMaxGrowthTick) {
        return core::Result<void, EvaluationError>::failure(make_error(
            EvaluationErrorCode::resource_limit,
            std::string(system_name) + " rejects requested ticks above 100000"));
    }
    auto work = core::checked_multiply_u64(units_per_tick, request.tick);
    if (work.is_error() || work.value() > limit) {
        return core::Result<void, EvaluationError>::failure(make_error(
            EvaluationErrorCode::resource_limit,
            std::string(system_name) + " requested tick/dimension workload exceeds the canonical safety limit"));
    }
    if (cancelled(request)) {
        return core::Result<void, EvaluationError>::failure(make_error(
            EvaluationErrorCode::cancelled,
            std::string(system_name) + " render cancelled before simulation"));
    }
    return core::Result<void, EvaluationError>::success();
}

struct Rgb8 final {
    u8 r{0U};
    u8 g{0U};
    u8 b{0U};
};

[[nodiscard]] u8 to_unorm8(const double value) noexcept {
    const double scaled = std::floor(std::clamp(value, 0.0, 1.0) * 255.0 + 0.5);
    return static_cast<u8>(scaled);
}

[[nodiscard]] Rgb8 palette_colour(const std::string& palette, const double value) noexcept {
    const double t = std::clamp(value, 0.0, 1.0);
    if (palette == "ember") {
        return {
            to_unorm8(std::clamp(t * 1.65, 0.0, 1.0)),
            to_unorm8(std::clamp((t - 0.22) * 1.25, 0.0, 1.0)),
            to_unorm8(std::clamp((t - 0.72) * 2.4, 0.0, 1.0)),
        };
    }
    if (palette == "cyan") {
        return {
            to_unorm8(t * 0.25),
            to_unorm8(t * 0.90),
            to_unorm8(std::clamp(0.12 + t, 0.0, 1.0)),
        };
    }
    if (palette == "forest") {
        return {
            to_unorm8(t * 0.38),
            to_unorm8(std::clamp(0.04 + t * 0.82, 0.0, 1.0)),
            to_unorm8(t * 0.30),
        };
    }
    const u8 channel = to_unorm8(t);
    return {channel, channel, channel};
}

template <typename Sampler>
[[nodiscard]] Image make_image(
    const u32 width,
    const u32 height,
    const std::string& palette,
    Sampler&& sampler) {
    Image image;
    image.width = width;
    image.height = height;
    image.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    for (u32 y = 0U; y < height; ++y) {
        for (u32 x = 0U; x < width; ++x) {
            const Rgb8 colour = palette_colour(palette, sampler(x, y));
            const std::size_t out = index_of(width, x, y) * 4U;
            image.rgba[out] = colour.r;
            image.rgba[out + 1U] = colour.g;
            image.rgba[out + 2U] = colour.b;
            image.rgba[out + 3U] = 255U;
        }
    }
    return image;
}

[[nodiscard]] u32 resolve_coordinate(const i64 value, const u32 extent, const bool wrap) noexcept {
    const i64 signed_extent = static_cast<i64>(extent);
    if (wrap) {
        const i64 normalized = ((value % signed_extent) + signed_extent) % signed_extent;
        return static_cast<u32>(normalized);
    }
    return static_cast<u32>(std::clamp<i64>(value, 0, signed_extent - 1));
}

[[nodiscard]] core::Result<Image, EvaluationError> render_reaction_diffusion(
    const core::Recipe& recipe,
    const core::NodeInstance& node,
    const GrowthRenderRequest& request) {
    const u32 width = recipe.render.width;
    const u32 height = recipe.render.height;
    const u64 pixels = static_cast<u64>(width) * static_cast<u64>(height);
    auto work_check = validate_tick_work(request, pixels, kMaxGridTickWork, "reaction-diffusion");
    if (work_check.is_error()) {
        return core::Result<Image, EvaluationError>::failure(work_check.error());
    }

    const double diff_a = real_parameter(node, "diff_a");
    const double diff_b = real_parameter(node, "diff_b");
    const double feed = real_parameter(node, "feed");
    const double kill = real_parameter(node, "kill");
    const double dt = real_parameter(node, "dt");
    const double seed_density = real_parameter(node, "seed_density");
    const bool wrap = enum_parameter(node, "boundary") == "wrap";
    const std::string& palette = enum_parameter(node, "palette");
    const u64 seed = node_seed(recipe, node);
    const std::size_t count = static_cast<std::size_t>(pixels);

    try {
        std::vector<double> a(count, 1.0);
        std::vector<double> b(count, 0.0);
        std::vector<double> next_a(count, 1.0);
        std::vector<double> next_b(count, 0.0);

        for (u32 y = 0U; y < height; ++y) {
            for (u32 x = 0U; x < width; ++x) {
                const double sample = unit_from_hash(cell_hash(seed, x, y, 0x52445052494d4553ULL));
                if (sample < seed_density) {
                    const std::size_t index = index_of(width, x, y);
                    a[index] = 0.15;
                    b[index] = 0.85;
                }
            }
        }

        for (u64 tick = 0U; tick < request.tick; ++tick) {
            if (cancelled(request)) {
                return core::Result<Image, EvaluationError>::failure(make_error(
                    EvaluationErrorCode::cancelled, "reaction-diffusion render cancelled"));
            }
            for (u32 y = 0U; y < height; ++y) {
                if ((y & 63U) == 0U && cancelled(request)) {
                    return core::Result<Image, EvaluationError>::failure(make_error(
                        EvaluationErrorCode::cancelled, "reaction-diffusion render cancelled"));
                }
                const u32 yn = resolve_coordinate(static_cast<i64>(y) - 1, height, wrap);
                const u32 ys = resolve_coordinate(static_cast<i64>(y) + 1, height, wrap);
                for (u32 x = 0U; x < width; ++x) {
                    const u32 xw = resolve_coordinate(static_cast<i64>(x) - 1, width, wrap);
                    const u32 xe = resolve_coordinate(static_cast<i64>(x) + 1, width, wrap);
                    const std::size_t index = index_of(width, x, y);
                    const double av = a[index];
                    const double bv = b[index];
                    const double lap_a =
                        a[index_of(width, xw, y)] + a[index_of(width, xe, y)] +
                        a[index_of(width, x, yn)] + a[index_of(width, x, ys)] - 4.0 * av;
                    const double lap_b =
                        b[index_of(width, xw, y)] + b[index_of(width, xe, y)] +
                        b[index_of(width, x, yn)] + b[index_of(width, x, ys)] - 4.0 * bv;
                    const double reaction = av * bv * bv;
                    next_a[index] = std::clamp(
                        av + (diff_a * lap_a - reaction + feed * (1.0 - av)) * dt,
                        0.0,
                        1.0);
                    next_b[index] = std::clamp(
                        bv + (diff_b * lap_b + reaction - (kill + feed) * bv) * dt,
                        0.0,
                        1.0);
                }
            }
            a.swap(next_a);
            b.swap(next_b);
        }

        return core::Result<Image, EvaluationError>::success(make_image(
            width,
            height,
            palette,
            [&](const u32 x, const u32 y) noexcept {
                return std::clamp(b[index_of(width, x, y)] * 1.45, 0.0, 1.0);
            }));
    } catch (const std::bad_alloc&) {
        return core::Result<Image, EvaluationError>::failure(make_error(
            EvaluationErrorCode::resource_limit,
            "reaction-diffusion could not allocate its bounded simulation state"));
    }
}

[[nodiscard]] core::Result<Image, EvaluationError> render_cellular_automaton(
    const core::Recipe& recipe,
    const core::NodeInstance& node,
    const GrowthRenderRequest& request) {
    const u32 width = recipe.render.width;
    const u32 height = recipe.render.height;
    const u64 pixels = static_cast<u64>(width) * static_cast<u64>(height);
    auto work_check = validate_tick_work(request, pixels, kMaxGridTickWork, "cellular-automaton");
    if (work_check.is_error()) {
        return core::Result<Image, EvaluationError>::failure(work_check.error());
    }

    const i64 states = integer_parameter(node, "states");
    const i64 birth_min = integer_parameter(node, "birth_min");
    const i64 birth_max = integer_parameter(node, "birth_max");
    const i64 survive_min = integer_parameter(node, "survive_min");
    const i64 survive_max = integer_parameter(node, "survive_max");
    if (birth_min > birth_max || survive_min > survive_max) {
        return core::Result<Image, EvaluationError>::failure(make_error(
            EvaluationErrorCode::invalid_recipe,
            "cellular-automaton requires min neighbour counts not to exceed max counts"));
    }
    const double initial_density = real_parameter(node, "initial_density");
    const bool wrap = enum_parameter(node, "boundary") == "wrap";
    const std::string& palette = enum_parameter(node, "palette");
    const u64 seed = node_seed(recipe, node);
    const std::size_t count = static_cast<std::size_t>(pixels);

    try {
        std::vector<u8> current(count, 0U);
        std::vector<u8> next(count, 0U);
        for (u32 y = 0U; y < height; ++y) {
            for (u32 x = 0U; x < width; ++x) {
                if (unit_from_hash(cell_hash(seed, x, y, 0x434155544f4d4154ULL)) < initial_density) {
                    current[index_of(width, x, y)] = 1U;
                }
            }
        }

        for (u64 tick = 0U; tick < request.tick; ++tick) {
            if (cancelled(request)) {
                return core::Result<Image, EvaluationError>::failure(make_error(
                    EvaluationErrorCode::cancelled, "cellular-automaton render cancelled"));
            }
            for (u32 y = 0U; y < height; ++y) {
                if ((y & 63U) == 0U && cancelled(request)) {
                    return core::Result<Image, EvaluationError>::failure(make_error(
                        EvaluationErrorCode::cancelled, "cellular-automaton render cancelled"));
                }
                for (u32 x = 0U; x < width; ++x) {
                    i64 live_neighbours = 0;
                    for (i64 oy = -1; oy <= 1; ++oy) {
                        for (i64 ox = -1; ox <= 1; ++ox) {
                            if (ox == 0 && oy == 0) {
                                continue;
                            }
                            const u32 nx = resolve_coordinate(static_cast<i64>(x) + ox, width, wrap);
                            const u32 ny = resolve_coordinate(static_cast<i64>(y) + oy, height, wrap);
                            if (current[index_of(width, nx, ny)] == 1U) {
                                ++live_neighbours;
                            }
                        }
                    }
                    const std::size_t index = index_of(width, x, y);
                    const u8 state = current[index];
                    if (state == 0U) {
                        next[index] = live_neighbours >= birth_min && live_neighbours <= birth_max ? 1U : 0U;
                    } else if (state == 1U) {
                        if (live_neighbours >= survive_min && live_neighbours <= survive_max) {
                            next[index] = 1U;
                        } else {
                            next[index] = states > 2 ? 2U : 0U;
                        }
                    } else {
                        const i64 advanced = static_cast<i64>(state) + 1;
                        next[index] = advanced < states ? static_cast<u8>(advanced) : 0U;
                    }
                }
            }
            current.swap(next);
        }

        return core::Result<Image, EvaluationError>::success(make_image(
            width,
            height,
            palette,
            [&](const u32 x, const u32 y) noexcept {
                const u8 state = current[index_of(width, x, y)];
                if (state == 0U) {
                    return 0.0;
                }
                if (state == 1U) {
                    return 1.0;
                }
                const double denominator = static_cast<double>((std::max)(states - 1, i64{1}));
                return std::clamp(1.0 - (static_cast<double>(state - 1U) / denominator), 0.08, 0.92);
            }));
    } catch (const std::bad_alloc&) {
        return core::Result<Image, EvaluationError>::failure(make_error(
            EvaluationErrorCode::resource_limit,
            "cellular-automaton could not allocate its bounded simulation state"));
    }
}

struct Walker final {
    u64 id{0U};
    i64 x{0};
    i64 y{0};
    int direction{0};
};

constexpr std::array<i64, 4> kDx{1, 0, -1, 0};
constexpr std::array<i64, 4> kDy{0, 1, 0, -1};

void step_walker(Walker& walker, const u32 width, const u32 height, const bool wrap) noexcept {
    i64 next_x = walker.x + kDx[static_cast<std::size_t>(walker.direction)];
    i64 next_y = walker.y + kDy[static_cast<std::size_t>(walker.direction)];
    if (wrap) {
        walker.x = static_cast<i64>(resolve_coordinate(next_x, width, true));
        walker.y = static_cast<i64>(resolve_coordinate(next_y, height, true));
        return;
    }
    const i64 max_x = static_cast<i64>(width) - 1;
    const i64 max_y = static_cast<i64>(height) - 1;
    if (next_x < 0 || next_x > max_x) {
        walker.direction = (2 - walker.direction + 4) % 4;
        next_x = walker.x + kDx[static_cast<std::size_t>(walker.direction)];
    }
    if (next_y < 0 || next_y > max_y) {
        walker.direction = (-walker.direction + 4) % 4;
        next_y = walker.y + kDy[static_cast<std::size_t>(walker.direction)];
    }
    walker.x = std::clamp<i64>(next_x, 0, max_x);
    walker.y = std::clamp<i64>(next_y, 0, max_y);
}

[[nodiscard]] core::Result<Image, EvaluationError> render_walkers(
    const core::Recipe& recipe,
    const core::NodeInstance& node,
    const GrowthRenderRequest& request) {
    const u32 width = recipe.render.width;
    const u32 height = recipe.render.height;
    const i64 walker_count_value = integer_parameter(node, "walkers");
    const i64 steps_value = integer_parameter(node, "steps_per_tick");
    const u64 walker_count = static_cast<u64>(walker_count_value);
    const u64 steps_per_tick = static_cast<u64>(steps_value);
    auto per_tick = core::checked_multiply_u64(walker_count, steps_per_tick);
    if (per_tick.is_error()) {
        return core::Result<Image, EvaluationError>::failure(make_error(
            EvaluationErrorCode::resource_limit, "walkers workload overflowed"));
    }
    auto work_check = validate_tick_work(request, per_tick.value(), kMaxAgentWork, "walkers");
    if (work_check.is_error()) {
        return core::Result<Image, EvaluationError>::failure(work_check.error());
    }

    const double turn_chance = real_parameter(node, "turn_chance");
    const double deposit = real_parameter(node, "deposit");
    const double decay = real_parameter(node, "decay");
    const bool wrap = enum_parameter(node, "boundary") == "wrap";
    const std::string& palette = enum_parameter(node, "palette");
    const u64 seed = node_seed(recipe, node);
    const std::size_t pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    if (decay > 0.0) {
        auto decay_work = core::checked_multiply_u64(static_cast<u64>(pixel_count), request.tick);
        if (decay_work.is_error() || decay_work.value() > kMaxGridTickWork) {
            return core::Result<Image, EvaluationError>::failure(make_error(
                EvaluationErrorCode::resource_limit,
                "walkers decay workload exceeds the canonical safety limit"));
        }
    }

    try {
        std::vector<double> field(pixel_count, 0.0);
        std::vector<Walker> walkers;
        walkers.reserve(static_cast<std::size_t>(walker_count));
        for (u64 id = 0U; id < walker_count; ++id) {
            const u64 hx = event_hash(seed, id, 0U, 0U, 0x57414c4b45525831ULL);
            const u64 hy = event_hash(seed, id, 0U, 0U, 0x57414c4b45525931ULL);
            const i64 x = static_cast<i64>(hx % static_cast<u64>(width));
            const i64 y = static_cast<i64>(hy % static_cast<u64>(height));
            const int direction = static_cast<int>(event_hash(seed, id, 0U, 0U, 0x57414c4b44495231ULL) & 3ULL);
            walkers.push_back(Walker{id, x, y, direction});
            field[index_of(width, static_cast<u32>(x), static_cast<u32>(y))] =
                std::min(1.0, field[index_of(width, static_cast<u32>(x), static_cast<u32>(y))] + deposit);
        }

        for (u64 tick = 0U; tick < request.tick; ++tick) {
            if (cancelled(request)) {
                return core::Result<Image, EvaluationError>::failure(make_error(
                    EvaluationErrorCode::cancelled, "walkers render cancelled"));
            }
            if (decay > 0.0) {
                const double retain = 1.0 - decay;
                for (double& value : field) {
                    value *= retain;
                }
            }
            for (Walker& walker : walkers) {
                for (u64 step = 0U; step < steps_per_tick; ++step) {
                    const u64 random = event_hash(seed, walker.id, tick, step, 0x57414c4b5455524eULL);
                    if (unit_from_hash(random) < turn_chance) {
                        const int delta = ((random >> 17U) & 1ULL) == 0ULL ? -1 : 1;
                        walker.direction = (walker.direction + delta + 4) % 4;
                    }
                    step_walker(walker, width, height, wrap);
                    const std::size_t index = index_of(
                        width,
                        static_cast<u32>(walker.x),
                        static_cast<u32>(walker.y));
                    field[index] = std::min(1.0, field[index] + deposit);
                }
            }
        }

        return core::Result<Image, EvaluationError>::success(make_image(
            width,
            height,
            palette,
            [&](const u32 x, const u32 y) noexcept { return field[index_of(width, x, y)]; }));
    } catch (const std::bad_alloc&) {
        return core::Result<Image, EvaluationError>::failure(make_error(
            EvaluationErrorCode::resource_limit,
            "walkers could not allocate their bounded simulation state"));
    }
}

struct BranchTip final {
    u64 id{0U};
    i64 x{0};
    i64 y{0};
    int direction{0};
    bool alive{true};
};

[[nodiscard]] core::Result<Image, EvaluationError> render_branching(
    const core::Recipe& recipe,
    const core::NodeInstance& node,
    const GrowthRenderRequest& request) {
    const u32 width = recipe.render.width;
    const u32 height = recipe.render.height;
    const i64 max_tips_value = integer_parameter(node, "max_tips");
    const u64 max_tips = static_cast<u64>(max_tips_value);
    auto work_check = validate_tick_work(request, max_tips, kMaxAgentWork, "branching-growth");
    if (work_check.is_error()) {
        return core::Result<Image, EvaluationError>::failure(work_check.error());
    }

    const i64 initial_branches = integer_parameter(node, "initial_branches");
    const double turn_chance = real_parameter(node, "turn_chance");
    const double branch_chance = real_parameter(node, "branch_chance");
    const bool wrap = enum_parameter(node, "boundary") == "wrap";
    const std::string& palette = enum_parameter(node, "palette");
    const u64 seed = node_seed(recipe, node);
    const std::size_t pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    try {
        std::vector<u8> occupied(pixel_count, 0U);
        const i64 center_x = static_cast<i64>(width / 2U);
        const i64 center_y = static_cast<i64>(height / 2U);
        occupied[index_of(width, static_cast<u32>(center_x), static_cast<u32>(center_y))] = 255U;

        std::vector<BranchTip> tips;
        tips.reserve(static_cast<std::size_t>(max_tips));
        const int phase = static_cast<int>(core::splitmix64(seed) & 3ULL);
        for (i64 branch = 0; branch < initial_branches; ++branch) {
            const int direction = (phase + static_cast<int>(branch % 4)) % 4;
            tips.push_back(BranchTip{static_cast<u64>(branch), center_x, center_y, direction, true});
        }
        u64 next_id = static_cast<u64>(initial_branches);

        for (u64 tick = 0U; tick < request.tick; ++tick) {
            if (cancelled(request)) {
                return core::Result<Image, EvaluationError>::failure(make_error(
                    EvaluationErrorCode::cancelled, "branching-growth render cancelled"));
            }
            std::vector<BranchTip> spawned;
            for (BranchTip& tip : tips) {
                if (!tip.alive) {
                    continue;
                }
                const u64 random = event_hash(seed, tip.id, tick, 0U, 0x4252414e43485431ULL);
                if (unit_from_hash(random) < turn_chance) {
                    const int delta = ((random >> 23U) & 1ULL) == 0ULL ? -1 : 1;
                    tip.direction = (tip.direction + delta + 4) % 4;
                }

                bool advanced = false;
                int chosen_direction = tip.direction;
                for (int attempt = 0; attempt < 4; ++attempt) {
                    const int direction = (tip.direction + attempt) % 4;
                    i64 nx = tip.x + kDx[static_cast<std::size_t>(direction)];
                    i64 ny = tip.y + kDy[static_cast<std::size_t>(direction)];
                    if (wrap) {
                        nx = static_cast<i64>(resolve_coordinate(nx, width, true));
                        ny = static_cast<i64>(resolve_coordinate(ny, height, true));
                    } else if (nx < 0 || ny < 0 || nx >= static_cast<i64>(width) || ny >= static_cast<i64>(height)) {
                        continue;
                    }
                    const std::size_t index = index_of(width, static_cast<u32>(nx), static_cast<u32>(ny));
                    if (occupied[index] != 0U) {
                        continue;
                    }
                    tip.x = nx;
                    tip.y = ny;
                    chosen_direction = direction;
                    occupied[index] = 255U;
                    advanced = true;
                    break;
                }
                if (!advanced) {
                    tip.alive = false;
                    continue;
                }
                tip.direction = chosen_direction;

                const u64 branch_random = event_hash(seed, tip.id, tick, 1U, 0x4252414e43484231ULL);
                const u64 projected_tip_count = static_cast<u64>(tips.size()) + static_cast<u64>(spawned.size());
                if (projected_tip_count < max_tips && unit_from_hash(branch_random) < branch_chance) {
                    const int delta = ((branch_random >> 29U) & 1ULL) == 0ULL ? -1 : 1;
                    const int direction = (tip.direction + delta + 4) % 4;
                    spawned.push_back(BranchTip{next_id++, tip.x, tip.y, direction, true});
                }
            }
            tips.insert(tips.end(), spawned.begin(), spawned.end());
        }

        return core::Result<Image, EvaluationError>::success(make_image(
            width,
            height,
            palette,
            [&](const u32 x, const u32 y) noexcept {
                return occupied[index_of(width, x, y)] == 0U ? 0.0 : 1.0;
            }));
    } catch (const std::bad_alloc&) {
        return core::Result<Image, EvaluationError>::failure(make_error(
            EvaluationErrorCode::resource_limit,
            "branching-growth could not allocate its bounded simulation state"));
    }
}

}  // namespace

bool is_growth_node(const std::string_view type_id) noexcept {
    return type_id == "core.growth.reaction_diffusion" ||
        type_id == "core.growth.cellular_automaton" ||
        type_id == "core.growth.walkers" ||
        type_id == "core.growth.branching";
}

core::Result<Image, EvaluationError> render_growth_node(
    const core::Recipe& recipe,
    const core::NodeInstance& node,
    const GrowthRenderRequest& request) {
    if (node.type_id == "core.growth.reaction_diffusion") {
        return render_reaction_diffusion(recipe, node, request);
    }
    if (node.type_id == "core.growth.cellular_automaton") {
        return render_cellular_automaton(recipe, node, request);
    }
    if (node.type_id == "core.growth.walkers") {
        return render_walkers(recipe, node, request);
    }
    if (node.type_id == "core.growth.branching") {
        return render_branching(recipe, node, request);
    }
    return core::Result<Image, EvaluationError>::failure(make_error(
        EvaluationErrorCode::unsupported_node,
        "growth evaluator does not implement node type '" + node.type_id + "'"));
}

}  // namespace artminer::nodes
