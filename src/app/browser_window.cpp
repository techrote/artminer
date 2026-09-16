#include "app/browser_window.hpp"

#include <Windows.h>
#include <commdlg.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/graph.hpp"
#include "core/local_text.hpp"
#include "core/recipe.hpp"
#include "core/specimen_browser.hpp"
#include "core/version.hpp"
#include "gpu/d3d11_preview.hpp"
#include "nodes/static_evaluator.hpp"
#include "platform/windows/browser_store.hpp"

namespace artminer::app {
namespace {

constexpr UINT_PTR kPreviewTimerId = 1U;
constexpr UINT kPreviewTimerMilliseconds = 16U;
constexpr UINT kThumbnailReadyMessage = WM_APP + 42U;
constexpr int kStatusHeight = 28;
constexpr int kGap = 6;
constexpr int kRightPanelMinimum = 340;
constexpr int kRightPanelMaximum = 440;
constexpr core::u32 kThumbnailMaximum = 176U;

constexpr UINT kCommandOpen = 1001U;
constexpr UINT kCommandSave = 1002U;
constexpr UINT kCommandExit = 1003U;
constexpr UINT kCommandMutate = 1101U;
constexpr UINT kCommandSeedVariants = 1102U;
constexpr UINT kCommandFavourite = 1103U;
constexpr UINT kCommandBack = 1104U;
constexpr UINT kCommandForward = 1105U;
constexpr UINT kCommandNextFavourite = 1106U;
constexpr UINT kCommandApplyParameter = 1110U;
constexpr UINT kCommandLockParameter = 1111U;
constexpr UINT kCommandLockGroup = 1112U;
constexpr UINT kSpecimenButtonBase = 2000U;
constexpr UINT kParameterListId = 3001U;

struct ThumbnailVisual final {
    core::u32 width{0U};
    core::u32 height{0U};
    std::vector<core::u8> bgra;
    std::string error;
    bool ready{false};
};

struct ThumbnailJob final {
    core::u64 generation{0U};
    std::size_t index{0U};
    core::Recipe recipe;
};

struct ThumbnailMessage final {
    core::u64 generation{0U};
    std::size_t index{0U};
    ThumbnailVisual visual;
};

class ThumbnailPool final {
public:
    explicit ThumbnailPool(HWND owner, const std::size_t worker_count = 4U) : owner_(owner) {
        workers_.reserve(worker_count);
        for (std::size_t index = 0U; index < worker_count; ++index) {
            workers_.emplace_back([this]() { worker_loop(); });
        }
    }

    ~ThumbnailPool() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            jobs_.clear();
        }
        condition_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    ThumbnailPool(const ThumbnailPool&) = delete;
    ThumbnailPool& operator=(const ThumbnailPool&) = delete;

    void submit(const core::u64 generation, const std::vector<core::GeneratedSpecimen>& specimens) {
        {
            std::lock_guard lock(mutex_);
            jobs_.clear();
            for (const auto& specimen : specimens) {
                jobs_.push_back(ThumbnailJob{generation, specimen.index, specimen.recipe});
            }
        }
        condition_.notify_all();
    }

private:
    [[nodiscard]] static nodes::Image render_thumbnail(const core::Recipe& source, std::string& error) {
        core::Recipe thumbnail_recipe = source;
        const core::u32 source_width = (std::max)(thumbnail_recipe.render.width, core::u32{1U});
        const core::u32 source_height = (std::max)(thumbnail_recipe.render.height, core::u32{1U});
        if (source_width >= source_height) {
            thumbnail_recipe.render.width = kThumbnailMaximum;
            thumbnail_recipe.render.height = (std::max)(
                core::u32{1U},
                static_cast<core::u32>(
                    (static_cast<core::u64>(source_height) * kThumbnailMaximum) / source_width));
        } else {
            thumbnail_recipe.render.height = kThumbnailMaximum;
            thumbnail_recipe.render.width = (std::max)(
                core::u32{1U},
                static_cast<core::u32>(
                    (static_cast<core::u64>(source_width) * kThumbnailMaximum) / source_height));
        }
        auto rendered = nodes::render_reference(thumbnail_recipe);
        if (rendered.is_error()) {
            error = rendered.error().message;
            return {};
        }
        return std::move(rendered).value();
    }

    void worker_loop() {
        while (true) {
            ThumbnailJob job;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this]() { return stopping_ || !jobs_.empty(); });
                if (stopping_) {
                    return;
                }
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }

            auto completed = std::make_unique<ThumbnailMessage>();
            completed->generation = job.generation;
            completed->index = job.index;
            std::string render_error;
            nodes::Image image = render_thumbnail(job.recipe, render_error);
            if (!render_error.empty()) {
                completed->visual.error = std::move(render_error);
            } else {
                completed->visual.width = image.width;
                completed->visual.height = image.height;
                completed->visual.bgra.resize(image.rgba.size());
                for (std::size_t offset = 0U; offset + 3U < image.rgba.size(); offset += 4U) {
                    completed->visual.bgra[offset + 0U] = image.rgba[offset + 2U];
                    completed->visual.bgra[offset + 1U] = image.rgba[offset + 1U];
                    completed->visual.bgra[offset + 2U] = image.rgba[offset + 0U];
                    completed->visual.bgra[offset + 3U] = image.rgba[offset + 3U];
                }
                completed->visual.ready = true;
            }

            {
                std::lock_guard lock(mutex_);
                if (stopping_) {
                    return;
                }
            }
            if (PostMessageW(owner_, kThumbnailReadyMessage, 0U, reinterpret_cast<LPARAM>(completed.get())) != FALSE) {
                (void)completed.release();
            }
        }
    }

    HWND owner_{nullptr};
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<ThumbnailJob> jobs_;
    std::vector<std::thread> workers_;
    bool stopping_{false};
};

