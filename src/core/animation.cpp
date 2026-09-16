#include "core/animation.hpp"

#include <limits>

namespace artminer::core {

FixedTickPlayback::FixedTickPlayback(const u32 ticks_per_second) noexcept {
    if (!set_ticks_per_second(ticks_per_second)) {
        ticks_per_second_ = 8U;
    }
}

void FixedTickPlayback::set_playing(const bool playing) noexcept {
    playing_ = playing;
    if (!playing_) {
        accumulated_milliticks_ = 0U;
    }
}

bool FixedTickPlayback::set_ticks_per_second(const u32 ticks_per_second) noexcept {
    if (ticks_per_second < kPlaybackMinimumTicksPerSecond || ticks_per_second > kPlaybackMaximumTicksPerSecond) {
        return false;
    }
    ticks_per_second_ = ticks_per_second;
    return true;
}

void FixedTickPlayback::set_tick(const u64 tick) noexcept {
    tick_ = tick;
    accumulated_milliticks_ = 0U;
}

void FixedTickPlayback::reset() noexcept {
    tick_ = 0U;
    accumulated_milliticks_ = 0U;
    playing_ = false;
}

void FixedTickPlayback::step() noexcept {
    playing_ = false;
    accumulated_milliticks_ = 0U;
    if (tick_ != (std::numeric_limits<u64>::max)()) {
        ++tick_;
    }
}

u64 FixedTickPlayback::advance_wall_time(const u64 elapsed_milliseconds) noexcept {
    if (!playing_ || elapsed_milliseconds == 0U) {
        return 0U;
    }

    const u64 maximum = (std::numeric_limits<u64>::max)();
    const u64 rate = static_cast<u64>(ticks_per_second_);
    u64 added_milliticks = maximum;
    if (elapsed_milliseconds <= maximum / rate) {
        added_milliticks = elapsed_milliseconds * rate;
    }
    if (accumulated_milliticks_ > maximum - added_milliticks) {
        accumulated_milliticks_ = maximum;
    } else {
        accumulated_milliticks_ += added_milliticks;
    }

    const u64 requested_ticks = accumulated_milliticks_ / 1000U;
    accumulated_milliticks_ %= 1000U;
    if (requested_ticks == 0U) {
        return 0U;
    }

    const u64 available = maximum - tick_;
    const u64 advanced = requested_ticks > available ? available : requested_ticks;
    tick_ += advanced;
    if (advanced != requested_ticks) {
        accumulated_milliticks_ = 0U;
        playing_ = false;
    }
    return advanced;
}

}  // namespace artminer::core
