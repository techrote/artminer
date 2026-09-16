#include <Windows.h>
#include <commdlg.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/graph.hpp"
#include "core/recipe.hpp"
#include "core/version.hpp"
#include "export/windows/wic_png.hpp"
#include "gpu/d3d11_preview.hpp"
#include "nodes/static_evaluator.hpp"
#include "platform/windows/portable_workspace.hpp"

namespace {

using artminer::platform::windows::PortableWorkspace;
using artminer::platform::windows::WorkspaceError;

constexpr UINT_PTR kPreviewTimerId = 1U;
constexpr UINT kPreviewTimerMilliseconds = 16U;
constexpr int kStatusHeight = 28;
constexpr UINT kCommandOpen = 1001U;
constexpr UINT kCommandExit = 1002U;

struct CommandLine {
    bool show_help{false};
    bool show_version{false};
    bool check_workspace{false};
    std::optional<std::filesystem::path> workspace;
    std::optional<std::filesystem::path> open_recipe;
};

struct AppState {
    std::wstring workspace_root;
    HWND main_window{nullptr};
    HWND preview_window{nullptr};
    HWND status_window{nullptr};
    artminer::gpu::D3d11Preview preview;
    std::optional<artminer::core::Recipe> recipe;
    std::filesystem::path recipe_path;
    std::wstring status_base{L"File > Open... to load an ArtMiner recipe."};
    bool preview_error_shown{false};
};

void print_help() {
    std::cout
        << "Usage: ArtMiner [options]\n"
        << "       ArtMiner recipe validate <file.amr>\n"
        << "       ArtMiner recipe inspect <file.amr>\n"
        << "       ArtMiner render <file.amr> <output.png>\n\n"
        << "Options:\n"
        << "  --help, -h             Show this help text.\n"
        << "  --version              Show product/version information.\n"
        << "  --workspace <path>     Use an explicit portable workspace root.\n"
        << "  --check-workspace      Validate/create the workspace layout and exit.\n"
        << "  --open <file.amr>      Open a recipe in the interactive D3D11 preview.\n\n"
        << "Headless recipe/render commands use the canonical CPU reference path and\n"
        << "do not open the GUI. PNG renders are accompanied by deterministic\n"
        << "<output>.artminer.txt provenance containing the complete recipe.\n\n"
        << "Without a headless command ArtMiner opens the native D3D11 preview.\n"
        << "GPU-supported pointwise graphs use the accelerated preview evaluator;\n"
        << "unsupported graphs are rendered by the unchanged canonical CPU path and\n"
        << "uploaded for display with the fallback stated explicitly in the UI.\n";
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
            ++index;
            output.workspace = std::filesystem::path(argv[index]);
        } else if (argument == L"--open") {
            if (index + 1 >= argc) {
                std::wcerr << L"error: --open requires a .amr path\n";
                return false;
            }
            ++index;
            output.open_recipe = std::filesystem::path(argv[index]);
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

[[nodiscard]] std::wstring widen_utf8(const std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (length <= 0) {
        return std::wstring(text.begin(), text.end());
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        length);
    return result;
}

void layout_children(AppState& state, const int width, const int height) {
    if (state.preview_window == nullptr || state.status_window == nullptr) {
        return;
    }
    const int preview_height = (std::max)(0, height - kStatusHeight);
    MoveWindow(state.preview_window, 0, 0, (std::max)(0, width), preview_height, TRUE);
    MoveWindow(state.status_window, 0, preview_height, (std::max)(0, width), kStatusHeight, TRUE);
    if (state.preview.initialized() && width > 0 && preview_height > 0) {
        auto resized = state.preview.resize(static_cast<artminer::core::u32>(width), static_cast<artminer::core::u32>(preview_height));
        if (resized.is_error()) {
            state.status_base = L"D3D11 resize error: " + widen_utf8(resized.error().message);
        }
    }
}

void update_status(AppState& state) {
    if (state.status_window == nullptr) {
        return;
    }
    std::wstring text = state.status_base;
    if (state.preview.initialized()) {
        const auto diagnostics = state.preview.diagnostics();
        text += L" | device ";
        text += diagnostics.warp_device ? L"WARP" : L"hardware";
        text += L" | frames " + std::to_wstring(diagnostics.presented_frames);
        text += L" | present " + std::to_wstring(diagnostics.last_present_milliseconds) + L" ms";
    }
    SetWindowTextW(state.status_window, text.c_str());
}

[[nodiscard]] bool load_recipe_into_preview(AppState& state, const std::filesystem::path& path) {
    auto loaded = load_validated_recipe(path);
    if (!loaded.has_value()) {
        state.status_base = L"Recipe load/validation failed: " + path.wstring();
        update_status(state);
        return false;
    }

    const std::string fingerprint_before = artminer::core::semantic_fingerprint(*loaded);
    auto preview_status = state.preview.set_recipe(*loaded);
    if (preview_status.is_error()) {
        state.status_base = L"Preview error: " + widen_utf8(preview_status.error().message);
        update_status(state);
        return false;
    }
    const std::string fingerprint_after = artminer::core::semantic_fingerprint(*loaded);
    if (fingerprint_before != fingerprint_after) {
        state.status_base = L"Internal error: preview mutated recipe semantics.";
        update_status(state);
        return false;
    }

    state.recipe = std::move(*loaded);
    state.recipe_path = path;
    const auto& status = preview_status.value();
    state.status_base = path.filename().wstring() + L" | " +
        std::to_wstring(state.recipe->render.width) + L"x" + std::to_wstring(state.recipe->render.height) +
        L" | recipe " + widen_utf8(fingerprint_before) + L" | " + widen_utf8(status.message);
    update_status(state);

    std::wstring title = L"ArtMiner — ";
    title += path.filename().wstring();
    title += status.path == artminer::gpu::PreviewPath::gpu ? L" — GPU Preview" : L" — Canonical CPU Fallback";
    SetWindowTextW(state.main_window, title.c_str());
    return true;
}

[[nodiscard]] std::optional<std::filesystem::path> choose_recipe_file(const AppState& state) {
    std::vector<wchar_t> path_buffer(32768U, L'\0');
    constexpr wchar_t filter[] = L"ArtMiner Recipes (*.amr)\0*.amr\0All Files (*.*)\0*.*\0\0";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = state.main_window;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path_buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(path_buffer.size());
    const std::filesystem::path initial = std::filesystem::path(state.workspace_root) / L"recipes";
    const std::wstring initial_text = initial.wstring();
    dialog.lpstrInitialDir = initial_text.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrDefExt = L"amr";
    if (GetOpenFileNameW(&dialog) == FALSE) {
        return std::nullopt;
    }
    return std::filesystem::path(path_buffer.data());
}

[[nodiscard]] HMENU create_main_menu() {
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    if (menu == nullptr || file == nullptr) {
        if (file != nullptr) {
            DestroyMenu(file);
        }
        if (menu != nullptr) {
            DestroyMenu(menu);
        }
        return nullptr;
    }
    AppendMenuW(file, MF_STRING, kCommandOpen, L"&Open Recipe...\tCtrl+O");
    AppendMenuW(file, MF_SEPARATOR, 0U, nullptr);
    AppendMenuW(file, MF_STRING, kCommandExit, L"E&xit");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    return menu;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_SIZE:
        if (state != nullptr) {
            layout_children(*state, LOWORD(l_param), HIWORD(l_param));
        }
        return 0;
    case WM_COMMAND:
        if (state == nullptr) {
            return 0;
        }
        if (LOWORD(w_param) == kCommandOpen) {
            if (auto path = choose_recipe_file(*state); path.has_value()) {
                load_recipe_into_preview(*state, *path);
            }
            return 0;
        }
        if (LOWORD(w_param) == kCommandExit) {
            DestroyWindow(window);
            return 0;
        }
        return 0;
    case WM_KEYDOWN:
        if (state != nullptr && w_param == static_cast<WPARAM>('O') && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            if (auto path = choose_recipe_file(*state); path.has_value()) {
                load_recipe_into_preview(*state, *path);
            }
            return 0;
        }
        break;
    case WM_TIMER:
        if (state != nullptr && w_param == kPreviewTimerId && state->preview.initialized()) {
            auto presented = state->preview.present();
            if (presented.is_error()) {
                state->status_base = L"D3D11 present/recovery error: " + widen_utf8(presented.error().message);
                if (!state->preview_error_shown) {
                    state->preview_error_shown = true;
                    update_status(*state);
                }
            } else {
                state->preview_error_shown = false;
                const auto diagnostics = state->preview.diagnostics();
                if ((diagnostics.presented_frames % 30U) == 0U) {
                    update_status(*state);
                }
            }
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kPreviewTimerId);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

[[nodiscard]] int run_native_shell(
    const std::filesystem::path& workspace_root,
    const std::optional<std::filesystem::path>& initial_recipe) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (instance == nullptr) {
        std::wcerr << L"error: GetModuleHandleW failed\n";
        return 3;
    }

    constexpr wchar_t kWindowClass[] = L"ArtMinerPreviewWindow";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    window_class.lpszClassName = kWindowClass;

    if (RegisterClassW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        std::wcerr << L"error: RegisterClassW failed with code " << GetLastError() << L'\n';
        return 3;
    }

    AppState state;
    state.workspace_root = workspace_root.wstring();
    HMENU menu = create_main_menu();
    HWND window = CreateWindowExW(
        0,
        kWindowClass,
        L"ArtMiner — AM-004 D3D11 Preview",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        980,
        760,
        nullptr,
        menu,
        instance,
        &state);

    if (window == nullptr) {
        if (menu != nullptr) {
            DestroyMenu(menu);
        }
        std::wcerr << L"error: CreateWindowExW failed with code " << GetLastError() << L'\n';
        return 3;
    }
    state.main_window = window;

    state.preview_window = CreateWindowExW(
        0,
        L"STATIC",
        nullptr,
        WS_CHILD | WS_VISIBLE | SS_BLACKRECT,
        0,
        0,
        1,
        1,
        window,
        nullptr,
        instance,
        nullptr);
    state.status_window = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"STATIC",
        state.status_base.c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        0,
        0,
        1,
        kStatusHeight,
        window,
        nullptr,
        instance,
        nullptr);
    if (state.preview_window == nullptr || state.status_window == nullptr) {
        std::wcerr << L"error: failed to create native preview/status controls\n";
        DestroyWindow(window);
        return 3;
    }
    SendMessageW(state.status_window, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);

    RECT client{};
    GetClientRect(window, &client);
    layout_children(state, client.right - client.left, client.bottom - client.top);

    auto initialized = state.preview.initialize(state.preview_window);
    if (initialized.is_error()) {
        const std::wstring message = L"D3D11 initialization failed:\r\n" + widen_utf8(initialized.error().message);
        MessageBoxW(window, message.c_str(), L"ArtMiner D3D11 Error", MB_OK | MB_ICONERROR);
        DestroyWindow(window);
        return 5;
    }

    GetClientRect(state.preview_window, &client);
    auto resized = state.preview.resize(
        static_cast<artminer::core::u32>((std::max)(client.right - client.left, 1L)),
        static_cast<artminer::core::u32>((std::max)(client.bottom - client.top, 1L)));
    if (resized.is_error()) {
        std::cerr << "preview resize error: " << resized.error().message << '\n';
    }

    if (initial_recipe.has_value()) {
        load_recipe_into_preview(state, *initial_recipe);
    } else {
        update_status(state);
    }

    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);
    SetTimer(window, kPreviewTimerId, kPreviewTimerMilliseconds, nullptr);

    MSG message{};
    while (true) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result == 0) {
            return static_cast<int>(message.wParam);
        }
        if (result == -1) {
            std::wcerr << L"error: GetMessageW failed with code " << GetLastError() << L'\n';
            return 3;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
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

    return run_native_shell(workspace.layout().root, command_line.open_recipe);
}
