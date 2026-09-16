#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/checked_math.hpp"
#include "core/graph.hpp"
#include "core/hash.hpp"
#include "core/prng.hpp"
#include "core/recipe.hpp"
#include "core/result.hpp"
#include "export/windows/wic_png.hpp"
#include "nodes/static_evaluator.hpp"
#include "platform/windows/portable_workspace.hpp"

#ifndef ARTMINER_SOURCE_DIR
#define ARTMINER_SOURCE_DIR "."
#endif

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

[[nodiscard]] std::string_view constant_golden_text() {
    return R"AMR(amr 1
evaluator 1
seed 7
render 2 2 reference
node source core.scalar.constant 1
param source value f64 0.5
node image core.image.from_scalar 1
param image palette enum grayscale
edge source value image source
output main image value
)AMR";
}

[[nodiscard]] artminer::core::Recipe parse_or_fail(const std::string_view text, const std::string_view name) {
    auto parsed = artminer::core::parse_recipe(text);
    if (parsed.is_error()) {
        ++g_failures;
        std::cerr << "FAIL: " << name << " did not parse: " << parsed.error().message << '\n';
        return {};
    }
    return std::move(parsed).value();
}

[[nodiscard]] artminer::core::Recipe make_valid_recipe() {
    return parse_or_fail(valid_recipe_text(), "valid recipe fixture");
}

[[nodiscard]] std::string read_file_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        ++g_failures;
        std::cerr << "FAIL: could not open test file " << path.string() << '\n';
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
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
        0xa15c02b7U, 0x7b47f409U, 0xba1d3330U, 0x83d2f293U, 0xbfa4784bU,
        0xcbed606eU, 0xbfc6a3adU, 0x812fff6dU, 0xe61f305aU, 0xf9384b90U,
    };
    Pcg32 generator(42ULL, 54ULL);
    for (const auto expected_value : expected) {
        expect(generator.next_u32() == expected_value, "PCG32 reference vector");
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
    const auto test_root = std::filesystem::temp_directory_path() / L"artminer-am003-workspace-test";
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

    for (const std::string_view type_id : {
             "core.scalar.coord_x", "core.scalar.radial", "core.scalar.angular", "core.noise.value",
             "core.noise.gradient", "core.noise.worley", "core.noise.fbm", "core.scalar.warp",
             "core.sdf.circle", "core.sdf.box", "core.scalar.threshold", "core.scalar.quantize",
             "core.scalar.transform.repeat", "core.scalar.transform.symmetry", "core.palette.gradient2",
             "core.colour.from_palette", "core.image.ordered_dither"}) {
        const auto* metadata = registry.find(type_id);
        expect(metadata != nullptr, "AM-003 node is registered");
        if (metadata != nullptr) {
            expect(metadata->evaluators.cpu, "AM-003 node advertises implemented CPU evaluator");
            expect(!metadata->evaluators.gpu, "AM-003 does not advertise premature GPU support");
        }
    }
}

void test_recipe_round_trip_and_fingerprint() {
    using artminer::core::parse_recipe;
    using artminer::core::semantic_fingerprint;
    using artminer::core::serialize_recipe_canonical;
    using artminer::core::validate_recipe;
    artminer::core::Recipe recipe = make_valid_recipe();
    expect(validate_recipe(recipe).empty(), "reference recipe validates");
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
            artminer::core::serialize_recipe_canonical(first.value()) != artminer::core::serialize_recipe_canonical(second.value()),
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
        expect(has_validation_error(artminer::core::validate_recipe(recipe), ValidationErrorCode::duplicate_node_id), "duplicate node ids are rejected");
    }
    {
        auto recipe = make_valid_recipe();
        recipe.nodes.front().semantic_version = 99U;
        expect(has_validation_error(artminer::core::validate_recipe(recipe), ValidationErrorCode::unsupported_node_version), "unsupported node semantic versions are rejected");
    }
    {
        auto recipe = make_valid_recipe();
        recipe.nodes.front().parameters.front().value = artminer::core::i64{1};
        expect(has_validation_error(artminer::core::validate_recipe(recipe), ValidationErrorCode::parameter_type_mismatch), "parameter type mismatches are rejected");
    }
    {
        auto recipe = make_valid_recipe();
        recipe.nodes.front().parameters.front().value = 2000000.0;
        expect(has_validation_error(artminer::core::validate_recipe(recipe), ValidationErrorCode::parameter_out_of_domain), "parameter domain violations are rejected");
    }
    {
        auto recipe = make_valid_recipe();
        recipe.edges.front().from_node = "missing";
        expect(has_validation_error(artminer::core::validate_recipe(recipe), ValidationErrorCode::missing_node), "dangling edges are rejected");
    }
    {
        auto recipe = make_valid_recipe();
        recipe.edges.clear();
        expect(has_validation_error(artminer::core::validate_recipe(recipe), ValidationErrorCode::missing_required_input), "required inputs are enforced from node metadata");
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
        expect(has_validation_error(artminer::core::validate_recipe(recipe), ValidationErrorCode::incompatible_port_kind), "incompatible typed ports are rejected");
    }
    {
        auto recipe = make_valid_recipe();
        recipe.render.width = 0U;
        expect(has_validation_error(artminer::core::validate_recipe(recipe), ValidationErrorCode::invalid_render_settings), "invalid render dimensions are rejected");
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
        expect(has_validation_error(artminer::core::validate_recipe(parsed_cycle.value()), ValidationErrorCode::cycle_detected), "ordinary graph cycles are rejected");
    }
}

