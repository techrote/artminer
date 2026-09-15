#include <array>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string_view>

#include "core/checked_math.hpp"
#include "core/hash.hpp"
#include "core/prng.hpp"
#include "core/result.hpp"
#include "platform/windows/portable_workspace.hpp"

namespace {

int g_failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void test_splitmix64_vectors() {
    using artminer::core::splitmix64;

    expect(splitmix64(0x0000000000000000ULL) == 0xe220a8397b1dcdafULL, "SplitMix64 vector 0");
    expect(splitmix64(0x0000000000000001ULL) == 0x910a2dec89025cc1ULL, "SplitMix64 vector 1");
    expect(splitmix64(0x0123456789abcdefULL) == 0x157a3807a48faa9dULL, "SplitMix64 vector 2");
    expect(splitmix64(0xffffffffffffffffULL) == 0xe4d971771b652c20ULL, "SplitMix64 vector 3");
}

void test_seed_derivation_vectors() {
    using artminer::core::derive_seed;

    expect(derive_seed(0ULL, 0ULL) == 0xa706dd2f4d197e6fULL, "derive_seed vector 0");
    expect(derive_seed(1ULL, 2ULL) == 0xe06dd043328bd285ULL, "derive_seed vector 1");
    expect(
        derive_seed(0x0123456789abcdefULL, 0x0fedcba987654321ULL) == 0xab0d666b1c2a7065ULL,
        "derive_seed vector 2");
    expect(derive_seed(42ULL, 54ULL) == 0xbf411dba522b2d0cULL, "derive_seed vector 3");
}

void test_pcg32_vectors() {
    using artminer::core::Pcg32;

    constexpr std::array<artminer::core::u32, 10> expected{
        0xa15c02b7U,
        0x7b47f409U,
        0xba1d3330U,
        0x83d2f293U,
        0xbfa4784bU,
        0xcbed606eU,
        0xbfc6a3adU,
        0x812fff6dU,
        0xe61f305aU,
        0xf9384b90U,
    };

    Pcg32 generator(42ULL, 54ULL);
    for (std::size_t index = 0; index < expected.size(); ++index) {
        expect(generator.next_u32() == expected[index], "PCG32 reference vector");
    }

    Pcg32 repeated(42ULL, 54ULL);
    for (const auto expected_value : expected) {
        expect(repeated.next_u32() == expected_value, "PCG32 repeatability");
    }
}

void test_stable_hash_vectors() {
    using artminer::core::fnv1a64;
    using artminer::core::hex_u64;

    expect(fnv1a64(std::string_view{}) == 0xcbf29ce484222325ULL, "FNV-1a empty vector");
    expect(fnv1a64("a") == 0xaf63dc4c8601ec8cULL, "FNV-1a a vector");
    expect(fnv1a64("ArtMiner") == 0x1d19c8c4094e4d49ULL, "FNV-1a ArtMiner vector");
    expect(fnv1a64("deterministic") == 0x97f2ebf85d31152dULL, "FNV-1a deterministic vector");
    expect(hex_u64(0x0123456789abcdefULL) == "0123456789abcdef", "stable u64 hex formatting");
}

void test_checked_math() {
    using artminer::core::ArithmeticError;
    using artminer::core::checked_image_byte_count;
    using artminer::core::checked_multiply_u64;
    using artminer::core::u64;

    auto bytes = checked_image_byte_count(1920U, 1080U, 4U);
    expect(bytes.is_ok(), "valid image byte count succeeds");
    if (bytes.is_ok()) {
        expect(bytes.value() == 8294400ULL, "valid image byte count value");
    }

    auto invalid = checked_image_byte_count(0U, 1080U, 4U);
    expect(invalid.is_error(), "zero image dimension rejected");
    if (invalid.is_error()) {
        expect(invalid.error() == ArithmeticError::invalid_dimension, "zero dimension error type");
    }

    auto overflow = checked_multiply_u64((std::numeric_limits<u64>::max)(), 2ULL);
    expect(overflow.is_error(), "u64 multiplication overflow rejected");
    if (overflow.is_error()) {
        expect(overflow.error() == ArithmeticError::overflow, "overflow error type");
    }
}

void test_result_contract() {
    using artminer::core::Result;

    auto success = Result<int, int>::success(7);
    expect(success.is_ok(), "Result success state");
    expect(success.value() == 7, "Result success value");

    auto failure = Result<int, int>::failure(11);
    expect(failure.is_error(), "Result failure state");
    expect(failure.error() == 11, "Result failure value");

    auto void_success = Result<void, int>::success();
    expect(void_success.is_ok(), "void Result success state");
}

void test_portable_workspace() {
    using artminer::platform::windows::PortableWorkspace;

    const auto test_root = std::filesystem::temp_directory_path() / L"artminer-am001-workspace-test";
    std::error_code cleanup_error;
    std::filesystem::remove_all(test_root, cleanup_error);

    auto workspace_result = PortableWorkspace::open(test_root);
    expect(workspace_result.is_ok(), "portable workspace root resolves");
    if (workspace_result.is_error()) {
        return;
    }

    PortableWorkspace workspace = std::move(workspace_result).value();
    auto ensure_result = workspace.ensure_layout();
    expect(ensure_result.is_ok(), "portable workspace layout is writable");

    if (ensure_result.is_ok()) {
        const auto& layout = workspace.layout();
        expect(std::filesystem::is_directory(layout.root), "workspace root created");
        expect(std::filesystem::is_directory(layout.recipes), "recipes directory created");
        expect(std::filesystem::is_directory(layout.palettes), "palettes directory created");
        expect(std::filesystem::is_directory(layout.output), "output directory created");
        expect(std::filesystem::is_directory(layout.cache), "cache directory created");
    }

    std::filesystem::remove_all(test_root, cleanup_error);
}

}  // namespace

int main() {
    test_splitmix64_vectors();
    test_seed_derivation_vectors();
    test_pcg32_vectors();
    test_stable_hash_vectors();
    test_checked_math();
    test_result_contract();
    test_portable_workspace();

    if (g_failures != 0) {
        std::cerr << g_failures << " test assertion(s) failed.\n";
        return 1;
    }

    std::cout << "ArtMiner AM-001 tests passed.\n";
    return 0;
}
