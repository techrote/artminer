#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include "core/graph.hpp"
#include "core/local_text.hpp"
#include "core/recipe.hpp"
#include "core/specimen_browser.hpp"
#include "export/export.hpp"
#include "platform/windows/browser_store.hpp"

namespace {

int g_failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

[[nodiscard]] artminer::core::Recipe make_recipe() {
    constexpr std::string_view text = R"AMR(amr 1
evaluator 1
seed 42
render 32 32 reference
node source core.scalar.constant 1
param source value f64 0.5
node image core.image.from_scalar 1
param image palette enum grayscale
edge source value image source
output main image value
)AMR";
    auto parsed = artminer::core::parse_recipe(text);
    if (parsed.is_error()) {
        ++g_failures;
        std::cerr << "FAIL: release fixture did not parse\n";
        return {};
    }
    auto recipe = std::move(parsed).value();
    expect(artminer::core::validate_recipe(recipe).empty(), "release fixture validates");
    return recipe;
}

[[nodiscard]] artminer::core::Recipe make_dependent_parameter_recipe() {
    constexpr std::string_view text = R"AMR(amr 1
evaluator 1
seed 99
render 32 32 reference
node source core.scalar.constant 1
param source value f64 0.5
node quant core.scalar.quantize 1
param quant levels i64 8
param quant minimum f64 0
param quant maximum f64 1
node image core.image.from_scalar 1
param image palette enum grayscale
edge source value quant source
edge quant value image source
output main image value
)AMR";
    auto parsed = artminer::core::parse_recipe(text);
    if (parsed.is_error()) {
        ++g_failures;
        std::cerr << "FAIL: dependent-parameter fixture did not parse\n";
        return {};
    }
    auto recipe = std::move(parsed).value();
    expect(artminer::core::validate_recipe(recipe).empty(), "dependent-parameter fixture validates");
    return recipe;
}

void test_utf8_and_text_limits() {
    using artminer::core::is_valid_utf8;
    using artminer::core::validate_local_text;

    expect(is_valid_utf8("ArtMiner \xe2\x9c\xa8"), "valid UTF-8 is accepted");
    expect(!is_valid_utf8(std::string_view("\xc0\xaf", 2U)), "overlong UTF-8 is rejected");
    expect(!is_valid_utf8(std::string_view("a\0b", 3U)), "embedded NUL is rejected");
    expect(validate_local_text("amr 1\n").is_ok(), "bounded local text accepts a normal recipe prefix");
    expect(validate_local_text(std::string(32U, 'x'), 64U, 16U).is_error(), "overlong source line is rejected");
    expect(validate_local_text(std::string(65U, 'x'), 64U, 64U).is_error(), "oversized source text is rejected");

    const auto invalid_utf8 = artminer::core::parse_recipe(std::string_view("\xc0\xaf", 2U));
    expect(invalid_utf8.is_error(), "recipe parser rejects invalid UTF-8 directly");
    const auto oversized = artminer::core::parse_recipe(
        std::string(artminer::core::kMaximumRecipeFileBytes + 1U, 'x'));
    expect(
        oversized.is_error() && oversized.error().code == artminer::core::RecipeErrorCode::resource_limit,
        "recipe parser reports an explicit resource limit for oversized source");
}

void test_graph_resource_limits() {
    auto recipe = make_recipe();
    recipe.nodes.resize(artminer::core::kMaximumRecipeNodes + 1U);
    const auto errors = artminer::core::validate_recipe(recipe);
    expect(
        !errors.empty() && errors.front().code == artminer::core::ValidationErrorCode::resource_limit,
        "programmatic oversized graph is rejected before graph-work allocations");
}

