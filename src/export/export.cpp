#include "export/export.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/checked_math.hpp"
#include "core/graph.hpp"
#include "core/hash.hpp"
#include "export/windows/wic_image.hpp"
#include "nodes/material_evaluator.hpp"
#include "nodes/motion_evaluator.hpp"

namespace artminer::exporting {
namespace {

struct PaletteColour final {
    double r{0.0};
    double g{0.0};
    double b{0.0};
    double a{1.0};
};

struct TickSelection final {
    std::optional<core::u64> single;
    std::optional<core::u64> first;
    std::optional<core::u64> last;
};

struct WorkflowProvenance final {
    nodes::MaterialOutputDescription output;
    std::optional<nodes::LoopValidation> loop;
};

[[nodiscard]] ExportError make_error(std::string message) {
    return ExportError{std::move(message)};
}

[[nodiscard]] const core::NodeInstance* find_node(
    const core::Recipe& recipe,
    const std::string_view node_id) noexcept {
    const auto found = std::find_if(
        recipe.nodes.begin(), recipe.nodes.end(),
        [node_id](const core::NodeInstance& node) { return node.id == node_id; });
    return found == recipe.nodes.end() ? nullptr : &*found;
}

[[nodiscard]] const core::ParameterAssignment* find_parameter(
    const core::NodeInstance& node,
    const std::string_view name) noexcept {
    const auto found = std::find_if(
        node.parameters.begin(), node.parameters.end(),
        [name](const core::ParameterAssignment& parameter) { return parameter.name == name; });
    return found == node.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] core::Result<std::vector<PaletteColour>, ExportError> extract_palette(
    const core::Recipe& recipe,
    const std::string_view node_id) {
    const auto errors = core::validate_recipe(recipe);
    if (!errors.empty()) {
        return core::Result<std::vector<PaletteColour>, ExportError>::failure(make_error(
            "palette export requires a valid recipe: " + errors.front().message));
    }
    const core::NodeInstance* node = find_node(recipe, node_id);
    if (node == nullptr) {
        return core::Result<std::vector<PaletteColour>, ExportError>::failure(make_error(
            "palette node '" + std::string(node_id) + "' was not found"));
    }
    if (node->type_id == "core.palette.default") {
        const auto* preset_parameter = find_parameter(*node, "preset");
        if (preset_parameter == nullptr || !std::holds_alternative<std::string>(preset_parameter->value)) {
            return core::Result<std::vector<PaletteColour>, ExportError>::failure(make_error(
                "palette preset parameter is missing or malformed"));
        }
        const std::string& preset = std::get<std::string>(preset_parameter->value);
        if (preset == "mono") {
            return core::Result<std::vector<PaletteColour>, ExportError>::success({
                {0.0, 0.0, 0.0, 1.0}, {1.0, 1.0, 1.0, 1.0}});
        }
        if (preset == "warm") {
            return core::Result<std::vector<PaletteColour>, ExportError>::success({
                {0.055, 0.016, 0.10, 1.0}, {0.45, 0.06, 0.08, 1.0},
                {0.90, 0.32, 0.08, 1.0}, {1.0, 0.82, 0.28, 1.0}});
        }
        if (preset == "cool") {
            return core::Result<std::vector<PaletteColour>, ExportError>::success({
                {0.01, 0.04, 0.12, 1.0}, {0.02, 0.22, 0.42, 1.0},
                {0.08, 0.62, 0.72, 1.0}, {0.72, 0.95, 0.98, 1.0}});
        }
        return core::Result<std::vector<PaletteColour>, ExportError>::failure(make_error(
            "unsupported palette preset '" + preset + "'"));
    }
    if (node->type_id == "core.palette.gradient2") {
        const auto value = [&](const std::string_view name) -> std::optional<double> {
            const auto* parameter = find_parameter(*node, name);
            if (parameter == nullptr || !std::holds_alternative<double>(parameter->value)) {
                return std::nullopt;
            }
            return std::get<double>(parameter->value);
        };
        const auto r0 = value("r0");
        const auto g0 = value("g0");
        const auto b0 = value("b0");
        const auto a0 = value("a0");
        const auto r1 = value("r1");
        const auto g1 = value("g1");
        const auto b1 = value("b1");
        const auto a1 = value("a1");
        if (!r0 || !g0 || !b0 || !a0 || !r1 || !g1 || !b1 || !a1) {
            return core::Result<std::vector<PaletteColour>, ExportError>::failure(make_error(
                "gradient palette parameters are missing or malformed"));
        }
        return core::Result<std::vector<PaletteColour>, ExportError>::success({
            {*r0, *g0, *b0, *a0}, {*r1, *g1, *b1, *a1}});
    }
    return core::Result<std::vector<PaletteColour>, ExportError>::failure(make_error(
        "node '" + std::string(node_id) + "' is not a palette node supported by the exporter"));
}

[[nodiscard]] std::ostringstream deterministic_stream() {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(17);
    return stream;
}

[[nodiscard]] bool safe_stem(const std::string_view stem) noexcept {
    if (stem.empty() || stem == "." || stem == "..") {
        return false;
    }
    return std::all_of(stem.begin(), stem.end(), [](const char value) {
        const unsigned char byte = static_cast<unsigned char>(value);
        return std::isalnum(byte) != 0 || value == '-' || value == '_' || value == '.';
    });
}

[[nodiscard]] std::string extension_for(const RasterFormat format) {
    switch (format) {
    case RasterFormat::png:
        return ".png";
    case RasterFormat::bmp:
        return ".bmp";
    case RasterFormat::raw_rgba:
        return ".rgba";
    }
    return {};
}

[[nodiscard]] core::Result<void, ExportError> write_text(
    const std::filesystem::path& path,
    const std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Result<void, ExportError>::failure(make_error(
            "could not create export file: " + path.string()));
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    if (!output) {
        return core::Result<void, ExportError>::failure(make_error(
            "failed while writing export file: " + path.string()));
    }
    return core::Result<void, ExportError>::success();
}

[[nodiscard]] core::Result<void, ExportError> write_bytes(
    const std::filesystem::path& path,
    const std::span<const core::u8> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Result<void, ExportError>::failure(make_error(
            "could not create export file: " + path.string()));
    }
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    output.flush();
    if (!output) {
        return core::Result<void, ExportError>::failure(make_error(
            "failed while writing export file: " + path.string()));
    }
    return core::Result<void, ExportError>::success();
}

[[nodiscard]] core::Result<ExportedFile, ExportError> describe_file(
    const std::filesystem::path& root,
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return core::Result<ExportedFile, ExportError>::failure(make_error(
            "could not reopen produced export file: " + path.string()));
    }
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (!input.good() && !input.eof()) {
        return core::Result<ExportedFile, ExportError>::failure(make_error(
            "failed while checksumming produced export file: " + path.string()));
    }
    const std::filesystem::path relative = path.lexically_relative(root);
    if (relative.empty()) {
        return core::Result<ExportedFile, ExportError>::failure(make_error(
            "could not derive an export-relative produced filename"));
    }
    return core::Result<ExportedFile, ExportError>::success(ExportedFile{
        relative.generic_string(), static_cast<core::u64>(bytes.size()),
        core::hex_u64(core::fnv1a64(std::string_view(bytes)))});
}

