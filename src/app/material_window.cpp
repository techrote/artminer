#include "app/material_window.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include "nodes/material_evaluator.hpp"
#include "quarry/seam.hpp"

namespace artminer::app {
namespace {

constexpr wchar_t kWindowClassName[] = L"ArtMinerMaterialInspectionWindow";

struct WindowState final {
    const core::Recipe* recipe{nullptr};
    std::string output_name{"main"};
    nodes::Image inspection;
    quarry::TileSeamDiagnostics diagnostics;
    double threshold{0.02};
    std::wstring error;
};

[[nodiscard]] std::wstring widen_ascii(const std::string& value) {
    return std::wstring(value.begin(), value.end());
}

void refresh(WindowState& state) {
    state.error.clear();
    state.inspection = {};
    auto rendered = nodes::render_workflow_reference(*state.recipe, state.output_name);
    if (rendered.is_error()) {
        state.error = L"Render failed: " + widen_ascii(rendered.error().message);
        return;
    }
    auto diagnostics = quarry::compute_tile_seam_diagnostics(rendered.value());
    if (diagnostics.is_error()) {
        state.error = L"Seam diagnostics failed: " + widen_ascii(diagnostics.error().message);
        return;
    }
    state.diagnostics = diagnostics.value();
    auto inspection = quarry::make_tile_seam_inspection(rendered.value());
    if (inspection.is_error()) {
        state.error = L"Inspection image failed: " + widen_ascii(inspection.error().message);
        return;
    }
    state.inspection = std::move(inspection).value();
}

[[nodiscard]] std::wstring diagnostics_text(const WindowState& state) {
    if (!state.error.empty()) {
        return state.error;
    }
    std::wostringstream text;
    text << std::fixed << std::setprecision(6)
         << L"Output: " << widen_ascii(state.output_name)
         << L"   horizontal=" << state.diagnostics.horizontal_error
         << L"   vertical=" << state.diagnostics.vertical_error
         << L"   combined=" << state.diagnostics.combined_error
         << L"   threshold=" << state.threshold
         << L"   " << (state.diagnostics.combined_error <= state.threshold ? L"PASS" : L"FAIL");
    return text.str();
}

void paint(HWND window, WindowState& state) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(32, 32, 32));
    const std::wstring line = diagnostics_text(state);
    TextOutW(dc, 16, 14, line.c_str(), static_cast<int>(line.size()));
    constexpr wchar_t help[] = L"2x2 inspection: central joins are the measured wrap seams.  +/- threshold   R refresh   Esc close";
    TextOutW(dc, 16, 38, help, static_cast<int>(std::size(help) - 1U));

    if (!state.inspection.rgba.empty()) {
        std::vector<core::u8> bgra(state.inspection.rgba.size());
        for (std::size_t offset = 0U; offset < state.inspection.rgba.size(); offset += 4U) {
            bgra[offset + 0U] = state.inspection.rgba[offset + 2U];
            bgra[offset + 1U] = state.inspection.rgba[offset + 1U];
            bgra[offset + 2U] = state.inspection.rgba[offset + 0U];
            bgra[offset + 3U] = state.inspection.rgba[offset + 3U];
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(state.inspection.width);
        info.bmiHeader.biHeight = -static_cast<LONG>(state.inspection.height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        const int available_width = (std::max)(1, static_cast<int>(client.right - client.left - 32L));
        const int available_height = (std::max)(1, static_cast<int>(client.bottom - client.top - 84L));
        const double scale_x = static_cast<double>(available_width) / state.inspection.width;
        const double scale_y = static_cast<double>(available_height) / state.inspection.height;
        const double scale = (std::min)(scale_x, scale_y);
        const int draw_width = (std::max)(1, static_cast<int>(std::floor(state.inspection.width * scale)));
        const int draw_height = (std::max)(1, static_cast<int>(std::floor(state.inspection.height * scale)));
        const int draw_x = 16 + (available_width - draw_width) / 2;
        const int draw_y = 68 + (available_height - draw_height) / 2;
        SetStretchBltMode(dc, HALFTONE);
        StretchDIBits(
            dc,
            draw_x, draw_y, draw_width, draw_height,
            0, 0, static_cast<int>(state.inspection.width), static_cast<int>(state.inspection.height),
            bgra.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    }
    EndPaint(window, &paint);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        state = static_cast<WindowState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    switch (message) {
    case WM_KEYDOWN:
        if (state != nullptr) {
            if (w_param == VK_ESCAPE) {
                DestroyWindow(window);
                return 0;
            }
            if (w_param == L'R') {
                refresh(*state);
                InvalidateRect(window, nullptr, TRUE);
                return 0;
            }
            if (w_param == VK_OEM_PLUS || w_param == VK_ADD) {
                state->threshold = (std::min)(1.0, state->threshold + 0.005);
                InvalidateRect(window, nullptr, TRUE);
                return 0;
            }
            if (w_param == VK_OEM_MINUS || w_param == VK_SUBTRACT) {
                state->threshold = (std::max)(0.0, state->threshold - 0.005);
                InvalidateRect(window, nullptr, TRUE);
                return 0;
            }
        }
        break;
    case WM_PAINT:
        if (state != nullptr) {
            paint(window, *state);
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

}  // namespace

int run_material_inspection_application(const core::Recipe& recipe, std::string output_name) {
    WindowState state;
    state.recipe = &recipe;
    state.output_name = std::move(output_name);
    refresh(state);

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClassName;
    RegisterClassExW(&window_class);

    HWND window = CreateWindowExW(
        0,
        kWindowClassName,
        L"ArtMiner AM-013 Material / Seam Inspection",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 980, 820,
        nullptr, nullptr, instance, &state);
    if (window == nullptr) {
        return 5;
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

}  // namespace artminer::app
