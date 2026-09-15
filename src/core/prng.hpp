#pragma once

#include "core/types.hpp"

namespace artminer::core {

// Stateless SplitMix64 transform. This exact function and its reference vectors
// are part of the deterministic seed-derivation contract from AM-001 onward.
[[nodiscard]] u64 splitmix64(u64 value) noexcept;

// Derive a purpose-local seed without consuming any shared/global RNG state.
[[nodiscard]] u64 derive_seed(u64 root_seed, u64 domain) noexcept;

class Pcg32 final {
public:
    Pcg32(u64 seed, u64 sequence) noexcept;

    [[nodiscard]] u32 next_u32() noexcept;
    [[nodiscard]] u64 state() const noexcept { return state_; }
    [[nodiscard]] u64 increment() const noexcept { return increment_; }

private:
    u64 state_{0};
    u64 increment_{1};
};

}  // namespace artminer::core
