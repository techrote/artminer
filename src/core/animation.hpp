#pragma once

#include "core/types.hpp"

namespace artminer::core {

inline constexpr u32 kPlaybackMinimumTicksPerSecond = 1U;
inline constexpr u32 kPlaybackMaximumTicksPerSecond = 60U;

// Wall-clock playback is deliberately separate from simulation semantics. This
// object only maps elapsed UI time to an integer fixed tick. The authoritative
// animation state remains recipe + initial state + tick.
class FixedTickPlayback final {
public:
    explicit FixedTickPlayback(u32 ticks_per_second = 8U) noexcept;

    [[nodiscard]] u64 tick() const noexcept { return tick_; }
    [[nodiscard]] u32 ticks_per_second() const noexcept { return ticks_per_second_; }
    [[nodiscard]] bool playing() const noexcept { return playing_; }

    void set_playing(bool playing) noexcept;
    [[nodiscard]] bool set_ticks_per_second(u32 ticks_per_second) noexcept;
    void set_tick(u64 tick) noexcept;
    void reset() noexcept;
    void step() noexcept;

    // Returns the number of fixed ticks advanced. elapsed_milliseconds can be
    // irregular; only the resulting integer tick is semantically observable.
    [[nodiscard]] u64 advance_wall_time(u64 elapsed_milliseconds) noexcept;

private:
    u64 tick_{0U};
    u64 accumulated_milliticks_{0U};
    u32 ticks_per_second_{8U};
    bool playing_{false};
};

}  // namespace artminer::core
