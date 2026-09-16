#include <cerrno>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "app/browser_window.hpp"
#include "app/lineage_window.hpp"
#include "app/playback_window.hpp"
#include "core/graph.hpp"
#include "core/recipe.hpp"
#include "core/version.hpp"
#include "export/windows/wic_png.hpp"
#include "nodes/motion_evaluator.hpp"
#include "nodes/static_evaluator.hpp"
#include "platform/windows/portable_workspace.hpp"

namespace {

using artminer::platform::windows::PortableWorkspace;
using artminer::platform::windows::WorkspaceError;

struct CommandLine {
    bool show_help{false};
    bool show_version{false};
    bool check_workspace{false};
    std::optional<std::filesystem::path> workspace;
    std::optional<std::filesystem::path> open_recipe;
};

void print_help() {
    std::cout
        << "Usage: ArtMiner [options]\n"
        << "       ArtMiner recipe validate <file.amr>\n"
        << "       ArtMiner recipe inspect <file.amr>\n"
        << "       ArtMiner render <file.amr> <output.png>\n"
        << "       ArtMiner render-tick <file.amr> <tick> <output.png>\n"
        << "       ArtMiner render-range <file.amr> <start> <end> <output-dir>\n"
        << "       ArtMiner animate <file.amr>\n"
        << "       ArtMiner lineage [parent-a.amr] [parent-b.amr]\n\n"
        << "Options:\n"
        << "  --help, -h             Show this help text.\n"
        << "  --version              Show product/version information.\n"
        << "  --workspace <path>     Use an explicit portable workspace root.\n"
        << "  --check-workspace      Validate/create the workspace layout and exit.\n"
        << "  --open <file.amr>      Open a recipe in the specimen browser.\n\n"
        << "Static render uses the AM-003 canonical CPU path. render-tick and render-range\n"
        << "use the AM-007 canonical fixed-tick motion/feedback path. PNG renders are\n"
        << "accompanied by deterministic provenance sidecars. animate opens the native\n"
        << "pause/play, single-step, reset and preview-speed inspector; speed changes\n"
        << "wall-clock playback only and never changes the requested simulation tick.\n"
        << "lineage opens the AM-008 ordered two-parent breeder and portable ancestry browser.\n\n"
        << "Without a command ArtMiner opens the native 4x4 specimen browser.\n"
        << "Mutation and seed-only variation are deterministic from explicit seeds.\n"
        << "Main shortcuts: M mutate, N seed variants, F favourite, arrow keys select,\n"
        << "Enter choose parent, Alt+Left/Right history, Ctrl+O open, Ctrl+S save.\n";
}

void print_version() {
    std::cout << artminer::core::kProductName << ' ' << artminer::core::kVersion
              << " (" << artminer::core::kVersionDetail << ")\n";
}

[[nodiscard]] bool parse_command_line(const int argc, wchar_t* argv[], CommandLine& output) {
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--help" || argument == L"-h" || argument == L"/?") {
            output.show_help = true;
        } else if (argument == L"--version") {
            output.show_version = true;
        } else if (argument == L"--check-workspace") {
            output.check_workspace = true;
        } else if (argument == L"--workspace") {
            if (index + 1 >= argc) {
                std::wcerr << L"error: --workspace requires a path\n";
                return false;
            }
            output.workspace = std::filesystem::path(argv[++index]);
        } else if (argument == L"--open") {
            if (index + 1 >= argc) {
                std::wcerr << L"error: --open requires a .amr path\n";
                return false;
            }
            output.open_recipe = std::filesystem::path(argv[++index]);
        } else {
            std::wcerr << L"error: unknown argument: " << argument << L"\n";
            return false;
        }
    }
    return true;
}

void print_workspace_error(const WorkspaceError& error) {
    std::wcerr << L"workspace error: " << error.message;
    if (error.system_error) {
        std::wcerr << L" (system error " << error.system_error.value() << L')';
    }
    std::wcerr << L'\n';
}

