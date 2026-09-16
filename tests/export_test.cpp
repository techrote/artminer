#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "core/recipe.hpp"
#include "export/export.hpp"
#include "nodes/static_evaluator.hpp"

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

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        ++g_failures;
        std::cerr << "FAIL: could not read " << path.string() << '\n';
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

[[nodiscard]] std::string read_binary(const std::filesystem::path& path) {
    return read_text(path);
}

[[nodiscard]] artminer::core::Recipe parse_recipe_or_fail(
    const std::string_view text,
    const std::string_view name) {
    auto parsed = artminer::core::parse_recipe(text);
    if (parsed.is_error()) {
        ++g_failures;
        std::cerr << "FAIL: " << name << " parse failed: " << parsed.error().message << '\n';
        return {};
    }
    auto recipe = std::move(parsed).value();
    const auto errors = artminer::core::validate_recipe(recipe);
    if (!errors.empty()) {
        ++g_failures;
        std::cerr << "FAIL: " << name << " validation failed: " << errors.front().message << '\n';
    }
    return recipe;
}

[[nodiscard]] artminer::core::Recipe static_palette_recipe(const std::string_view preset = "mono") {
    std::string text = R"AMR(amr 1
evaluator 1
seed 7
render 2 2 reference
node source core.scalar.constant 1
param source value f64 0.5
node palette core.palette.default 1
param palette preset enum PRESET
node image core.image.from_scalar 1
param image palette enum grayscale
edge source value image source
output main image value
)AMR";
    const std::size_t marker = text.find("PRESET");
    text.replace(marker, 6U, preset);
    return parse_recipe_or_fail(text, "static palette recipe");
}

[[nodiscard]] artminer::core::Recipe load_recipe(const std::filesystem::path& path) {
    return parse_recipe_or_fail(read_text(path), path.string());
}

[[nodiscard]] std::filesystem::path test_root() {
    return std::filesystem::temp_directory_path() / L"artminer-am009-export-tests";
}

void reset_root() {
    std::error_code error;
    std::filesystem::remove_all(test_root(), error);
    std::filesystem::create_directories(test_root(), error);
    expect(!error, "test root can be recreated");
}

void test_sprite_sheet_layout() {
    using artminer::nodes::Image;
    Image red{1U, 1U, {255U, 0U, 0U, 255U}};
    Image green{1U, 1U, {0U, 255U, 0U, 255U}};
    Image blue{1U, 1U, {0U, 0U, 255U, 255U}};
    const std::vector<Image> frames{red, green, blue};
    auto sheet = artminer::exporting::compose_sprite_sheet(frames, 2U);
    expect(sheet.is_ok(), "sprite sheet composes");
    if (sheet.is_error()) {
        return;
    }
    expect(sheet.value().width == 2U && sheet.value().height == 2U, "sprite sheet dimensions are row-major grid extents");
    const std::vector<artminer::core::u8> expected{
        255U, 0U, 0U, 255U, 0U, 255U, 0U, 255U,
        0U, 0U, 255U, 255U, 0U, 0U, 0U, 0U,
    };
    expect(sheet.value().rgba == expected, "sprite sheet layout preserves deterministic input order and transparent unused cells");
}

