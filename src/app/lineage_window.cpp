#include "app/lineage_window.hpp"

#include <Windows.h>
#include <commdlg.h>

#include <cerrno>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/browser_window.hpp"
#include "core/breeding.hpp"
#include "core/graph.hpp"
#include "core/recipe.hpp"
#include "platform/windows/lineage_store.hpp"

namespace artminer::app {
namespace {

constexpr int kGap = 8;
constexpr int kStatusHeight = 28;
constexpr UINT kLoadA = 1001U;
constexpr UINT kLoadB = 1002U;
constexpr UINT kBreed = 1003U;
constexpr UINT kDiff = 1004U;
constexpr UINT kUseAsA = 1005U;
constexpr UINT kOpenBrowser = 1006U;
constexpr UINT kRelationList = 1100U;

struct RelationItem {
    std::wstring label;
    std::string fingerprint;
    bool available{false};
};

struct State {
    platform::windows::WorkspaceLayout workspace;
    HWND window{nullptr};
    HWND parent_a_label{nullptr};
    HWND parent_b_label{nullptr};
    HWND seed_label{nullptr};
    HWND seed_edit{nullptr};
    HWND load_a_button{nullptr};
    HWND load_b_button{nullptr};
    HWND breed_button{nullptr};
    HWND diff_button{nullptr};
    HWND relation_list{nullptr};
    HWND use_as_a_button{nullptr};
    HWND open_browser_button{nullptr};
    HWND status{nullptr};
    std::optional<core::Recipe> parent_a;
    std::optional<core::Recipe> parent_b;
    std::map<std::string, platform::windows::StoredLineageSpecimen> specimens;
    std::vector<platform::windows::StoredLineageRecord> records;
    std::vector<RelationItem> relations;
    std::optional<std::filesystem::path> open_browser_path;
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

void set_status(State& state, const std::wstring_view text) {
    SetWindowTextW(state.status, std::wstring(text).c_str());
}

[[nodiscard]] std::wstring control_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
    const int copied = GetWindowTextW(control, text.data(), length + 1);
    if (copied <= 0) {
        return {};
    }
    text.resize(static_cast<std::size_t>(copied));
    return text;
}

[[nodiscard]] std::optional<core::u64> parse_seed(HWND edit) {
    const std::wstring text = control_text(edit);
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

[[nodiscard]] std::optional<std::filesystem::path> choose_recipe(HWND owner) {
    std::vector<wchar_t> path(32768U, L'\0');
    constexpr wchar_t filter[] = L"ArtMiner Recipes (*.amr)\0*.amr\0All Files (*.*)\0*.*\0\0";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrDefExt = L"amr";
    if (GetOpenFileNameW(&dialog) == FALSE) {
        return std::nullopt;
    }
    return std::filesystem::path(path.data());
}

[[nodiscard]] std::optional<core::Recipe> load_recipe(
    const std::filesystem::path& path,
    std::wstring& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = L"Could not open recipe: " + path.wstring();
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
        error = L"Failed while reading recipe: " + path.wstring();
        return std::nullopt;
    }
    auto parsed = core::parse_recipe(text);
    if (parsed.is_error()) {
        error = L"Recipe parse error: " + widen_utf8(parsed.error().message);
        return std::nullopt;
    }
    core::Recipe recipe = std::move(parsed).value();
    const auto validation = core::validate_recipe(recipe);
    if (!validation.empty()) {
        error = L"Recipe validation error: " + widen_utf8(validation.front().message);
        return std::nullopt;
    }
    return recipe;
}

[[nodiscard]] std::wstring short_fingerprint(const core::Recipe& recipe) {
    const std::wstring fingerprint = widen_utf8(core::semantic_fingerprint(recipe));
    return fingerprint.substr(0U, (std::min)(std::size_t{16U}, fingerprint.size()));
}

void update_parent_labels(State& state) {
    const std::wstring a = state.parent_a.has_value()
        ? L"Parent A: " + short_fingerprint(*state.parent_a)
        : L"Parent A: <not loaded>";
    const std::wstring b = state.parent_b.has_value()
        ? L"Parent B: " + short_fingerprint(*state.parent_b)
        : L"Parent B: <not loaded>";
    SetWindowTextW(state.parent_a_label, a.c_str());
    SetWindowTextW(state.parent_b_label, b.c_str());
    EnableWindow(state.breed_button, state.parent_a.has_value() && state.parent_b.has_value());
    EnableWindow(state.diff_button, state.parent_a.has_value() && state.parent_b.has_value());
}

void reload_store(State& state) {
    state.records.clear();
    state.specimens.clear();
    auto records = platform::windows::load_lineage_records(state.workspace.recipes);
    if (records.is_ok()) {
        state.records = std::move(records).value();
    }
    auto specimens = platform::windows::load_lineage_specimens(state.workspace.recipes);
    if (specimens.is_ok()) {
        for (auto& specimen : specimens.value()) {
            state.specimens.emplace(specimen.fingerprint, std::move(specimen));
        }
    }
}

void add_relation(State& state, std::wstring role, const std::string& fingerprint) {
    const bool available = state.specimens.contains(fingerprint);
    std::wstring label = std::move(role) + L"  " + widen_utf8(fingerprint.substr(0U, 16U));
    if (!available) {
        label += L"  [snapshot missing]";
    }
    state.relations.push_back(RelationItem{std::move(label), fingerprint, available});
}

void refresh_relations(State& state) {
    state.relations.clear();
    SendMessageW(state.relation_list, LB_RESETCONTENT, 0U, 0U);
    if (!state.parent_a.has_value()) {
        return;
    }
    const std::string current = core::semantic_fingerprint(*state.parent_a);
    for (const auto& stored : state.records) {
        const auto& record = stored.record;
        if (record.child_fingerprint == current) {
            add_relation(state, L"parent A", record.parent_a_fingerprint);
            if (record.parent_b_fingerprint.has_value()) {
                add_relation(state, L"parent B", *record.parent_b_fingerprint);
            }
        }
    }
    for (const auto& stored : state.records) {
        const auto& record = stored.record;
        const bool child_of_current = record.parent_a_fingerprint == current ||
            (record.parent_b_fingerprint.has_value() && *record.parent_b_fingerprint == current);
        if (child_of_current) {
            add_relation(state, L"child", record.child_fingerprint);
        }
    }
    for (const auto& item : state.relations) {
        (void)SendMessageW(
            state.relation_list,
            LB_ADDSTRING,
            0U,
            reinterpret_cast<LPARAM>(item.label.c_str()));
    }
    if (!state.relations.empty()) {
        SendMessageW(state.relation_list, LB_SETCURSEL, 0U, 0U);
    }
}

void load_parent(State& state, const bool parent_b, const std::filesystem::path& path) {
    std::wstring error;
    auto recipe = load_recipe(path, error);
    if (!recipe.has_value()) {
        set_status(state, error);
        return;
    }
    if (parent_b) {
        state.parent_b = std::move(*recipe);
        set_status(state, L"Loaded parent B.");
    } else {
        state.parent_a = std::move(*recipe);
        set_status(state, L"Loaded parent A and refreshed its lineage neighbourhood.");
    }
    update_parent_labels(state);
    reload_store(state);
    refresh_relations(state);
}

void breed(State& state) {
    if (!state.parent_a.has_value() || !state.parent_b.has_value()) {
        set_status(state, L"Load both ordered parents before breeding.");
        return;
    }
    const auto seed = parse_seed(state.seed_edit);
    if (!seed.has_value()) {
        set_status(state, L"Crossover seed must be an unsigned 64-bit integer.");
        return;
    }
    core::ParameterLocks locks;
    auto child = core::crossover_recipes(
        *state.parent_a,
        *state.parent_b,
        *seed,
        core::kCrossoverOperatorVersion,
        locks);
    if (child.is_error()) {
        set_status(state, L"Crossover rejected: " + widen_utf8(child.error().message));
        return;
    }
    core::Recipe produced = std::move(child).value();
    const core::LineageRecord record = core::make_crossover_lineage_record(
        produced,
        *state.parent_a,
        *state.parent_b,
        *seed,
        core::kCrossoverOperatorVersion,
        locks);

    auto saved_a = platform::windows::save_lineage_specimen(state.workspace.recipes, *state.parent_a);
    auto saved_b = platform::windows::save_lineage_specimen(state.workspace.recipes, *state.parent_b);
    auto saved_child = platform::windows::save_lineage_specimen(state.workspace.recipes, produced);
    auto saved_record = platform::windows::save_lineage_record(state.workspace.recipes, record);
    if (saved_a.is_error() || saved_b.is_error() || saved_child.is_error() || saved_record.is_error()) {
        set_status(state, L"Crossover succeeded but lineage persistence failed; the current parents remain unchanged.");
        return;
    }

    state.parent_a = std::move(produced);
    update_parent_labels(state);
    reload_store(state);
    refresh_relations(state);
    set_status(state, L"Bred deterministic child, persisted both parent snapshots and lineage record, and made child Parent A.");
}

[[nodiscard]] std::optional<std::size_t> selected_relation(const State& state) {
    const LRESULT selection = SendMessageW(state.relation_list, LB_GETCURSEL, 0U, 0U);
    if (selection == LB_ERR || static_cast<std::size_t>(selection) >= state.relations.size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(selection);
}

void use_selected_as_a(State& state) {
    const auto index = selected_relation(state);
    if (!index.has_value()) {
        return;
    }
    const RelationItem& relation = state.relations[*index];
    const auto found = state.specimens.find(relation.fingerprint);
    if (found == state.specimens.end()) {
        set_status(state, L"That ancestry reference is valid provenance, but its recipe snapshot is unavailable.");
        return;
    }
    state.parent_a = found->second.recipe;
    update_parent_labels(state);
    refresh_relations(state);
    set_status(state, L"Moved to stored lineage specimen as Parent A.");
}

void open_selected_in_browser(State& state) {
    const auto index = selected_relation(state);
    if (!index.has_value()) {
        return;
    }
    const auto found = state.specimens.find(state.relations[*index].fingerprint);
    if (found == state.specimens.end()) {
        set_status(state, L"Cannot open missing ancestry snapshot; the current child remains intact.");
        return;
    }
    state.open_browser_path = found->second.path;
    DestroyWindow(state.window);
}

void show_diff(State& state) {
    if (!state.parent_a.has_value() || !state.parent_b.has_value()) {
        return;
    }
    std::string text = core::format_recipe_diff(core::diff_recipes(*state.parent_a, *state.parent_b));
    constexpr std::size_t kMaximumDialogText = 30000U;
    if (text.size() > kMaximumDialogText) {
        text.resize(kMaximumDialogText);
        text += "\n... diff truncated for dialog display\n";
    }
    const std::wstring wide = widen_utf8(text);
    MessageBoxW(state.window, wide.c_str(), L"ArtMiner Recipe Diff — A to B", MB_OK | MB_ICONINFORMATION);
}

[[nodiscard]] HWND create_control(
    const wchar_t* class_name,
    const wchar_t* text,
    const DWORD style,
    HWND parent,
    const UINT id,
    HINSTANCE instance,
    const DWORD extended_style = 0U) {
    HWND control = CreateWindowExW(
        extended_style,
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

void layout(State& state, const int width, const int height) {
    const int content_height = (std::max)(0, height - kStatusHeight);
    const int button_width = (std::max)(90, (width - kGap * 7) / 6);
    int y = kGap;
    MoveWindow(state.parent_a_label, kGap, y, width - kGap * 2, 22, TRUE);
    y += 24;
    MoveWindow(state.parent_b_label, kGap, y, width - kGap * 2, 22, TRUE);
    y += 28;
    MoveWindow(state.seed_label, kGap, y + 3, 110, 22, TRUE);
    MoveWindow(state.seed_edit, 120, y, (std::max)(120, width - 128), 24, TRUE);
    y += 32;

    HWND buttons[] = {
        state.load_a_button,
        state.load_b_button,
        state.breed_button,
        state.diff_button,
        state.use_as_a_button,
        state.open_browser_button};
    for (int index = 0; index < 6; ++index) {
        const int x = kGap + index * (button_width + kGap);
        const int actual_width = index == 5 ? (std::max)(1, width - x - kGap) : button_width;
        MoveWindow(buttons[index], x, y, actual_width, 28, TRUE);
    }
    y += 36;
    MoveWindow(state.relation_list, kGap, y, (std::max)(1, width - kGap * 2), (std::max)(60, content_height - y - kGap), TRUE);
    MoveWindow(state.status, 0, content_height, (std::max)(0, width), kStatusHeight, TRUE);
}

void handle_command(State& state, const UINT command, const UINT notification) {
    switch (command) {
    case kLoadA:
    case kLoadB:
        if (auto path = choose_recipe(state.window); path.has_value()) {
            load_parent(state, command == kLoadB, *path);
        }
        break;
    case kBreed:
        breed(state);
        break;
    case kDiff:
        show_diff(state);
        break;
    case kUseAsA:
        use_selected_as_a(state);
        break;
    case kOpenBrowser:
        open_selected_in_browser(state);
        break;
    case kRelationList:
        if (notification == LBN_DBLCLK) {
            use_selected_as_a(state);
        }
        break;
    default:
        break;
    }
}

LRESULT CALLBACK window_proc(HWND window, const UINT message, const WPARAM w_param, const LPARAM l_param) {
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_SIZE:
        if (state != nullptr) {
            layout(*state, LOWORD(l_param), HIWORD(l_param));
        }
        return 0;
    case WM_COMMAND:
        if (state != nullptr) {
            handle_command(*state, LOWORD(w_param), HIWORD(w_param));
        }
        return 0;
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

[[nodiscard]] bool create_controls(State& state, HINSTANCE instance) {
    state.parent_a_label = create_control(L"STATIC", L"Parent A: <not loaded>", SS_LEFT, state.window, 0U, instance);
    state.parent_b_label = create_control(L"STATIC", L"Parent B: <not loaded>", SS_LEFT, state.window, 0U, instance);
    state.seed_label = create_control(L"STATIC", L"Crossover seed", SS_LEFT, state.window, 0U, instance);
    state.seed_edit = create_control(L"EDIT", L"1", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, state.window, 0U, instance);
    state.load_a_button = create_control(L"BUTTON", L"Load A...", BS_PUSHBUTTON | WS_TABSTOP, state.window, kLoadA, instance);
    state.load_b_button = create_control(L"BUTTON", L"Load B...", BS_PUSHBUTTON | WS_TABSTOP, state.window, kLoadB, instance);
    state.breed_button = create_control(L"BUTTON", L"Breed", BS_PUSHBUTTON | WS_TABSTOP, state.window, kBreed, instance);
    state.diff_button = create_control(L"BUTTON", L"Diff A/B", BS_PUSHBUTTON | WS_TABSTOP, state.window, kDiff, instance);
    state.use_as_a_button = create_control(L"BUTTON", L"Use as A", BS_PUSHBUTTON | WS_TABSTOP, state.window, kUseAsA, instance);
    state.open_browser_button = create_control(L"BUTTON", L"Open Browser", BS_PUSHBUTTON | WS_TABSTOP, state.window, kOpenBrowser, instance);
    state.relation_list = create_control(
        L"LISTBOX",
        L"",
        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_BORDER | WS_VSCROLL | WS_TABSTOP,
        state.window,
        kRelationList,
        instance,
        WS_EX_CLIENTEDGE);
    state.status = create_control(
        L"STATIC",
        L"Load ordered parents. Stored lineage is portable and non-semantic.",
        SS_LEFTNOWORDWRAP,
        state.window,
        0U,
        instance,
        WS_EX_CLIENTEDGE);
    return state.parent_a_label != nullptr && state.parent_b_label != nullptr && state.seed_label != nullptr &&
        state.seed_edit != nullptr && state.load_a_button != nullptr && state.load_b_button != nullptr &&
        state.breed_button != nullptr && state.diff_button != nullptr && state.use_as_a_button != nullptr &&
        state.open_browser_button != nullptr && state.relation_list != nullptr && state.status != nullptr;
}

}  // namespace

int run_lineage_application(
    const platform::windows::WorkspaceLayout& workspace,
    const std::optional<std::filesystem::path>& initial_parent_a,
    const std::optional<std::filesystem::path>& initial_parent_b) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (instance == nullptr) {
        return 3;
    }
    constexpr wchar_t kClassName[] = L"ArtMinerLineageWindow";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    window_class.lpszClassName = kClassName;
    if (RegisterClassW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return 3;
    }

    State state;
    state.workspace = workspace;
    state.window = CreateWindowExW(
        0,
        kClassName,
        L"ArtMiner — AM-008 Breeding & Lineage",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1060,
        640,
        nullptr,
        nullptr,
        instance,
        &state);
    if (state.window == nullptr || !create_controls(state, instance)) {
        if (state.window != nullptr) {
            DestroyWindow(state.window);
        }
        return 3;
    }

    reload_store(state);
    if (initial_parent_a.has_value()) {
        load_parent(state, false, *initial_parent_a);
    }
    if (initial_parent_b.has_value()) {
        load_parent(state, true, *initial_parent_b);
    }
    update_parent_labels(state);
    refresh_relations(state);

    RECT client{};
    GetClientRect(state.window, &client);
    layout(state, client.right - client.left, client.bottom - client.top);
    ShowWindow(state.window, SW_SHOWDEFAULT);
    UpdateWindow(state.window);

    MSG message{};
    while (true) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result == 0) {
            break;
        }
        if (result == -1) {
            return 3;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (state.open_browser_path.has_value()) {
        return run_browser_application(workspace, state.open_browser_path);
    }
    return 0;
}

}  // namespace artminer::app