[[nodiscard]] std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::wcerr << L"recipe error: could not open " << path.wstring() << L'\n';
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
        std::wcerr << L"recipe error: failed while reading " << path.wstring() << L'\n';
        return std::nullopt;
    }
    return text;
}

void print_recipe_parse_error(const artminer::core::RecipeError& error) {
    std::cerr << "recipe parse error";
    if (error.line != 0U) {
        std::cerr << " at line " << error.line;
    }
    std::cerr << ": " << error.message << '\n';
}

[[nodiscard]] std::optional<artminer::core::Recipe> load_validated_recipe(const std::filesystem::path& path) {
    const auto text = read_text_file(path);
    if (!text.has_value()) {
        return std::nullopt;
    }
    auto parsed = artminer::core::parse_recipe(*text);
    if (parsed.is_error()) {
        print_recipe_parse_error(parsed.error());
        return std::nullopt;
    }
    artminer::core::Recipe recipe = std::move(parsed).value();
    const auto validation_errors = artminer::core::validate_recipe(recipe);
    if (!validation_errors.empty()) {
        for (const auto& error : validation_errors) {
            std::cerr << "recipe validation error: " << error.message << '\n';
        }
        return std::nullopt;
    }
    return recipe;
}

[[nodiscard]] std::optional<artminer::core::u64> parse_tick(const wchar_t* text) {
    if (text == nullptr || *text == L'\0' || *text == L'-') {
        return std::nullopt;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long long value = std::wcstoull(text, &end, 10);
    if (errno == ERANGE || end == text || end == nullptr || *end != L'\0') {
        return std::nullopt;
    }
    return static_cast<artminer::core::u64>(value);
}

[[nodiscard]] int run_recipe_command(const int argc, wchar_t* argv[]) {
    if (argc != 4) {
        std::cerr << "usage: ArtMiner recipe <validate|inspect> <file.amr>\n";
        return 2;
    }
    const std::wstring_view action(argv[2]);
    if (action != L"validate" && action != L"inspect") {
        std::wcerr << L"recipe error: unknown recipe action: " << action << L'\n';
        return 2;
    }
    auto recipe = load_validated_recipe(std::filesystem::path(argv[3]));
    if (!recipe.has_value()) {
        return 6;
    }
    const std::string fingerprint = artminer::core::semantic_fingerprint(*recipe);
    if (action == L"validate") {
        std::cout << "valid " << fingerprint << '\n';
        return 0;
    }
    std::cout << "ArtMiner recipe\n"
              << "schema: " << recipe->schema_version << '\n'
              << "evaluator: " << recipe->evaluator_version << '\n'
              << "seed: " << recipe->root_seed << '\n'
              << "render: " << recipe->render.width << 'x' << recipe->render.height << ' ' << recipe->render.quality << '\n'
              << "nodes: " << recipe->nodes.size() << '\n'
              << "edges: " << recipe->edges.size() << '\n'
              << "outputs: " << recipe->outputs.size() << '\n'
              << "metadata: " << recipe->metadata.size() << '\n'
              << "fingerprint: " << fingerprint << '\n';
    return 0;
}

[[nodiscard]] int run_render_command(const int argc, wchar_t* argv[]) {
    if (argc != 4) {
        std::cerr << "usage: ArtMiner render <file.amr> <output.png>\n";
        return 2;
    }
    const std::filesystem::path recipe_path(argv[2]);
    const std::filesystem::path output_path(argv[3]);
    auto recipe = load_validated_recipe(recipe_path);
    if (!recipe.has_value()) {
        return 6;
    }
    auto rendered = artminer::nodes::render_reference(*recipe);
    if (rendered.is_error()) {
        std::cerr << "render error: " << rendered.error().message << '\n';
        return 7;
    }
    artminer::nodes::Image image = std::move(rendered).value();
    auto written = artminer::exporting::windows::write_png_with_provenance(output_path, image, *recipe);
    if (written.is_error()) {
        std::cerr << "export error: " << written.error().message << '\n';
        return 8;
    }
    const auto sidecar = artminer::exporting::windows::provenance_sidecar_path(output_path);
    const std::string image_hash = artminer::nodes::image_fingerprint(image);
    const std::string recipe_hash = artminer::core::semantic_fingerprint(*recipe);
    const std::wstring image_hash_wide(image_hash.begin(), image_hash.end());
    const std::wstring recipe_hash_wide(recipe_hash.begin(), recipe_hash.end());
    std::wcout << L"rendered " << output_path.wstring() << L" " << image.width << L"x" << image.height
               << L" image-hash " << image_hash_wide
               << L" recipe " << recipe_hash_wide
               << L" provenance " << sidecar.wstring() << L'\n';
    return 0;
}

[[nodiscard]] int write_animation_frame(
    const artminer::core::Recipe& recipe,
    const artminer::core::u64 tick,
    const std::filesystem::path& output_path,
    artminer::nodes::FrameSnapshotCache* cache) {
    auto rendered = artminer::nodes::render_animation_reference(recipe, tick, "main", cache);
    if (rendered.is_error()) {
        std::cerr << "animation render error at tick " << tick << ": " << rendered.error().message << '\n';
        return 7;
    }
    artminer::nodes::Image image = std::move(rendered).value();
    artminer::core::Recipe provenance_recipe = recipe;
    provenance_recipe.metadata.push_back({"render.tick", std::to_string(tick)});
    auto written = artminer::exporting::windows::write_png_with_provenance(output_path, image, provenance_recipe);
    if (written.is_error()) {
        std::cerr << "export error: " << written.error().message << '\n';
        return 8;
    }
    const std::string image_hash = artminer::nodes::image_fingerprint(image);
    const std::string recipe_hash = artminer::core::semantic_fingerprint(recipe);
    const std::wstring image_hash_wide(image_hash.begin(), image_hash.end());
    const std::wstring recipe_hash_wide(recipe_hash.begin(), recipe_hash.end());
    std::wcout << L"rendered tick " << tick << L" " << output_path.wstring() << L" "
               << image.width << L"x" << image.height << L" image-hash " << image_hash_wide
               << L" recipe " << recipe_hash_wide << L'\n';
    return 0;
}

[[nodiscard]] int run_render_tick_command(const int argc, wchar_t* argv[]) {
    if (argc != 5) {
        std::cerr << "usage: ArtMiner render-tick <file.amr> <tick> <output.png>\n";
        return 2;
    }
    const auto tick = parse_tick(argv[3]);
    if (!tick.has_value()) {
        std::cerr << "render-tick error: tick must be an unsigned integer\n";
        return 2;
    }
    auto recipe = load_validated_recipe(std::filesystem::path(argv[2]));
    if (!recipe.has_value()) {
        return 6;
    }
    artminer::nodes::FrameSnapshotCache cache(8U);
    return write_animation_frame(*recipe, *tick, std::filesystem::path(argv[4]), &cache);
}

[[nodiscard]] std::filesystem::path range_frame_path(
    const std::filesystem::path& directory,
    const artminer::core::u64 tick) {
    std::wostringstream name;
    name << L"tick-" << std::setw(10) << std::setfill(L'0') << tick << L".png";
    return directory / name.str();
}

[[nodiscard]] int run_render_range_command(const int argc, wchar_t* argv[]) {
    if (argc != 6) {
        std::cerr << "usage: ArtMiner render-range <file.amr> <start> <end> <output-dir>\n";
        return 2;
    }
    const auto start = parse_tick(argv[3]);
    const auto end = parse_tick(argv[4]);
    if (!start.has_value() || !end.has_value() || *end < *start || *end - *start >= 256U) {
        std::cerr << "render-range error: require unsigned start <= end with at most 256 inclusive frames\n";
        return 2;
    }
    auto recipe = load_validated_recipe(std::filesystem::path(argv[2]));
    if (!recipe.has_value()) {
        return 6;
    }
    const std::filesystem::path output_directory(argv[5]);
    std::error_code directory_error;
    std::filesystem::create_directories(output_directory, directory_error);
    if (directory_error) {
        std::cerr << "render-range error: could not create output directory\n";
        return 8;
    }

    artminer::nodes::FrameSnapshotCache cache(32U);
    for (artminer::core::u64 tick = *start;; ++tick) {
        const int status = write_animation_frame(*recipe, tick, range_frame_path(output_directory, tick), &cache);
        if (status != 0) {
            return status;
        }
        if (tick == *end) {
            break;
        }
    }
    return 0;
}

[[nodiscard]] int run_animate_command(const int argc, wchar_t* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: ArtMiner animate <file.amr>\n";
        return 2;
    }
    auto recipe = load_validated_recipe(std::filesystem::path(argv[2]));
    if (!recipe.has_value()) {
        return 6;
    }
    return artminer::app::run_playback_application(*recipe);
}

