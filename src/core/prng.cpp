#include "core/prng.hpp"

#include <bit>

namespace artminer::core {

u64 splitmix64(u64 value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

u64 derive_seed(const u64 root_seed, const u64 domain) noexcept {
    return splitmix64(root_seed ^ splitmix64(domain));
}

Pcg32::Pcg32(const u64 seed, const u64 sequence) noexcept
    : state_(0), increment_((sequence << 1U) | 1U) {
    static_cast<void>(next_u32());
    state_ += seed;
    static_cast<void>(next_u32());
}

u32 Pcg32::next_u32() noexcept {
    const u64 old_state = state_;
    state_ = old_state * 6364136223846793005ULL + increment_;

    const auto xorshifted = static_cast<u32>(((old_state >> 18U) ^ old_state) >> 27U);
    const auto rotation = static_cast<int>(old_state >> 59U);
    return std::rotr(xorshifted, rotation);
}

}  // namespace artminer::core
