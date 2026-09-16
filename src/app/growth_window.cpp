#include "app/growth_window.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cwchar>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "nodes/static_evaluator.hpp"

namespace artminer::app {
namespace {

constexpr wchar_t kWindowClass[] = L"ArtMinerGrowthInspector";
constexpr int kTickEditId = 101;
constexpr int kRenderButtonId = 102;
constexpr int kResetButtonId = 103;

struct WindowState final {
    core::Recipe recipe;
    core::u64 tick{0U};
    nodes::Image image;
    std::vector<core::u8> bgra;
    HWND tick_edit{nullptr};
    HWND status{nullptr};
};

[[nodiscard]] bool parse_tick(const wchar_t* text, core::u64& output) noexcept {
    if (text == nullptr || *text == L'\0' || *text == L'-') {
        return false;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long long value = std::wcstoull(text, &end, 10);
    if (errno == ERANGE || end == text || end == nullptr || *end != L'\0') {
        return false;
    }
    output = static_cast<core::u64>(value);
    return true;
}

void set_status(WindowState& state, const std::wstring& text) {
    SetWindowTextW(state.status, text.c_str());
}

void convert_preview(WindowState& state) {
    state.bgra.resize(state.image.rgba.size());
    for (std::size_t offset = 0U; offset + 3U < state.image.rgba.size(); offset += 4U) {
        state.bgra[offset] = state.image.rgba[offset + 2U];
        state.bgra[offset + 1U] = state.image.rgba[offset + 1U];
        state.bgra[offset + 2U] = state.image.rgba[offset];
        state.bgra[offset + 3U] = state.image.rgba[offset + 3U];
    }
}

void render_tick(HWND window, WindowState& state, const core::u64 tick) {
    auto rendered = nodes::render_reference_at_tick(state.recipe, tick);
    if (rendered.is_error()) {
        std::wstring message = L"Render error: ";
        const std::string& narrow = rendered.error().message;
        for (const unsigned char value : narrow) {
            message.push_back(static_cast<wchar_t>(value));
        }
        set_status(state, message);
        return;
    }
    state.tick = tick;
    state.image = std::move(rendered).value();
    convert_preview(state);
    std::wstring status = L"Canonical CPU tick ";
    status += std::to_wstring(tick);
    status += L" — reset/replay is authoritative; preview cadence is not simulation time.";
    set_status(state, status);
    InvalidateRect(window, nullptr, FALSE);
}

void render_from_edit(HWND window, WindowState& state) {
    std::array<wchar_t, 64> text{};
    GetWindowTextW(state.tick_edit, text.data(), static_cast<int>(text.size()));
    core::u64 tick = 0U;
    if (!parse_tick(text.data(), tick)) {
        set_status(state, L"Tick must be an unsigned decimal integer.");
        return;
    }
    render_tick(window, state, tick);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<WindowState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_CREATE: {
        if (state == nullptr) {
            return -1;
        }
        CreateWindowExW(0, L"STATIC", L"Simulation tick:", WS_CHILD | WS_VISIBLE,
                        14, 14, 110, 24, window, nullptr, GetModuleHandleW(nullptr), nullptr);
        state->tick_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0",
                                           WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
                                           126, 10, 120, 28, window,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTickEditId)),
                                           GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"Render tick", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        256, 10, 100, 28, window,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRenderButtonId)),
                        GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"Reset to 0", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        366, 10, 100, 28, window,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kResetButtonId)),
                        GetModuleHandleW(nullptr), nullptr);
        state->status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                        14, 46, 680, 36, window, nullptr,
                                        GetModuleHandleW(nullptr), nullptr);
        render_tick(window, *state, 0U);
        return 0;
    }
    case WM_COMMAND:
        if (state != nullptr) {
            const int control_id = LOWORD(wparam);
            if (control_id == kRenderButtonId) {
                render_from_edit(window, *state);
                return 0;
            }
            if (control_id == kResetButtonId) {
                SetWindowTextW(state->tick_edit, L"0");
                render_tick(window, *state, 0U);
                return 0;
            }
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        if (state != nullptr && !state->bgra.empty() && state->image.width != 0U && state->image.height != 0U) {
            RECT client{};
            GetClientRect(window, &client);
            const int left = 14;
            const int top = 88;
            const int available_width = (std::max)(1, static_cast<int>(client.right) - left - 14);
            const int available_height = (std::max)(1, static_cast<int>(client.bottom) - top - 14);
            const double sx = static_cast<double>(available_width) / static_cast<double>(state->image.width);
            const double sy = static_cast<double>(available_height) / static_cast<double>(state->image.height);
            const double scale = (std::min)(sx, sy);
            const int draw_width = (std::max)(1, static_cast<int>(static_cast<double>(state->image.width) * scale));
            const int draw_height = (std::max)(1, static_cast<int>(static_cast<double>(state->image.height) * scale));

            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = static_cast<LONG>(state->image.width);
            info.bmiHeader.biHeight = -static_cast<LONG>(state->image.height);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(dc, left, top, draw_width, draw_height,
                          0, 0, static_cast<int>(state->image.width), static_cast<int>(state->image.height),
                          state->bgra.data(), &info, DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int run_growth_inspector(const core::Recipe& recipe) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;
    const ATOM atom = RegisterClassExW(&window_class);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return 9;
    }

    WindowState state;
    state.recipe = recipe;
    HWND window = CreateWindowExW(
        0, kWindowClass, L"ArtMiner — Growth Tick Inspector",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 760, 760,
        nullptr, nullptr, GetModuleHandleW(nullptr), &state);
    if (window == nullptr) {
        return 9;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

}  // namespace artminer::app