[[nodiscard]] core::Result<void, ExportError> append_description(
    const std::filesystem::path& root,
    const std::filesystem::path& path,
    std::vector<ExportedFile>& files) {
    auto description = describe_file(root, path);
    if (description.is_error()) {
        return core::Result<void, ExportError>::failure(description.error());
    }
    files.push_back(std::move(description).value());
    return core::Result<void, ExportError>::success();
}

[[nodiscard]] std::filesystem::path provenance_path(const std::filesystem::path& asset_path) {
    std::filesystem::path result = asset_path;
    result += L".artminer.txt";
    return result;
}

void append_workflow_provenance(
    std::ostringstream& stream,
    const std::string_view output_name,
    const WorkflowProvenance& workflow) {
    stream << "output-name " << std::quoted(std::string(output_name)) << '\n'
           << "workflow-semantic " << workflow.output.semantic << '\n';
    for (const auto& [channel, source] : workflow.output.mappings) {
        stream << "material-map " << std::quoted(channel) << ' ' << std::quoted(source) << '\n';
    }
    for (const auto& [key, value] : workflow.output.settings) {
        stream << "material-setting " << std::quoted(key) << ' ' << std::quoted(value) << '\n';
    }
    if (workflow.loop) {
        const auto& loop = *workflow.loop;
        stream << "loop-length " << loop.loop_length << '\n'
               << "loop-validated " << (loop.validated ? "yes" : "no") << '\n'
               << "loop-stateful " << (loop.contains_stateful_nodes ? "yes" : "no") << '\n'
               << "loop-endpoint-error " << loop.endpoint_error << '\n'
               << "loop-transition-error " << loop.transition_error << '\n'
               << "loop-tolerance " << loop.tolerance << '\n'
               << "loop-reason " << std::quoted(loop.reason) << '\n';
    }
}

