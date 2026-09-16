#include "app/quarry_window.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include <windows.h>

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

struct Completion final {
    bool success{false};
    quarry::JobProgress progress;
    std::string error;
};

struct WindowState final {
    std::filesystem::path manifest_path;
    HWND window{nullptr};
    HWND status{nullptr};
    HWND results{nullptr};
    std::unique_ptr<quarry::CancellationToken> cancellation;
    std::thread worker;
    std::atomic<bool> running{false};
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

void refresh_results(WindowState& state) {
    if (state.results == nullptr) {
        return;
    }
    SendMessageW(state.results, LB_RESETCONTENT, 0U, 0);
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
        const std::wstring text = line.str();
        SendMessageW(state.results, LB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(text.c_str()));
    }
}

void start_run(WindowState& state) {
    if (state.running.load(std::memory_order_relaxed)) {
        return;
    }
    if (state.worker.joinable()) {
        state.worker.join();
    }
    state.cancellation = std::make_unique<quarry::CancellationToken>();
    quarry::CancellationToken* token = state.cancellation.get();
    const HWND window = state.window;
    const std::filesystem::path manifest = state.manifest_path;
    state.running.store(true, std::memory_order_relaxed);
    set_status(state, L"Quarry running...");
    state.worker = std::thread([&state, token, window, manifest]() {
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
        state.running.store(false, std::memory_order_relaxed);
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
                        12, 12, 120, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRunButton)), nullptr, nullptr);
        CreateWindowExW(0U, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        140, 12, 90, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelButton)), nullptr, nullptr);
        CreateWindowExW(0U, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        238, 12, 90, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshButton)), nullptr, nullptr);
        state->status = CreateWindowExW(0U, L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE,
                                        12, 50, 900, 24, window, nullptr, nullptr, nullptr);
        state->results = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT,
                                         12, 80, 940, 510, window,
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
    const std::wstring title = L"ArtMiner Quarry — " + widen_ascii(manifest.value().identity);
    HWND window = CreateWindowExW(
        0U,
        kWindowClass,
        title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        990,
        660,
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
        state.cancellation->cancel();
        state.worker.join();
    }
    return static_cast<int>(message.wParam);
}

}  // namespace artminer::app
