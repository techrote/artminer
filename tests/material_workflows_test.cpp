#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#include "core/recipe.hpp"
#include "export/export.hpp"
#include "nodes/material_evaluator.hpp"
#include "nodes/static_evaluator.hpp"
#include "quarry/seam.hpp"

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

[[nodiscard]] artminer::core::Recipe parse_or_fail(
    const std::string_view text,
    const std::string_view label) {
    auto parsed = artminer::core::parse_recipe(text);
    if (parsed.is_error()) {
        ++g_failures;
        std::cerr << "FAIL: " << label << " parse: " << parsed.error().message << '\n';
        return {};
    }
    auto recipe = std::move(parsed).value();
    const auto errors = artminer::core::validate_recipe(recipe);
    if (!errors.empty()) {
        ++g_failures;
        std::cerr << "FAIL: " << label << " validation: " << errors.front().message << '\n';
    }
    return recipe;
}

[[nodiscard]] artminer::core::Recipe load_example(const wchar_t* name) {
    const auto path = std::filesystem::path(ARTMINER_SOURCE_DIR) / L"examples" / name;
    return parse_or_fail(read_text(path), path.string());
}

[[nodiscard]] std::filesystem::path test_root() {
    return std::filesystem::temp_directory_path() / L"artminer-am013-material-tests";
}

void reset_root() {
    std::error_code ignored;
    std::filesystem::remove_all(test_root(), ignored);
    std::filesystem::create_directories(test_root(), ignored);
    expect(!ignored, "material export test root can be recreated");
}

void test_seam_diagnostics_and_periodic_example() {
    const auto recipe = load_example(L"am013-seamless-texture.amr");
    auto image = artminer::nodes::render_workflow_reference(recipe);
    expect(image.is_ok(), "seamless example renders through workflow path");
    if (image.is_error()) {
        return;
    }
    auto seam = artminer::quarry::compute_tile_seam_diagnostics(image.value());
    expect(seam.is_ok(), "seam diagnostics evaluate");
    if (seam.is_ok()) {
        expect(seam.value().combined_error <= 1.0e-12, "xy symmetry example has exact combined edge identity");
        expect(seam.value().horizontal_error <= 1.0e-12, "xy symmetry example horizontal seam");
        expect(seam.value().vertical_error <= 1.0e-12, "xy symmetry example vertical seam");
    }
    auto inspection = artminer::quarry::make_tile_seam_inspection(image.value());
    expect(inspection.is_ok(), "2x2 seam inspection image evaluates");
    if (inspection.is_ok()) {
        expect(inspection.value().width == image.value().width * 2U, "inspection doubles width");
        expect(inspection.value().height == image.value().height * 2U, "inspection doubles height");
    }

    artminer::nodes::Image broken;
    broken.width = 2U;
    broken.height = 2U;
    broken.rgba = {
        0U, 0U, 0U, 255U, 255U, 255U, 255U, 255U,
        255U, 255U, 255U, 255U, 0U, 0U, 0U, 255U,
    };
    auto broken_seam = artminer::quarry::compute_tile_seam_diagnostics(broken);
    expect(broken_seam.is_ok() && broken_seam.value().combined_error == 1.0,
           "known broken checker fails authoritative AM-010 seam metric maximally");
}

