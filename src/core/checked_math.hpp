#pragma once

#include <limits>

#include "core/result.hpp"
#include "core/types.hpp"

namespace artminer::core {

enum class ArithmeticError {
    overflow,
    invalid_dimension,
};

[[nodiscard]] inline Result<u64, ArithmeticError> checked_multiply_u64(const u64 lhs, const u64 rhs) {
    if (lhs != 0 && rhs > (std::numeric_limits<u64>::max)() / lhs) {
        return Result<u64, ArithmeticError>::failure(ArithmeticError::overflow);
    }
    return Result<u64, ArithmeticError>::success(lhs * rhs);
}

[[nodiscard]] inline Result<u64, ArithmeticError> checked_image_byte_count(
    const u32 width,
    const u32 height,
    const u32 bytes_per_pixel) {
    if (width == 0 || height == 0 || bytes_per_pixel == 0) {
        return Result<u64, ArithmeticError>::failure(ArithmeticError::invalid_dimension);
    }

    const auto pixels = checked_multiply_u64(static_cast<u64>(width), static_cast<u64>(height));
    if (pixels.is_error()) {
        return pixels;
    }
    return checked_multiply_u64(pixels.value(), static_cast<u64>(bytes_per_pixel));
}

}  // namespace artminer::core
