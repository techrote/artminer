#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/breeding.hpp"
#include "core/graph.hpp"
#include "core/recipe.hpp"
#include "core/specimen_browser.hpp"
#include "export/glyph_export.hpp"
#include "nodes/glyph_synthesis.hpp"

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

[[nodiscard]] artminer::core::NodeInstance glyph_settings_node() {
    using artminer::core::ParameterAssignment;
    artminer::core::NodeInstance node;
    node.id = "glyph";
    node.type_id = "core.glyph.settings";
    node.semantic_version = 1U;
    node.parameters = {
        ParameterAssignment{"cell_width", artminer::core::i64{4}},
        ParameterAssignment{"cell_height", artminer::core::i64{4}},
        ParameterAssignment{"glyph_set", std::string("lines")},
        ParameterAssignment{"choice_mode", std::string("balanced")},
        ParameterAssignment{"density_weight", 1.0},
        ParameterAssignment{"orientation_weight", 3.0},
        ParameterAssignment{"detail_weight", 0.5},
        ParameterAssignment{"corner_weight", 0.5},
        ParameterAssignment{"motion_weight", 0.0},
        ParameterAssignment{"colour_mode", std::string("ansi256")},
        ParameterAssignment{"limited_palette", std::string("cool8")},
        ParameterAssignment{"background_mode", std::string("black")},
        ParameterAssignment{"alpha_mode", std::string("composite-black")},
    };
    return node;
}

[[nodiscard]] artminer::core::Recipe static_glyph_recipe() {
    return parse_recipe_or_fail(R"AMR(amr 1
evaluator 1
seed 120012
render 16 8 reference
node coord core.scalar.coord_x 1
node image core.image.from_scalar 1
param image palette enum grayscale
node glyph core.glyph.settings 1
param glyph cell_width i64 4
param glyph cell_height i64 4
param glyph glyph_set enum lines
param glyph choice_mode enum balanced
param glyph density_weight f64 1
param glyph orientation_weight f64 3
param glyph detail_weight f64 0.5
param glyph corner_weight f64 0.5
param glyph motion_weight f64 0
param glyph colour_mode enum ansi256
param glyph limited_palette enum cool8
param glyph background_mode enum black
param glyph alpha_mode enum composite-black
edge coord value image source
output main image value
)AMR", "static glyph recipe");
}

[[nodiscard]] artminer::core::Recipe load_recipe(const std::filesystem::path& path) {
    return parse_recipe_or_fail(read_text(path), path.string());
}

[[nodiscard]] std::filesystem::path test_root() {
    return std::filesystem::temp_directory_path() / L"artminer-am012-glyph-tests";
}

void reset_root() {
    std::error_code error;
    std::filesystem::remove_all(test_root(), error);
    std::filesystem::create_directories(test_root(), error);
    expect(!error, "AM-012 test root can be recreated");
}

[[nodiscard]] artminer::nodes::Image split_image(const bool vertical) {
    artminer::nodes::Image image;
    image.width = 8U;
    image.height = 8U;
    image.rgba.resize(8U * 8U * 4U, 255U);
    for (artminer::core::u32 y = 0U; y < image.height; ++y) {
        for (artminer::core::u32 x = 0U; x < image.width; ++x) {
            const bool bright = vertical ? x >= 4U : y >= 4U;
            const artminer::core::u8 value = bright ? 255U : 0U;
            const std::size_t offset =
                (static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x)) * 4U;
            image.rgba[offset] = value;
            image.rgba[offset + 1U] = value;
            image.rgba[offset + 2U] = value;
            image.rgba[offset + 3U] = 255U;
        }
    }
    return image;
}

