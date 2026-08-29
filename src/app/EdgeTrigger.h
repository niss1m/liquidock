#pragma once

#include <windows.h>

#include <functional>

namespace liquidock {

// Notices the cursor arriving at a strip of the screen edge, and says so.
//
// Armed only while the dock is hidden; silent the rest of the time. See the
// comment in Create() for why this polls the cursor rather than using a window
// at the edge, a mouse hook, or raw input.
class EdgeTrigger {
public:
    using Callback = std::function<void()>;

    EdgeTrigger() = default;
    EdgeTrigger(const EdgeTrigger&) = delete;
    EdgeTrigger& operator=(const EdgeTrigger&) = delete;
    ~EdgeTrigger();

    bool Create(Callback onEnter);
    void Destroy();

    // The region of screen that counts as "the edge". Called whenever the dock
    // is placed.
    void SetBounds(int x, int y, int width, int height);

    // Arming is what starts the polling; while the dock is on screen there is
    // nothing to watch for and no timer runs.
    void SetEnabled(bool enabled);

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    HWND hwnd_ = nullptr;
    Callback onEnter_;
    RECT bounds_{};
    bool enabled_ = true;
};

} // namespace liquidock