[[nodiscard]] std::string provenance_text(
    const core::Recipe& recipe,
    const nodes::Image& image,
    const RasterFormat format,
    const TickSelection& ticks,
    const std::string_view output_name,
    const WorkflowProvenance& workflow) {
    auto stream = deterministic_stream();
    stream << "ArtMiner-Provenance 2\n"
           << "exporter " << kExporterSemanticVersion << '\n'
           << "fingerprint " << core::semantic_fingerprint(recipe) << '\n'
           << "schema " << recipe.schema_version << '\n'
           << "evaluator " << recipe.evaluator_version << '\n'
           << "width " << image.width << '\n'
           << "height " << image.height << '\n'
           << "source-pixel-layout RGBA8-row-major-tight\n"
           << "colour-space sRGB-encoded\n"
           << "alpha " << (format == RasterFormat::bmp ? "opaque-required" : "straight") << '\n'
           << "asset-format " << to_string(format) << '\n';
    append_workflow_provenance(stream, output_name, workflow);
    if (format == RasterFormat::raw_rgba) {
        stream << "raw-channel-order RGBA\n"
               << "raw-row-order top-to-bottom\n"
               << "raw-stride-bytes " << static_cast<core::u64>(image.width) * 4ULL << '\n';
    }
    if (ticks.single) {
        stream << "tick " << *ticks.single << '\n';
    }
    if (ticks.first && ticks.last) {
        stream << "tick-range " << *ticks.first << ' ' << *ticks.last << '\n';
    }
    stream << "recipe-begin\n" << core::serialize_recipe_canonical(recipe) << "recipe-end\n";
    return stream.str();
}

[[nodiscard]] core::Result<void, ExportError> write_raster_asset(
    const std::filesystem::path& root,
    const std::filesystem::path& path,
    const nodes::Image& image,
    const core::Recipe& recipe,
    const RasterFormat format,
    const TickSelection& ticks,
    const std::string_view output_name,
    const WorkflowProvenance& workflow,
    std::vector<ExportedFile>& files) {
    if (format == RasterFormat::raw_rgba) {
        auto written = write_bytes(path, image.rgba);
        if (written.is_error()) {
            return written;
        }
    } else {
        const auto wic_format = format == RasterFormat::bmp
            ? windows::WicImageFormat::bmp : windows::WicImageFormat::png;
        auto written = windows::write_wic_image(path, image, wic_format);
        if (written.is_error()) {
            return core::Result<void, ExportError>::failure(make_error(written.error().message));
        }
    }
    auto described = append_description(root, path, files);
    if (described.is_error()) {
        return described;
    }
    const std::filesystem::path sidecar = provenance_path(path);
    auto sidecar_written = write_text(
        sidecar, provenance_text(recipe, image, format, ticks, output_name, workflow));
    if (sidecar_written.is_error()) {
        return sidecar_written;
    }
    return append_description(root, sidecar, files);
}

[[nodiscard]] core::Result<nodes::Image, ExportError> render_still(
    const core::Recipe& recipe,
    const std::string_view output_name) {
    auto rendered = nodes::render_workflow_reference(recipe, output_name);
    if (rendered.is_error()) {
        return core::Result<nodes::Image, ExportError>::failure(make_error(
            "reference/workflow render failed: " + rendered.error().message));
    }
    return core::Result<nodes::Image, ExportError>::success(std::move(rendered).value());
}

[[nodiscard]] core::Result<nodes::Image, ExportError> render_tick(
    const core::Recipe& recipe,
    const core::u64 tick,
    const std::string_view output_name) {
    auto rendered = nodes::render_workflow_tick_reference(recipe, tick, output_name);
    if (rendered.is_error()) {
        return core::Result<nodes::Image, ExportError>::failure(make_error(
            "animation/workflow render failed at tick " + std::to_string(tick) + ": " + rendered.error().message));
    }
    return core::Result<nodes::Image, ExportError>::success(std::move(rendered).value());
}

[[nodiscard]] core::Result<WorkflowProvenance, ExportError> build_workflow_provenance(
    const core::Recipe& recipe,
    const ExportRequest& request) {
    WorkflowProvenance result;
    auto described = nodes::describe_workflow_output(recipe, request.output_name);
    if (described.is_error()) {
        return core::Result<WorkflowProvenance, ExportError>::failure(make_error(described.error().message));
    }
    result.output = std::move(described).value();
    if (request.loop_length) {
        auto validated = nodes::validate_workflow_loop(recipe, *request.loop_length, request.output_name);
        if (validated.is_error()) {
            return core::Result<WorkflowProvenance, ExportError>::failure(make_error(
                "loop validation failed: " + validated.error().message));
        }
        result.loop = std::move(validated).value();
        if (request.require_validated_loop && !result.loop->validated) {
            return core::Result<WorkflowProvenance, ExportError>::failure(make_error(
                "loop export was requested as validated, but continuity proof failed: " + result.loop->reason));
        }
    }
    return core::Result<WorkflowProvenance, ExportError>::success(std::move(result));
}

