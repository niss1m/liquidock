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

    // `owner` is posted `showSettings` when Preferences is chosen. The tray
    // does not own the preferences window; the dock does, because the dock owns
    // the graphics device it draws with.
    bool Create(HWND owner = nullptr, UINT showSettings = 0);
    void Destroy();

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool AddIcon();
    void ShowMenu();

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    UINT showSettings_ = 0;
    UINT taskbarCreatedMessage_ = 0;
    bool iconAdded_ = false;
};

} // namespace liquidock
