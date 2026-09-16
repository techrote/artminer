#include "app/export_window.hpp"

#include <Windows.h>

#include <cerrno>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/recipe.hpp"
#include "export/export.hpp"

namespace artminer::app {
namespace {

constexpr UINT kCommandExport = 5101U;
constexpr UINT kKindComboId = 5102U;
constexpr UINT kFormatComboId = 5103U;
constexpr int kGap = 10;

struct AppState final {
    core::Recipe recipe;
    std::filesystem::path output_parent;
    HWND window{nullptr};
    HWND kind_combo{nullptr};
    HWND format_combo{nullptr};
    HWND tick_edit{nullptr};
    HWND end_edit{nullptr};
    HWND columns_edit{nullptr};
    HWND palette_edit{nullptr};
    HWND destination_edit{nullptr};
    HWND export_button{nullptr};
    HWND status_label{nullptr};
};

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
        return L"<invalid UTF-8>";
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    (void)MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        length);
    return result;
}

[[nodiscard]] std::string narrow_utf8(const std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    (void)WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        length,
        nullptr,
        nullptr);
    return result;
}

[[nodiscard]] std::wstring control_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length) + 1U, L'\0');
    const int copied = GetWindowTextW(control, result.data(), length + 1);
    if (copied <= 0) {
        return {};
    }
    result.resize(static_cast<std::size_t>(copied));
    return result;
}

[[nodiscard]] std::optional<core::u64> parse_u64(HWND control) {
    const std::wstring text = control_text(control);
    if (text.empty() || text.front() == L'-') {
        return std::nullopt;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long long value = std::wcstoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || end == nullptr || *end != L'\0') {
        return std::nullopt;
    }
    return static_cast<core::u64>(value);
}

[[nodiscard]] HWND create_control(
    const wchar_t* class_name,
    const wchar_t* text,
    const DWORD style,
    HWND parent,
    const UINT id,
    HINSTANCE instance) {
    HWND control = CreateWindowExW(
        0U,
        class_name,
        text,
        WS_CHILD | WS_VISIBLE | style,
        0,
        0,
        1,
        1,
        parent,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
        instance,
        nullptr);
    if (control != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    }
    return control;
}

void add_combo_item(HWND combo, const wchar_t* text) {
    (void)SendMessageW(combo, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(text));
}

[[nodiscard]] bool create_controls(AppState& state, HINSTANCE instance) {
    state.kind_combo = create_control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, state.window, kKindComboId, instance);
    state.format_combo = create_control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, state.window, kFormatComboId, instance);
    state.tick_edit = create_control(L"EDIT", L"0", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, state.window, 0U, instance);
    state.end_edit = create_control(L"EDIT", L"15", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, state.window, 0U, instance);
    state.columns_edit = create_control(L"EDIT", L"4", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, state.window, 0U, instance);
    state.palette_edit = create_control(L"EDIT", L"palette", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, state.window, 0U, instance);
    state.destination_edit = create_control(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, state.window, 0U, instance);
    state.export_button = create_control(L"BUTTON", L"Export", BS_DEFPUSHBUTTON | WS_TABSTOP, state.window, kCommandExport, instance);
    state.status_label = create_control(L"STATIC", L"Ready. Existing destinations are never overwritten.", SS_LEFT, state.window, 0U, instance);
    if (state.kind_combo == nullptr || state.format_combo == nullptr || state.tick_edit == nullptr ||
        state.end_edit == nullptr || state.columns_edit == nullptr || state.palette_edit == nullptr ||
        state.destination_edit == nullptr || state.export_button == nullptr || state.status_label == nullptr) {
        return false;
    }

    add_combo_item(state.kind_combo, L"Still image");
    add_combo_item(state.kind_combo, L"Single animation frame");
    add_combo_item(state.kind_combo, L"Image sequence");
    add_combo_item(state.kind_combo, L"Sprite sheet + atlas");
    add_combo_item(state.kind_combo, L"Palette / LUT");
    (void)SendMessageW(state.kind_combo, CB_SETCURSEL, 0U, 0U);

    add_combo_item(state.format_combo, L"PNG");
    add_combo_item(state.format_combo, L"BMP (opaque only)");
    add_combo_item(state.format_combo, L"Raw RGBA8");
    add_combo_item(state.format_combo, L"Palette text");
    add_combo_item(state.format_combo, L"Palette CSV");
    add_combo_item(state.format_combo, L".cube LUT");
    (void)SendMessageW(state.format_combo, CB_SETCURSEL, 0U, 0U);

    const std::string fingerprint = core::semantic_fingerprint(state.recipe);
    const std::string short_fingerprint = fingerprint.substr(0U, (std::min)(std::size_t{12U}, fingerprint.size()));
    const std::filesystem::path default_destination = state.output_parent / (L"export-" + widen_utf8(short_fingerprint));
    SetWindowTextW(state.destination_edit, default_destination.c_str());
    return true;
}