[[nodiscard]] std::string manifest_text(
    const core::Recipe& recipe,
    const ExportRequest& request,
    const core::u32 width,
    const core::u32 height,
    const WorkflowProvenance& workflow,
    const std::vector<ExportedFile>& files) {
    auto stream = deterministic_stream();
    stream << "ArtMiner-Export-Manifest 1\n"
           << "exporter " << kExporterSemanticVersion << '\n'
           << "fingerprint " << core::semantic_fingerprint(recipe) << '\n'
           << "schema " << recipe.schema_version << '\n'
           << "evaluator " << recipe.evaluator_version << '\n'
           << "kind " << to_string(request.kind) << '\n'
           << "width " << width << '\n'
           << "height " << height << '\n';
    append_workflow_provenance(stream, request.output_name, workflow);
    const bool raster = request.kind == ExportKind::still || request.kind == ExportKind::frame ||
        request.kind == ExportKind::sequence || request.kind == ExportKind::sprite_sheet;
    if (raster) {
        stream << "raster-format " << to_string(request.raster_format) << '\n'
               << "source-pixel-layout RGBA8-row-major-tight\n"
               << "colour-space sRGB-encoded\n"
               << "alpha " << (request.raster_format == RasterFormat::bmp ? "opaque-required" : "straight") << '\n';
    } else {
        stream << "raster-format none\ncolour-space sRGB-encoded\nalpha palette-defined\n";
    }
    if (request.kind == ExportKind::frame && request.tick) {
        stream << "tick " << *request.tick << '\n';
    }
    if (request.kind == ExportKind::sequence || request.kind == ExportKind::sprite_sheet) {
        stream << "tick-range " << request.start_tick << ' ' << request.end_tick << '\n';
    }
    if (request.kind == ExportKind::sprite_sheet) {
        stream << "sheet-columns " << request.sheet_columns << '\n';
    }
    if (request.kind == ExportKind::palette_text || request.kind == ExportKind::palette_csv ||
        request.kind == ExportKind::cube_lut) {
        stream << "palette-node " << std::quoted(request.palette_node_id) << '\n';
    }
    stream << "file-count " << files.size() << '\n';
    for (const auto& file : files) {
        stream << "file " << std::quoted(file.relative_path) << ' '
               << file.byte_count << ' ' << file.checksum_fnv1a64 << '\n';
    }
    stream << "recipe-begin\n" << core::serialize_recipe_canonical(recipe) << "recipe-end\n";
    return stream.str();
}

[[nodiscard]] core::Result<void, ExportError> validate_request(
    const core::Recipe& recipe,
    const ExportRequest& request) {
    const auto errors = core::validate_recipe(recipe);
    if (!errors.empty()) {
        return core::Result<void, ExportError>::failure(make_error(
            "export requires a valid recipe: " + errors.front().message));
    }
    if (request.destination_directory.empty()) {
        return core::Result<void, ExportError>::failure(make_error(
            "export destination directory must not be empty"));
    }
    if (!safe_stem(request.stem)) {
        return core::Result<void, ExportError>::failure(make_error(
            "export stem must contain only ASCII letters, digits, '.', '-' or '_' and must not be '.' or '..'"));
    }
    if (request.output_name.empty()) {
        return core::Result<void, ExportError>::failure(make_error("export output name must not be empty"));
    }
    if (request.require_validated_loop && !request.loop_length) {
        return core::Result<void, ExportError>::failure(make_error(
            "require_validated_loop requires an explicit loop_length"));
    }
    if (request.kind == ExportKind::frame) {
        if (!request.tick || *request.tick > nodes::kMaximumAnimationTick) {
            return core::Result<void, ExportError>::failure(make_error(
                "frame export requires an explicit tick within the canonical animation limit"));
        }
    }
    if (request.kind == ExportKind::sequence || request.kind == ExportKind::sprite_sheet) {
        if (request.end_tick < request.start_tick) {
            return core::Result<void, ExportError>::failure(make_error(
                "animation export requires start_tick <= end_tick"));
        }
        const core::u64 count = request.end_tick - request.start_tick + 1ULL;
        if (count > kMaximumExportFrames || request.end_tick > nodes::kMaximumAnimationTick) {
            return core::Result<void, ExportError>::failure(make_error(
                "animation export exceeds the 256-frame or canonical tick limit"));
        }
        if (request.kind == ExportKind::sprite_sheet && request.sheet_columns == 0U) {
            return core::Result<void, ExportError>::failure(make_error(
                "sprite-sheet columns must be greater than zero"));
        }
    } else if (request.loop_length) {
        return core::Result<void, ExportError>::failure(make_error(
            "loop continuity provenance is supported on sequence and sprite-sheet exports"));
    }
    if ((request.kind == ExportKind::palette_text || request.kind == ExportKind::palette_csv ||
         request.kind == ExportKind::cube_lut) && request.palette_node_id.empty()) {
        return core::Result<void, ExportError>::failure(make_error(
            "palette/LUT export requires an explicit palette node id"));
    }
    return core::Result<void, ExportError>::success();
}

}  // namespace

