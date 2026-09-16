#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "core/result.hpp"

namespace artminer::core {

inline constexpr std::size_t kMaximumRecipeFileBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kMaximumRecipeSourceLineBytes = 64U * 1024U;

struct LocalTextError final {
    std::string message;
};

[[nodiscard]] inline bool is_valid_utf8(const std::string_view text) noexcept {
    std::size_t index = 0U;
    while (index < text.size()) {
        const unsigned char first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7fU) {
            if (first == 0U) {
                return false;
            }
            ++index;
            continue;
        }

        std::size_t continuation_count = 0U;
        unsigned int code_point = 0U;
        if (first >= 0xc2U && first <= 0xdfU) {
            continuation_count = 1U;
            code_point = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            continuation_count = 2U;
            code_point = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            continuation_count = 3U;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (continuation_count > text.size() - index - 1U) {
            return false;
        }
        for (std::size_t offset = 1U; offset <= continuation_count; ++offset) {
            const unsigned char next = static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (next & 0x3fU);
        }

        if ((continuation_count == 2U && code_point < 0x800U) ||
            (continuation_count == 3U && code_point < 0x10000U) ||
            (code_point >= 0xd800U && code_point <= 0xdfffU) || code_point > 0x10ffffU) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

[[nodiscard]] inline Result<void, LocalTextError> validate_local_text(
    const std::string_view text,
    const std::size_t maximum_bytes = kMaximumRecipeFileBytes,
    const std::size_t maximum_line_bytes = kMaximumRecipeSourceLineBytes) {
    if (text.size() > maximum_bytes) {
        return Result<void, LocalTextError>::failure(LocalTextError{"local text exceeds the configured byte limit"});
    }
    if (!is_valid_utf8(text)) {
        return Result<void, LocalTextError>::failure(LocalTextError{"local text is not valid UTF-8 or contains NUL bytes"});
    }
    std::size_t line_bytes = 0U;
    for (const char byte : text) {
        if (byte == '\n') {
            line_bytes = 0U;
            continue;
        }
        ++line_bytes;
        if (line_bytes > maximum_line_bytes) {
            return Result<void, LocalTextError>::failure(LocalTextError{"local text contains a line that exceeds the configured byte limit"});
        }
    }
    return Result<void, LocalTextError>::success();
}

[[nodiscard]] inline Result<std::string, LocalTextError> read_local_text_file(
    const std::filesystem::path& path,
    const std::size_t maximum_bytes = kMaximumRecipeFileBytes,
    const std::size_t maximum_line_bytes = kMaximumRecipeSourceLineBytes) {
    std::error_code size_error;
    const std::uintmax_t file_size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        return Result<std::string, LocalTextError>::failure(LocalTextError{"could not stat local text file"});
    }
    if (file_size > maximum_bytes) {
        return Result<std::string, LocalTextError>::failure(LocalTextError{"local text file exceeds the configured byte limit"});
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Result<std::string, LocalTextError>::failure(LocalTextError{"could not open local text file"});
    }
    std::string text(static_cast<std::size_t>(file_size), '\0');
    if (!text.empty()) {
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (input.gcount() != static_cast<std::streamsize>(text.size())) {
            return Result<std::string, LocalTextError>::failure(LocalTextError{"local text file changed or was truncated while reading"});
        }
    }
    if (input.peek() != std::char_traits<char>::eof()) {
        return Result<std::string, LocalTextError>::failure(LocalTextError{"local text file grew while reading and exceeds the validated snapshot"});
    }
    auto validated = validate_local_text(text, maximum_bytes, maximum_line_bytes);
    if (validated.is_error()) {
        return Result<std::string, LocalTextError>::failure(validated.error());
    }
    return Result<std::string, LocalTextError>::success(std::move(text));
}

}  // namespace artminer::core