void layout_controls(AppState& state, const int width, const int height) {
    constexpr int label_width = 150;
    const int control_x = kGap + label_width;
    const int control_width = (std::max)(180, width - control_x - kGap);
    int y = kGap;

    const auto place = [&](const wchar_t* label, HWND control, const int control_height = 26) mutable {
        RECT label_rect{kGap, y + 4, kGap + label_width - kGap, y + 24};
        HDC dc = GetDC(state.window);
        SetBkMode(dc, TRANSPARENT);
        (void)DrawTextW(dc, label, -1, &label_rect, DT_LEFT | DT_SINGLELINE);
        ReleaseDC(state.window, dc);
        MoveWindow(control, control_x, y, control_width, control_height, TRUE);
        y += control_height + kGap;
    };

    place(L"Kind", state.kind_combo, 160);
    y -= 134;
    place(L"Format", state.format_combo, 160);
    y -= 134;
    place(L"Tick / start tick", state.tick_edit);
    place(L"End tick", state.end_edit);
    place(L"Sheet columns", state.columns_edit);
    place(L"Palette node ID", state.palette_edit);
    place(L"Destination folder", state.destination_edit);
    MoveWindow(state.export_button, control_x, y, 120, 30, TRUE);
    y += 40;
    MoveWindow(state.status_label, kGap, y, (std::max)(1, width - 2 * kGap), (std::max)(40, height - y - kGap), TRUE);
}

[[nodiscard]] std::optional<exporting::ExportRequest> build_request(AppState& state, std::wstring& error) {
    const LRESULT kind_index = SendMessageW(state.kind_combo, CB_GETCURSEL, 0U, 0U);
    const LRESULT format_index = SendMessageW(state.format_combo, CB_GETCURSEL, 0U, 0U);
    if (kind_index == CB_ERR || format_index == CB_ERR) {
        error = L"Choose an export kind and format.";
        return std::nullopt;
    }

    exporting::ExportRequest request;
    request.destination_directory = std::filesystem::path(control_text(state.destination_edit));
    request.stem = kind_index == 4 ? "palette" : "art";

    if (kind_index == 0) {
        request.kind = exporting::ExportKind::still;
    } else if (kind_index == 1) {
        request.kind = exporting::ExportKind::frame;
        request.tick = parse_u64(state.tick_edit);
        if (!request.tick) {
            error = L"Frame export requires an unsigned tick.";
            return std::nullopt;
        }
    } else if (kind_index == 2 || kind_index == 3) {
        request.kind = kind_index == 2
            ? exporting::ExportKind::sequence
            : exporting::ExportKind::sprite_sheet;
        const auto start = parse_u64(state.tick_edit);
        const auto end = parse_u64(state.end_edit);
        if (!start || !end) {
            error = L"Sequence/sheet export requires unsigned start and end ticks.";
            return std::nullopt;
        }
        request.start_tick = *start;
        request.end_tick = *end;
        if (kind_index == 3) {
            const auto columns = parse_u64(state.columns_edit);
            if (!columns || *columns == 0U || *columns > 65535U) {
                error = L"Sprite sheet requires a positive column count up to 65535.";
                return std::nullopt;
            }
            request.sheet_columns = static_cast<core::u32>(*columns);
        }
    } else {
        const std::string palette_node = narrow_utf8(control_text(state.palette_edit));
        if (palette_node.empty()) {
            error = L"Palette/LUT export requires a palette node ID.";
            return std::nullopt;
        }
        request.palette_node_id = palette_node;
        if (format_index == 3) {
            request.kind = exporting::ExportKind::palette_text;
        } else if (format_index == 4) {
            request.kind = exporting::ExportKind::palette_csv;
        } else if (format_index == 5) {
            request.kind = exporting::ExportKind::cube_lut;
        } else {
            error = L"Palette/LUT kind requires Palette text, Palette CSV, or .cube LUT format.";
            return std::nullopt;
        }
        return request;
    }

    if (format_index == 0) {
        request.raster_format = exporting::RasterFormat::png;
    } else if (format_index == 1) {
        request.raster_format = exporting::RasterFormat::bmp;
    } else if (format_index == 2) {
        request.raster_format = exporting::RasterFormat::raw_rgba;
    } else {
        error = L"Raster export kind requires PNG, BMP, or Raw RGBA8 format.";
        return std::nullopt;
    }
    return request;
}