void test_raw_export_round_trip_collision_and_manifest() {
    reset_root();
    const auto recipe = static_palette_recipe();
    const std::string fingerprint_before = artminer::core::semantic_fingerprint(recipe);
    artminer::exporting::ExportRequest request;
    request.kind = artminer::exporting::ExportKind::still;
    request.raster_format = artminer::exporting::RasterFormat::raw_rgba;
    request.destination_directory = test_root() / L"raw-a";
    request.stem = "sample";

    auto exported = artminer::exporting::export_recipe(recipe, request);
    expect(exported.is_ok(), "raw export succeeds");
    if (exported.is_error()) {
        return;
    }
    expect(
        artminer::core::semantic_fingerprint(recipe) == fingerprint_before,
        "export does not mutate recipe semantic fingerprint");

    auto reference = artminer::nodes::render_reference(recipe);
    expect(reference.is_ok(), "reference image renders for raw comparison");
    if (reference.is_ok()) {
        const std::string raw = read_binary(request.destination_directory / L"sample.rgba");
        const std::string expected(
            reinterpret_cast<const char*>(reference.value().rgba.data()),
            reference.value().rgba.size());
        expect(raw == expected, "raw RGBA bytes exactly match canonical top-to-bottom tight RGBA8 storage");
    }

    const std::string sidecar = read_text(request.destination_directory / L"sample.rgba.artminer.txt");
    expect(sidecar.find("raw-channel-order RGBA\n") != std::string::npos, "raw sidecar records channel order");
    expect(sidecar.find("raw-stride-bytes 8\n") != std::string::npos, "raw sidecar records byte stride");
    const std::string begin_marker = "recipe-begin\n";
    const std::string end_marker = "recipe-end\n";
    const std::size_t begin = sidecar.find(begin_marker);
    const std::size_t end = sidecar.find(end_marker);
    expect(begin != std::string::npos && end != std::string::npos && end > begin, "sidecar embeds canonical source recipe");
    if (begin != std::string::npos && end != std::string::npos && end > begin) {
        const std::size_t recipe_begin = begin + begin_marker.size();
        auto reparsed = artminer::core::parse_recipe(sidecar.substr(recipe_begin, end - recipe_begin));
        expect(reparsed.is_ok(), "embedded sidecar recipe parses");
        if (reparsed.is_ok()) {
            expect(
                artminer::core::semantic_fingerprint(reparsed.value()) == fingerprint_before,
                "sidecar recipe reconstructs exact semantic source identity");
        }
    }

    const std::string manifest_a = read_text(request.destination_directory / L"manifest.artminer.txt");
    expect(manifest_a.find("ArtMiner-Export-Manifest 1\n") == 0U, "manifest has explicit version");
    expect(manifest_a.find("fingerprint " + fingerprint_before + "\n") != std::string::npos, "manifest identifies source recipe");
    expect(manifest_a.find("raster-format raw-rgba\n") != std::string::npos, "manifest identifies raw format");
    expect(manifest_a.find("file \"sample.rgba\"") != std::string::npos, "manifest lists asset checksum record");

    const std::string preserved = read_binary(request.destination_directory / L"sample.rgba");
    auto collision = artminer::exporting::export_recipe(recipe, request);
    expect(collision.is_error(), "existing export destination is rejected rather than overwritten");
    expect(read_binary(request.destination_directory / L"sample.rgba") == preserved, "collision leaves existing export bytes untouched");

    artminer::exporting::ExportRequest second = request;
    second.destination_directory = test_root() / L"raw-b";
    auto exported_second = artminer::exporting::export_recipe(recipe, second);
    expect(exported_second.is_ok(), "same export can be produced at a second destination");
    if (exported_second.is_ok()) {
        const std::string manifest_b = read_text(second.destination_directory / L"manifest.artminer.txt");
        expect(manifest_a == manifest_b, "manifest content is stable and independent of destination path");
    }
}

void test_palette_and_cube_validation_and_cleanup() {
    reset_root();
    const auto mono = static_palette_recipe("mono");
    auto text = artminer::exporting::serialize_palette(mono, "palette", false);
    auto csv = artminer::exporting::serialize_palette(mono, "palette", true);
    auto cube = artminer::exporting::serialize_cube_lut(mono, "palette");
    expect(text.is_ok() && text.value().find("# columns: index r g b a") != std::string::npos, "plain palette export is explicit RGBA stop data");
    expect(csv.is_ok() && csv.value().find("index,r,g,b,a\n") == 0U, "CSV palette export has stable column contract");
    expect(cube.is_ok() && cube.value().find("LUT_1D_SIZE 2\n") != std::string::npos, "neutral opaque palette has faithful .cube representation");

    const auto warm = static_palette_recipe("warm");
    auto rejected = artminer::exporting::serialize_cube_lut(warm, "palette");
    expect(rejected.is_error(), "coloured scalar palette is rejected as an unsupported .cube interpretation");

    artminer::exporting::ExportRequest request;
    request.kind = artminer::exporting::ExportKind::cube_lut;
    request.destination_directory = test_root() / L"bad-cube";
    request.palette_node_id = "palette";
    request.stem = "warm";
    auto failed_export = artminer::exporting::export_recipe(warm, request);
    expect(failed_export.is_error(), "unsupported cube export fails through high-level subsystem");
    expect(!std::filesystem::exists(request.destination_directory), "failed export never publishes a final directory");
    expect(
        !std::filesystem::exists(artminer::exporting::staging_path_for_destination(request.destination_directory)),
        "failed export cleans its staging directory rather than leaving a false completed set");
}

