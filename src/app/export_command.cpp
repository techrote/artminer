#include "app/export_command.hpp"

#include <cerrno>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "app/export_window.hpp"
#include "app/glyph_preview_window.hpp"
#include "core/graph.hpp"
#include "core/local_text.hpp"
#include "core/recipe.hpp"
#include "core/types.hpp"
#include "export/export.hpp"
#include "export/glyph_export.hpp"
#include "platform/windows/portable_workspace.hpp"

namespace artminer::app {
namespace {

[[nodiscard]] std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    auto text = core::read_local_text_file(path);
    if (text.is_error()) {
        std::wcerr << L"export error: bounded UTF-8 recipe read rejected " << path.wstring() << L'\n';
        std::cerr << "detail: " << text.error().message << '\n';
        return std::nullopt;
    }
    return std::move(text).value();
}

[[nodiscard]] std::optional<core::Recipe> load_recipe(const std::filesystem::path& path) {
    auto text = read_text_file(path);
    if (!text) {
        return std::nullopt;
    }
    auto parsed = core::parse_recipe(*text);
    if (parsed.is_error()) {
        std::cerr << "export recipe parse error";
        if (parsed.error().line != 0U) {
            std::cerr << " at line " << parsed.error().line;
        }
        std::cerr << ": " << parsed.error().message << '\n';
        return std::nullopt;
    }
    core::Recipe recipe = std::move(parsed).value();
    const auto errors = core::validate_recipe(recipe);
    if (!errors.empty()) {
        std::cerr << "export recipe validation error: " << errors.front().message << '\n';
        return std::nullopt;
    }
    return recipe;
}

[[nodiscard]] std::optional<core::u64> parse_u64(const wchar_t* text) {
    if (text == nullptr || *text == L'\0' || *text == L'-') {
        return std::nullopt;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long long value = std::wcstoull(text, &end, 10);
    if (errno == ERANGE || end == text || end == nullptr || *end != L'\0') {
        return std::nullopt;
    }
    return static_cast<core::u64>(value);
}

[[nodiscard]] std::optional<exporting::RasterFormat> parse_format(const std::wstring_view text) {
    if (text == L"png") {
        return exporting::RasterFormat::png;
    }
    if (text == L"bmp") {
        return exporting::RasterFormat::bmp;
    }
    if (text == L"rgba" || text == L"raw" || text == L"raw-rgba") {
        return exporting::RasterFormat::raw_rgba;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<exporting::GlyphTextFormat> parse_glyph_format(const std::wstring_view text) {
    if (text == L"text" || text == L"txt" || text == L"utf8" || text == L"utf-8") {
        return exporting::GlyphTextFormat::utf8;
    }
    if (text == L"ansi" || text == L"ans") {
        return exporting::GlyphTextFormat::ansi;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> parse_ascii_node_id(const std::wstring_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::string result;
    result.reserve(text.size());
    for (const wchar_t value : text) {
        if (static_cast<unsigned int>(value) > 0x7fU) {
            return std::nullopt;
        }
        result.push_back(static_cast<char>(value));
    }
    return result;
}

void print_usage() {
    std::cerr
        << "usage:\n"
        << "  ArtMiner export ui <file.amr>\n"
        << "  ArtMiner export still <file.amr> <output-dir> <png|bmp|rgba>\n"
        << "  ArtMiner export frame <file.amr> <tick> <output-dir> <png|bmp|rgba>\n"
        << "  ArtMiner export sequence <file.amr> <start> <end> <output-dir> <png|bmp|rgba>\n"
        << "  ArtMiner export sheet <file.amr> <start> <end> <columns> <output-dir> <png|bmp|rgba>\n"
        << "  ArtMiner export palette <file.amr> <palette-node> <output-dir> <text|csv|cube>\n"
        << "  ArtMiner export glyph-preview <file.amr> <glyph-settings-node> [tick]\n"
        << "  ArtMiner export glyph <file.amr> <glyph-settings-node> <output-dir> <text|ansi>\n"
        << "  ArtMiner export glyph-frame <file.amr> <glyph-settings-node> <tick> <output-dir> <text|ansi>\n"
        << "  ArtMiner export glyph-sequence <file.amr> <glyph-settings-node> <start> <end> <output-dir> <text|ansi>\n";
}

[[nodiscard]] int finish_export(const core::Recipe& recipe, exporting::ExportRequest request) {
    auto exported = exporting::export_recipe(recipe, request);
    if (exported.is_error()) {
        std::cerr << "export error: " << exported.error().message << '\n';
        return 8;
    }
    std::wcout << L"exported " << exported.value().directory.wstring()
               << L" manifest " << exported.value().manifest_path.wstring() << L'\n';
    std::cout << "recipe " << exported.value().recipe_fingerprint
              << " files " << exported.value().files.size() << '\n';
    return 0;
}

[[nodiscard]] int finish_glyph_export(const core::Recipe& recipe, exporting::GlyphExportRequest request) {
    auto exported = exporting::export_glyph_recipe(recipe, request);
    if (exported.is_error()) {
        std::cerr << "glyph export error: " << exported.error().message << '\n';
        return 8;
    }
    std::wcout << L"exported glyph set " << exported.value().directory.wstring()
               << L" manifest " << exported.value().manifest_path.wstring() << L'\n';
    std::cout << "recipe " << exported.value().recipe_fingerprint
              << " files " << exported.value().files.size() << '\n';
    return 0;
}

}  // namespace

int run_export_command(const int argc, wchar_t* argv[]) {
    if (argc < 3) {
        print_usage();
        return 2;
    }
    const std::wstring_view action(argv[2]);

    if (action == L"ui") {
        if (argc != 4) {
            print_usage();
            return 2;
        }
        auto recipe = load_recipe(std::filesystem::path(argv[3]));
        if (!recipe) {
            return 6;
        }
        auto workspace_result = platform::windows::PortableWorkspace::from_executable();
        if (workspace_result.is_error()) {
            std::wcerr << L"export workspace error: " << workspace_result.error().message << L'\n';
            return 4;
        }
        auto workspace = std::move(workspace_result).value();
        auto layout = workspace.ensure_layout();
        if (layout.is_error()) {
            std::wcerr << L"export workspace error: " << layout.error().message << L'\n';
            return 4;
        }
        return run_export_application(*recipe, workspace.layout().output);
    }

    if (action == L"glyph-preview") {
        if (argc != 5 && argc != 6) {
            print_usage();
            return 2;
        }
        auto recipe = load_recipe(std::filesystem::path(argv[3]));
        auto node_id = parse_ascii_node_id(std::wstring_view(argv[4]));
        if (!recipe || !node_id) {
            if (!node_id) {
                std::cerr << "glyph preview error: settings node id must be non-empty ASCII\n";
            }
            return 6;
        }
        std::optional<core::u64> tick;
        if (argc == 6) {
            tick = parse_u64(argv[5]);
            if (!tick) {
                std::cerr << "glyph preview error: tick must be an unsigned integer\n";
                return 2;
            }
        }
        return run_glyph_preview_application(*recipe, *node_id, tick);
    }

    if (action == L"glyph") {
        if (argc != 7) {
            print_usage();
            return 2;
        }
        auto recipe = load_recipe(std::filesystem::path(argv[3]));
        auto node_id = parse_ascii_node_id(std::wstring_view(argv[4]));
        auto format = parse_glyph_format(std::wstring_view(argv[6]));
        if (!recipe || !node_id || !format) {
            if (!format) {
                std::cerr << "glyph export error: format must be text or ansi\n";
            }
            return 6;
        }
        exporting::GlyphExportRequest request;
        request.kind = exporting::GlyphExportKind::single;
        request.format = *format;
        request.destination_directory = std::filesystem::path(argv[5]);
        request.stem = "glyph";
        request.settings_node_id = std::move(*node_id);
        return finish_glyph_export(*recipe, std::move(request));
    }

    if (action == L"glyph-frame") {
        if (argc != 8) {
            print_usage();
            return 2;
        }
        auto recipe = load_recipe(std::filesystem::path(argv[3]));
        auto node_id = parse_ascii_node_id(std::wstring_view(argv[4]));
        auto tick = parse_u64(argv[5]);
        auto format = parse_glyph_format(std::wstring_view(argv[7]));
        if (!recipe || !node_id || !tick || !format) {
            std::cerr << "glyph frame export error: require ASCII settings node, unsigned tick, and text or ansi format\n";
            return 2;
        }
        exporting::GlyphExportRequest request;
        request.kind = exporting::GlyphExportKind::single;
        request.format = *format;
        request.tick = *tick;
        request.destination_directory = std::filesystem::path(argv[6]);
        request.stem = "glyph";
        request.settings_node_id = std::move(*node_id);
        return finish_glyph_export(*recipe, std::move(request));
    }

    if (action == L"glyph-sequence") {
        if (argc != 9) {
            print_usage();
            return 2;
        }
        auto recipe = load_recipe(std::filesystem::path(argv[3]));
        auto node_id = parse_ascii_node_id(std::wstring_view(argv[4]));
        auto start = parse_u64(argv[5]);
        auto end = parse_u64(argv[6]);
        auto format = parse_glyph_format(std::wstring_view(argv[8]));
        if (!recipe || !node_id || !start || !end || !format) {
            std::cerr << "glyph sequence export error: require ASCII settings node, unsigned ticks, and text or ansi format\n";
            return 2;
        }
        exporting::GlyphExportRequest request;
        request.kind = exporting::GlyphExportKind::sequence;
        request.format = *format;
        request.start_tick = *start;
        request.end_tick = *end;
        request.destination_directory = std::filesystem::path(argv[7]);
        request.stem = "glyph";
        request.settings_node_id = std::move(*node_id);
        return finish_glyph_export(*recipe, std::move(request));
    }

    if (action == L"still") {
        if (argc != 6) {
            print_usage();
            return 2;
        }
        auto format = parse_format(argv[5]);
        if (!format) {
            std::cerr << "export error: format must be png, bmp, or rgba\n";
            return 2;
        }
        auto recipe = load_recipe(std::filesystem::path(argv[3]));
        if (!recipe) {
            return 6;
        }
        exporting::ExportRequest request;
        request.kind = exporting::ExportKind::still;
        request.raster_format = *format;
        request.destination_directory = std::filesystem::path(argv[4]);
        request.stem = "art";
        return finish_export(*recipe, std::move(request));
    }

    if (action == L"frame") {
        if (argc != 7) {
            print_usage();
            return 2;
        }
        const auto tick = parse_u64(argv[4]);
        const auto format = parse_format(argv[6]);
        if (!tick || !format) {
            std::cerr << "export error: frame requires an unsigned tick and png, bmp, or rgba format\n";
            return 2;
        }
        auto recipe = load_recipe(std::filesystem::path(argv[3]));
        if (!recipe) {
            return 6;
        }
        exporting::ExportRequest request;
        request.kind = exporting::ExportKind::frame;
        request.raster_format = *format;
        request.tick = *tick;
        request.destination_directory = std::filesystem::path(argv[5]);
        request.stem = "art";
        return finish_export(*recipe, std::move(request));
    }

    if (action == L"sequence") {
        if (argc != 8) {
            print_usage();
            return 2;
        }
        const auto start = parse_u64(argv[4]);
        const auto end = parse_u64(argv[5]);
        const auto format = parse_format(argv[7]);
        if (!start || !end || !format) {
            std::cerr << "export error: sequence requires unsigned start/end ticks and png, bmp, or rgba format\n";
            return 2;
        }
        auto recipe = load_recipe(std::filesystem::path(argv[3]));
        if (!recipe) {
            return 6;
        }
        exporting::ExportRequest request;
        request.kind = exporting::ExportKind::sequence;
        request.raster_format = *format;
        request.start_tick = *start;
        request.end_tick = *end;
        request.destination_directory = std::filesystem::path(argv[6]);
        request.stem = "art";
        return finish_export(*recipe, std::move(request));
    }

    if (action == L"sheet") {
        if (argc != 9) {
            print_usage();
            return 2;
        }
        const auto start = parse_u64(argv[4]);
        const auto end = parse_u64(argv[5]);
        const auto columns = parse_u64(argv[6]);
        const auto format = parse_format(argv[8]);
        if (!start || !end || !columns || *columns == 0U ||
            *columns > static_cast<core::u64>((std::numeric_limits<core::u32>::max)()) || !format) {
            std::cerr << "export error: sheet requires unsigned start/end ticks, positive columns, and png, bmp, or rgba format\n";
            return 2;
        }
        auto recipe = load_recipe(std::filesystem::path(argv[3]));
        if (!recipe) {
            return 6;
        }
        exporting::ExportRequest request;
        request.kind = exporting::ExportKind::sprite_sheet;
        request.raster_format = *format;
        request.start_tick = *start;
        request.end_tick = *end;
        request.sheet_columns = static_cast<core::u32>(*columns);
        request.destination_directory = std::filesystem::path(argv[7]);
        request.stem = "art";
        return finish_export(*recipe, std::move(request));
    }

    if (action == L"palette") {
        if (argc != 7) {
            print_usage();
            return 2;
        }
        auto recipe = load_recipe(std::filesystem::path(argv[3]));
        if (!recipe) {
            return 6;
        }
        const std::wstring_view format(argv[6]);
        exporting::ExportRequest request;
        if (format == L"text" || format == L"txt") {
            request.kind = exporting::ExportKind::palette_text;
        } else if (format == L"csv") {
            request.kind = exporting::ExportKind::palette_csv;
        } else if (format == L"cube") {
            request.kind = exporting::ExportKind::cube_lut;
        } else {
            std::cerr << "export error: palette format must be text, csv, or cube\n";
            return 2;
        }
        request.destination_directory = std::filesystem::path(argv[5]);
        request.stem = "palette";
        auto node_id = parse_ascii_node_id(std::wstring_view(argv[4]));
        if (!node_id) {
            std::cerr << "export error: palette node id must be non-empty ASCII\n";
            return 2;
        }
        request.palette_node_id = std::move(*node_id);
        return finish_export(*recipe, std::move(request));
    }

    std::wcerr << L"export error: unknown export action: " << action << L'\n';
    print_usage();
    return 2;
}

}  // namespace artminer::app
