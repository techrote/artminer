#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/recipe.hpp"
#include "core/result.hpp"
#include "core/types.hpp"
#include "nodes/static_evaluator.hpp"

namespace artminer::nodes {

inline constexpr core::u64 kMaximumAnimationTick = 16384ULL;
inline constexpr core::u64 kMaximumFeedbackTick = 256ULL;

struct ParticleTrailPoint final {
    double x{0.0};
    double y{0.0};
};

struct Particle final {
    core::u64 id{0U};
    double x{0.0};
    double y{0.0};
    double velocity_x{0.0};
    double velocity_y{0.0};
    core::u32 age{0U};
    core::u32 lifetime{0U};
    std::vector<ParticleTrailPoint> trail;
};

struct ParticleSet final {
    // Canonical ordering is ascending stable particle id.
    std::vector<Particle> particles;
};

enum class MotionErrorCode {
    invalid_recipe,
    resource_limit,
    missing_output,
    unsupported_output_kind,
    unsupported_node,
    internal_graph_error,
};

struct MotionError final {
    MotionErrorCode code{MotionErrorCode::internal_graph_error};
    std::string message;
};

class FrameSnapshotCache final {
public:
    explicit FrameSnapshotCache(std::size_t capacity = 32U) noexcept;

    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    struct Entry final {
        std::string key;
        Image image;
    };

    std::size_t capacity_{32U};
    std::vector<Entry> entries_;

    friend core::Result<Image, MotionError> render_animation_reference(
        const core::Recipe&, core::u64, std::string_view, FrameSnapshotCache*);
};

// Reconstructs a ParticleSet from recipe initial state using exactly `tick`
// fixed updates. Spawn/update/death decisions are model-owned and keyed by
// stable particle id; worker order and display cadence are not inputs.
[[nodiscard]] core::Result<ParticleSet, MotionError> evaluate_particle_set(
    const core::Recipe& recipe,
    const core::NodeInstance& node,
    core::u64 tick);

// Canonical AM-007 CPU animation renderer. Stateful boundaries evaluate their
// `next` input at N-1 and expose an explicit initial value at tick zero.
[[nodiscard]] core::Result<Image, MotionError> render_animation_reference(
    const core::Recipe& recipe,
    core::u64 tick,
    std::string_view output_name = "main",
    FrameSnapshotCache* cache = nullptr);

}  // namespace artminer::nodes
