#include "app/material_command.hpp"

#include <cerrno>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "app/material_window.hpp"
#include "core/graph.hpp"
#include "core/local_text.hpp"
#include "core/recipe.hpp"
#include "core/types.hpp"
#include "export/export.hpp"
#include "nodes/material_evaluator.hpp"
#include "quarry/seam.hpp"

namespace artminer::app {
namespace {

[[nodiscard]] std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    auto text = core::read_local_text_file(path);
    if (text.is_error()) {
        std::wcerr << L"material error: bounded UTF-8 recipe read rejected " << path.wstring() << L'\n';
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
        std::cerr << "material recipe parse error";
        if (parsed.error().line != 0U) {
            std::cerr << " at line " << parsed.error().line;
        }
        std::cerr << ": " << parsed.error().message << '\n';
        return std::nullopt;
    }
    core::Recipe recipe = std::move(parsed).value();
    const auto errors = core::validate_recipe(recipe);
    if (!errors.empty()) {
        std::cerr << "material recipe validation error: " << errors.front().message << '\n';
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

[[nodiscard]] std::optional<double> parse_unit_double(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return std::nullopt;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const double value = std::wcstod(text, &end);
    if (errno == ERANGE || end == text || end == nullptr || *end != L'\0' ||
        !std::isfinite(value) || value < 0.0 || value > 1.0) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::string> parse_ascii(const std::wstring_view text) {
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

void print_usage() {
    std::cerr
        << "usage:\n"
        << "  ArtMiner material seam <file.amr> [output-name] [threshold]\n"
        << "  ArtMiner material loop <file.amr> <loop-length> [output-name] [tolerance]\n"
        << "  ArtMiner material ui <file.amr> [output-name]\n"
        << "  ArtMiner material export <file.amr> <output-name> <output-dir> <png|bmp|rgba>\n"
        << "  ArtMiner material loop-sequence <file.amr> <loop-length> <output-dir> <png|bmp|rgba> [output-name]\n";
}

[[nodiscard]] int finish_export(const core::Recipe& recipe, exporting::ExportRequest request) {
    auto exported = exporting::export_recipe(recipe, request);
    if (exported.is_error()) {
        std::cerr << "material export error: " << exported.error().message << '\n';
        return 8;
    }
    std::wcout << L"exported " << exported.value().directory.wstring()
               << L" manifest " << exported.value().manifest_path.wstring() << L'\n';
    std::cout << "recipe " << exported.value().recipe_fingerprint
              << " files " << exported.value().files.size() << '\n';
    return 0;
}

}  // namespace

int run_material_command(const int argc, wchar_t* argv[]) {
    if (argc < 4) {
        print_usage();
        return 2;
    }
    const std::wstring_view action(argv[2]);

    if (action == L"seam") {
        if (argc < 4 || argc > 6) {
            print_usage();
            return 2;
        }
        auto recipe = load_recipe(std::filesystem::path(argv[3]));
        if (!recipe) {
            return 6;
        }
        std::string output_name = "main";
        if (argc >= 5) {
            auto parsed = parse_ascii(argv[4]);
            if (!parsed) {
                std::cerr << "material seam error: output name must be non-empty ASCII\n";
                return 2;
            }
            output_name = std::move(*parsed);
        }
        double threshold = 0.02;
        if (argc == 6) {
            auto parsed = parse_unit_double(argv[5]);
            if (!parsed) {
                std::cerr << "material seam error: threshold must be within [0,1]\n";
                return 2;
            }
            threshold = *parsed;
        }
        auto image = nodes::render_workflow_reference(*recipe, output_name);
        if (image.is_error()) {
            std::cerr << "material seam render error: " << image.error().message << '\n';
            return 7;
        }
        auto seam = quarry::compute_tile_seam_diagnostics(image.value());
        if (seam.is_error()) {
            std::cerr << "material seam error: " << seam.error().message << '\n';
            return 7;
        }
        const bool pass = seam.value().combined_error <= threshold;
        std::cout << std::setprecision(17)
                  << "horizontal " << seam.value().horizontal_error << '\n'
                  << "vertical " << seam.value().vertical_error << '\n'
                  << "combined " << seam.value().combined_error << '\n'
                  << "threshold " << threshold << '\n'
                  << "seam " << (pass ? "PASS" : "FAIL") << '\n';
        return pass ? 0 : 9;
    }

    if (action == L"loop") {
        if (argc < 5 || argc > 7) {
            print_usage();
            return 2;
        }
        auto recipe = load_recipe(std::filesystem::path(argv[3]));
        auto length = parse_u64(argv[4]);
        if (!recipe || !length) {
            std::cerr << "material loop error: loop length must be an unsigned integer\n";
            return 2;
        }
        std::string output_name = "main";
        if (argc >= 6) {
            auto parsed = parse_ascii(argv[5]);
            if (!parsed) {
                std::cerr << "material loop error: output name must be non-empty ASCII\n";
                return 2;
            }
            output_name = std::move(*parsed);
        }
        double tolerance = nodes::kDefaultLoopContinuityTolerance;
        if (argc == 7) {
            auto parsed = parse_unit_double(argv[6]);
            if (!parsed) {
                std::cerr << "material loop error: tolerance must be within [0,1]\n";
                return 2;
            }
            tolerance = *parsed;
        }
        auto validation = nodes::validate_workflow_loop(*recipe, *length, output_name, tolerance);
        if (validation.is_error()) {
            std::cerr << "material loop validation error: " << validation.error().message << '\n';
            return 7;
        }
        std::cout << std::setprecision(17)
                  << "loop-length " << validation.value().loop_length << '\n'
                  << "endpoint-error " << validation.value().endpoint_error << '\n'
                  << "transition-error " << validation.value().transition_error << '\n'
                  << "tolerance " << validation.value().tolerance << '\n'
                  << "stateful " << (validation.value().contains_stateful_nodes ? "yes" : "no") << '\n'
                  << "reason " << validation.value().reason << '\n'
                  << "loop " << (validation.value().validated ? "VALIDATED" : "NOT-VALIDATED") << '\n';
        return validation.value().validated ? 0 : 9;
    }

    if (action == L"ui") {
        if (argc != 4 && argc != 5) {
            print_usage();
            return 2;
        }
        auto recipe = load_recipe(std::filesystem::path(argv[3]));
        if (!recipe) {
            return 6;
        }
        std::string output_name = "main";
        if (argc == 5) {
            auto parsed = parse_ascii(argv[4]);
            if (!parsed) {
                std::cerr << "material ui error: output name must be non-empty ASCII\n";
                return 2;
            }
            output_name = std::move(*parsed);
        }
        return run_material_inspection_application(*recipe, std::move(output_name));
    }

    if (action == L"export") {
        if (argc != 7) {
            print_usage();
            return 2;
        }
        auto recipe = load_recipe(std::filesystem::path(argv[3]));
        auto output_name = parse_ascii(argv[4]);
        auto format = parse_format(argv[6]);
        if (!recipe || !output_name || !format) {
            std::cerr << "material export error: require ASCII output name and png, bmp, or rgba format\n";
            return 2;
        }
        exporting::ExportRequest request;
        request.kind = exporting::ExportKind::still;
        request.raster_format = *format;
        request.destination_directory = std::filesystem::path(argv[5]);
        request.stem = "material";
        request.output_name = std::move(*output_name);
        return finish_export(*recipe, std::move(request));
    }

    if (action == L"loop-sequence") {
        if (argc != 7 && argc != 8) {
            print_usage();
            return 2;
        }
        auto recipe = load_recipe(std::filesystem::path(argv[3]));
        auto length = parse_u64(argv[4]);
        auto format = parse_format(argv[6]);
        std::string output_name = "main";
        if (argc == 8) {
            auto parsed = parse_ascii(argv[7]);
            if (!parsed) {
                std::cerr << "material loop export error: output name must be non-empty ASCII\n";
                return 2;
            }
            output_name = std::move(*parsed);
        }
        if (!recipe || !length || *length < 2U || *length > exporting::kMaximumExportFrames || !format) {
            std::cerr << "material loop export error: loop length must be 2..256 and format png, bmp, or rgba\n";
            return 2;
        }
        exporting::ExportRequest request;
        request.kind = exporting::ExportKind::sequence;
        request.raster_format = *format;
        request.destination_directory = std::filesystem::path(argv[5]);
        request.stem = "loop";
        request.output_name = std::move(output_name);
        request.start_tick = 0U;
        request.end_tick = *length - 1U;
        request.loop_length = *length;
        request.require_validated_loop = true;
        return finish_export(*recipe, std::move(request));
    }

    print_usage();
    return 2;
}

}  // namespace artminer::app