std::string_view to_string(const ExportKind kind) noexcept {
    switch (kind) {
    case ExportKind::still:
        return "still";
    case ExportKind::frame:
        return "frame";
    case ExportKind::sequence:
        return "sequence";
    case ExportKind::sprite_sheet:
        return "sprite-sheet";
    case ExportKind::palette_text:
        return "palette-text";
    case ExportKind::palette_csv:
        return "palette-csv";
    case ExportKind::cube_lut:
        return "cube-lut";
    }
    return "unknown";
}

std::string_view to_string(const RasterFormat format) noexcept {
    switch (format) {
    case RasterFormat::png:
        return "png";
    case RasterFormat::bmp:
        return "bmp";
    case RasterFormat::raw_rgba:
        return "raw-rgba";
    }
    return "unknown";
}

std::string deterministic_frame_filename(const core::u64 tick, const RasterFormat format) {
    std::ostringstream name;
    name.imbue(std::locale::classic());
    name << "tick-" << std::setw(10) << std::setfill('0') << tick << extension_for(format);
    return name.str();
}

std::filesystem::path staging_path_for_destination(const std::filesystem::path& destination) {
    std::filesystem::path staging = destination;
    staging += L".artminer-part";
    return staging;
}

core::Result<nodes::Image, ExportError> compose_sprite_sheet(
    const std::span<const nodes::Image> frames,
    const core::u32 columns) {
    if (frames.empty()) {
        return core::Result<nodes::Image, ExportError>::failure(make_error(
            "sprite sheet requires at least one frame"));
    }
    if (columns == 0U) {
        return core::Result<nodes::Image, ExportError>::failure(make_error(
            "sprite-sheet columns must be greater than zero"));
    }
    const nodes::Image& first = frames.front();
    auto first_bytes = core::checked_image_byte_count(first.width, first.height, 4U);
    if (first_bytes.is_error() || first_bytes.value() != static_cast<core::u64>(first.rgba.size())) {
        return core::Result<nodes::Image, ExportError>::failure(make_error(
            "sprite-sheet source frame has an invalid RGBA8 byte layout"));
    }
    for (const auto& frame : frames) {
        auto bytes = core::checked_image_byte_count(frame.width, frame.height, 4U);
        if (frame.width != first.width || frame.height != first.height || bytes.is_error() ||
            bytes.value() != static_cast<core::u64>(frame.rgba.size())) {
            return core::Result<nodes::Image, ExportError>::failure(make_error(
                "sprite-sheet frames must have equal dimensions and valid RGBA8 byte layouts"));
        }
    }
    const core::u64 actual_columns = (std::min)(
        static_cast<core::u64>(columns), static_cast<core::u64>(frames.size()));
    const core::u64 rows = (static_cast<core::u64>(frames.size()) + actual_columns - 1ULL) / actual_columns;
    const core::u64 sheet_width = static_cast<core::u64>(first.width) * actual_columns;
    const core::u64 sheet_height = static_cast<core::u64>(first.height) * rows;
    if (sheet_width > static_cast<core::u64>((std::numeric_limits<core::u32>::max)()) ||
        sheet_height > static_cast<core::u64>((std::numeric_limits<core::u32>::max)())) {
        return core::Result<nodes::Image, ExportError>::failure(make_error(
            "sprite-sheet dimensions exceed the canonical u32 image extent"));
    }
    auto sheet_bytes = core::checked_image_byte_count(
        static_cast<core::u32>(sheet_width), static_cast<core::u32>(sheet_height), 4U);
    if (sheet_bytes.is_error() ||
        sheet_bytes.value() > static_cast<core::u64>((std::numeric_limits<std::size_t>::max)())) {
        return core::Result<nodes::Image, ExportError>::failure(make_error(
            "sprite-sheet byte count exceeds addressable memory"));
    }
    nodes::Image sheet;
    sheet.width = static_cast<core::u32>(sheet_width);
    sheet.height = static_cast<core::u32>(sheet_height);
    sheet.rgba.assign(static_cast<std::size_t>(sheet_bytes.value()), 0U);
    const std::size_t source_stride = static_cast<std::size_t>(first.width) * 4U;
    const std::size_t sheet_stride = static_cast<std::size_t>(sheet.width) * 4U;
    for (std::size_t frame_index = 0U; frame_index < frames.size(); ++frame_index) {
        const std::size_t column = frame_index % static_cast<std::size_t>(actual_columns);
        const std::size_t row = frame_index / static_cast<std::size_t>(actual_columns);
        for (core::u32 y = 0U; y < first.height; ++y) {
            const std::size_t source_offset = static_cast<std::size_t>(y) * source_stride;
            const std::size_t destination_offset =
                (row * static_cast<std::size_t>(first.height) + static_cast<std::size_t>(y)) * sheet_stride +
                column * source_stride;
            std::copy_n(
                frames[frame_index].rgba.begin() + static_cast<std::ptrdiff_t>(source_offset), source_stride,
                sheet.rgba.begin() + static_cast<std::ptrdiff_t>(destination_offset));
        }
    }
    return core::Result<nodes::Image, ExportError>::success(std::move(sheet));
}

