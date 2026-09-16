#include "app/quarry_window.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <locale>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "core/recipe.hpp"
#include "platform/windows/browser_store.hpp"
#include "quarry/diversity.hpp"
#include "quarry/quarry.hpp"

namespace artminer::app {
namespace {

constexpr wchar_t kWindowClass[] = L"ArtMinerQuarryWindow";
constexpr UINT kProgressMessage = WM_APP + 41U;
constexpr UINT kCompleteMessage = WM_APP + 42U;
constexpr int kRunButton = 1001;
constexpr int kCancelButton = 1002;
constexpr int kRefreshButton = 1003;
constexpr int kResultList = 1004;
constexpr int kDedupeButton = 1005;
constexpr int kUnusualButton = 1006;
constexpr int kNearbyButton = 1007;
constexpr int kProjectButton = 1008;
constexpr int kFilterButton = 1009;
constexpr int kSaveButton = 1010;
constexpr int kFavouriteButton = 1011;
constexpr int kNeighbourMode = 1012;
constexpr int kFilterMetric = 1013;
constexpr int kFilterMin = 1014;
constexpr int kFilterMax = 1015;
constexpr int kProjectionX = 1016;
constexpr int kProjectionY = 1017;

struct Completion final {
    bool success{false};
    quarry::JobProgress progress;
    std::string error;
};

struct WindowState final {
    std::filesystem::path manifest_path;
    quarry::JobManifest manifest;
    HWND window{nullptr};
    HWND status{nullptr};
    HWND results{nullptr};
    HWND neighbour_mode{nullptr};
    HWND filter_metric{nullptr};
    HWND filter_min{nullptr};
    HWND filter_max{nullptr};
    HWND projection_x{nullptr};
    HWND projection_y{nullptr};
    std::unique_ptr<quarry::CancellationToken> cancellation;
    std::thread worker;
    std::atomic<bool> running{false};
    bool analysis_ready{false};
    std::vector<quarry::CandidateResult> analysis_results;
    quarry::NormalizationModel normalization;
    std::vector<quarry::CandidateFeatures> features;
    std::vector<core::u64> listed_indices;
};

[[nodiscard]] std::wstring widen_ascii(const std::string& value) {
    return std::wstring(value.begin(), value.end());
}

[[nodiscard]] artminer::core::u32 default_worker_count() noexcept {
    const unsigned int count = std::thread::hardware_concurrency();
    if (count == 0U) {
        return 1U;
    }
    return static_cast<artminer::core::u32>((std::min)(count, quarry::kMaximumQuarryWorkers));
}

void set_status(WindowState& state, const std::wstring& text) {
    if (state.status != nullptr) {
        SetWindowTextW(state.status, text.c_str());
    }
}

void clear_results(WindowState& state) {
    if (state.results != nullptr) {
        SendMessageW(state.results, LB_RESETCONTENT, 0U, 0);
    }
    state.listed_indices.clear();
}

void add_result_line(WindowState& state, const core::u64 index, const std::wstring& line) {
    if (state.results == nullptr) {
        return;
    }
    SendMessageW(state.results, LB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(line.c_str()));
    state.listed_indices.push_back(index);
}

[[nodiscard]] const quarry::CandidateFeatures* feature_for(
    const WindowState& state,
    const core::u64 index) noexcept {
    const auto found = std::find_if(state.features.begin(), state.features.end(), [index](const auto& candidate) {
        return candidate.index == index;
    });
    return found == state.features.end() ? nullptr : &*found;
}

[[nodiscard]] std::optional<core::u64> selected_candidate(const WindowState& state) {
    if (state.results == nullptr) {
        return std::nullopt;
    }
    const LRESULT selection = SendMessageW(state.results, LB_GETCURSEL, 0U, 0);
    if (selection == LB_ERR) {
        return std::nullopt;
    }
    const std::size_t index = static_cast<std::size_t>(selection);
    if (index >= state.listed_indices.size()) {
        return std::nullopt;
    }
    return state.listed_indices[index];
}

[[nodiscard]] std::optional<double> edit_double(const HWND edit) {
    if (edit == nullptr) {
        return std::nullopt;
    }
    wchar_t buffer[128]{};
    const int length = GetWindowTextW(edit, buffer, static_cast<int>(std::size(buffer)));
    if (length <= 0) {
        return std::nullopt;
    }
    std::wistringstream stream(std::wstring(buffer, static_cast<std::size_t>(length)));
    stream.imbue(std::locale::classic());
    double value = 0.0;
    stream >> value;
    if (!stream || !stream.eof() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::string> selected_combo_text(const HWND combo) {
    if (combo == nullptr) {
        return std::nullopt;
    }
    const LRESULT selection = SendMessageW(combo, CB_GETCURSEL, 0U, 0);
    if (selection == CB_ERR) {
        return std::nullopt;
    }
    const LRESULT length = SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(selection), 0);
    if (length == CB_ERR || length <= 0) {
        return std::nullopt;
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(selection), reinterpret_cast<LPARAM>(wide.data()));
    std::string narrow;
    narrow.reserve(wide.size());
    for (const wchar_t character : wide) {
        if (character < 0 || character > 127) {
            return std::nullopt;
        }
        narrow.push_back(static_cast<char>(character));
    }
    return narrow;
}

void populate_metric_combo(const HWND combo, const quarry::JobManifest& manifest, const std::size_t initial) {
    if (combo == nullptr) {
        return;
    }
    for (const std::string& metric : manifest.metrics) {
        const std::wstring wide = widen_ascii(metric);
        SendMessageW(combo, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(wide.c_str()));
    }
    if (!manifest.metrics.empty()) {
        const std::size_t selected = (std::min)(initial, manifest.metrics.size() - 1U);
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
    }
}

void refresh_results(WindowState& state) {
    clear_results(state);
    auto loaded = quarry::read_results(state.manifest_path, 500U);
    if (loaded.is_error()) {
        set_status(state, L"Results unavailable: " + widen_ascii(loaded.error().message));
        return;
    }
    for (const auto& result : loaded.value()) {
        std::wostringstream line;
        line << L'#' << result.index << L"  " << widen_ascii(result.candidate_id) << L"  ";
        for (std::size_t index = 0U; index < result.metrics.size(); ++index) {
            if (index != 0U) {
                line << L" | ";
            }
            line << widen_ascii(result.metrics[index].name) << L'=' << result.metrics[index].value;
        }
        add_result_line(state, result.index, line.str());
    }
    set_status(state, L"Raw authoritative results; showing at most the first 500 candidates.");
}

[[nodiscard]] bool ensure_analysis(WindowState& state) {
    if (state.analysis_ready) {
        return true;
    }
    auto progress = quarry::inspect_job(state.manifest_path);
    if (progress.is_error()) {
        set_status(state, L"Analysis unavailable: " + widen_ascii(progress.error().message));
        return false;
    }
    if (!progress.value().complete) {
        set_status(state, L"Diversity analysis requires a completed Quarry job.");
        return false;
    }
    set_status(state, L"Loading deterministic diversity features and canonical thumbnails...");
    auto results = quarry::read_results(state.manifest_path);
    if (results.is_error()) {
        set_status(state, L"Analysis unavailable: " + widen_ascii(results.error().message));
        return false;
    }
    auto normalization = quarry::build_normalization_model(results.value());
    if (normalization.is_error()) {
        set_status(state, L"Analysis unavailable: " + widen_ascii(normalization.error().message));
        return false;
    }
    auto features = quarry::build_candidate_features(
        state.manifest,
        results.value(),
        quarry::derive_job_paths(state.manifest_path).cache_directory,
        normalization.value());
    if (features.is_error()) {
        set_status(state, L"Analysis unavailable: " + widen_ascii(features.error().message));
        return false;
    }
    state.analysis_results = std::move(results).value();
    state.normalization = std::move(normalization).value();
    state.features = std::move(features).value();
    state.analysis_ready = true;
    return true;
}

void show_dedupe(WindowState& state) {
    if (!ensure_analysis(state)) {
        return;
    }
    auto groups = quarry::deduplicate_candidates(state.features, state.normalization);
    if (groups.is_error()) {
        set_status(state, L"Dedupe error: " + widen_ascii(groups.error().message));
        return;
    }
    clear_results(state);
    const std::size_t display_count = (std::min<std::size_t>)(groups.value().size(), 500U);
    for (std::size_t index = 0U; index < display_count; ++index) {
        const auto& group = groups.value()[index];
        std::wostringstream line;
        line << L'#' << group.representative << L"  representative | near-duplicate group size=" << group.members.size();
        add_result_line(state, group.representative, line.str());
    }
    std::wostringstream status;
    status << L"Dedupe view: " << groups.value().size() << L" representatives from " << state.features.size()
           << L" authoritative candidates; defaults metric<=0.025 AND image-MAD<=0.020. Results are not deleted.";
    set_status(state, status.str());
}

void show_unusual(WindowState& state) {
    if (!ensure_analysis(state)) {
        return;
    }
    auto ranked = quarry::rank_unusual(state.features, state.normalization, {}, quarry::UnusualBasis::population);
    if (ranked.is_error()) {
        set_status(state, L"Unusual ranking error: " + widen_ascii(ranked.error().message));
        return;
    }
    clear_results(state);
    const std::size_t display_count = (std::min<std::size_t>)(ranked.value().size(), 500U);
    for (std::size_t index = 0U; index < display_count; ++index) {
        const auto& score = ranked.value()[index];
        std::wostringstream line;
        line << L'#' << score.index << L"  distance=" << score.distance << L" | contributors ";
        const std::size_t contributor_count = (std::min<std::size_t>)(score.contributions.size(), 3U);
        for (std::size_t contributor = 0U; contributor < contributor_count; ++contributor) {
            if (contributor != 0U) {
                line << L", ";
            }
            line << widen_ascii(score.contributions[contributor].name) << L'=' << score.contributions[contributor].contribution;
        }
        add_result_line(state, score.index, line.str());
    }
    set_status(state, L"Unusual view: weighted normalized distance from the population centre, not an aesthetic/quality score.");
}

void show_neighbours(WindowState& state) {
    if (!ensure_analysis(state)) {
        return;
    }
    auto selected = selected_candidate(state);
    if (!selected.has_value() && !state.features.empty()) {
        selected = state.features.front().index;
    }
    if (!selected.has_value()) {
        set_status(state, L"Select a candidate before neighbourhood exploration.");
        return;
    }
    quarry::NeighbourSettings settings;
    settings.maximum_results = 500U;
    const LRESULT mode = state.neighbour_mode == nullptr ? 2 : SendMessageW(state.neighbour_mode, CB_GETCURSEL, 0U, 0);
    if (mode == 0) {
        settings.mode = quarry::NeighbourMode::metric;
    } else if (mode == 1) {
        settings.mode = quarry::NeighbourMode::parameter;
    } else {
        settings.mode = quarry::NeighbourMode::combined;
        settings.minimum_metric_diversity = 0.025;
    }
    auto nearby = quarry::nearest_neighbours(state.features, state.normalization, *selected, settings);
    if (nearby.is_error()) {
        set_status(state, L"Neighbour error: " + widen_ascii(nearby.error().message));
        return;
    }
    clear_results(state);
    for (const auto& result : nearby.value()) {
        std::wostringstream line;
        line << L'#' << result.index << L"  d=" << result.distance
             << L" | metric=" << result.metric_distance << L" | parameter=" << result.parameter_distance;
        add_result_line(state, result.index, line.str());
    }
    const wchar_t* mode_name = settings.mode == quarry::NeighbourMode::metric ? L"metric distance" :
        settings.mode == quarry::NeighbourMode::parameter ? L"normalized parameter distance" :
        L"50/50 RMS metric+parameter distance with metric-diversity floor 0.025";
    set_status(state, L"Neighbourhood around #" + std::to_wstring(*selected) + L": " + mode_name + L".");
}

void show_projection(WindowState& state) {
    if (!ensure_analysis(state)) {
        return;
    }
    const auto x = selected_combo_text(state.projection_x);
    const auto y = selected_combo_text(state.projection_y);
    if (!x.has_value() || !y.has_value()) {
        set_status(state, L"Choose two metric axes for projection.");
        return;
    }
    auto projected = quarry::project_metric_axes(state.features, *x, *y);
    if (projected.is_error()) {
        set_status(state, L"Projection error: " + widen_ascii(projected.error().message));
        return;
    }
    clear_results(state);
    const std::size_t display_count = (std::min<std::size_t>)(projected.value().size(), 500U);
    for (std::size_t index = 0U; index < display_count; ++index) {
        const auto& point = projected.value()[index];
        std::wostringstream line;
        line << L'#' << point.index << L"  (" << point.x << L", " << point.y << L')';
        add_result_line(state, point.index, line.str());
    }
    set_status(state, L"Projection uses literal normalized axes " + widen_ascii(*x) + L" / " + widen_ascii(*y) +
        L"; it is not literal geometry for the full metric space.");
}

void apply_filter(WindowState& state) {
    if (!ensure_analysis(state)) {
        return;
    }
    const auto metric = selected_combo_text(state.filter_metric);
    if (!metric.has_value()) {
        set_status(state, L"Choose a metric to filter.");
        return;
    }
    quarry::MetricRangeFilter filter;
    filter.name = *metric;
    filter.minimum = edit_double(state.filter_min);
    filter.maximum = edit_double(state.filter_max);
    auto filtered = quarry::filter_and_sort_candidates(
        state.features,
        {filter},
        {quarry::StableSortKind::raw_metric, *metric, false});
    if (filtered.is_error()) {
        set_status(state, L"Filter error: " + widen_ascii(filtered.error().message));
        return;
    }
    clear_results(state);
    const std::size_t display_count = (std::min<std::size_t>)(filtered.value().size(), 500U);
    for (std::size_t index = 0U; index < display_count; ++index) {
        const core::u64 candidate_index = filtered.value()[index];
        const quarry::CandidateFeatures* candidate = feature_for(state, candidate_index);
        double raw = 0.0;
        if (candidate != nullptr) {
            const auto value = std::find_if(candidate->raw_metrics.begin(), candidate->raw_metrics.end(), [&](const auto& item) {
                return item.name == *metric;
            });
            if (value != candidate->raw_metrics.end()) {
                raw = value->value;
            }
        }
        std::wostringstream line;
        line << L'#' << candidate_index << L"  " << widen_ascii(*metric) << L'=' << raw;
        add_result_line(state, candidate_index, line.str());
    }
    set_status(state, L"Stable raw-metric filter/sort applied; blank min/max means unbounded on that side.");
}

void save_selected(WindowState& state, const bool favourite) {
    if (!ensure_analysis(state)) {
        return;
    }
    const auto selected = selected_candidate(state);
    if (!selected.has_value()) {
        set_status(state, L"Select a candidate to save.");
        return;
    }
    const auto authoritative = std::find_if(state.analysis_results.begin(), state.analysis_results.end(), [&](const auto& result) {
        return result.index == *selected;
    });
    if (authoritative == state.analysis_results.end()) {
        set_status(state, L"Selected candidate is not present in authoritative job results.");
        return;
    }
    auto recipe = quarry::materialize_candidate_recipe(state.manifest, *selected);
    if (recipe.is_error()) {
        set_status(state, L"Save error: " + widen_ascii(recipe.error().message));
        return;
    }
    if (core::semantic_fingerprint(recipe.value()) != authoritative->recipe_fingerprint) {
        set_status(state, L"Save refused: reconstructed recipe identity disagrees with authoritative result row.");
        return;
    }
    const std::filesystem::path recipes_root = state.manifest_path.parent_path() / "recipes";
    auto saved = favourite
        ? platform::windows::save_favourite(recipes_root, recipe.value())
        : platform::windows::save_recipe_copy(recipes_root, recipe.value());
    if (saved.is_error()) {
        set_status(state, L"Save error: " + saved.error().message);
        return;
    }
    set_status(state, (favourite ? L"Favourite saved: " : L"Recipe saved: ") + saved.value().wstring() +
        L"; semantic fingerprint and Quarry provenance preserved.");
}

void start_run(WindowState& state) {
    if (state.running.load(std::memory_order_relaxed)) {
        return;
    }
    if (state.worker.joinable()) {
        state.worker.join();
    }
    state.analysis_ready = false;
    state.analysis_results.clear();
    state.features.clear();
    state.cancellation = std::make_unique<quarry::CancellationToken>();
    quarry::CancellationToken* token = state.cancellation.get();
    const HWND window = state.window;
    const std::filesystem::path manifest = state.manifest_path;
    state.running.store(true, std::memory_order_relaxed);
    set_status(state, L"Quarry running...");
    state.worker = std::thread([token, window, manifest]() {
        auto completed = quarry::run_job(
            manifest,
            default_worker_count(),
            token,
            [window](const quarry::JobProgress& progress) {
                auto* copy = new quarry::JobProgress(progress);
                if (PostMessageW(window, kProgressMessage, 0U, reinterpret_cast<LPARAM>(copy)) == FALSE) {
                    delete copy;
                }
            });
        auto* message = new Completion;
        if (completed.is_error()) {
            message->success = false;
            message->error = completed.error().message;
        } else {
            message->success = true;
            message->progress = completed.value();
        }
        if (PostMessageW(window, kCompleteMessage, 0U, reinterpret_cast<LPARAM>(message)) == FALSE) {
            delete message;
        }
    });
}

void cancel_run(WindowState& state) {
    if (state.cancellation != nullptr) {
        state.cancellation->cancel();
        set_status(state, L"Cancellation requested; finishing the current bounded worker batch...");
    }
}

void discard_pending_worker_messages(const HWND window) {
    MSG pending{};
    while (PeekMessageW(&pending, window, kProgressMessage, kCompleteMessage, PM_REMOVE) != FALSE) {
        if (pending.message == kProgressMessage) {
            delete reinterpret_cast<quarry::JobProgress*>(pending.lParam);
        } else if (pending.message == kCompleteMessage) {
            delete reinterpret_cast<Completion*>(pending.lParam);
        }
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<WindowState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    switch (message) {
    case WM_CREATE: {
        CreateWindowExW(0U, L"BUTTON", L"Run / Resume", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        12, 12, 120, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRunButton)), nullptr, nullptr);
        CreateWindowExW(0U, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        140, 12, 76, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelButton)), nullptr, nullptr);
        CreateWindowExW(0U, L"BUTTON", L"Raw", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        224, 12, 62, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshButton)), nullptr, nullptr);
        CreateWindowExW(0U, L"BUTTON", L"Dedupe", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        294, 12, 78, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDedupeButton)), nullptr, nullptr);
        CreateWindowExW(0U, L"BUTTON", L"Unusual", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        380, 12, 78, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUnusualButton)), nullptr, nullptr);
        state->neighbour_mode = CreateWindowExW(0U, L"COMBOBOX", nullptr,
                        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                        466, 12, 128, 120, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNeighbourMode)), nullptr, nullptr);
        SendMessageW(state->neighbour_mode, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(L"Metric nearby"));
        SendMessageW(state->neighbour_mode, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(L"Parameter nearby"));
        SendMessageW(state->neighbour_mode, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(L"Mixed + diverse"));
        SendMessageW(state->neighbour_mode, CB_SETCURSEL, 2U, 0);
        CreateWindowExW(0U, L"BUTTON", L"Nearby", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        602, 12, 74, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNearbyButton)), nullptr, nullptr);
        CreateWindowExW(0U, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        684, 12, 62, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSaveButton)), nullptr, nullptr);
        CreateWindowExW(0U, L"BUTTON", L"Favourite", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        754, 12, 86, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFavouriteButton)), nullptr, nullptr);

        CreateWindowExW(0U, L"STATIC", L"Filter", WS_CHILD | WS_VISIBLE, 12, 50, 38, 22, window, nullptr, nullptr, nullptr);
        state->filter_metric = CreateWindowExW(0U, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                        54, 47, 150, 180, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFilterMetric)), nullptr, nullptr);
        state->filter_min = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                        212, 47, 82, 24, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFilterMin)), nullptr, nullptr);
        state->filter_max = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                        302, 47, 82, 24, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFilterMax)), nullptr, nullptr);
        CreateWindowExW(0U, L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        392, 46, 62, 27, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFilterButton)), nullptr, nullptr);
        CreateWindowExW(0U, L"STATIC", L"Axes", WS_CHILD | WS_VISIBLE, 468, 50, 34, 22, window, nullptr, nullptr, nullptr);
        state->projection_x = CreateWindowExW(0U, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                        506, 47, 142, 180, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProjectionX)), nullptr, nullptr);
        state->projection_y = CreateWindowExW(0U, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                        656, 47, 142, 180, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProjectionY)), nullptr, nullptr);
        CreateWindowExW(0U, L"BUTTON", L"Project", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        806, 46, 74, 27, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProjectButton)), nullptr, nullptr);

        populate_metric_combo(state->filter_metric, state->manifest, 0U);
        populate_metric_combo(state->projection_x, state->manifest, 0U);
        populate_metric_combo(state->projection_y, state->manifest, 1U);

        state->status = CreateWindowExW(0U, L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE,
                                        12, 80, 950, 42, window, nullptr, nullptr, nullptr);
        state->results = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT,
                                         12, 126, 950, 520, window,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kResultList)), nullptr, nullptr);
        refresh_results(*state);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case kRunButton:
            start_run(*state);
            return 0;
        case kCancelButton:
            cancel_run(*state);
            return 0;
        case kRefreshButton:
            refresh_results(*state);
            return 0;
        case kDedupeButton:
            show_dedupe(*state);
            return 0;
        case kUnusualButton:
            show_unusual(*state);
            return 0;
        case kNearbyButton:
            show_neighbours(*state);
            return 0;
        case kProjectButton:
            show_projection(*state);
            return 0;
        case kFilterButton:
            apply_filter(*state);
            return 0;
        case kSaveButton:
            save_selected(*state, false);
            return 0;
        case kFavouriteButton:
            save_selected(*state, true);
            return 0;
        default:
            break;
        }
        break;
    case kProgressMessage: {
        std::unique_ptr<quarry::JobProgress> progress(reinterpret_cast<quarry::JobProgress*>(lparam));
        if (progress != nullptr) {
            std::wostringstream text;
            text << L"Committed " << progress->committed << L'/' << progress->total
                 << L"; cache hits this run " << progress->cache_hits;
            set_status(*state, text.str());
        }
        return 0;
    }
    case kCompleteMessage: {
        std::unique_ptr<Completion> complete(reinterpret_cast<Completion*>(lparam));
        if (state->worker.joinable()) {
            state->worker.join();
        }
        state->running.store(false, std::memory_order_relaxed);
        if (complete != nullptr && complete->success) {
            std::wostringstream text;
            text << (complete->progress.complete ? L"Quarry complete: " : L"Quarry paused: ")
                 << complete->progress.committed << L'/' << complete->progress.total;
            set_status(*state, text.str());
        } else if (complete != nullptr) {
            set_status(*state, L"Quarry error: " + widen_ascii(complete->error));
        }
        refresh_results(*state);
        return 0;
    }
    case WM_CLOSE:
        cancel_run(*state);
        if (state->worker.joinable()) {
            state->worker.join();
        }
        state->running.store(false, std::memory_order_relaxed);
        discard_pending_worker_messages(window);
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int run_quarry_application(const std::filesystem::path& manifest_path) {
    auto manifest = quarry::read_job_manifest(manifest_path);
    if (manifest.is_error()) {
        MessageBoxA(nullptr, manifest.error().message.c_str(), "ArtMiner Quarry", MB_OK | MB_ICONERROR);
        return 6;
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0U && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return 5;
    }

    WindowState state;
    state.manifest_path = manifest_path;
    state.manifest = manifest.value();
    const std::wstring title = L"ArtMiner Quarry — " + widen_ascii(manifest.value().identity);
    HWND window = CreateWindowExW(
        0U,
        kWindowClass,
        title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        990,
        720,
        nullptr,
        nullptr,
        instance,
        &state);
    if (window == nullptr) {
        return 5;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0U, 0U) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (state.worker.joinable()) {
        if (state.cancellation != nullptr) {
            state.cancellation->cancel();
        }
        state.worker.join();
    }
    return static_cast<int>(message.wParam);
}

}  // namespace artminer::app