void test_descriptors_and_orientation_sensitive_choice() {
    using namespace artminer::nodes;
    const Image vertical = split_image(true);
    const Image horizontal = split_image(false);
    auto vertical_descriptor = extract_glyph_descriptors(vertical, 8U, 8U, GlyphAlphaMode::ignore);
    auto horizontal_descriptor = extract_glyph_descriptors(horizontal, 8U, 8U, GlyphAlphaMode::ignore);
    expect(vertical_descriptor.is_ok() && horizontal_descriptor.is_ok(), "equal-luminance edge descriptors extract");
    if (vertical_descriptor.is_error() || horizontal_descriptor.is_error()) {
        return;
    }
    const GlyphDescriptor& vd = vertical_descriptor.value().front();
    const GlyphDescriptor& hd = horizontal_descriptor.value().front();
    expect(std::abs(vd.luminance - hd.luminance) < 1.0e-12, "orthogonal fixtures have equal luminance");
    expect(vd.edge_strength > 0.05 && hd.edge_strength > 0.05, "orthogonal fixtures expose structural edge strength");
    expect(std::abs(vd.edge_orientation - hd.edge_orientation) > 1.0, "edge orientation descriptor distinguishes orthogonal structure");

    GlyphSettings settings;
    settings.cell_width = 8U;
    settings.cell_height = 8U;
    settings.glyph_set = GlyphSet::lines;
    settings.choice_mode = GlyphChoiceMode::structure;
    settings.density_weight = 1.0;
    settings.orientation_weight = 4.0;
    settings.detail_weight = 0.5;
    settings.corner_weight = 0.25;
    settings.motion_weight = 0.0;
    settings.colour_mode = GlyphColourMode::monochrome;
    settings.alpha_mode = GlyphAlphaMode::ignore;
    auto vertical_grid = synthesize_glyph_grid(vertical, settings);
    auto horizontal_grid = synthesize_glyph_grid(horizontal, settings);
    expect(vertical_grid.is_ok() && horizontal_grid.is_ok(), "orthogonal structural glyph grids synthesize");
    if (vertical_grid.is_ok() && horizontal_grid.is_ok()) {
        expect(vertical_grid.value().cells.front().codepoint == U'│', "vertical edge maps to vertical line glyph");
        expect(horizontal_grid.value().cells.front().codepoint == U'─', "horizontal edge maps to horizontal line glyph");
        expect(
            vertical_grid.value().cells.front().codepoint != horizontal_grid.value().cells.front().codepoint,
            "equal luminance but different orientation yields different glyph selection");
    }

    Image checker;
    checker.width = 8U;
    checker.height = 8U;
    checker.rgba.resize(8U * 8U * 4U, 255U);
    for (artminer::core::u32 y = 0U; y < checker.height; ++y) {
        for (artminer::core::u32 x = 0U; x < checker.width; ++x) {
            const artminer::core::u8 value = ((x + y) & 1U) == 0U ? 0U : 255U;
            const std::size_t offset =
                (static_cast<std::size_t>(y) * checker.width + static_cast<std::size_t>(x)) * 4U;
            checker.rgba[offset] = value;
            checker.rgba[offset + 1U] = value;
            checker.rgba[offset + 2U] = value;
        }
    }
    auto checker_descriptor = extract_glyph_descriptors(checker, 8U, 8U, GlyphAlphaMode::ignore);
    expect(
        checker_descriptor.is_ok() && checker_descriptor.value().front().detail > 0.9,
        "local high-frequency checker structure produces a high detail descriptor");
}

