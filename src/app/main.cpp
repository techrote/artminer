#include <Windows.h>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/version.hpp"
#include "platform/windows/portable_workspace.hpp"

namespace {

using artminer::platform::windows::PortableWorkspace;
using artminer::platform::windows::WorkspaceError;

struct CommandLine {
    bool show_help{false};
    bool show_version{false};
    bool check_workspace{false};
    std::optional<std::filesystem::path> workspace;
};

struct AppState {
    std::wstring workspace_root;
};

void print_help() {
    std::cout
        << "Usage: ArtMiner [options]\n\n"
        << "Options:\n"
        << "  --help, -h             Show this help text.\n"
        << "  --version              Show product/version information.\n"
        << "  --workspace <path>     Use an explicit portable workspace root.\n"
        << "  --check-workspace      Validate/create the workspace layout and exit.\n\n"
        << "Without options ArtMiner validates its portable workspace and opens the\n"
        << "minimal AM-001 native application shell.\n";
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

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC device_context = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(device_context, &client, GetSysColorBrush(COLOR_WINDOW));

        std::wstring text = L"ArtMiner 0.1.0-dev\r\n\r\nAM-001 native foundation is running.\r\n\r\nPortable workspace:\r\n";
        if (state != nullptr) {
            text += state->workspace_root;
        }
        text += L"\r\n\r\nClose this window to exit.";

        client.left += 24;
        client.top += 24;
        client.right -= 24;
        client.bottom -= 24;
        DrawTextW(device_context, text.c_str(), -1, &client, DT_LEFT | DT_TOP | DT_WORDBREAK);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, w_param, l_param);
    }
}

[[nodiscard]] int run_native_shell(const std::filesystem::path& workspace_root) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (instance == nullptr) {
        std::wcerr << L"error: GetModuleHandleW failed\n";
        return 3;
    }

    constexpr wchar_t kWindowClass[] = L"ArtMinerBootstrapWindow";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    window_class.lpszClassName = kWindowClass;

    if (RegisterClassW(&window_class) == 0) {
        std::wcerr << L"error: RegisterClassW failed with code " << GetLastError() << L'\n';
        return 3;
    }

    AppState state{workspace_root.wstring()};
    HWND window = CreateWindowExW(
        0,
        kWindowClass,
        L"ArtMiner — AM-001 Bootstrap",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        760,
        420,
        nullptr,
        nullptr,
        instance,
        &state);

    if (window == nullptr) {
        std::wcerr << L"error: CreateWindowExW failed with code " << GetLastError() << L'\n';
        return 3;
    }

    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);

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

    return run_native_shell(workspace.layout().root);
}