void test_bounded_file_read() {
    const auto root = std::filesystem::temp_directory_path() / L"artminer-am015-input-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root, ignored);

    const auto invalid_path = root / L"invalid-utf8.amr";
    {
        std::ofstream output(invalid_path, std::ios::binary);
        const char bytes[] = {static_cast<char>(0xc0), static_cast<char>(0xaf)};
        output.write(bytes, 2);
    }
    expect(artminer::core::read_local_text_file(invalid_path).is_error(), "invalid UTF-8 file is rejected");

    const auto large_path = root / L"too-large.amr";
    {
        std::ofstream output(large_path, std::ios::binary);
        std::string block(1024U, 'x');
        const std::size_t blocks = artminer::core::kMaximumRecipeFileBytes / block.size() + 1U;
        for (std::size_t index = 0U; index < blocks; ++index) {
            output.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
    }
    expect(artminer::core::read_local_text_file(large_path).is_error(), "oversized recipe file is rejected before parsing");
    std::filesystem::remove_all(root, ignored);
}

void test_session_recovery_round_trip_and_corruption() {
    const auto root = std::filesystem::temp_directory_path() / L"artminer-am015-recovery-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root, ignored);

    const auto recipe = make_recipe();
    const std::string fingerprint = artminer::core::semantic_fingerprint(recipe);
    auto saved = artminer::platform::windows::save_session_recovery(root, recipe);
    expect(saved.is_ok(), "session recovery snapshot saves atomically");
    auto loaded = artminer::platform::windows::load_session_recovery(root);
    expect(loaded.is_ok() && loaded.value().has_value(), "session recovery snapshot reloads");
    if (loaded.is_ok() && loaded.value().has_value()) {
        expect(loaded.value()->fingerprint == fingerprint, "recovery preserves semantic fingerprint");
    }

    const auto recovery_path = root / L"recovery" / L"current.amr";
    {
        std::ofstream output(recovery_path, std::ios::binary | std::ios::trunc);
        output << "not a recipe\n";
    }
    auto corrupt = artminer::platform::windows::load_session_recovery(root);
    expect(corrupt.is_error(), "corrupt recovery is reported instead of guessed");

    auto restored = artminer::platform::windows::save_session_recovery(root, recipe);
    expect(restored.is_ok(), "valid recovery can replace a corrupt snapshot atomically");
    std::filesystem::remove_all(root, ignored);
}

void test_export_path_hardening() {
    const auto root = std::filesystem::temp_directory_path() / L"artminer-am015-export-path-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    const auto recipe = make_recipe();
    artminer::exporting::ExportRequest request;
    request.kind = artminer::exporting::ExportKind::still;
    request.raster_format = artminer::exporting::RasterFormat::png;
    request.destination_directory = root / L"set";
    request.stem = "../escape";
    auto exported = artminer::exporting::export_recipe(recipe, request);
    expect(exported.is_error(), "export stem path traversal is rejected");
    expect(!std::filesystem::exists(root / L"escape.png"), "rejected export cannot escape its destination");

    std::filesystem::remove_all(root, ignored);
}

void test_dependent_parameter_mutation_is_validation_safe() {
    using namespace artminer::core;
    const Recipe parent = make_dependent_parameter_recipe();
    ParameterLocks locks;
    for (u64 seed = 0U; seed < 128U; ++seed) {
        auto first = mutate_recipe_parameters(
            parent, seed, kParameterMutationOperatorVersion, 0.25, locks);
        auto second = mutate_recipe_parameters(
            parent, seed, kParameterMutationOperatorVersion, 0.25, locks);
        expect(first.is_ok() && second.is_ok(), "bounded mutation retry produces a valid dependent-parameter child");
        if (first.is_error() || second.is_error()) {
            return;
        }
        expect(validate_recipe(first.value()).empty(), "retried mutation child passes normal recipe validation");
        expect(
            semantic_fingerprint(first.value()) == semantic_fingerprint(second.value()),
            "validation-safe mutation retry is deterministic for the same inputs");
    }
}

}  // namespace

int main() {
    test_utf8_and_text_limits();
    test_graph_resource_limits();
    test_bounded_file_read();
    test_session_recovery_round_trip_and_corruption();
    test_export_path_hardening();
    test_dependent_parameter_mutation_is_validation_safe();
    if (g_failures != 0) {
        std::cerr << g_failures << " release-hardening test(s) failed\n";
        return 1;
    }
    std::cout << "AM-015 release hardening tests passed\n";
    return 0;
}