void test_motion_direction_descriptor_and_choice() {
    using namespace artminer::nodes;
    Image gray;
    gray.width = 8U;
    gray.height = 8U;
    gray.rgba.resize(8U * 8U * 4U, 255U);
    for (std::size_t offset = 0U; offset < gray.rgba.size(); offset += 4U) {
        gray.rgba[offset] = 128U;
        gray.rgba[offset + 1U] = 128U;
        gray.rgba[offset + 2U] = 128U;
    }
    std::vector<GlyphMotionVector> horizontal(64U, GlyphMotionVector{1.0, 0.0});
    std::vector<GlyphMotionVector> vertical(64U, GlyphMotionVector{0.0, 1.0});
    GlyphSettings settings;
    settings.cell_width = 8U;
    settings.cell_height = 8U;
    settings.glyph_set = GlyphSet::lines;
    settings.choice_mode = GlyphChoiceMode::balanced;
    settings.density_weight = 0.5;
    settings.orientation_weight = 0.0;
    settings.detail_weight = 0.0;
    settings.corner_weight = 0.0;
    settings.motion_weight = 8.0;
    settings.colour_mode = GlyphColourMode::monochrome;
    settings.alpha_mode = GlyphAlphaMode::ignore;

    auto h = synthesize_glyph_grid(gray, settings, horizontal);
    auto v = synthesize_glyph_grid(gray, settings, vertical);
    expect(h.is_ok() && v.is_ok(), "explicit motion-vector descriptor sources synthesize");
    if (h.is_ok() && v.is_ok()) {
        expect(h.value().cells.front().descriptor.motion_strength == 1.0, "motion strength is retained canonically");
        expect(h.value().cells.front().codepoint == U'─', "horizontal motion chooses horizontal line when motion weight dominates");
        expect(v.value().cells.front().codepoint == U'│', "vertical motion chooses vertical line when motion weight dominates");
    }
}

void test_colour_quantization_and_exact_text_bytes() {
    using namespace artminer::nodes;
    const TerminalColour ansi16 = quantize_terminal_colour({255U, 0U, 0U}, GlyphColourMode::ansi16, GlyphLimitedPalette::cool8);
    const TerminalColour ansi256 = quantize_terminal_colour({255U, 0U, 0U}, GlyphColourMode::ansi256, GlyphLimitedPalette::cool8);
    expect(ansi16.encoding == TerminalColourEncoding::ansi16 && ansi16.index == 9U, "ANSI16 pure red maps exactly to bright-red index 9");
    expect(ansi256.encoding == TerminalColourEncoding::ansi256 && ansi256.index == 9U, "ANSI256 tie policy chooses the lowest exact red index deterministically");

    GlyphGrid plain;
    plain.source_width = 2U;
    plain.source_height = 1U;
    plain.cell_width = 1U;
    plain.cell_height = 1U;
    plain.columns = 2U;
    plain.rows = 1U;
    plain.cells.resize(2U);
    plain.cells[0].codepoint = U'─';
    plain.cells[1].codepoint = U'│';
    auto utf8 = serialize_glyph_utf8(plain);
    expect(utf8.is_ok(), "UTF-8 glyph serialization succeeds");
    if (utf8.is_ok()) {
        expect(utf8.value() == std::string("\xe2\x94\x80\xe2\x94\x82\n", 7U), "UTF-8 glyph bytes and LF row terminator are exact");
    }

    GlyphGrid ansi;
    ansi.source_width = 1U;
    ansi.source_height = 1U;
    ansi.cell_width = 1U;
    ansi.cell_height = 1U;
    ansi.columns = 1U;
    ansi.rows = 1U;
    ansi.settings.background_mode = GlyphBackgroundMode::black;
    ansi.cells.resize(1U);
    ansi.cells[0].codepoint = U'@';
    ansi.cells[0].foreground = TerminalColour{TerminalColourEncoding::ansi16, 9U, {255U, 0U, 0U}};
    auto ansi_bytes = serialize_glyph_ansi(ansi);
    expect(ansi_bytes.is_ok(), "ANSI serialization succeeds");
    if (ansi_bytes.is_ok()) {
        expect(ansi_bytes.value() == "\x1b[91;40m@\x1b[0m\n", "ANSI SGR/reset bytes are exact and deterministic");
    }

    plain.cells[0].codepoint = U'\n';
    auto unsupported = serialize_glyph_utf8(plain);
    expect(unsupported.is_error(), "control characters are rejected as unsupported glyph codepoints");
}