void test_reference_constant_golden() {
    auto recipe = parse_or_fail(constant_golden_text(), "constant golden recipe");
    expect(artminer::core::validate_recipe(recipe).empty(), "constant golden recipe validates");
    auto rendered = artminer::nodes::render_reference(recipe);
    expect(rendered.is_ok(), "constant golden recipe renders");
    if (rendered.is_error()) {
        return;
    }
    const auto& image = rendered.value();
    expect(image.width == 2U && image.height == 2U, "constant golden dimensions");
    constexpr std::array<artminer::core::u8, 16> expected{
        128U, 128U, 128U, 255U, 128U, 128U, 128U, 255U,
        128U, 128U, 128U, 255U, 128U, 128U, 128U, 255U,
    };
    expect(image.rgba.size() == expected.size(), "constant golden byte count");
    if (image.rgba.size() == expected.size()) {
        expect(std::equal(image.rgba.begin(), image.rgba.end(), expected.begin()), "constant golden exact RGBA8 bytes");
    }
    expect(artminer::nodes::image_fingerprint(image) == "962f2aa014c6f5c5", "constant golden image hash");
}

void test_reference_coordinate_golden() {
    constexpr std::string_view text = R"AMR(amr 1
evaluator 1
seed 0
render 4 1 reference
node x core.scalar.coord_x 1
node image core.image.from_scalar 1
param image palette enum grayscale
edge x value image source
output main image value
)AMR";
    auto recipe = parse_or_fail(text, "coordinate golden recipe");
    auto rendered = artminer::nodes::render_reference(recipe);
    expect(rendered.is_ok(), "coordinate golden recipe renders");
    if (rendered.is_error()) {
        return;
    }
    constexpr std::array<artminer::core::u8, 16> expected{
        32U, 32U, 32U, 255U,
        96U, 96U, 96U, 255U,
        159U, 159U, 159U, 255U,
        223U, 223U, 223U, 255U,
    };
    expect(std::equal(rendered.value().rgba.begin(), rendered.value().rgba.end(), expected.begin()), "pixel-centre coordinate golden bytes");
}

void test_seeded_noise_replay_and_seed_sensitivity() {
    constexpr std::string_view text = R"AMR(amr 1
evaluator 1
seed 12345
render 32 32 reference
node noise core.noise.gradient 1
param noise frequency f64 7.25
param noise offset_x f64 1.5
param noise offset_y f64 -2.75
node image core.image.from_scalar 1
param image palette enum heat
edge noise value image source
output main image value
)AMR";
    auto recipe = parse_or_fail(text, "seeded noise recipe");
    auto first = artminer::nodes::render_reference(recipe);
    auto second = artminer::nodes::render_reference(recipe);
    expect(first.is_ok() && second.is_ok(), "seeded noise renders repeatedly");
    if (first.is_ok() && second.is_ok()) {
        expect(first.value().rgba == second.value().rgba, "same seeded recipe is byte-identical on replay");
        const std::string first_hash = artminer::nodes::image_fingerprint(first.value());
        recipe.root_seed += 1U;
        auto changed = artminer::nodes::render_reference(recipe);
        expect(changed.is_ok(), "changed root seed still renders");
        if (changed.is_ok()) {
            expect(artminer::nodes::image_fingerprint(changed.value()) != first_hash, "root seed deterministically alters seeded noise output");
        }
    }
}

