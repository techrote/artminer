#include "app/glyph_preview_window.hpp"

#include <windows.h>

#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "nodes/glyph_synthesis.hpp"

namespace artminer::app {
namespace {

constexpr wchar_t kWindowClassName[] = L"ArtMinerGlyphPreviewWindow";
constexpr int kNoticeHeight = 44;

struct PreviewState final {
    std::wstring text;
    HWND notice{nullptr};
    HWND edit{nullptr};
    HFONT font{nullptr};
};

[[nodiscard]] std::optional<std::wstring> utf8_to_wide(const std::string_view text) {
    if (text.empty()) {
        return std::wstring{};
    }
    if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    const int source_length = static_cast<int>(text.size());
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), source_length, nullptr, 0);
    if (required <= 0) {
        return std::nullopt;
    }
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text.data(),
            source_length,
            wide.data(),
            required) != required) {
        return std::nullopt;
    }
    return wide;
}

LRESULT CALLBACK preview_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<PreviewState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<PreviewState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_CREATE:
        if (state == nullptr) {
            return -1;
        }
        state->notice = CreateWindowExW(
            0,
            L"STATIC",
            L"Canonical result: UTF-8 glyph cell sequence. Font metrics, fallback and rasterization in this preview are noncanonical.",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            8,
            8,
            900,
            32,
            window,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        state->edit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            state->text.c_str(),
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
            8,
            kNoticeHeight,
            900,
            620,
            window,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        if (state->notice == nullptr || state->edit == nullptr) {
            return -1;
        }
        state->font = CreateFontW(
            18,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            FIXED_PITCH | FF_MODERN,
            L"Consolas");
        if (state->font != nullptr) {
            SendMessageW(state->edit, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
        }
        return 0;
    case WM_SIZE:
        if (state != nullptr) {
            const int width = LOWORD(lparam);
            const int height = HIWORD(lparam);
            if (state->notice != nullptr) {
                MoveWindow(state->notice, 8, 8, (std::max)(0, width - 16), 32, TRUE);
            }
            if (state->edit != nullptr) {
                MoveWindow(
                    state->edit,
                    8,
                    kNoticeHeight,
                    (std::max)(0, width - 16),
                    (std::max)(0, height - kNoticeHeight - 8),
                    TRUE);
            }
        }
        return 0;
    case WM_DESTROY:
        if (state != nullptr && state->font != nullptr) {
            DeleteObject(state->font);
            state->font = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

[[nodiscard]] bool register_preview_class(const HINSTANCE instance) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = preview_window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&window_class) != 0U) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

}  // namespace

int run_glyph_preview_application(
    const core::Recipe& recipe,
    const std::string_view settings_node_id,
    const std::optional<core::u64> tick) {
    auto grid = nodes::synthesize_recipe_glyph_grid(recipe, settings_node_id, tick);
    if (grid.is_error()) {
        MessageBoxA(nullptr, grid.error().message.c_str(), "ArtMiner glyph preview error", MB_OK | MB_ICONERROR);
        return 7;
    }
    auto serialized = nodes::serialize_glyph_utf8(grid.value());
    if (serialized.is_error()) {
        MessageBoxA(nullptr, serialized.error().message.c_str(), "ArtMiner glyph preview error", MB_OK | MB_ICONERROR);
        return 7;
    }
    auto wide = utf8_to_wide(serialized.value());
    if (!wide.has_value()) {
        MessageBoxW(nullptr, L"Canonical UTF-8 glyph output could not be converted for Windows preview.",
                    L"ArtMiner glyph preview error", MB_OK | MB_ICONERROR);
        return 7;
    }

    PreviewState state;
    state.text = std::move(*wide);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!register_preview_class(instance)) {
        MessageBoxW(nullptr, L"Could not register the glyph preview window class.",
                    L"ArtMiner glyph preview error", MB_OK | MB_ICONERROR);
        return 5;
    }

    std::wstring title = L"ArtMiner Glyph Preview — ";
    title += std::to_wstring(grid.value().columns);
    title += L"x";
    title += std::to_wstring(grid.value().rows);
    if (tick.has_value()) {
        title += L" — tick ";
        title += std::to_wstring(*tick);
    }

    const HWND window = CreateWindowExW(
        0,
        kWindowClassName,
        title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1000,
        720,
        nullptr,
        nullptr,
        instance,
        &state);
    if (window == nullptr) {
        MessageBoxW(nullptr, L"Could not create the glyph preview window.",
                    L"ArtMiner glyph preview error", MB_OK | MB_ICONERROR);
        return 5;
    }

    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);
    MSG message{};
    while (true) {
        const BOOL status = GetMessageW(&message, nullptr, 0U, 0U);
        if (status == 0) {
            return static_cast<int>(message.wParam);
        }
        if (status == -1) {
            return 5;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

}  // namespace artminer::app