void test_recipe_identity_mutation_and_breeding() {
    using namespace artminer;
    const core::Recipe parent = static_glyph_recipe();
    const core::NodeMetadata* metadata = core::builtin_node_registry().find("core.glyph.settings");
    expect(metadata != nullptr, "glyph settings node is registered in the authoritative node catalog");
    if (metadata != nullptr) {
        expect(metadata->parameters.size() == 13U, "glyph settings metadata owns all canonical mapping/colour parameters");
        expect(
            std::all_of(metadata->parameters.begin(), metadata->parameters.end(), [](const core::ParameterSpec& spec) {
                return spec.mutation.mutable_parameter;
            }),
            "glyph settings are first-class mutable recipe parameters");
    }

    core::Recipe changed = parent;
    auto glyph_node = std::find_if(changed.nodes.begin(), changed.nodes.end(), [](const core::NodeInstance& node) {
        return node.id == "glyph";
    });
    if (glyph_node != changed.nodes.end()) {
        auto parameter = std::find_if(glyph_node->parameters.begin(), glyph_node->parameters.end(), [](const core::ParameterAssignment& assignment) {
            return assignment.name == "orientation_weight";
        });
        if (parameter != glyph_node->parameters.end()) {
            parameter->value = 7.0;
        }
    }
    expect(
        core::semantic_fingerprint(changed) != core::semantic_fingerprint(parent),
        "changing glyph mapping settings changes semantic recipe fingerprint");

    core::ParameterLocks locks;
    locks.set_group("image", "palette", true);
    auto mutation_a = core::mutate_recipe_parameters(parent, 0x120012ULL, core::kParameterMutationOperatorVersion, 1.0, locks);
    auto mutation_b = core::mutate_recipe_parameters(parent, 0x120012ULL, core::kParameterMutationOperatorVersion, 1.0, locks);
    expect(mutation_a.is_ok() && mutation_b.is_ok(), "generic deterministic mutation accepts glyph settings metadata");
    if (mutation_a.is_ok() && mutation_b.is_ok()) {
        expect(
            core::semantic_fingerprint(mutation_a.value()) == core::semantic_fingerprint(mutation_b.value()),
            "glyph-setting mutation repeats exactly from parent/seed/operator/strength");
        expect(
            core::semantic_fingerprint(mutation_a.value()) != core::semantic_fingerprint(parent),
            "unlocked glyph settings participate in mutation rather than being UI-only state");
    }

    core::Recipe parent_b = parent;
    auto b_node = std::find_if(parent_b.nodes.begin(), parent_b.nodes.end(), [](const core::NodeInstance& node) {
        return node.id == "glyph";
    });
    if (b_node != parent_b.nodes.end()) {
        for (auto& assignment : b_node->parameters) {
            if (assignment.name == "glyph_set") {
                assignment.value = std::string("sparkle");
            } else if (assignment.name == "orientation_weight") {
                assignment.value = 6.0;
            }
        }
    }
    expect(core::validate_recipe(parent_b).empty(), "second glyph parent remains contract-valid");
    bool observed_parent_b_mapping = false;
    std::optional<core::u64> observed_seed;
    for (core::u64 seed = 0U; seed < 64U && !observed_parent_b_mapping; ++seed) {
        auto child = core::crossover_recipes(parent, parent_b, seed, core::kCrossoverOperatorVersion, {});
        expect(child.is_ok(), "compatible glyph recipes can cross without topology repair");
        if (child.is_error()) {
            break;
        }
        const auto child_node = std::find_if(child.value().nodes.begin(), child.value().nodes.end(), [](const core::NodeInstance& node) {
            return node.id == "glyph";
        });
        if (child_node != child.value().nodes.end()) {
            const auto parameter = std::find_if(child_node->parameters.begin(), child_node->parameters.end(), [](const core::ParameterAssignment& assignment) {
                return assignment.name == "glyph_set";
            });
            if (parameter != child_node->parameters.end() && std::get<std::string>(parameter->value) == "sparkle") {
                observed_parent_b_mapping = true;
                observed_seed = seed;
            }
        }
    }
    expect(observed_parent_b_mapping, "glyph mapping group can be inherited from ordered Parent B");
    if (observed_seed.has_value()) {
        auto first = core::crossover_recipes(parent, parent_b, *observed_seed, core::kCrossoverOperatorVersion, {});
        auto second = core::crossover_recipes(parent, parent_b, *observed_seed, core::kCrossoverOperatorVersion, {});
        expect(
            first.is_ok() && second.is_ok() &&
                core::semantic_fingerprint(first.value()) == core::semantic_fingerprint(second.value()),
            "glyph-setting crossover repeats exactly for the same ordered parents and seed");
    }
}

