#include "app/playback_window.hpp"

#include <Windows.h>
#include <CommCtrl.h>

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/animation.hpp"
#include "nodes/motion_evaluator.hpp"
#include "nodes/static_evaluator.hpp"

namespace artminer::app {
namespace {

constexpr UINT_PTR kPlaybackTimerId = 1U;
constexpr UINT kPlaybackTimerMilliseconds = 16U;
constexpr UINT kFrameReadyMessage = WM_APP + 73U;
constexpr UINT kCommandPlay = 4101U;
constexpr UINT kCommandStep = 4102U;
constexpr UINT kCommandReset = 4103U;
constexpr UINT kSpeedSliderId = 4104U;
constexpr int kControlBandHeight = 94;
constexpr int kGap = 8;

[[nodiscard]] std::wstring widen_utf8(const std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return L"<invalid UTF-8>";
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    (void)MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

struct FrameMessage final {
    core::u64 generation{0U};
    core::u64 tick{0U};
    nodes::Image image;
    std::string error;
};

class RenderWorker final {
public:
    RenderWorker(HWND owner, core::Recipe recipe)
        : owner_(owner), recipe_(std::move(recipe)), thread_([this]() { run(); }) {}

    ~RenderWorker() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            pending_.reset();
        }
        condition_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    RenderWorker(const RenderWorker&) = delete;
    RenderWorker& operator=(const RenderWorker&) = delete;

    void request(const core::u64 generation, const core::u64 tick) {
        {
            std::lock_guard lock(mutex_);
            pending_ = Request{generation, tick};
        }
        condition_.notify_one();
    }

private:
    struct Request final {
        core::u64 generation{0U};
        core::u64 tick{0U};
    };

    void run() {
        nodes::FrameSnapshotCache cache(32U);
        while (true) {
            Request request;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this]() { return stopping_ || pending_.has_value(); });
                if (stopping_) {
                    return;
                }
                request = *pending_;
                pending_.reset();
            }

            auto completed = std::make_unique<FrameMessage>();
            completed->generation = request.generation;
            completed->tick = request.tick;
            auto rendered = nodes::render_animation_reference(recipe_, request.tick, "main", &cache);
            if (rendered.is_error()) {
                completed->error = rendered.error().message;
            } else {
                completed->image = std::move(rendered).value();
            }

            {
                std::lock_guard lock(mutex_);
                if (stopping_) {
                    return;
                }
            }
            if (PostMessageW(owner_, kFrameReadyMessage, 0U, reinterpret_cast<LPARAM>(completed.get())) != FALSE) {
                (void)completed.release();
            }
        }
    }

    HWND owner_{nullptr};
    core::Recipe recipe_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<Request> pending_;
    bool stopping_{false};
    std::thread thread_;
};

struct AppState final {
    core::Recipe recipe;
    core::FixedTickPlayback playback{8U};
    HWND window{nullptr};
    HWND play_button{nullptr};
    HWND step_button{nullptr};
    HWND reset_button{nullptr};
    HWND tick_label{nullptr};
    HWND speed_label{nullptr};
    HWND speed_slider{nullptr};
    HWND status_label{nullptr};
    std::unique_ptr<RenderWorker> worker;
    nodes::Image frame;
    std::vector<core::u8> bgra;
    core::u64 requested_generation{0U};
    core::u64 rendered_tick{0U};
    ULONGLONG last_clock_milliseconds{0U};
    std::wstring status{L"Preparing canonical frame..."};
};

void update_labels(AppState& state) {
    const std::wstring tick_text = L"Tick: " + std::to_wstring(state.playback.tick());
    SetWindowTextW(state.tick_label, tick_text.c_str());
    const std::wstring speed_text = L"Speed: " + std::to_wstring(state.playback.ticks_per_second()) + L" tick/s";
    SetWindowTextW(state.speed_label, speed_text.c_str());
    SetWindowTextW(state.play_button, state.playback.playing() ? L"Pause" : L"Play");
    SetWindowTextW(state.status_label, state.status.c_str());
}

void request_frame(AppState& state) {
    ++state.requested_generation;
    state.status = L"Rendering canonical tick " + std::to_wstring(state.playback.tick()) + L"...";
    update_labels(state);
    state.worker->request(state.requested_generation, state.playback.tick());
}