void test_explicit_height_and_normal_reference() {
    constexpr std::string_view text = R"AMR(amr 1
evaluator 1
seed 1
render 3 3 reference
node x core.scalar.coord_x 1
node source core.image.from_scalar 1
param source palette enum grayscale
node height core.material.height_from_image 1
param height channel enum r
param height minimum f64 0
param height maximum f64 1
param height invert enum no
node normal core.material.normal_from_height 1
param normal filter enum central
param normal edge enum clamp
param normal strength f64 1
param normal texel_scale f64 1
param normal handedness enum right
param normal convention enum opengl
edge x value source source
edge source value height source
edge height height normal height
output main normal value
)AMR";
    const auto recipe = parse_or_fail(text, "normal reference recipe");
    auto first = artminer::nodes::render_workflow_reference(recipe);
    auto second = artminer::nodes::render_workflow_reference(recipe);
    expect(first.is_ok() && second.is_ok(), "explicit height normal recipe renders");
    if (first.is_error() || second.is_error()) {
        return;
    }
    expect(first.value().rgba == second.value().rgba, "height-to-normal output is reproducible");
    const std::size_t center = (1U * 3U + 1U) * 4U;
    const int r = first.value().rgba[center + 0U];
    const int g = first.value().rgba[center + 1U];
    const int b = first.value().rgba[center + 2U];
    expect(std::abs(r - 87) <= 2, "central-difference right-handed X slope reference red channel");
    expect(std::abs(g - 128) <= 1, "zero Y slope reference green channel");
    expect(std::abs(b - 248) <= 2, "normalized Z reference blue channel");

    constexpr std::string_view invalid_text = R"AMR(amr 1
evaluator 1
seed 1
render 3 3 reference
node scalar core.scalar.constant 1
param scalar value f64 0.5
node normal core.material.normal_from_height 1
param normal filter enum central
param normal edge enum clamp
param normal strength f64 1
param normal texel_scale f64 1
param normal handedness enum right
param normal convention enum opengl
edge scalar value normal height
output main normal value
)AMR";
    const auto invalid = parse_or_fail(invalid_text, "untyped height recipe");
    auto rejected = artminer::nodes::render_workflow_reference(invalid);
    expect(rejected.is_error(), "ordinary ScalarField cannot silently acquire physical height semantics");
    if (rejected.is_error()) {
        expect(rejected.error().message.find("height") != std::string::npos,
               "untyped height rejection is diagnostic rather than accidental evaluator failure");
    }
}

void test_packed_mapping_and_material_description() {
    const auto recipe = load_example(L"am013-packed-masks.amr");
    auto rendered = artminer::nodes::render_workflow_reference(recipe);
    expect(rendered.is_ok(), "packed mask example renders");
    auto description = artminer::nodes::describe_workflow_output(recipe);
    expect(description.is_ok(), "packed mask semantic description is available");
    if (description.is_ok()) {
        expect(description.value().semantic == "packed-mask", "packed output semantic is explicit");
        expect(description.value().mappings.size() == 4U, "packed output records all four channels");
        if (description.value().mappings.size() == 4U) {
            expect(description.value().mappings[0] == std::pair<std::string, std::string>{"r", "rmask.mask"},
                   "packed R mapping exact");
            expect(description.value().mappings[1] == std::pair<std::string, std::string>{"g", "gmask.mask"},
                   "packed G mapping exact");
            expect(description.value().mappings[2] == std::pair<std::string, std::string>{"b", "bmask.mask"},
                   "packed B mapping exact");
            expect(description.value().mappings[3] == std::pair<std::string, std::string>{"a", "amask.mask"},
                   "packed A mapping exact");
        }
    }
}

void test_loop_validation_and_false_loop_rejection() {
    const auto loop = load_example(L"am013-validated-loop.amr");
    auto valid = artminer::nodes::validate_workflow_loop(loop, 32U);
    expect(valid.is_ok() && valid.value().validated, "analytic 32-tick loop validates");
    if (valid.is_ok()) {
        expect(valid.value().endpoint_error == 0.0, "validated loop endpoint is exact");
        expect(valid.value().transition_error == 0.0, "validated loop boundary transition is exact");
        expect(valid.value().contains_stateful_nodes, "phase primitive is explicitly tick-stateful");
    }
    auto wrong_period = artminer::nodes::validate_workflow_loop(loop, 31U);
    expect(wrong_period.is_ok() && !wrong_period.value().validated,
           "wrong requested period is not falsely labelled a perfect loop");

    const auto particles = load_example(L"am007-particle-flow.amr");
    auto stateful = artminer::nodes::validate_workflow_loop(particles, 16U);
    expect(stateful.is_ok() && !stateful.value().validated,
           "accumulated particle state is not labelled a closed loop merely because a sequence can repeat");
    if (stateful.is_ok()) {
        expect(stateful.value().reason.find("state") != std::string::npos,
               "stateful non-loop rejection explains missing state-closure proof");
    }
}