[[nodiscard]] int run_lineage_command(const int argc, wchar_t* argv[]) {
    if (argc < 2 || argc > 4) {
        std::cerr << "usage: ArtMiner lineage [parent-a.amr] [parent-b.amr]\n";
        return 2;
    }
    auto workspace_result = PortableWorkspace::from_executable();
    if (workspace_result.is_error()) {
        print_workspace_error(workspace_result.error());
        return 4;
    }
    PortableWorkspace workspace = std::move(workspace_result).value();
    auto layout_result = workspace.ensure_layout();
    if (layout_result.is_error()) {
        print_workspace_error(layout_result.error());
        return 4;
    }
    const std::optional<std::filesystem::path> parent_a = argc >= 3
        ? std::optional<std::filesystem::path>{std::filesystem::path(argv[2])}
        : std::nullopt;
    const std::optional<std::filesystem::path> parent_b = argc >= 4
        ? std::optional<std::filesystem::path>{std::filesystem::path(argv[3])}
        : std::nullopt;
    return artminer::app::run_lineage_application(workspace.layout(), parent_a, parent_b);
}

}  // namespace

int wmain(const int argc, wchar_t* argv[]) {
    static_assert(sizeof(void*) == 8, "ArtMiner requires an x64 process.");

    if (argc >= 2 && std::wstring_view(argv[1]) == L"recipe") {
        return run_recipe_command(argc, argv);
    }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"render") {
        return run_render_command(argc, argv);
    }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"render-tick") {
        return run_render_tick_command(argc, argv);
    }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"render-range") {
        return run_render_range_command(argc, argv);
    }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"animate") {
        return run_animate_command(argc, argv);
    }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"lineage") {
        return run_lineage_command(argc, argv);
    }

    CommandLine command_line;
    if (!parse_command_line(argc, argv, command_line)) {
        print_help();
        return 2;
    }
    if (command_line.show_help) {
        print_help();
        return 0;
    }
    if (command_line.show_version) {
        print_version();
        return 0;
    }

    auto workspace_result = PortableWorkspace::from_executable(command_line.workspace);
    if (workspace_result.is_error()) {
        print_workspace_error(workspace_result.error());
        return 4;
    }
    PortableWorkspace workspace = std::move(workspace_result).value();
    auto layout_result = workspace.ensure_layout();
    if (layout_result.is_error()) {
        print_workspace_error(layout_result.error());
        return 4;
    }
    if (command_line.check_workspace) {
        std::wcout << L"workspace ready: " << workspace.layout().root.wstring() << L'\n';
        return 0;
    }

    return artminer::app::run_browser_application(workspace.layout(), command_line.open_recipe);
}