void test_animation_replay() {
    using namespace artminer;
    const std::filesystem::path source =
        std::filesystem::path(ARTMINER_SOURCE_DIR) / L"examples" / L"am007-particle-flow.amr";
    core::Recipe recipe = load_recipe(source);
    recipe.nodes.push_back(glyph_settings_node());
    expect(core::validate_recipe(recipe).empty(), "AM-007 recipe remains valid with disconnected semantic glyph settings");

    auto tick_four_a = nodes::synthesize_recipe_glyph_grid(recipe, "glyph", 4U);
    auto reset = nodes::synthesize_recipe_glyph_grid(recipe, "glyph", 0U);
    auto tick_four_b = nodes::synthesize_recipe_glyph_grid(recipe, "glyph", 4U);
    expect(tick_four_a.is_ok() && reset.is_ok() && tick_four_b.is_ok(), "fixed-tick glyph synthesis supports replay around tick zero reset");
    if (tick_four_a.is_ok() && tick_four_b.is_ok()) {
        auto bytes_a = nodes::serialize_glyph_ansi(tick_four_a.value());
        auto bytes_b = nodes::serialize_glyph_ansi(tick_four_b.value());
        expect(bytes_a.is_ok() && bytes_b.is_ok() && bytes_a.value() == bytes_b.value(), "animated ANSI cell bytes replay exactly at the same tick");
    }
}

void test_transactional_export_and_provenance() {
    using namespace artminer;
    reset_root();
    const core::Recipe recipe = static_glyph_recipe();
    const std::string fingerprint = core::semantic_fingerprint(recipe);
    exporting::GlyphExportRequest request;
    request.kind = exporting::GlyphExportKind::single;
    request.format = exporting::GlyphTextFormat::utf8;
    request.destination_directory = test_root() / L"plain-a";
    request.stem = "glyph";
    request.settings_node_id = "glyph";

    auto exported = exporting::export_glyph_recipe(recipe, request);
    expect(exported.is_ok(), "transactional plain UTF-8 glyph export succeeds");
    if (exported.is_error()) {
        return;
    }
    auto direct_grid = nodes::synthesize_recipe_glyph_grid(recipe, "glyph");
    auto direct_bytes = direct_grid.is_ok() ? nodes::serialize_glyph_utf8(direct_grid.value())
                                            : core::Result<std::string, nodes::GlyphError>::failure({"render failed"});
    expect(direct_bytes.is_ok(), "direct canonical glyph bytes can be regenerated");
    if (direct_bytes.is_ok()) {
        expect(read_text(request.destination_directory / L"glyph.txt") == direct_bytes.value(), "exported text bytes equal direct canonical serialization");
    }

    const std::string sidecar = read_text(request.destination_directory / L"glyph.txt.artminer.txt");
    expect(sidecar.find("ArtMiner-Provenance 2\n") == 0U, "glyph sidecar reuses AM-009 provenance envelope version");
    expect(sidecar.find("fingerprint " + fingerprint + "\n") != std::string::npos, "glyph sidecar identifies semantic recipe fingerprint");
    expect(sidecar.find("canonical-output glyph-cell-sequence\n") != std::string::npos, "glyph provenance distinguishes canonical cells from font pixels");
    expect(sidecar.find("font-rasterization noncanonical-preview-only\n") != std::string::npos, "glyph provenance records preview font as noncanonical");
    expect(sidecar.find("recipe-begin\n") != std::string::npos && sidecar.find("node \"glyph\" \"core.glyph.settings\" 1") != std::string::npos,
           "glyph provenance embeds canonical recipe including semantic glyph settings");

    const std::string manifest_a = read_text(request.destination_directory / L"manifest.artminer.txt");
    expect(manifest_a.find("ArtMiner-Export-Manifest 1\n") == 0U, "glyph export reuses versioned AM-009 manifest envelope");
    expect(manifest_a.find("kind glyph-utf8\n") != std::string::npos, "glyph manifest identifies canonical text format");
    expect(manifest_a.find("text-encoding UTF-8\n") != std::string::npos, "glyph manifest declares encoding");
    expect(manifest_a.find("line-endings LF\n") != std::string::npos, "glyph manifest declares LF line endings");

    const std::string preserved = read_text(request.destination_directory / L"glyph.txt");
    auto collision = exporting::export_glyph_recipe(recipe, request);
    expect(collision.is_error(), "glyph export refuses an existing completed destination");
    expect(read_text(request.destination_directory / L"glyph.txt") == preserved, "collision does not alter existing glyph bytes");

    exporting::GlyphExportRequest second = request;
    second.destination_directory = test_root() / L"plain-b";
    auto exported_second = exporting::export_glyph_recipe(recipe, second);
    expect(exported_second.is_ok(), "same glyph export succeeds at an independent destination");
    if (exported_second.is_ok()) {
        expect(
            read_text(second.destination_directory / L"manifest.artminer.txt") == manifest_a,
            "glyph manifest content is deterministic and independent of destination path");
    }
}