struct ParameterUiItem final {
    std::string node_id;
    std::string parameter;
    std::string group;
};

struct AppState final {
    platform::windows::WorkspaceLayout workspace;
    HWND main_window{nullptr};
    std::array<HWND, core::kSpecimenGridSize> specimen_buttons{};
    HWND preview_window{nullptr};
    HWND status_window{nullptr};
    HWND fingerprint_label{nullptr};
    HWND seed_label{nullptr};
    HWND seed_edit{nullptr};
    HWND strength_label{nullptr};
    HWND strength_edit{nullptr};
    HWND mutate_button{nullptr};
    HWND seed_button{nullptr};
    HWND favourite_button{nullptr};
    HWND back_button{nullptr};
    HWND forward_button{nullptr};
    HWND save_button{nullptr};
    HWND next_favourite_button{nullptr};
    HWND parameter_list{nullptr};
    HWND parameter_value_edit{nullptr};
    HWND apply_parameter_button{nullptr};
    HWND lock_parameter_button{nullptr};
    HWND lock_group_button{nullptr};
    gpu::D3d11Preview preview;
    std::optional<core::RecipeHistory> history;
    core::ParameterLocks locks;
    std::vector<core::GeneratedSpecimen> specimens;
    std::array<ThumbnailVisual, core::kSpecimenGridSize> thumbnail_visuals{};
    std::vector<ParameterUiItem> parameter_items;
    std::map<std::string, platform::windows::StoredRecipe> favourites;
    std::unique_ptr<ThumbnailPool> thumbnail_pool;
    std::optional<std::size_t> selected_slot;
    std::size_t keyboard_slot{0U};
    core::u64 thumbnail_generation{0U};
    std::wstring status_base{L"Open a recipe to begin prospecting."};
    bool preview_error_shown{false};
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
    if (control == nullptr) {
        return {};
    }
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

[[nodiscard]] std::optional<std::string> read_text_file(
    const std::filesystem::path& path,
    std::wstring& error) {
    auto text = core::read_local_text_file(path);
    if (text.is_error()) {
        error = L"Could not read bounded UTF-8 recipe: " + path.wstring() + L" (" + widen_utf8(text.error().message) + L")";
        return std::nullopt;
    }
    return std::move(text).value();
}

[[nodiscard]] std::optional<core::Recipe> load_validated_recipe(
    const std::filesystem::path& path,
    std::wstring& error) {
    auto text = read_text_file(path, error);
    if (!text.has_value()) {
        return std::nullopt;
    }
    auto parsed = core::parse_recipe(*text);
    if (parsed.is_error()) {
        error = L"Recipe parse error: " + widen_utf8(parsed.error().message);
        return std::nullopt;
    }
    core::Recipe recipe = std::move(parsed).value();
    const auto validation_errors = core::validate_recipe(recipe);
    if (!validation_errors.empty()) {
        error = L"Recipe validation error: " + widen_utf8(validation_errors.front().message);
        return std::nullopt;
    }
    return recipe;
}

[[nodiscard]] const core::ParameterAssignment* find_assignment(
    const core::Recipe& recipe,
    const std::string_view node_id,
    const std::string_view parameter_name) {
    const auto node = std::find_if(recipe.nodes.begin(), recipe.nodes.end(), [node_id](const core::NodeInstance& item) {
        return item.id == node_id;
    });
    if (node == recipe.nodes.end()) {
        return nullptr;
    }
    const auto assignment = std::find_if(
        node->parameters.begin(),
        node->parameters.end(),
        [parameter_name](const core::ParameterAssignment& item) { return item.name == parameter_name; });
    return assignment == node->parameters.end() ? nullptr : &*assignment;
}

[[nodiscard]] const core::ParameterSpec* find_parameter_spec(
    const core::Recipe& recipe,
    const ParameterUiItem& item) {
    const auto node = std::find_if(recipe.nodes.begin(), recipe.nodes.end(), [&item](const core::NodeInstance& instance) {
        return instance.id == item.node_id;
    });
    if (node == recipe.nodes.end()) {
        return nullptr;
    }
    const core::NodeMetadata* metadata = core::builtin_node_registry().find(node->type_id);
    if (metadata == nullptr) {
        return nullptr;
    }
    const auto spec = std::find_if(
        metadata->parameters.begin(),
        metadata->parameters.end(),
        [&item](const core::ParameterSpec& parameter) { return parameter.name == item.parameter; });
    return spec == metadata->parameters.end() ? nullptr : &*spec;
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

void invalidate_specimens(AppState& state) {
    for (HWND button : state.specimen_buttons) {
        if (button != nullptr) {
            InvalidateRect(button, nullptr, FALSE);
        }
    }
}

void update_history_buttons(AppState& state) {
    const bool available = state.history.has_value();
    EnableWindow(state.back_button, available && state.history->can_back());
    EnableWindow(state.forward_button, available && state.history->can_forward());
}

void update_favourite_button(AppState& state) {
    bool favourite = false;
    if (state.history.has_value()) {
        favourite = state.favourites.contains(core::semantic_fingerprint(state.history->current()));
    }
    SetWindowTextW(state.favourite_button, favourite ? L"Unfavourite (F)" : L"Favourite (F)");
}

void refresh_parameter_list(AppState& state) {
    state.parameter_items.clear();
    SendMessageW(state.parameter_list, LB_RESETCONTENT, 0U, 0U);
    if (!state.history.has_value()) {
        SetWindowTextW(state.parameter_value_edit, L"");
        return;
    }

    const core::Recipe& recipe = state.history->current();
    for (const auto& node : recipe.nodes) {
        const core::NodeMetadata* metadata = core::builtin_node_registry().find(node.type_id);
        if (metadata == nullptr) {
            continue;
        }
        for (const auto& spec : metadata->parameters) {
            if (find_assignment(recipe, node.id, spec.name) != nullptr) {
                state.parameter_items.push_back(ParameterUiItem{node.id, spec.name, spec.mutation.group});
            }
        }
    }
    std::sort(
        state.parameter_items.begin(),
        state.parameter_items.end(),
        [](const ParameterUiItem& left, const ParameterUiItem& right) {
            if (left.node_id != right.node_id) {
                return left.node_id < right.node_id;
            }
            return left.parameter < right.parameter;
        });

    for (const auto& item : state.parameter_items) {
        const core::ParameterAssignment* assignment = find_assignment(recipe, item.node_id, item.parameter);
        if (assignment == nullptr) {
            continue;
        }
        std::string marker = "[ ] ";
        if (state.locks.parameter_locked(item.node_id, item.parameter)) {
            marker = "[P] ";
        } else if (state.locks.group_locked(item.node_id, item.group)) {
            marker = "[G] ";
        }
        const std::string line = marker + item.node_id + "." + item.parameter + " = " +
            core::format_parameter_value(assignment->value);
        const std::wstring wide = widen_utf8(line);
        (void)SendMessageW(state.parameter_list, LB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(wide.c_str()));
    }

    if (state.parameter_items.empty()) {
        SetWindowTextW(state.parameter_value_edit, L"");
        return;
    }
    SendMessageW(state.parameter_list, LB_SETCURSEL, 0U, 0U);
    const auto& first = state.parameter_items.front();
    if (const auto* assignment = find_assignment(recipe, first.node_id, first.parameter); assignment != nullptr) {
        const std::wstring value = widen_utf8(core::format_parameter_value(assignment->value));
        SetWindowTextW(state.parameter_value_edit, value.c_str());
    }
}

[[nodiscard]] bool present_current_recipe(AppState& state, const std::wstring_view reason) {
    if (!state.history.has_value()) {
        return false;
    }
    const core::Recipe& recipe = state.history->current();
    const std::string fingerprint_before = core::semantic_fingerprint(recipe);
    auto preview_status = state.preview.set_recipe(recipe);
    if (preview_status.is_error()) {
        state.status_base = L"Preview error: " + widen_utf8(preview_status.error().message);
        update_status(state);
        return false;
    }
    if (fingerprint_before != core::semantic_fingerprint(recipe)) {
        state.status_base = L"Internal error: preview mutated recipe semantics.";
        update_status(state);
        return false;
    }

    refresh_parameter_list(state);
    update_history_buttons(state);
    update_favourite_button(state);
    const std::wstring fingerprint = widen_utf8(fingerprint_before);
    const std::wstring fingerprint_text = L"Selected: " + fingerprint;
    SetWindowTextW(state.fingerprint_label, fingerprint_text.c_str());

    const auto& status = preview_status.value();
    state.status_base = std::wstring(reason) + L" | recipe " + fingerprint + L" | " + widen_utf8(status.message) +
        L" | history " + std::to_wstring(state.history->position() + 1U) + L"/" +
        std::to_wstring(state.history->size()) + L" | favourites " + std::to_wstring(state.favourites.size());
    update_status(state);

    std::wstring title = L"ArtMiner " + widen_utf8(core::kVersion) + L" — " +
        fingerprint.substr(0U, (std::min)(std::size_t{12U}, fingerprint.size()));
    title += status.path == gpu::PreviewPath::gpu ? L" — GPU Preview" : L" — Canonical CPU Fallback";
    if (state.favourites.contains(fingerprint_before)) {
        title += L" — Favourite";
    }
    SetWindowTextW(state.main_window, title.c_str());
    return true;
}

void persist_session_recovery(AppState& state) {
    if (!state.history.has_value()) {
        return;
    }
    auto saved = platform::windows::save_session_recovery(state.workspace.recipes, state.history->current());
    if (saved.is_error()) {
        state.status_base += L" | recovery write warning: " + saved.error().message;
        update_status(state);
    }
}

void adopt_recipe(AppState& state, core::Recipe recipe, const std::wstring_view reason) {
    if (state.history.has_value()) {
        state.history->push(std::move(recipe));
    } else {
        state.history.emplace(std::move(recipe));
    }
    state.selected_slot.reset();
    (void)present_current_recipe(state, reason);
    persist_session_recovery(state);
    invalidate_specimens(state);
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
    const std::wstring initial_text = state.workspace.recipes.wstring();
    dialog.lpstrInitialDir = initial_text.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrDefExt = L"amr";
    if (GetOpenFileNameW(&dialog) == FALSE) {
        return std::nullopt;
    }
    return std::filesystem::path(path_buffer.data());
}

void open_recipe(AppState& state, const std::filesystem::path& path) {
    std::wstring error;
    auto recipe = load_validated_recipe(path, error);
    if (!recipe.has_value()) {
        state.status_base = std::move(error);
        update_status(state);
        return;
    }
    state.locks.clear();
    adopt_recipe(state, std::move(*recipe), path.filename().wstring());
}

[[nodiscard]] bool parse_generation_controls(AppState& state, core::u64& seed, double& strength) {
    const std::string seed_text = narrow_utf8(control_text(state.seed_edit));
    const auto seed_result = std::from_chars(seed_text.data(), seed_text.data() + seed_text.size(), seed);
    if (seed_result.ec != std::errc{} || seed_result.ptr != seed_text.data() + seed_text.size()) {
        state.status_base = L"Mutation seed must be an unsigned 64-bit integer.";
        update_status(state);
        return false;
    }

    const std::string strength_text = narrow_utf8(control_text(state.strength_edit));
    const auto strength_result = std::from_chars(
        strength_text.data(),
        strength_text.data() + strength_text.size(),
        strength,
        std::chars_format::general);
    if (strength_result.ec != std::errc{} || strength_result.ptr != strength_text.data() + strength_text.size() ||
        strength < 0.0 || strength > 1.0) {
        state.status_base = L"Mutation strength must be a real value in [0, 1].";
        update_status(state);
        return false;
    }
    return true;
}

void generate_grid(AppState& state, const core::SpecimenGenerationMode mode) {
    if (!state.history.has_value()) {
        state.status_base = L"Open or select a parent recipe before generating specimens.";
        update_status(state);
        return;
    }
    core::u64 generation_seed = 0U;
    double strength = 0.0;
    if (!parse_generation_controls(state, generation_seed, strength)) {
        return;
    }

    auto generated = core::generate_specimen_grid(
        state.history->current(),
        generation_seed,
        core::kParameterMutationOperatorVersion,
        strength,
        mode,
        state.locks);
    if (generated.is_error()) {
        state.status_base = L"Generation failed: " + widen_utf8(generated.error().message);
        update_status(state);
        return;
    }

    state.specimens = std::move(generated).value();
    for (auto& visual : state.thumbnail_visuals) {
        visual = ThumbnailVisual{};
    }
    state.selected_slot.reset();
    state.keyboard_slot = 0U;
    ++state.thumbnail_generation;
    state.thumbnail_pool->submit(state.thumbnail_generation, state.specimens);
    const std::wstring kind = mode == core::SpecimenGenerationMode::parameter_mutation
        ? L"parameter mutations"
        : L"seed-only variants";
    state.status_base = L"Queued 16 deterministic " + kind + L" | seed " + std::to_wstring(generation_seed) +
        L" | operator " + std::to_wstring(core::kParameterMutationOperatorVersion) +
        L" | strength " + control_text(state.strength_edit);
    update_status(state);
    invalidate_specimens(state);
}

void select_specimen(AppState& state, const std::size_t index) {
    if (index >= state.specimens.size()) {
        return;
    }
    state.keyboard_slot = index;
    adopt_recipe(state, state.specimens[index].recipe, L"Selected specimen " + std::to_wstring(index + 1U));
    state.selected_slot = index;
    invalidate_specimens(state);
}

void toggle_favourite(AppState& state) {
    if (!state.history.has_value()) {
        return;
    }
    const core::Recipe& recipe = state.history->current();
    const std::string fingerprint = core::semantic_fingerprint(recipe);
    if (state.favourites.contains(fingerprint)) {
        auto removed = platform::windows::remove_favourite(state.workspace.recipes, fingerprint);
        if (removed.is_error()) {
            state.status_base = L"Could not remove favourite: " + removed.error().message;
            update_status(state);
            return;
        }
        state.favourites.erase(fingerprint);
    } else {
        auto saved = platform::windows::save_favourite(state.workspace.recipes, recipe);
        if (saved.is_error()) {
            state.status_base = L"Could not persist favourite: " + saved.error().message;
            update_status(state);
            return;
        }
        state.favourites.emplace(
            fingerprint,
            platform::windows::StoredRecipe{saved.value(), recipe, fingerprint});
    }
    (void)present_current_recipe(state, L"Favourite state updated");
    invalidate_specimens(state);
}

void save_current_recipe(AppState& state) {
    if (!state.history.has_value()) {
        return;
    }
    auto saved = platform::windows::save_recipe_copy(state.workspace.recipes, state.history->current());
    if (saved.is_error()) {
        state.status_base = L"Save failed: " + saved.error().message;
    } else {
        state.status_base = L"Saved selected recipe: " + saved.value().wstring();
    }
    update_status(state);
}

void navigate_history(AppState& state, const bool forward) {
    if (!state.history.has_value()) {
        return;
    }
    const bool moved = forward ? state.history->forward() : state.history->back();
    if (!moved) {
        return;
    }
    state.selected_slot.reset();
    (void)present_current_recipe(state, forward ? L"History forward" : L"History back");
    persist_session_recovery(state);
    invalidate_specimens(state);
}

void next_favourite(AppState& state) {
    if (state.favourites.empty()) {
        state.status_base = L"No persisted favourites are available.";
        update_status(state);
        return;
    }
    auto selected = state.favourites.begin();
    if (state.history.has_value()) {
        selected = state.favourites.upper_bound(core::semantic_fingerprint(state.history->current()));
        if (selected == state.favourites.end()) {
            selected = state.favourites.begin();
        }
    }
    adopt_recipe(state, selected->second.recipe, L"Loaded persisted favourite");
}

[[nodiscard]] std::optional<std::size_t> selected_parameter_index(const AppState& state) {
    const LRESULT selection = SendMessageW(state.parameter_list, LB_GETCURSEL, 0U, 0U);
    if (selection == LB_ERR || static_cast<std::size_t>(selection) >= state.parameter_items.size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(selection);
}

void sync_parameter_value_edit(AppState& state) {
    if (!state.history.has_value()) {
        return;
    }
    const auto index = selected_parameter_index(state);
    if (!index.has_value()) {
        return;
    }
    const auto& item = state.parameter_items[*index];
    if (const auto* assignment = find_assignment(state.history->current(), item.node_id, item.parameter); assignment != nullptr) {
        const std::wstring value = widen_utf8(core::format_parameter_value(assignment->value));
        SetWindowTextW(state.parameter_value_edit, value.c_str());
    }
}

void apply_parameter_edit(AppState& state) {
    if (!state.history.has_value()) {
        return;
    }
    const auto index = selected_parameter_index(state);
    if (!index.has_value()) {
        return;
    }
    const auto& item = state.parameter_items[*index];
    const std::string value_text = narrow_utf8(control_text(state.parameter_value_edit));
    auto edited = core::set_parameter_from_text(
        state.history->current(),
        item.node_id,
        item.parameter,
        value_text);
    if (edited.is_error()) {
        state.status_base = L"Parameter edit rejected: " + widen_utf8(edited.error().message);
        update_status(state);
        return;
    }
    adopt_recipe(state, std::move(edited).value(), L"Applied parameter edit");
}

void toggle_parameter_lock(AppState& state, const bool group) {
    const auto index = selected_parameter_index(state);
    if (!index.has_value()) {
        return;
    }
    const auto& item = state.parameter_items[*index];
    if (group) {
        if (item.group.empty()) {
            state.status_base = L"The selected parameter has no logical mutation group.";
            update_status(state);
            return;
        }
        state.locks.toggle_group(item.node_id, item.group);
    } else {
        state.locks.toggle_parameter(item.node_id, item.parameter);
    }
    refresh_parameter_list(state);
    state.status_base = group ? L"Toggled logical-group mutation lock." : L"Toggled parameter mutation lock.";
    update_status(state);
}

void draw_specimen(AppState& state, const DRAWITEMSTRUCT& draw) {
    if (draw.CtlID < kSpecimenButtonBase || draw.CtlID >= kSpecimenButtonBase + core::kSpecimenGridSize) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(draw.CtlID - kSpecimenButtonBase);
    HDC dc = draw.hDC;
    RECT rect = draw.rcItem;
    FillRect(dc, &rect, GetSysColorBrush(COLOR_BTNFACE));

    RECT image_rect = rect;
    image_rect.bottom = (std::max)(image_rect.top, image_rect.bottom - 22);
    const ThumbnailVisual& visual = state.thumbnail_visuals[index];
    if (visual.ready && !visual.bgra.empty()) {
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = static_cast<LONG>(visual.width);
        bitmap.bmiHeader.biHeight = -static_cast<LONG>(visual.height);
        bitmap.bmiHeader.biPlanes = 1U;
        bitmap.bmiHeader.biBitCount = 32U;
        bitmap.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        (void)StretchDIBits(
            dc,
            image_rect.left,
            image_rect.top,
            image_rect.right - image_rect.left,
            image_rect.bottom - image_rect.top,
            0,
            0,
            static_cast<int>(visual.width),
            static_cast<int>(visual.height),
            visual.bgra.data(),
            &bitmap,
            DIB_RGB_COLORS,
            SRCCOPY);
    } else {
        const wchar_t* message = visual.error.empty() ? L"Rendering..." : L"Render error";
        SetBkMode(dc, TRANSPARENT);
        (void)DrawTextW(dc, message, -1, &image_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    std::wstring label = L"#" + std::to_wstring(index + 1U);
    if (index < state.specimens.size()) {
        const std::wstring fingerprint = widen_utf8(state.specimens[index].fingerprint);
        label += L" " + fingerprint.substr(0U, (std::min)(std::size_t{8U}, fingerprint.size()));
        if (state.favourites.contains(state.specimens[index].fingerprint)) {
            label += L" F";
        }
    }
    RECT label_rect = rect;
    label_rect.top = image_rect.bottom;
    FillRect(dc, &label_rect, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(dc, TRANSPARENT);
    (void)DrawTextW(dc, label.c_str(), -1, &label_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    const bool selected = state.selected_slot.has_value() && *state.selected_slot == index;
    const bool keyboard = !state.specimens.empty() && state.keyboard_slot == index;
    const COLORREF border = selected
        ? GetSysColor(COLOR_HIGHLIGHT)
        : (keyboard ? GetSysColor(COLOR_HOTLIGHT) : GetSysColor(COLOR_BTNSHADOW));
    HPEN pen = CreatePen(PS_SOLID, selected ? 3 : 1, border);
    if (pen != nullptr) {
        HGDIOBJ old_pen = SelectObject(dc, pen);
        HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
    }
}

void layout_children(AppState& state, const int width, const int height) {
    const int content_height = (std::max)(0, height - kStatusHeight);
    const int right_width = (std::min)(kRightPanelMaximum, (std::max)(kRightPanelMinimum, width / 3));
    const int grid_width = (std::max)(220, width - right_width);
    const int cell_width = (std::max)(1, (grid_width - kGap * 5) / 4);
    const int cell_height = (std::max)(1, (content_height - kGap * 5) / 4);

    for (std::size_t index = 0U; index < state.specimen_buttons.size(); ++index) {
        const int column = static_cast<int>(index % 4U);
        const int row = static_cast<int>(index / 4U);
        MoveWindow(
            state.specimen_buttons[index],
            kGap + column * (cell_width + kGap),
            kGap + row * (cell_height + kGap),
            cell_width,
            cell_height,
            TRUE);
    }

    const int panel_x = grid_width + kGap;
    const int panel_width = (std::max)(1, width - panel_x - kGap);
    int y = kGap;
    const int preview_height = (std::min)(230, (std::max)(120, content_height / 3));
    MoveWindow(state.preview_window, panel_x, y, panel_width, preview_height, TRUE);
    y += preview_height + kGap;
    MoveWindow(state.fingerprint_label, panel_x, y, panel_width, 22, TRUE);
    y += 26;

    constexpr int label_width = 76;
    MoveWindow(state.seed_label, panel_x, y + 3, label_width - kGap, 20, TRUE);
    MoveWindow(state.seed_edit, panel_x + label_width, y, panel_width - label_width, 24, TRUE);
    y += 28;
    MoveWindow(state.strength_label, panel_x, y + 3, label_width - kGap, 20, TRUE);
    MoveWindow(state.strength_edit, panel_x + label_width, y, panel_width - label_width, 24, TRUE);
    y += 30;

    const int third = (std::max)(1, (panel_width - kGap * 2) / 3);
    MoveWindow(state.mutate_button, panel_x, y, third, 26, TRUE);
    MoveWindow(state.seed_button, panel_x + third + kGap, y, third, 26, TRUE);
    MoveWindow(
        state.favourite_button,
        panel_x + (third + kGap) * 2,
        y,
        panel_width - (third + kGap) * 2,
        26,
        TRUE);
    y += 30;

    const int quarter = (std::max)(1, (panel_width - kGap * 3) / 4);
    MoveWindow(state.back_button, panel_x, y, quarter, 25, TRUE);
    MoveWindow(state.forward_button, panel_x + quarter + kGap, y, quarter, 25, TRUE);
    MoveWindow(state.save_button, panel_x + (quarter + kGap) * 2, y, quarter, 25, TRUE);
    MoveWindow(
        state.next_favourite_button,
        panel_x + (quarter + kGap) * 3,
        y,
        panel_width - (quarter + kGap) * 3,
        25,
        TRUE);
    y += 30;

    const int available = (std::max)(70, content_height - y - 66);
    MoveWindow(state.parameter_list, panel_x, y, panel_width, available, TRUE);
    y += available + kGap;
    MoveWindow(state.parameter_value_edit, panel_x, y, panel_width, 24, TRUE);
    y += 28;
    const int action_third = (std::max)(1, (panel_width - kGap * 2) / 3);
    MoveWindow(state.apply_parameter_button, panel_x, y, action_third, 26, TRUE);
    MoveWindow(state.lock_parameter_button, panel_x + action_third + kGap, y, action_third, 26, TRUE);
    MoveWindow(
        state.lock_group_button,
        panel_x + (action_third + kGap) * 2,
        y,
        panel_width - (action_third + kGap) * 2,
        26,
        TRUE);

    MoveWindow(state.status_window, 0, content_height, (std::max)(0, width), kStatusHeight, TRUE);
    if (state.preview.initialized() && panel_width > 0 && preview_height > 0) {
        auto resized = state.preview.resize(static_cast<core::u32>(panel_width), static_cast<core::u32>(preview_height));
        if (resized.is_error()) {
            state.status_base = L"D3D11 resize error: " + widen_utf8(resized.error().message);
            update_status(state);
        }
    }
}

[[nodiscard]] HMENU create_main_menu() {
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU explore = CreatePopupMenu();
    if (menu == nullptr || file == nullptr || explore == nullptr) {
        if (file != nullptr) {
            DestroyMenu(file);
        }
        if (explore != nullptr) {
            DestroyMenu(explore);
        }
        if (menu != nullptr) {
            DestroyMenu(menu);
        }
        return nullptr;
    }
    AppendMenuW(file, MF_STRING, kCommandOpen, L"&Open Recipe...\tCtrl+O");
    AppendMenuW(file, MF_STRING, kCommandSave, L"&Save Selected Copy\tCtrl+S");
    AppendMenuW(file, MF_SEPARATOR, 0U, nullptr);
    AppendMenuW(file, MF_STRING, kCommandExit, L"E&xit");
    AppendMenuW(explore, MF_STRING, kCommandMutate, L"&Mutate Grid\tM");
    AppendMenuW(explore, MF_STRING, kCommandSeedVariants, L"&Seed Variants\tN");
    AppendMenuW(explore, MF_STRING, kCommandFavourite, L"&Favourite\tF");
    AppendMenuW(explore, MF_SEPARATOR, 0U, nullptr);
    AppendMenuW(explore, MF_STRING, kCommandBack, L"History &Back\tAlt+Left");
    AppendMenuW(explore, MF_STRING, kCommandForward, L"History &Forward\tAlt+Right");
    AppendMenuW(explore, MF_STRING, kCommandNextFavourite, L"Next Favourite\tCtrl+Shift+F");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(explore), L"&Explore");
    return menu;
}

void handle_command(AppState& state, const UINT command, const UINT notification) {
    if (command >= kSpecimenButtonBase && command < kSpecimenButtonBase + core::kSpecimenGridSize &&
        notification == BN_CLICKED) {
        select_specimen(state, static_cast<std::size_t>(command - kSpecimenButtonBase));
        return;
    }
    switch (command) {
    case kCommandOpen:
        if (auto path = choose_recipe_file(state); path.has_value()) {
            open_recipe(state, *path);
        }
        break;
    case kCommandSave:
        save_current_recipe(state);
        break;
    case kCommandExit:
        DestroyWindow(state.main_window);
        break;
    case kCommandMutate:
        generate_grid(state, core::SpecimenGenerationMode::parameter_mutation);
        break;
    case kCommandSeedVariants:
        generate_grid(state, core::SpecimenGenerationMode::seed_only);
        break;
    case kCommandFavourite:
        toggle_favourite(state);
        break;
    case kCommandBack:
        navigate_history(state, false);
        break;
    case kCommandForward:
        navigate_history(state, true);
        break;
    case kCommandNextFavourite:
        next_favourite(state);
        break;
    case kCommandApplyParameter:
        apply_parameter_edit(state);
        break;
    case kCommandLockParameter:
        toggle_parameter_lock(state, false);
        break;
    case kCommandLockGroup:
        toggle_parameter_lock(state, true);
        break;
    case kParameterListId:
        if (notification == LBN_SELCHANGE) {
            sync_parameter_value_edit(state);
        }
        break;
    default:
        break;
    }
}

[[nodiscard]] bool focus_is_text_editor(const AppState& state) {
    const HWND focus = GetFocus();
    return focus == state.seed_edit || focus == state.strength_edit || focus == state.parameter_value_edit;
}

[[nodiscard]] bool handle_key(AppState& state, const WPARAM key) {
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    if (control && key == static_cast<WPARAM>('O')) {
        handle_command(state, kCommandOpen, 0U);
        return true;
    }
    if (control && key == static_cast<WPARAM>('S')) {
        handle_command(state, kCommandSave, 0U);
        return true;
    }
    if (control && shift && key == static_cast<WPARAM>('F')) {
        handle_command(state, kCommandNextFavourite, 0U);
        return true;
    }
    if (alt && key == VK_LEFT) {
        navigate_history(state, false);
        return true;
    }
    if (alt && key == VK_RIGHT) {
        navigate_history(state, true);
        return true;
    }
    if (control || alt || focus_is_text_editor(state)) {
        return false;
    }
    if (key == static_cast<WPARAM>('M')) {
        generate_grid(state, core::SpecimenGenerationMode::parameter_mutation);
        return true;
    }
    if (key == static_cast<WPARAM>('N')) {
        generate_grid(state, core::SpecimenGenerationMode::seed_only);
        return true;
    }
    if (key == static_cast<WPARAM>('F')) {
        toggle_favourite(state);
        return true;
    }
    if (state.specimens.empty()) {
        return false;
    }
    if (key == VK_RETURN) {
        select_specimen(state, state.keyboard_slot);
        return true;
    }
    std::size_t row = state.keyboard_slot / 4U;
    std::size_t column = state.keyboard_slot % 4U;
    bool moved = true;
    switch (key) {
    case VK_LEFT:
        if (column > 0U) {
            --column;
        }
        break;
    case VK_RIGHT:
        if (column < 3U) {
            ++column;
        }
        break;
    case VK_UP:
        if (row > 0U) {
            --row;
        }
        break;
    case VK_DOWN:
        if (row < 3U) {
            ++row;
        }
        break;
    default:
        moved = false;
        break;
    }
    if (moved) {
        state.keyboard_slot = row * 4U + column;
        invalidate_specimens(state);
    }
    return moved;
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
            layout_children(*state, LOWORD(l_param), HIWORD(l_param));
        }
        return 0;
    case WM_COMMAND:
        if (state != nullptr) {
            handle_command(*state, LOWORD(w_param), HIWORD(w_param));
        }
        return 0;
    case WM_DRAWITEM:
        if (state != nullptr && l_param != 0) {
            draw_specimen(*state, *reinterpret_cast<const DRAWITEMSTRUCT*>(l_param));
            return TRUE;
        }
        break;
    case kThumbnailReadyMessage:
        if (state != nullptr && l_param != 0) {
            std::unique_ptr<ThumbnailMessage> completed(reinterpret_cast<ThumbnailMessage*>(l_param));
            if (completed->generation == state->thumbnail_generation && completed->index < state->thumbnail_visuals.size()) {
                state->thumbnail_visuals[completed->index] = std::move(completed->visual);
                InvalidateRect(state->specimen_buttons[completed->index], nullptr, FALSE);
            }
        }
        return 0;
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
        if (state != nullptr) {
            KillTimer(window, kPreviewTimerId);
            state->thumbnail_pool.reset();
            MSG pending{};
            while (PeekMessageW(&pending, window, kThumbnailReadyMessage, kThumbnailReadyMessage, PM_REMOVE) != FALSE) {
                delete reinterpret_cast<ThumbnailMessage*>(pending.lParam);
            }
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
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

[[nodiscard]] bool create_controls(AppState& state, HINSTANCE instance) {
    for (std::size_t index = 0U; index < state.specimen_buttons.size(); ++index) {
        state.specimen_buttons[index] = create_control(
            L"BUTTON",
            L"",
            BS_OWNERDRAW | WS_TABSTOP,
            state.main_window,
            kSpecimenButtonBase + static_cast<UINT>(index),
            instance);
        if (state.specimen_buttons[index] == nullptr) {
            return false;
        }
    }

    state.preview_window = create_control(L"STATIC", L"", SS_BLACKRECT, state.main_window, 0U, instance);
    state.fingerprint_label = create_control(L"STATIC", L"Selected: none", SS_LEFTNOWORDWRAP, state.main_window, 0U, instance);
    state.seed_label = create_control(L"STATIC", L"Seed", SS_LEFT, state.main_window, 0U, instance);
    state.seed_edit = create_control(L"EDIT", L"1", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, state.main_window, 0U, instance);
    state.strength_label = create_control(L"STATIC", L"Strength", SS_LEFT, state.main_window, 0U, instance);
    state.strength_edit = create_control(L"EDIT", L"0.25", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, state.main_window, 0U, instance);
    state.mutate_button = create_control(L"BUTTON", L"Mutate (M)", BS_PUSHBUTTON | WS_TABSTOP, state.main_window, kCommandMutate, instance);
    state.seed_button = create_control(L"BUTTON", L"Seeds (N)", BS_PUSHBUTTON | WS_TABSTOP, state.main_window, kCommandSeedVariants, instance);
    state.favourite_button = create_control(L"BUTTON", L"Favourite (F)", BS_PUSHBUTTON | WS_TABSTOP, state.main_window, kCommandFavourite, instance);
    state.back_button = create_control(L"BUTTON", L"Back", BS_PUSHBUTTON | WS_TABSTOP, state.main_window, kCommandBack, instance);
    state.forward_button = create_control(L"BUTTON", L"Forward", BS_PUSHBUTTON | WS_TABSTOP, state.main_window, kCommandForward, instance);
    state.save_button = create_control(L"BUTTON", L"Save", BS_PUSHBUTTON | WS_TABSTOP, state.main_window, kCommandSave, instance);
    state.next_favourite_button = create_control(L"BUTTON", L"Next Fav", BS_PUSHBUTTON | WS_TABSTOP, state.main_window, kCommandNextFavourite, instance);
    state.parameter_list = create_control(
        L"LISTBOX",
        L"",
        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_BORDER | WS_VSCROLL | WS_TABSTOP,
        state.main_window,
        kParameterListId,
        instance,
        WS_EX_CLIENTEDGE);
    state.parameter_value_edit = create_control(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, state.main_window, 0U, instance);
    state.apply_parameter_button = create_control(L"BUTTON", L"Apply", BS_PUSHBUTTON | WS_TABSTOP, state.main_window, kCommandApplyParameter, instance);
    state.lock_parameter_button = create_control(L"BUTTON", L"Lock Param", BS_PUSHBUTTON | WS_TABSTOP, state.main_window, kCommandLockParameter, instance);
    state.lock_group_button = create_control(L"BUTTON", L"Lock Group", BS_PUSHBUTTON | WS_TABSTOP, state.main_window, kCommandLockGroup, instance);
    state.status_window = create_control(
        L"STATIC",
        state.status_base.c_str(),
        SS_LEFTNOWORDWRAP,
        state.main_window,
        0U,
        instance,
        WS_EX_CLIENTEDGE);

    return state.preview_window != nullptr && state.fingerprint_label != nullptr && state.seed_label != nullptr &&
        state.seed_edit != nullptr && state.strength_label != nullptr && state.strength_edit != nullptr &&
        state.mutate_button != nullptr && state.seed_button != nullptr && state.favourite_button != nullptr &&
        state.back_button != nullptr && state.forward_button != nullptr && state.save_button != nullptr &&
        state.next_favourite_button != nullptr && state.parameter_list != nullptr && state.parameter_value_edit != nullptr &&
        state.apply_parameter_button != nullptr && state.lock_parameter_button != nullptr && state.lock_group_button != nullptr &&
        state.status_window != nullptr;
}

}  // namespace

int run_browser_application(
    const platform::windows::WorkspaceLayout& workspace,
    const std::optional<std::filesystem::path>& initial_recipe) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (instance == nullptr) {
        return 3;
    }

    constexpr wchar_t kWindowClass[] = L"ArtMinerSpecimenBrowserWindow";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return 3;
    }

    AppState state;
    state.workspace = workspace;
    HMENU menu = create_main_menu();
    HWND window = CreateWindowExW(
        0,
        kWindowClass,
        L"ArtMiner — Specimen Browser",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1220,
        820,
        nullptr,
        menu,
        instance,
        &state);
    if (window == nullptr) {
        if (menu != nullptr) {
            DestroyMenu(menu);
        }
        return 3;
    }
    state.main_window = window;
    const std::wstring release_title = L"ArtMiner " + widen_utf8(core::kVersion) + L" — Specimen Browser";
    SetWindowTextW(window, release_title.c_str());
    if (!create_controls(state, instance)) {
        DestroyWindow(window);
        return 3;
    }

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
        static_cast<core::u32>((std::max)(client.right - client.left, 1L)),
        static_cast<core::u32>((std::max)(client.bottom - client.top, 1L)));
    if (resized.is_error()) {
        state.status_base = L"Initial D3D11 resize error: " + widen_utf8(resized.error().message);
    }

    state.thumbnail_pool = std::make_unique<ThumbnailPool>(window);
    auto loaded_favourites = platform::windows::load_favourites(workspace.recipes);
    if (loaded_favourites.is_ok()) {
        for (auto& favourite : loaded_favourites.value()) {
            state.favourites.emplace(favourite.fingerprint, std::move(favourite));
        }
    } else {
        state.status_base = L"Favourite load warning: " + loaded_favourites.error().message;
    }

    std::wstring recovery_warning;
    auto recovery = platform::windows::load_session_recovery(workspace.recipes);
    if (recovery.is_error()) {
        recovery_warning = L"Session recovery ignored: " + recovery.error().message;
    }

    if (initial_recipe.has_value()) {
        open_recipe(state, *initial_recipe);
    } else if (recovery.is_ok() && recovery.value().has_value()) {
        adopt_recipe(state, recovery.value()->recipe, L"Restored workspace session recovery");
    } else if (!state.favourites.empty()) {
        adopt_recipe(state, state.favourites.begin()->second.recipe, L"Restored persisted favourite");
    } else {
        update_status(state);
    }
    if (!recovery_warning.empty()) {
        state.status_base += L" | " + recovery_warning;
        update_status(state);
    }
    if (state.history.has_value()) {
        generate_grid(state, core::SpecimenGenerationMode::parameter_mutation);
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
            return 3;
        }
        if ((message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) && handle_key(state, message.wParam)) {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

}  // namespace artminer::app