void test_reference_limits_and_unsupported_nodes() {
    {
        auto recipe = make_valid_recipe();
        recipe.render.width = 4096U;
        recipe.render.height = 4096U;
        expect(artminer::core::validate_recipe(recipe).empty(), "large but schema-valid raster validates before renderer limit");
        auto rendered = artminer::nodes::render_reference(recipe);
        expect(rendered.is_error(), "canonical renderer rejects excessive raster");
        if (rendered.is_error()) {
            expect(rendered.error().code == artminer::nodes::EvaluationErrorCode::resource_limit, "excessive raster reports resource limit");
        }
    }
    {
        constexpr std::string_view text = R"AMR(amr 1
evaluator 1
seed 1
render 8 8 reference
node source core.scalar.constant 1
param source value f64 0.5
node delayed core.state.delay.scalar 1
node image core.image.from_scalar 1
param image palette enum grayscale
edge source value delayed next
edge delayed previous image source
output main image value
)AMR";
        auto recipe = parse_or_fail(text, "unsupported canonical node recipe");
        expect(artminer::core::validate_recipe(recipe).empty(), "reserved state node recipe validates structurally");
        auto rendered = artminer::nodes::render_reference(recipe);
        expect(rendered.is_error(), "non-implemented state node fails explicitly");
        if (rendered.is_error()) {
            expect(rendered.error().code == artminer::nodes::EvaluationErrorCode::unsupported_node, "unsupported node has explicit evaluator error code");
        }
    }
}

void test_committed_static_examples() {
    const std::filesystem::path root(ARTMINER_SOURCE_DIR);
    const std::array<std::filesystem::path, 4> examples{
        root / "examples" / "am003-fbm-warp.amr",
        root / "examples" / "am003-worley.amr",
        root / "examples" / "am003-sdf.amr",
        root / "examples" / "am003-angular-repeat.amr",
    };
    std::set<std::string> hashes;
    for (const auto& path : examples) {
        const std::string text = read_file_text(path);
        if (text.empty()) {
            continue;
        }
        auto parsed = artminer::core::parse_recipe(text);
        expect(parsed.is_ok(), "committed AM-003 example parses");
        if (parsed.is_error()) {
            continue;
        }
        expect(artminer::core::validate_recipe(parsed.value()).empty(), "committed AM-003 example validates");
        auto rendered = artminer::nodes::render_reference(parsed.value());
        expect(rendered.is_ok(), "committed AM-003 example renders canonically");
        if (rendered.is_ok()) {
            hashes.insert(artminer::nodes::image_fingerprint(rendered.value()));
        }
    }
    expect(hashes.size() == examples.size(), "committed AM-003 examples produce materially distinct image hashes");
}

void test_png_and_provenance_export() {
    auto recipe = parse_or_fail(constant_golden_text(), "PNG export recipe");
    auto rendered = artminer::nodes::render_reference(recipe);
    expect(rendered.is_ok(), "PNG export source renders");
    if (rendered.is_error()) {
        return;
    }

    const auto root = std::filesystem::temp_directory_path() / L"artminer-am003-png-test";
    const auto png = root / L"golden.png";
    const auto sidecar = artminer::exporting::windows::provenance_sidecar_path(png);
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    expect(!error, "PNG test directory created");
    if (error) {
        return;
    }

    auto exported = artminer::exporting::windows::write_png_with_provenance(png, rendered.value(), recipe);
    expect(exported.is_ok(), "WIC PNG + provenance export succeeds");
    if (exported.is_ok()) {
        std::ifstream input(png, std::ios::binary);
        std::array<unsigned char, 8> signature{};
        input.read(reinterpret_cast<char*>(signature.data()), static_cast<std::streamsize>(signature.size()));
        constexpr std::array<unsigned char, 8> expected{137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
        expect(signature == expected, "exported file has PNG signature");

        const std::string provenance = read_file_text(sidecar);
        expect(provenance.find("ArtMiner-Provenance 1\n") == 0U, "provenance sidecar has versioned header");
        expect(provenance.find(artminer::core::semantic_fingerprint(recipe)) != std::string::npos, "provenance identifies semantic recipe fingerprint");
        expect(provenance.find(artminer::core::serialize_recipe_canonical(recipe)) != std::string::npos, "provenance carries complete canonical recipe");
    }
    std::filesystem::remove_all(root, error);
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
    test_reference_constant_golden();
    test_reference_coordinate_golden();
    test_seeded_noise_replay_and_seed_sensitivity();
    test_reference_limits_and_unsupported_nodes();
    test_committed_static_examples();
    test_png_and_provenance_export();

    if (g_failures != 0) {
        std::cerr << g_failures << " test assertion(s) failed.\n";
        return 1;
    }
    std::cout << "ArtMiner AM-003 tests passed.\n";
    return 0;
}