void test_sequence_order_and_tick_provenance() {
    reset_root();
    const std::filesystem::path recipe_path =
        std::filesystem::path(ARTMINER_SOURCE_DIR) / L"examples" / L"am007-particle-flow.amr";
    const auto recipe = load_recipe(recipe_path);
    artminer::exporting::ExportRequest request;
    request.kind = artminer::exporting::ExportKind::sequence;
    request.raster_format = artminer::exporting::RasterFormat::raw_rgba;
    request.destination_directory = test_root() / L"sequence";
    request.start_tick = 0U;
    request.end_tick = 2U;
    request.stem = "flow";
    auto exported = artminer::exporting::export_recipe(recipe, request);
    expect(exported.is_ok(), "three-frame canonical sequence export succeeds");
    if (exported.is_error()) {
        return;
    }
    const std::vector<std::string> expected{
        "frames/tick-0000000000.rgba",
        "frames/tick-0000000000.rgba.artminer.txt",
        "frames/tick-0000000001.rgba",
        "frames/tick-0000000001.rgba.artminer.txt",
        "frames/tick-0000000002.rgba",
        "frames/tick-0000000002.rgba.artminer.txt",
    };
    std::vector<std::string> actual;
    for (const auto& file : exported.value().files) {
        actual.push_back(file.relative_path);
    }
    expect(actual == expected, "sequence file/checksum ordering is deterministic tick-major order");
    const std::string tick_two = read_text(
        request.destination_directory / L"frames" / L"tick-0000000002.rgba.artminer.txt");
    expect(tick_two.find("tick 2\n") != std::string::npos, "frame sidecar identifies exact observation tick");
    expect(tick_two.find("tick-range 0 2\n") != std::string::npos, "frame sidecar identifies selected sequence range");
}

void test_bmp_export_and_stale_staging_guard() {
    reset_root();
    const auto recipe = static_palette_recipe();
    artminer::exporting::ExportRequest request;
    request.kind = artminer::exporting::ExportKind::still;
    request.raster_format = artminer::exporting::RasterFormat::bmp;
    request.destination_directory = test_root() / L"bmp";
    request.stem = "opaque";
    auto exported = artminer::exporting::export_recipe(recipe, request);
    expect(exported.is_ok(), "opaque canonical image exports through WIC BMP");
    if (exported.is_ok()) {
        const std::string bmp = read_binary(request.destination_directory / L"opaque.bmp");
        expect(bmp.size() >= 2U && bmp[0] == 'B' && bmp[1] == 'M', "BMP export has Windows bitmap signature");
    }

    const auto stale_destination = test_root() / L"stale";
    const auto stale = artminer::exporting::staging_path_for_destination(stale_destination);
    std::error_code error;
    std::filesystem::create_directory(stale, error);
    artminer::exporting::ExportRequest stale_request;
    stale_request.kind = artminer::exporting::ExportKind::still;
    stale_request.raster_format = artminer::exporting::RasterFormat::raw_rgba;
    stale_request.destination_directory = stale_destination;
    stale_request.stem = "art";
    auto guarded = artminer::exporting::export_recipe(recipe, stale_request);
    expect(guarded.is_error(), "pre-existing staging residue is not silently treated as or overwritten by a complete export");
    expect(std::filesystem::exists(stale), "pre-existing staging residue is preserved for explicit review");
}

}  // namespace

int main() {
    test_sprite_sheet_layout();
    test_raw_export_round_trip_collision_and_manifest();
    test_palette_and_cube_validation_and_cleanup();
    test_sequence_order_and_tick_provenance();
    test_bmp_export_and_stale_staging_guard();

    std::error_code cleanup_error;
    std::filesystem::remove_all(test_root(), cleanup_error);
    if (g_failures != 0) {
        std::cerr << g_failures << " AM-009 export test(s) failed\n";
        return 1;
    }
    std::cout << "AM-009 export tests passed\n";
    return 0;
}
