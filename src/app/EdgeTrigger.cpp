#include "app/EdgeTrigger.h"

#include "core/Log.h"

namespace liquidock {
namespace {

constexpr wchar_t kWindowClass[] = L"LiquiDock.EdgeTrigger";
constexpr UINT_PTR kPollTimer = 1;

// 60 ms is under the threshold where an edge slam feels laggy, and costs about
// 17 wakeups a second. Each one is a GetCursorPos and a rectangle test - call
// it a microsecond - so the whole mechanism runs at roughly 0.002% of a core,
// and only while the dock is actually hidden.
constexpr UINT kPollIntervalMs = 60;

} // namespace

EdgeTrigger::~EdgeTrigger() {
    Destroy();
}

bool EdgeTrigger::Create(Callback onEnter) {
    onEnter_ = std::move(onEnter);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &EdgeTrigger::WndProcThunk;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    // A message-only window purely to own the timer. Nothing is ever drawn.
    //
    // The first attempt here was the tidier idea: a two-pixel layered window
    // pressed against the screen edge, so the cursor arriving would deliver one
    // WM_MOUSEMOVE and cost nothing the rest of the time. WindowFromPoint
    // confirmed that window was correctly placed and was the window at the
    // cursor's position - but no mouse message was ever delivered to it. Rather
    // than keep guessing at layered-window input rules, this polls.
    //
    // A WH_MOUSE_LL hook would also work and would be event-driven, but it puts
    // our code in the path of every mouse event system-wide and serialises
    // input behind our message loop. Raw input with RIDEV_INPUTSINK avoids the
    // hook but still wakes us for every mouse move, which on a 1000 Hz gaming
    // mouse is far more work than seventeen cursor reads a second.
    hwnd_ = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                            wc.hInstance, this);
    if (!hwnd_) {
        LogError("Edge trigger creation failed: {}", GetLastError());
        return false;
    }
    return true;
}

void EdgeTrigger::Destroy() {
    if (hwnd_) {
        KillTimer(hwnd_, kPollTimer);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void EdgeTrigger::SetBounds(int x, int y, int width, int height) {
    bounds_.left = x;
    bounds_.top = y;
    bounds_.right = x + width;
    bounds_.bottom = y + height;
    LogDebug("Edge trigger armed over ({},{})-({},{})", bounds_.left, bounds_.top, bounds_.right,
             bounds_.bottom);
}

void EdgeTrigger::SetEnabled(bool enabled) {
    if (enabled_ == enabled) {
        return;
    }
    enabled_ = enabled;
    if (!hwnd_) {
        return;
    }
    if (enabled) {
        SetTimer(hwnd_, kPollTimer, kPollIntervalMs, nullptr);
    } else {
        KillTimer(hwnd_, kPollTimer);
    }
}

LRESULT CALLBACK EdgeTrigger::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    EdgeTrigger* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<EdgeTrigger*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<EdgeTrigger*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->WndProc(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT EdgeTrigger::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_TIMER && wParam == kPollTimer) {
        POINT cursor{};
        if (GetCursorPos(&cursor) && PtInRect(&bounds_, cursor)) {
            LogDebug("Edge trigger hit at ({},{})", cursor.x, cursor.y);
            if (onEnter_) {
                onEnter_();
            }
        }
        return 0;
    }
    if (message == WM_DESTROY) {
        hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace liquidock
