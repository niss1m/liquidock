#pragma once

#include <windows.h>

namespace liquidock {

// A notification-area icon and its context menu.
//
// The dock itself is click-through and has no chrome, so this is the only way
// to quit it or reach preferences. It lives on its own message-only window
// rather than on the dock window, which keeps shell callbacks off the hot
// rendering path.
class TrayIcon {
public:
    TrayIcon() = default;
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;
    ~TrayIcon();

    bool Create();
    void Destroy();

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool AddIcon();
    void ShowMenu();

    HWND hwnd_ = nullptr;
    UINT taskbarCreatedMessage_ = 0;
    bool iconAdded_ = false;
};

} // namespace liquidock