void test_common_export_provenance() {
    reset_root();
    const auto packed = load_example(L"am013-packed-masks.amr");
    artminer::exporting::ExportRequest material_request;
    material_request.kind = artminer::exporting::ExportKind::still;
    material_request.raster_format = artminer::exporting::RasterFormat::raw_rgba;
    material_request.destination_directory = test_root() / L"packed";
    material_request.stem = "packed";
    material_request.output_name = "main";
    auto material = artminer::exporting::export_recipe(packed, material_request);
    expect(material.is_ok(), "material output uses common transactional exporter");
    if (material.is_ok()) {
        const std::string manifest = read_text(material_request.destination_directory / L"manifest.artminer.txt");
        expect(manifest.find("workflow-semantic packed-mask\n") != std::string::npos,
               "material manifest retains semantic role");
        expect(manifest.find("material-map \"r\" \"rmask.mask\"\n") != std::string::npos,
               "material manifest retains exact packed R mapping");
        expect(manifest.find("recipe-begin\n") != std::string::npos,
               "material export retains canonical recipe provenance");
    }

    const auto loop = load_example(L"am013-validated-loop.amr");
    artminer::exporting::ExportRequest loop_request;
    loop_request.kind = artminer::exporting::ExportKind::sequence;
    loop_request.raster_format = artminer::exporting::RasterFormat::raw_rgba;
    loop_request.destination_directory = test_root() / L"loop";
    loop_request.stem = "loop";
    loop_request.output_name = "main";
    loop_request.start_tick = 0U;
    loop_request.end_tick = 31U;
    loop_request.loop_length = 32U;
    loop_request.require_validated_loop = true;
    auto exported_loop = artminer::exporting::export_recipe(loop, loop_request);
    expect(exported_loop.is_ok(), "validated loop sequence uses common exporter");
    if (exported_loop.is_ok()) {
        const std::string manifest = read_text(loop_request.destination_directory / L"manifest.artminer.txt");
        expect(manifest.find("loop-length 32\n") != std::string::npos, "loop manifest retains selected period");
        expect(manifest.find("loop-validated yes\n") != std::string::npos, "loop manifest records successful proof");
        expect(manifest.find("tick-range 0 31\n") != std::string::npos, "loop sequence ordering/range remains AM-009 deterministic contract");
    }

    const auto particles = load_example(L"am007-particle-flow.amr");
    artminer::exporting::ExportRequest false_loop = loop_request;
    false_loop.destination_directory = test_root() / L"false-loop";
    false_loop.end_tick = 15U;
    false_loop.loop_length = 16U;
    auto rejected = artminer::exporting::export_recipe(particles, false_loop);
    expect(rejected.is_error(), "export requiring validated loop rejects unproved stateful repetition");
    expect(!std::filesystem::exists(false_loop.destination_directory),
           "rejected false-loop export never publishes a destination");
}

}  // namespace

int main() {
    test_seam_diagnostics_and_periodic_example();
    test_explicit_height_and_normal_reference();
    test_packed_mapping_and_material_description();
    test_loop_validation_and_false_loop_rejection();
    test_common_export_provenance();

    std::error_code ignored;
    std::filesystem::remove_all(test_root(), ignored);
    if (g_failures != 0) {
        std::cerr << g_failures << " AM-013 material workflow test(s) failed\n";
        return 1;
    }
    std::cout << "AM-013 material workflow tests passed\n";
    return 0;
}