void layout_children(AppState& state, const int width, const int height) {
    const int band_top = (std::max)(0, height - kControlBandHeight);
    const int button_width = 86;
    const int y = band_top + kGap;
    MoveWindow(state.play_button, kGap, y, button_width, 28, TRUE);
    MoveWindow(state.step_button, kGap * 2 + button_width, y, button_width, 28, TRUE);
    MoveWindow(state.reset_button, kGap * 3 + button_width * 2, y, button_width, 28, TRUE);
    MoveWindow(state.tick_label, kGap * 4 + button_width * 3, y + 4, 150, 22, TRUE);

    const int slider_y = y + 36;
    MoveWindow(state.speed_label, kGap, slider_y + 4, 150, 22, TRUE);
    MoveWindow(state.speed_slider, 160, slider_y, (std::max)(120, width - 168), 28, TRUE);
    MoveWindow(state.status_label, kGap, slider_y + 30, (std::max)(1, width - kGap * 2), 22, TRUE);
}

void draw_frame(AppState& state, HDC dc, const RECT& client) {
    RECT image_area = client;
    image_area.bottom = (std::max)(image_area.top, image_area.bottom - kControlBandHeight);
    FillRect(dc, &image_area, GetSysColorBrush(COLOR_WINDOW));
    if (state.frame.width == 0U || state.frame.height == 0U || state.bgra.empty()) {
        SetBkMode(dc, TRANSPARENT);
        (void)DrawTextW(
            dc,
            state.status.c_str(),
            -1,
            &image_area,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }

    const int available_width = (std::max)(1, image_area.right - image_area.left - kGap * 2);
    const int available_height = (std::max)(1, image_area.bottom - image_area.top - kGap * 2);
    const double x_scale = static_cast<double>(available_width) / static_cast<double>(state.frame.width);
    const double y_scale = static_cast<double>(available_height) / static_cast<double>(state.frame.height);
    const double scale = (std::min)(x_scale, y_scale);
    const int draw_width = (std::max)(1, static_cast<int>(static_cast<double>(state.frame.width) * scale));
    const int draw_height = (std::max)(1, static_cast<int>(static_cast<double>(state.frame.height) * scale));
    const int left = image_area.left + (image_area.right - image_area.left - draw_width) / 2;
    const int top = image_area.top + (image_area.bottom - image_area.top - draw_height) / 2;

    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = static_cast<LONG>(state.frame.width);
    bitmap.bmiHeader.biHeight = -static_cast<LONG>(state.frame.height);
    bitmap.bmiHeader.biPlanes = 1U;
    bitmap.bmiHeader.biBitCount = 32U;
    bitmap.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    (void)StretchDIBits(
        dc,
        left,
        top,
        draw_width,
        draw_height,
        0,
        0,
        static_cast<int>(state.frame.width),
        static_cast<int>(state.frame.height),
        state.bgra.data(),
        &bitmap,
        DIB_RGB_COLORS,
        SRCCOPY);
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

[[nodiscard]] bool create_controls(AppState& state, HINSTANCE instance) {
    state.play_button = create_control(L"BUTTON", L"Play", BS_PUSHBUTTON | WS_TABSTOP, state.window, kCommandPlay, instance);
    state.step_button = create_control(L"BUTTON", L"Step", BS_PUSHBUTTON | WS_TABSTOP, state.window, kCommandStep, instance);
    state.reset_button = create_control(L"BUTTON", L"Reset", BS_PUSHBUTTON | WS_TABSTOP, state.window, kCommandReset, instance);
    state.tick_label = create_control(L"STATIC", L"Tick: 0", SS_LEFT, state.window, 0U, instance);
    state.speed_label = create_control(L"STATIC", L"Speed: 8 tick/s", SS_LEFT, state.window, 0U, instance);
    state.speed_slider = create_control(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, state.window, kSpeedSliderId, instance);
    state.status_label = create_control(L"STATIC", state.status.c_str(), SS_LEFTNOWORDWRAP, state.window, 0U, instance);
    if (state.speed_slider != nullptr) {
        SendMessageW(
            state.speed_slider,
            TBM_SETRANGE,
            TRUE,
            MAKELONG(core::kPlaybackMinimumTicksPerSecond, core::kPlaybackMaximumTicksPerSecond));
        SendMessageW(state.speed_slider, TBM_SETPOS, TRUE, state.playback.ticks_per_second());
        SendMessageW(state.speed_slider, TBM_SETTICFREQ, 5U, 0U);
    }
    return state.play_button != nullptr && state.step_button != nullptr && state.reset_button != nullptr &&
        state.tick_label != nullptr && state.speed_label != nullptr && state.speed_slider != nullptr &&
        state.status_label != nullptr;
}

void handle_command(AppState& state, const UINT command) {
    switch (command) {
    case kCommandPlay:
        state.playback.set_playing(!state.playback.playing());
        state.last_clock_milliseconds = GetTickCount64();
        state.status = state.playback.playing() ? L"Playing fixed ticks." : L"Paused.";
        update_labels(state);
        break;
    case kCommandStep:
        state.playback.step();
        request_frame(state);
        break;
    case kCommandReset:
        state.playback.reset();
        request_frame(state);
        break;
    default:
        break;
    }
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
            handle_command(*state, LOWORD(w_param));
        }
        return 0;
    case WM_HSCROLL:
        if (state != nullptr && reinterpret_cast<HWND>(l_param) == state->speed_slider) {
            const LRESULT position = SendMessageW(state->speed_slider, TBM_GETPOS, 0U, 0U);
            if (position >= static_cast<LRESULT>(core::kPlaybackMinimumTicksPerSecond) &&
                position <= static_cast<LRESULT>(core::kPlaybackMaximumTicksPerSecond)) {
                (void)state->playback.set_ticks_per_second(static_cast<core::u32>(position));
                state->last_clock_milliseconds = GetTickCount64();
                update_labels(*state);
            }
        }
        return 0;
    case kFrameReadyMessage:
        if (state != nullptr && l_param != 0) {
            std::unique_ptr<FrameMessage> completed(reinterpret_cast<FrameMessage*>(l_param));
            if (completed->generation == state->requested_generation) {
                if (!completed->error.empty()) {
                    state->status = L"Render error: " + widen_utf8(completed->error);
                    state->playback.set_playing(false);
                } else {
                    state->frame = std::move(completed->image);
                    state->bgra.resize(state->frame.rgba.size());
                    for (std::size_t offset = 0U; offset + 3U < state->frame.rgba.size(); offset += 4U) {
                        state->bgra[offset + 0U] = state->frame.rgba[offset + 2U];
                        state->bgra[offset + 1U] = state->frame.rgba[offset + 1U];
                        state->bgra[offset + 2U] = state->frame.rgba[offset + 0U];
                        state->bgra[offset + 3U] = state->frame.rgba[offset + 3U];
                    }
                    state->rendered_tick = completed->tick;
                    state->status = L"Canonical tick " + std::to_wstring(completed->tick) +
                        L" | image " + widen_utf8(nodes::image_fingerprint(state->frame));
                }
                update_labels(*state);
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;
    case WM_TIMER:
        if (state != nullptr && w_param == kPlaybackTimerId) {
            const ULONGLONG now = GetTickCount64();
            if (state->last_clock_milliseconds == 0U) {
                state->last_clock_milliseconds = now;
            }
            const ULONGLONG elapsed = now - state->last_clock_milliseconds;
            state->last_clock_milliseconds = now;
            if (state->playback.advance_wall_time(static_cast<core::u64>(elapsed)) != 0U) {
                request_frame(*state);
            }
        }
        return 0;
    case WM_KEYDOWN:
        if (state != nullptr) {
            if (w_param == VK_SPACE) {
                handle_command(*state, kCommandPlay);
                return 0;
            }
            if (w_param == VK_RIGHT) {
                handle_command(*state, kCommandStep);
                return 0;
            }
            if (w_param == static_cast<WPARAM>('R')) {
                handle_command(*state, kCommandReset);
                return 0;
            }
        }
        break;
    case WM_PAINT:
        if (state != nullptr) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            draw_frame(*state, dc, client);
            EndPaint(window, &paint);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (state != nullptr) {
            KillTimer(window, kPlaybackTimerId);
            state->worker.reset();
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

}  // namespace

int run_playback_application(const core::Recipe& recipe) {
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_BAR_CLASSES;
    if (InitCommonControlsEx(&controls) == FALSE) {
        return 3;
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (instance == nullptr) {
        return 3;
    }
    constexpr wchar_t kClassName[] = L"ArtMinerFixedTickPlaybackWindow";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    window_class.lpszClassName = kClassName;
    if (RegisterClassW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return 3;
    }

    AppState state;
    state.recipe = recipe;
    HWND window = CreateWindowExW(
        0U,
        kClassName,
        L"ArtMiner — AM-007 Fixed-Tick Playback",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        980,
        760,
        nullptr,
        nullptr,
        instance,
        &state);
    if (window == nullptr) {
        return 3;
    }
    state.window = window;
    if (!create_controls(state, instance)) {
        DestroyWindow(window);
        return 3;
    }

    RECT client{};
    GetClientRect(window, &client);
    layout_children(state, client.right - client.left, client.bottom - client.top);
    state.worker = std::make_unique<RenderWorker>(window, state.recipe);
    state.last_clock_milliseconds = GetTickCount64();
    request_frame(state);

    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);
    SetTimer(window, kPlaybackTimerId, kPlaybackTimerMilliseconds, nullptr);

    MSG message{};
    while (true) {
        const BOOL result = GetMessageW(&message, nullptr, 0U, 0U);
        if (result == 0) {
            return static_cast<int>(message.wParam);
        }
        if (result == -1) {
            return 3;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

}  // namespace artminer::app