core::Result<std::string, ExportError> serialize_palette(
    const core::Recipe& recipe,
    const std::string_view palette_node_id,
    const bool csv) {
    auto palette_result = extract_palette(recipe, palette_node_id);
    if (palette_result.is_error()) {
        return core::Result<std::string, ExportError>::failure(palette_result.error());
    }
    const auto& colours = palette_result.value();
    auto stream = deterministic_stream();
    if (csv) {
        stream << "index,r,g,b,a\n";
        for (std::size_t index = 0U; index < colours.size(); ++index) {
            const auto& colour = colours[index];
            stream << index << ',' << colour.r << ',' << colour.g << ',' << colour.b << ',' << colour.a << '\n';
        }
    } else {
        stream << "# ArtMiner palette 1\n# recipe " << core::semantic_fingerprint(recipe) << '\n'
               << "# node " << palette_node_id << '\n# columns: index r g b a\n";
        for (std::size_t index = 0U; index < colours.size(); ++index) {
            const auto& colour = colours[index];
            stream << index << ' ' << colour.r << ' ' << colour.g << ' ' << colour.b << ' ' << colour.a << '\n';
        }
    }
    return core::Result<std::string, ExportError>::success(stream.str());
}

core::Result<std::string, ExportError> serialize_cube_lut(
    const core::Recipe& recipe,
    const std::string_view palette_node_id) {
    auto palette_result = extract_palette(recipe, palette_node_id);
    if (palette_result.is_error()) {
        return core::Result<std::string, ExportError>::failure(palette_result.error());
    }
    const auto& colours = palette_result.value();
    if (colours.size() < 2U) {
        return core::Result<std::string, ExportError>::failure(make_error(
            ".cube export requires at least two palette stops"));
    }
    for (const auto& colour : colours) {
        if (colour.a != 1.0) {
            return core::Result<std::string, ExportError>::failure(make_error(
                ".cube export rejects palette alpha because the current LUT contract has no alpha channel"));
        }
        if (colour.r != colour.g || colour.g != colour.b) {
            return core::Result<std::string, ExportError>::failure(make_error(
                ".cube export supports only channel-neutral palette transfers; a coloured scalar palette cannot be represented losslessly without inventing RGB-to-scalar semantics"));
        }
    }
    auto stream = deterministic_stream();
    stream << "# ArtMiner .cube export 1\n# recipe " << core::semantic_fingerprint(recipe) << '\n'
           << "# palette-node " << palette_node_id << '\n'
           << "TITLE \"ArtMiner " << core::semantic_fingerprint(recipe) << "\"\n"
           << "LUT_1D_SIZE " << colours.size() << '\n'
           << "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n";
    for (const auto& colour : colours) {
        stream << colour.r << ' ' << colour.g << ' ' << colour.b << '\n';
    }
    return core::Result<std::string, ExportError>::success(stream.str());
}

