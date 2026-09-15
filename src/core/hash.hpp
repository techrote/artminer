#pragma once

#include <span>
#include <string>
#include <string_view>

#include "core/types.hpp"

namespace artminer::core {

inline constexpr u64 kFnv1a64Offset = 0xcbf29ce484222325ULL;
inline constexpr u64 kFnv1a64Prime = 0x100000001b3ULL;

[[nodiscard]] u64 fnv1a64(std::span<const u8> bytes) noexcept;
[[nodiscard]] u64 fnv1a64(std::string_view text) noexcept;
[[nodiscard]] std::string hex_u64(u64 value);

}  // namespace artminer::core
