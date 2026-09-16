#include "export/glyph_export.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/graph.hpp"
#include "core/hash.hpp"
#include "nodes/glyph_synthesis.hpp"
#include "nodes/motion_evaluator.hpp"

namespace artminer::exporting {
namespace {

[[nodiscard]] ExportError make_error(std::string message) {
    return ExportError{std::move(message)};
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

[[nodiscard]] std::string extension_for(const GlyphTextFormat format) {
    return format == GlyphTextFormat::ansi ? ".ans" : ".txt";
}

[[nodiscard]] core::Result<void, ExportError> write_text(
    const std::filesystem::path& path,
    const std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Result<void, ExportError>::failure(make_error(
            "could not create glyph export file: " + path.string()));
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    if (!output) {
        return core::Result<void, ExportError>::failure(make_error(
            "failed while writing glyph export file: " + path.string()));
    }
    return core::Result<void, ExportError>::success();
}

[[nodiscard]] core::Result<ExportedFile, ExportError> describe_file(
    const std::filesystem::path& root,
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return core::Result<ExportedFile, ExportError>::failure(make_error(
            "could not reopen produced glyph export file: " + path.string()));
    }
    const std::string bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (!input.good() && !input.eof()) {
        return core::Result<ExportedFile, ExportError>::failure(make_error(
            "failed while checksumming produced glyph export file: " + path.string()));
    }
    const std::filesystem::path relative = path.lexically_relative(root);
    if (relative.empty()) {
        return core::Result<ExportedFile, ExportError>::failure(make_error(
            "could not derive a glyph export-relative filename"));
    }
    return core::Result<ExportedFile, ExportError>::success(ExportedFile{
        relative.generic_string(),
        static_cast<core::u64>(bytes.size()),
        core::hex_u64(core::fnv1a64(std::string_view(bytes))),
    });
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

[[nodiscard]] std::string settings_text(
    const nodes::GlyphGrid& grid,
    const std::string_view settings_node_id) {
    auto stream = deterministic_stream();
    stream << "glyph-settings-node " << std::quoted(std::string(settings_node_id)) << '\n'
           << "glyph-set " << nodes::to_string(grid.settings.glyph_set) << '\n'
           << "choice-mode " << nodes::to_string(grid.settings.choice_mode) << '\n'
           << "density-weight " << grid.settings.density_weight << '\n'
           << "orientation-weight " << grid.settings.orientation_weight << '\n'
           << "detail-weight " << grid.settings.detail_weight << '\n'
           << "corner-weight " << grid.settings.corner_weight << '\n'
           << "motion-weight " << grid.settings.motion_weight << '\n'
           << "colour-mode " << nodes::to_string(grid.settings.colour_mode) << '\n'
           << "limited-palette " << nodes::to_string(grid.settings.limited_palette) << '\n'
           << "background-mode " << nodes::to_string(grid.settings.background_mode) << '\n'
           << "alpha-mode " << nodes::to_string(grid.settings.alpha_mode) << '\n'
           << "cell-width " << grid.cell_width << '\n'
           << "cell-height " << grid.cell_height << '\n'
           << "grid-columns " << grid.columns << '\n'
           << "grid-rows " << grid.rows << '\n';
    return stream.str();
}

[[nodiscard]] std::string provenance_text(
    const core::Recipe& recipe,
    const nodes::GlyphGrid& grid,
    const GlyphTextFormat format,
    const std::string_view settings_node_id,
    const std::optional<core::u64> tick,
    const std::optional<core::u64> range_start,
    const std::optional<core::u64> range_end) {
    auto stream = deterministic_stream();
    stream << "ArtMiner-Provenance 2\n"
           << "exporter " << kExporterSemanticVersion << '\n'
           << "fingerprint " << core::semantic_fingerprint(recipe) << '\n'
           << "schema " << recipe.schema_version << '\n'
           << "evaluator " << recipe.evaluator_version << '\n'
           << "asset-format glyph-" << to_string(format) << '\n'
           << "canonical-output glyph-cell-sequence\n"
           << "text-encoding UTF-8\n"
           << "bom none\n"
           << "line-endings LF\n"
           << "source-pixel-layout RGBA8-row-major-tight\n"
           << "source-colour-space sRGB-encoded\n"
           << "sampling partial-edge-cells-source-bounds\n"
           << "font-rasterization noncanonical-preview-only\n"
           << settings_text(grid, settings_node_id);
    if (tick.has_value()) {
        stream << "tick " << *tick << '\n';
    }
    if (range_start.has_value() && range_end.has_value()) {
        stream << "tick-range " << *range_start << ' ' << *range_end << '\n';
    }
    stream << "recipe-begin\n"
           << core::serialize_recipe_canonical(recipe)
           << "recipe-end\n";
    return stream.str();
}

[[nodiscard]] core::Result<std::string, ExportError> serialize_grid(
    const nodes::GlyphGrid& grid,
    const GlyphTextFormat format) {
    auto serialized = format == GlyphTextFormat::ansi
        ? nodes::serialize_glyph_ansi(grid)
        : nodes::serialize_glyph_utf8(grid);
    if (serialized.is_error()) {
        return core::Result<std::string, ExportError>::failure(make_error(serialized.error().message));
    }
    return core::Result<std::string, ExportError>::success(std::move(serialized).value());
}

[[nodiscard]] core::Result<void, ExportError> write_glyph_asset(
    const std::filesystem::path& root,
    const std::filesystem::path& path,
    const core::Recipe& recipe,
    const nodes::GlyphGrid& grid,
    const GlyphTextFormat format,
    const std::string_view settings_node_id,
    const std::optional<core::u64> tick,
    const std::optional<core::u64> range_start,
    const std::optional<core::u64> range_end,
    std::vector<ExportedFile>& files) {
    auto payload = serialize_grid(grid, format);
    if (payload.is_error()) {
        return core::Result<void, ExportError>::failure(payload.error());
    }
    auto written = write_text(path, payload.value());
    if (written.is_error()) {
        return written;
    }
    auto described = append_description(root, path, files);
    if (described.is_error()) {
        return described;
    }

    const std::filesystem::path sidecar = provenance_path(path);
    auto sidecar_written = write_text(
        sidecar,
        provenance_text(recipe, grid, format, settings_node_id, tick, range_start, range_end));
    if (sidecar_written.is_error()) {
        return sidecar_written;
    }
    return append_description(root, sidecar, files);
}

[[nodiscard]] std::string manifest_text(
    const core::Recipe& recipe,
    const GlyphExportRequest& request,
    const nodes::GlyphGrid& representative_grid,
    const std::vector<ExportedFile>& files) {
    auto stream = deterministic_stream();
    stream << "ArtMiner-Export-Manifest 1\n"
           << "exporter " << kExporterSemanticVersion << '\n'
           << "fingerprint " << core::semantic_fingerprint(recipe) << '\n'
           << "schema " << recipe.schema_version << '\n'
           << "evaluator " << recipe.evaluator_version << '\n'
           << "kind glyph-" << to_string(request.format)
           << (request.kind == GlyphExportKind::sequence ? "-sequence" : "") << '\n'
           << "width " << representative_grid.columns << '\n'
           << "height " << representative_grid.rows << '\n'
           << "raster-format none\n"
           << "canonical-output glyph-cell-sequence\n"
           << "text-encoding UTF-8\n"
           << "bom none\n"
           << "line-endings LF\n"
           << "source-colour-space sRGB-encoded\n"
           << "font-rasterization noncanonical-preview-only\n"
           << settings_text(representative_grid, request.settings_node_id);
    if (request.kind == GlyphExportKind::single && request.tick.has_value()) {
        stream << "tick " << *request.tick << '\n';
    }
    if (request.kind == GlyphExportKind::sequence) {
        stream << "tick-range " << request.start_tick << ' ' << request.end_tick << '\n';
    }
    stream << "file-count " << files.size() << '\n';
    for (const auto& file : files) {
        stream << "file " << std::quoted(file.relative_path) << ' '
               << file.byte_count << ' ' << file.checksum_fnv1a64 << '\n';
    }
    stream << "recipe-begin\n"
           << core::serialize_recipe_canonical(recipe)
           << "recipe-end\n";
    return stream.str();
}

[[nodiscard]] core::Result<void, ExportError> validate_request(
    const core::Recipe& recipe,
    const GlyphExportRequest& request) {
    const auto validation_errors = core::validate_recipe(recipe);
    if (!validation_errors.empty()) {
        return core::Result<void, ExportError>::failure(make_error(
            "glyph export requires a valid recipe: " + validation_errors.front().message));
    }
    if (request.destination_directory.empty()) {
        return core::Result<void, ExportError>::failure(make_error(
            "glyph export destination directory must not be empty"));
    }
    if (!safe_stem(request.stem)) {
        return core::Result<void, ExportError>::failure(make_error(
            "glyph export stem must contain only ASCII letters, digits, '.', '-' or '_' and must not be '.' or '..'"));
    }
    if (request.settings_node_id.empty()) {
        return core::Result<void, ExportError>::failure(make_error(
            "glyph export requires an explicit core.glyph.settings node id"));
    }
    auto settings = nodes::glyph_settings_from_recipe(recipe, request.settings_node_id);
    if (settings.is_error()) {
        return core::Result<void, ExportError>::failure(make_error(settings.error().message));
    }
    if (request.kind == GlyphExportKind::single && request.tick.has_value() &&
        *request.tick > nodes::kMaximumAnimationTick) {
        return core::Result<void, ExportError>::failure(make_error(
            "glyph frame tick exceeds the canonical animation limit"));
    }
    if (request.kind == GlyphExportKind::sequence) {
        if (request.end_tick < request.start_tick) {
            return core::Result<void, ExportError>::failure(make_error(
                "glyph sequence requires start_tick <= end_tick"));
        }
        const core::u64 count = request.end_tick - request.start_tick + 1ULL;
        if (count > kMaximumExportFrames) {
            return core::Result<void, ExportError>::failure(make_error(
                "glyph sequence is limited to 256 inclusive frames"));
        }
        if (request.end_tick > nodes::kMaximumAnimationTick) {
            return core::Result<void, ExportError>::failure(make_error(
                "glyph sequence range exceeds the canonical animation limit"));
        }
    }
    return core::Result<void, ExportError>::success();
}

}  // namespace

std::string_view to_string(const GlyphTextFormat format) noexcept {
    switch (format) {
    case GlyphTextFormat::utf8:
        return "utf8";
    case GlyphTextFormat::ansi:
        return "ansi";
    }
    return "unknown";
}

std::string deterministic_glyph_frame_filename(const core::u64 tick, const GlyphTextFormat format) {
    std::ostringstream name;
    name.imbue(std::locale::classic());
    name << "tick-" << std::setw(10) << std::setfill('0') << tick << extension_for(format);
    return name.str();
}

core::Result<ExportResult, ExportError> export_glyph_recipe(
    const core::Recipe& recipe,
    const GlyphExportRequest& request) {
    const std::string source_fingerprint = core::semantic_fingerprint(recipe);
    auto request_valid = validate_request(recipe, request);
    if (request_valid.is_error()) {
        return core::Result<ExportResult, ExportError>::failure(request_valid.error());
    }

    const std::filesystem::path destination = request.destination_directory;
    const std::filesystem::path staging = staging_path_for_destination(destination);
    std::error_code filesystem_error;
    if (std::filesystem::exists(destination, filesystem_error)) {
        return core::Result<ExportResult, ExportError>::failure(make_error(
            "glyph export destination already exists; ArtMiner never overwrites an existing export set: " +
            destination.string()));
    }
    if (filesystem_error) {
        return core::Result<ExportResult, ExportError>::failure(make_error(
            "could not inspect glyph export destination: " + filesystem_error.message()));
    }
    if (std::filesystem::exists(staging, filesystem_error)) {
        return core::Result<ExportResult, ExportError>::failure(make_error(
            "glyph export staging path already exists; it may be evidence of an interrupted export and must be reviewed or removed explicitly: " +
            staging.string()));
    }
    if (filesystem_error) {
        return core::Result<ExportResult, ExportError>::failure(make_error(
            "could not inspect glyph export staging path: " + filesystem_error.message()));
    }

    const std::filesystem::path parent = destination.parent_path().empty()
        ? std::filesystem::path(L".")
        : destination.parent_path();
    std::filesystem::create_directories(parent, filesystem_error);
    if (filesystem_error) {
        return core::Result<ExportResult, ExportError>::failure(make_error(
            "could not create glyph export parent directory: " + filesystem_error.message()));
    }
    if (!std::filesystem::create_directory(staging, filesystem_error) || filesystem_error) {
        return core::Result<ExportResult, ExportError>::failure(make_error(
            "could not create glyph export staging directory: " +
            (filesystem_error ? filesystem_error.message() : std::string("path already exists"))));
    }

    const auto fail = [&](ExportError error) -> core::Result<ExportResult, ExportError> {
        std::error_code cleanup_error;
        std::filesystem::remove_all(staging, cleanup_error);
        if (cleanup_error) {
            error.message += "; glyph staging cleanup also failed for '" + staging.string() + "': " +
                cleanup_error.message();
        }
        return core::Result<ExportResult, ExportError>::failure(std::move(error));
    };

    ExportResult result;
    result.directory = destination;
    result.manifest_path = destination / L"manifest.artminer.txt";
    result.recipe_fingerprint = source_fingerprint;
    std::optional<nodes::GlyphGrid> representative_grid;

    if (request.kind == GlyphExportKind::single) {
        auto grid = nodes::synthesize_recipe_glyph_grid(recipe, request.settings_node_id, request.tick);
        if (grid.is_error()) {
            return fail(make_error(grid.error().message));
        }
        representative_grid = grid.value();
        const std::filesystem::path path = staging / (request.stem + extension_for(request.format));
        auto written = write_glyph_asset(
            staging,
            path,
            recipe,
            grid.value(),
            request.format,
            request.settings_node_id,
            request.tick,
            std::nullopt,
            std::nullopt,
            result.files);
        if (written.is_error()) {
            return fail(written.error());
        }
    } else {
        const std::filesystem::path frames_directory = staging / L"frames";
        if (!std::filesystem::create_directory(frames_directory, filesystem_error) || filesystem_error) {
            return fail(make_error("could not create glyph sequence frames directory"));
        }
        for (core::u64 tick = request.start_tick;; ++tick) {
            auto grid = nodes::synthesize_recipe_glyph_grid(recipe, request.settings_node_id, tick);
            if (grid.is_error()) {
                return fail(make_error(
                    "glyph sequence synthesis failed at tick " + std::to_string(tick) + ": " + grid.error().message));
            }
            if (!representative_grid.has_value()) {
                representative_grid = grid.value();
            }
            const std::filesystem::path path = frames_directory /
                deterministic_glyph_frame_filename(tick, request.format);
            auto written = write_glyph_asset(
                staging,
                path,
                recipe,
                grid.value(),
                request.format,
                request.settings_node_id,
                tick,
                request.start_tick,
                request.end_tick,
                result.files);
            if (written.is_error()) {
                return fail(written.error());
            }
            if (tick == request.end_tick) {
                break;
            }
        }
    }

    if (!representative_grid.has_value()) {
        return fail(make_error("glyph export produced no canonical grid"));
    }
    if (core::semantic_fingerprint(recipe) != source_fingerprint) {
        return fail(make_error("glyph export unexpectedly changed the source recipe semantic fingerprint"));
    }

    const std::filesystem::path staging_manifest = staging / L"manifest.artminer.txt";
    auto manifest_written = write_text(
        staging_manifest,
        manifest_text(recipe, request, *representative_grid, result.files));
    if (manifest_written.is_error()) {
        return fail(manifest_written.error());
    }

    std::filesystem::rename(staging, destination, filesystem_error);
    if (filesystem_error) {
        return fail(make_error(
            "could not atomically publish completed glyph export set: " + filesystem_error.message()));
    }
    return core::Result<ExportResult, ExportError>::success(std::move(result));
}

}  // namespace artminer::exporting