void run_export(AppState& state) {
    std::wstring request_error;
    auto request = build_request(state, request_error);
    if (!request) {
        SetWindowTextW(state.status_label, request_error.c_str());
        return;
    }
    EnableWindow(state.export_button, FALSE);
    SetWindowTextW(state.status_label, L"Exporting canonical output...");
    UpdateWindow(state.window);

    auto result = exporting::export_recipe(state.recipe, *request);
    if (result.is_error()) {
        const std::wstring error = L"Export failed: " + widen_utf8(result.error().message);
        SetWindowTextW(state.status_label, error.c_str());
    } else {
        const std::wstring status = L"Complete: " + result.value().manifest_path.wstring();
        SetWindowTextW(state.status_label, status.c_str());
    }
    EnableWindow(state.export_button, TRUE);
}

LRESULT CALLBACK window_proc(HWND window, const UINT message, const WPARAM w_param, const LPARAM l_param) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_SIZE:
        if (state != nullptr) {
            InvalidateRect(window, nullptr, TRUE);
            layout_controls(*state, static_cast<int>(LOWORD(l_param)), static_cast<int>(HIWORD(l_param)));
        }
        return 0;
    case WM_COMMAND:
        if (state != nullptr && LOWORD(w_param) == kCommandExport) {
            run_export(*state);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

}  // namespace

int run_export_application(
    const core::Recipe& recipe,
    const std::filesystem::path& default_output_parent) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (instance == nullptr) {
        return 9;
    }

    constexpr wchar_t kClassName[] = L"ArtMinerExportWindow";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kClassName;
    if (RegisterClassW(&window_class) == 0U && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return 9;
    }

    AppState state;
    state.recipe = recipe;
    state.output_parent = default_output_parent;
    const std::string fingerprint = core::semantic_fingerprint(recipe);
    const std::wstring title = L"ArtMiner Export — " + widen_utf8(fingerprint.substr(0U, 12U));
    state.window = CreateWindowExW(
        0U,
        kClassName,
        title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        760,
        520,
        nullptr,
        nullptr,
        instance,
        &state);
    if (state.window == nullptr) {
        return 9;
    }
    if (!create_controls(state, instance)) {
        DestroyWindow(state.window);
        return 9;
    }
    RECT client{};
    GetClientRect(state.window, &client);
    layout_controls(state, client.right - client.left, client.bottom - client.top);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0U, 0U) > 0) {
        if (!IsDialogMessageW(state.window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return static_cast<int>(message.wParam);
}

}  // namespace artminer::app
