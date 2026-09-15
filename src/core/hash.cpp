#include "core/hash.hpp"

namespace artminer::core {

u64 fnv1a64(const std::span<const u8> bytes) noexcept {
    u64 hash = kFnv1a64Offset;
    for (const u8 byte : bytes) {
        hash ^= static_cast<u64>(byte);
        hash *= kFnv1a64Prime;
    }
    return hash;
}

u64 fnv1a64(const std::string_view text) noexcept {
    u64 hash = kFnv1a64Offset;
    for (const char character : text) {
        hash ^= static_cast<u64>(static_cast<unsigned char>(character));
        hash *= kFnv1a64Prime;
    }
    return hash;
}

std::string hex_u64(const u64 value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string output(16, '0');
    for (int index = 15; index >= 0; --index) {
        const auto nibble = static_cast<unsigned int>((value >> (static_cast<unsigned int>(index) * 4U)) & 0xFULL);
        output[static_cast<std::size_t>(15 - index)] = kDigits[nibble];
    }
    return output;
}

}  // namespace artminer::core
