#include <array>
#include <filesystem>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <string_view>

#include "core/checked_math.hpp"
#include "core/graph.hpp"
#include "core/hash.hpp"
#include "core/prng.hpp"
#include "core/recipe.hpp"
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

[[nodiscard]] bool has_validation_error(
    const std::vector<artminer::core::ValidationError>& errors,
    const artminer::core::ValidationErrorCode code) {
    for (const auto& error : errors) {
        if (error.code == code) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::string_view valid_recipe_text() {
    return R"AMR(# ArtMiner AM-002 reference recipe
amr 1
evaluator 1
seed 42
render 64 64 "reference"
node "source" "core.scalar.constant" 1
param "source" "value" f64 0.5
node "img" "core.image.from_scalar" 1
param "img" "palette" enum "grayscale"
edge "source" "value" "img" "source"
output "main" "img" "value"
meta "example.note" "non-semantic metadata"
)AMR";
}

[[nodiscard]] artminer::core::Recipe make_valid_recipe() {
    auto parsed = artminer::core::parse_recipe(valid_recipe_text());
    if (parsed.is_error()) {
        ++g_failures;
        std::cerr << "FAIL: valid recipe fixture did not parse: " << parsed.error().message << '\n';
        return {};
    }
    return std::move(parsed).value();
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

    const auto test_root = std::filesystem::temp_directory_path() / L"artminer-am002-workspace-test";
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

void test_registry_metadata_contract() {
    using artminer::core::DataKind;

    const auto& registry = artminer::core::builtin_node_registry();
    std::set<DataKind> kinds;
    std::set<std::string> type_ids;
    for (const auto& node : registry.nodes()) {
        expect(type_ids.insert(node.type_id).second, "built-in node type identifiers are unique");
        expect(node.semantic_version > 0U, "built-in node semantic versions are explicit");
        for (const auto& port : node.inputs) {
            kinds.insert(port.kind);
        }
        for (const auto& port : node.outputs) {
            kinds.insert(port.kind);
        }
    }

    expect(kinds.contains(DataKind::scalar_field), "registry exposes ScalarField");
    expect(kinds.contains(DataKind::vector_field), "registry exposes VectorField");
    expect(kinds.contains(DataKind::colour_field), "registry exposes ColourField");
    expect(kinds.contains(DataKind::mask), "registry exposes Mask");
    expect(kinds.contains(DataKind::particle_set), "registry exposes ParticleSet");
    expect(kinds.contains(DataKind::palette), "registry exposes Palette");
    expect(kinds.contains(DataKind::image), "registry exposes Image");
}

void test_recipe_round_trip_and_fingerprint() {
    using artminer::core::parse_recipe;
    using artminer::core::semantic_fingerprint;
    using artminer::core::serialize_recipe_canonical;
    using artminer::core::validate_recipe;

    artminer::core::Recipe recipe = make_valid_recipe();
    const auto errors = validate_recipe(recipe);
    expect(errors.empty(), "reference recipe validates");

    const std::string canonical = serialize_recipe_canonical(recipe);
    auto reparsed = parse_recipe(canonical);
    expect(reparsed.is_ok(), "canonical recipe parses again");
    if (reparsed.is_ok()) {
        const artminer::core::Recipe round_trip = std::move(reparsed).value();
        expect(validate_recipe(round_trip).empty(), "round-tripped recipe validates");
        expect(serialize_recipe_canonical(round_trip) == canonical, "canonical serialization is idempotent");
        expect(
            semantic_fingerprint(round_trip) == "b9aed1288926e59c8c40115733cd32bf",
            "semantic fingerprint reference vector");
    }
}

void test_recipe_order_and_metadata_do_not_change_semantics() {
    constexpr std::string_view reordered = R"AMR(
# Same semantics, deliberately different order/spacing and metadata.
amr 1
 evaluator 1
seed 42
render 64 64 reference
node img core.image.from_scalar 1
node source core.scalar.constant 1
param img palette enum grayscale
param source value f64 0.5
output main img value
edge source value img source
meta ui.note "different note"
)AMR";

    auto first = artminer::core::parse_recipe(valid_recipe_text());
    auto second = artminer::core::parse_recipe(reordered);
    expect(first.is_ok() && second.is_ok(), "reordered equivalent recipe parses");
    if (first.is_ok() && second.is_ok()) {
        expect(artminer::core::validate_recipe(first.value()).empty(), "first equivalent recipe validates");
        expect(artminer::core::validate_recipe(second.value()).empty(), "second equivalent recipe validates");
        expect(
            artminer::core::semantic_fingerprint(first.value()) == artminer::core::semantic_fingerprint(second.value()),
            "semantic fingerprint ignores ordering, whitespace, comments and non-semantic metadata");
        expect(
            artminer::core::serialize_recipe_canonical(first.value()) !=
                artminer::core::serialize_recipe_canonical(second.value()),
            "canonical file serialization preserves differing metadata");
    }
}

void test_recipe_parse_failures() {
    auto malformed = artminer::core::parse_recipe(
        "amr 1\nevaluator 1\nseed 1\nrender 16 16 reference\nnode \"broken core.scalar.constant 1\n");
    expect(malformed.is_error(), "unterminated quote is rejected");
    if (malformed.is_error()) {
        expect(malformed.error().line == 5U, "malformed recipe reports source line");
    }

    auto unsupported = artminer::core::parse_recipe("amr 99\nevaluator 1\nseed 1\nrender 16 16 reference\n");
    expect(unsupported.is_error(), "unsupported schema is rejected");
    if (unsupported.is_error()) {
        expect(
            unsupported.error().code == artminer::core::RecipeErrorCode::unsupported_schema_version,
            "unsupported schema has specific error code");
    }

    auto unknown_record = artminer::core::parse_recipe(
        "amr 1\nevaluator 1\nseed 1\nrender 16 16 reference\nfuture-semantic surprise\n");
    expect(unknown_record.is_error(), "unknown semantic record is rejected rather than ignored");
}

void test_graph_validation_failures() {
    using artminer::core::ValidationErrorCode;

    {
        auto recipe = make_valid_recipe();
        recipe.nodes.push_back(recipe.nodes.front());
        expect(
            has_validation_error(artminer::core::validate_recipe(recipe), ValidationErrorCode::duplicate_node_id),
            "duplicate node ids are rejected");
    }
    {
        auto recipe = make_valid_recipe();
        recipe.nodes.front().semantic_version = 99U;
        expect(
            has_validation_error(artminer::core::validate_recipe(recipe), ValidationErrorCode::unsupported_node_version),
            "unsupported node semantic versions are rejected");
    }
    {
        auto recipe = make_valid_recipe();
        recipe.nodes.front().parameters.front().value = artminer::core::i64{1};
        expect(
            has_validation_error(artminer::core::validate_recipe(recipe), ValidationErrorCode::parameter_type_mismatch),
            "parameter type mismatches are rejected");
    }
    {
        auto recipe = make_valid_recipe();
        recipe.nodes.front().parameters.front().value = 2000000.0;
        expect(
            has_validation_error(artminer::core::validate_recipe(recipe), ValidationErrorCode::parameter_out_of_domain),
            "parameter domain violations are rejected");
    }
    {
        auto recipe = make_valid_recipe();
        recipe.edges.front().from_node = "missing";
        expect(
            has_validation_error(artminer::core::validate_recipe(recipe), ValidationErrorCode::missing_node),
            "dangling edges are rejected");
    }
    {
        auto recipe = make_valid_recipe();
        recipe.edges.clear();
        expect(
            has_validation_error(artminer::core::validate_recipe(recipe), ValidationErrorCode::missing_required_input),
            "required inputs are enforced from node metadata");
    }
    {
        auto recipe = make_valid_recipe();
        artminer::core::NodeInstance palette;
        palette.id = "palette";
        palette.type_id = "core.palette.default";
        palette.semantic_version = 1U;
        palette.parameters.push_back({"preset", std::string("mono")});
        recipe.nodes.push_back(std::move(palette));
        recipe.edges.front().from_node = "palette";
        expect(
            has_validation_error(artminer::core::validate_recipe(recipe), ValidationErrorCode::incompatible_port_kind),
            "incompatible typed ports are rejected");
    }

    constexpr std::string_view cyclic = R"AMR(amr 1
evaluator 1
seed 5
render 32 32 reference
node a core.scalar.pass 1
node b core.scalar.pass 1
edge a value b source
edge b value a source
output main a value
)AMR";
    auto parsed_cycle = artminer::core::parse_recipe(cyclic);
    expect(parsed_cycle.is_ok(), "cyclic fixture parses structurally");
    if (parsed_cycle.is_ok()) {
        expect(
            has_validation_error(artminer::core::validate_recipe(parsed_cycle.value()), ValidationErrorCode::cycle_detected),
            "ordinary graph cycles are rejected");
    }
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
    test_registry_metadata_contract();
    test_recipe_round_trip_and_fingerprint();
    test_recipe_order_and_metadata_do_not_change_semantics();
    test_recipe_parse_failures();
    test_graph_validation_failures();

    if (g_failures != 0) {
        std::cerr << g_failures << " test assertion(s) failed.\n";
        return 1;
    }

    std::cout << "ArtMiner AM-002 tests passed.\n";
    return 0;
}