void test_sequence_order_and_tick_provenance() {
    using namespace artminer;
    reset_root();
    const std::filesystem::path source =
        std::filesystem::path(ARTMINER_SOURCE_DIR) / L"examples" / L"am007-particle-flow.amr";
    core::Recipe recipe = load_recipe(source);
    recipe.nodes.push_back(glyph_settings_node());
    exporting::GlyphExportRequest request;
    request.kind = exporting::GlyphExportKind::sequence;
    request.format = exporting::GlyphTextFormat::ansi;
    request.destination_directory = test_root() / L"sequence";
    request.settings_node_id = "glyph";
    request.start_tick = 0U;
    request.end_tick = 2U;
    auto exported = exporting::export_glyph_recipe(recipe, request);
    expect(exported.is_ok(), "fixed-tick ANSI glyph sequence exports");
    if (exported.is_error()) {
        return;
    }
    expect(std::filesystem::exists(request.destination_directory / L"frames" / L"tick-0000000000.ans"), "sequence includes tick 0 with stable zero-padded name");
    expect(std::filesystem::exists(request.destination_directory / L"frames" / L"tick-0000000001.ans"), "sequence includes tick 1 in deterministic order");
    expect(std::filesystem::exists(request.destination_directory / L"frames" / L"tick-0000000002.ans"), "sequence includes tick 2 in deterministic order");
    const std::string sidecar = read_text(
        request.destination_directory / L"frames" / L"tick-0000000001.ans.artminer.txt");
    expect(sidecar.find("tick 1\n") != std::string::npos, "per-frame ANSI provenance records the exact observation tick");
    expect(sidecar.find("tick-range 0 2\n") != std::string::npos, "per-frame ANSI provenance records the containing deterministic range");
    const std::string manifest = read_text(request.destination_directory / L"manifest.artminer.txt");
    expect(manifest.find("kind glyph-ansi-sequence\n") != std::string::npos, "sequence manifest identifies ANSI terminal output");
    expect(manifest.find("tick-range 0 2\n") != std::string::npos, "sequence manifest preserves inclusive tick range");
}

}  // namespace

int main() {
    test_descriptors_and_orientation_sensitive_choice();
    test_motion_direction_descriptor_and_choice();
    test_colour_quantization_and_exact_text_bytes();
    test_recipe_identity_mutation_and_breeding();
    test_animation_replay();
    test_transactional_export_and_provenance();
    test_sequence_order_and_tick_provenance();

    if (g_failures != 0) {
        std::cerr << g_failures << " AM-012 glyph test(s) failed\n";
        return 1;
    }
    std::cout << "AM-012 glyph/ANSI tests passed\n";
    return 0;
}