core::Result<ExportResult, ExportError> export_recipe(
    const core::Recipe& recipe,
    const ExportRequest& request) {
    const std::string source_fingerprint = core::semantic_fingerprint(recipe);
    auto request_valid = validate_request(recipe, request);
    if (request_valid.is_error()) {
        return core::Result<ExportResult, ExportError>::failure(request_valid.error());
    }
    auto workflow_result = build_workflow_provenance(recipe, request);
    if (workflow_result.is_error()) {
        return core::Result<ExportResult, ExportError>::failure(workflow_result.error());
    }
    const WorkflowProvenance workflow = std::move(workflow_result).value();

    const std::filesystem::path destination = request.destination_directory;
    const std::filesystem::path staging = staging_path_for_destination(destination);
    std::error_code filesystem_error;
    if (std::filesystem::exists(destination, filesystem_error)) {
        return core::Result<ExportResult, ExportError>::failure(make_error(
            "export destination already exists; ArtMiner never overwrites an existing export set: " + destination.string()));
    }
    if (filesystem_error) {
        return core::Result<ExportResult, ExportError>::failure(make_error(
            "could not inspect export destination: " + filesystem_error.message()));
    }
    if (std::filesystem::exists(staging, filesystem_error)) {
        return core::Result<ExportResult, ExportError>::failure(make_error(
            "export staging path already exists; it may be evidence of an interrupted export and must be reviewed or removed explicitly: " + staging.string()));
    }
    if (filesystem_error) {
        return core::Result<ExportResult, ExportError>::failure(make_error(
            "could not inspect export staging path: " + filesystem_error.message()));
    }
    const std::filesystem::path parent = destination.parent_path().empty()
        ? std::filesystem::path(L".") : destination.parent_path();
    std::filesystem::create_directories(parent, filesystem_error);
    if (filesystem_error) {
        return core::Result<ExportResult, ExportError>::failure(make_error(
            "could not create export parent directory: " + filesystem_error.message()));
    }
    if (!std::filesystem::create_directory(staging, filesystem_error) || filesystem_error) {
        return core::Result<ExportResult, ExportError>::failure(make_error(
            "could not create export staging directory: " +
            (filesystem_error ? filesystem_error.message() : std::string("path already exists"))));
    }

    const auto fail = [&](ExportError error) -> core::Result<ExportResult, ExportError> {
        std::error_code cleanup_error;
        std::filesystem::remove_all(staging, cleanup_error);
        if (cleanup_error) {
            error.message += "; staging cleanup also failed for '" + staging.string() + "': " + cleanup_error.message();
        }
        return core::Result<ExportResult, ExportError>::failure(std::move(error));
    };

    ExportResult result;
    result.directory = destination;
    result.manifest_path = destination / L"manifest.artminer.txt";
    result.recipe_fingerprint = source_fingerprint;
    core::u32 manifest_width = 0U;
    core::u32 manifest_height = 0U;

    const auto write_asset = [&](
        const std::filesystem::path& path,
        const nodes::Image& image,
        const TickSelection& ticks) -> core::Result<void, ExportError> {
        return write_raster_asset(
            staging, path, image, recipe, request.raster_format, ticks,
            request.output_name, workflow, result.files);
    };

    if (request.kind == ExportKind::still) {
        auto image = render_still(recipe, request.output_name);
        if (image.is_error()) {
            return fail(image.error());
        }
        manifest_width = image.value().width;
        manifest_height = image.value().height;
        const std::filesystem::path path = staging / (request.stem + extension_for(request.raster_format));
        auto written = write_asset(path, image.value(), {});
        if (written.is_error()) {
            return fail(written.error());
        }
    } else if (request.kind == ExportKind::frame) {
        auto image = render_tick(recipe, *request.tick, request.output_name);
        if (image.is_error()) {
            return fail(image.error());
        }
        manifest_width = image.value().width;
        manifest_height = image.value().height;
        const std::filesystem::path path = staging /
            (request.stem + "-" + deterministic_frame_filename(*request.tick, request.raster_format));
        auto written = write_asset(path, image.value(), TickSelection{request.tick, {}, {}});
        if (written.is_error()) {
            return fail(written.error());
        }
    } else if (request.kind == ExportKind::sequence) {
        const std::filesystem::path frames_directory = staging / L"frames";
        if (!std::filesystem::create_directory(frames_directory, filesystem_error) || filesystem_error) {
            return fail(make_error("could not create sequence frames directory"));
        }
        for (core::u64 tick = request.start_tick;; ++tick) {
            auto image = render_tick(recipe, tick, request.output_name);
            if (image.is_error()) {
                return fail(image.error());
            }
            if (manifest_width == 0U) {
                manifest_width = image.value().width;
                manifest_height = image.value().height;
            }
            const std::filesystem::path path = frames_directory /
                deterministic_frame_filename(tick, request.raster_format);
            auto written = write_asset(
                path, image.value(), TickSelection{tick, request.start_tick, request.end_tick});
            if (written.is_error()) {
                return fail(written.error());
            }
            if (tick == request.end_tick) {
                break;
            }
        }
    } else if (request.kind == ExportKind::sprite_sheet) {
        std::vector<nodes::Image> frames;
        frames.reserve(static_cast<std::size_t>(request.end_tick - request.start_tick + 1ULL));
        for (core::u64 tick = request.start_tick;; ++tick) {
            auto image = render_tick(recipe, tick, request.output_name);
            if (image.is_error()) {
                return fail(image.error());
            }
            frames.push_back(std::move(image).value());
            if (tick == request.end_tick) {
                break;
            }
        }
        auto sheet = compose_sprite_sheet(frames, request.sheet_columns);
        if (sheet.is_error()) {
            return fail(sheet.error());
        }
        manifest_width = sheet.value().width;
        manifest_height = sheet.value().height;
        const std::filesystem::path sheet_path = staging /
            (request.stem + "-sheet" + extension_for(request.raster_format));
        auto written = write_asset(
            sheet_path, sheet.value(), TickSelection{{}, request.start_tick, request.end_tick});
        if (written.is_error()) {
            return fail(written.error());
        }
        const core::u32 actual_columns = (std::min)(
            request.sheet_columns, static_cast<core::u32>(frames.size()));
        auto atlas = deterministic_stream();
        atlas << "tick,x,y,width,height\n";
        for (std::size_t index = 0U; index < frames.size(); ++index) {
            const core::u64 tick = request.start_tick + static_cast<core::u64>(index);
            const core::u32 column = static_cast<core::u32>(index % actual_columns);
            const core::u32 row = static_cast<core::u32>(index / actual_columns);
            atlas << tick << ','
                  << static_cast<core::u64>(column) * frames.front().width << ','
                  << static_cast<core::u64>(row) * frames.front().height << ','
                  << frames.front().width << ',' << frames.front().height << '\n';
        }
        const std::filesystem::path atlas_path = staging / (request.stem + "-atlas.csv");
        auto atlas_written = write_text(atlas_path, atlas.str());
        if (atlas_written.is_error()) {
            return fail(atlas_written.error());
        }
        auto atlas_described = append_description(staging, atlas_path, result.files);
        if (atlas_described.is_error()) {
            return fail(atlas_described.error());
        }
    } else if (request.kind == ExportKind::palette_text || request.kind == ExportKind::palette_csv) {
        const bool csv = request.kind == ExportKind::palette_csv;
        auto palette = serialize_palette(recipe, request.palette_node_id, csv);
        if (palette.is_error()) {
            return fail(palette.error());
        }
        const std::filesystem::path path = staging / (request.stem + (csv ? ".csv" : ".txt"));
        auto written = write_text(path, palette.value());
        if (written.is_error()) {
            return fail(written.error());
        }
        auto described = append_description(staging, path, result.files);
        if (described.is_error()) {
            return fail(described.error());
        }
    } else if (request.kind == ExportKind::cube_lut) {
        auto cube = serialize_cube_lut(recipe, request.palette_node_id);
        if (cube.is_error()) {
            return fail(cube.error());
        }
        const std::filesystem::path path = staging / (request.stem + ".cube");
        auto written = write_text(path, cube.value());
        if (written.is_error()) {
            return fail(written.error());
        }
        auto described = append_description(staging, path, result.files);
        if (described.is_error()) {
            return fail(described.error());
        }
    }

    if (core::semantic_fingerprint(recipe) != source_fingerprint) {
        return fail(make_error("export unexpectedly changed the source recipe semantic fingerprint"));
    }
    const std::filesystem::path staging_manifest = staging / L"manifest.artminer.txt";
    auto manifest_written = write_text(
        staging_manifest,
        manifest_text(recipe, request, manifest_width, manifest_height, workflow, result.files));
    if (manifest_written.is_error()) {
        return fail(manifest_written.error());
    }
    std::filesystem::rename(staging, destination, filesystem_error);
    if (filesystem_error) {
        return fail(make_error("could not publish completed export set atomically: " + filesystem_error.message()));
    }
    return core::Result<ExportResult, ExportError>::success(std::move(result));
}

}  // namespace artminer::exporting
