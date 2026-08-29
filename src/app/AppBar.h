#pragma once

#include <windows.h>

namespace liquidock {

// Reserves a strip of the screen so maximised windows stop above the dock.
//
// This is the appbar protocol, which is how the taskbar itself does it and the
// only way to change the work area that other applications will actually
// respect. It is worth the fiddliness: without it a maximised window sits
// underneath the dock, and a dock you have to move windows away from is worse
// than no dock.
//
// Registration is deliberately not automatic. Reserving space is a strong claim
// on someone's screen, and it makes no sense at all with auto-hide on - the
// whole point of a dock that hides is that it is not taking any room. So it is
// off by default and the dock only claims space while it is permanently visible.
//
// The one thing this class must never do is leak a registration. An appbar that
// is not unregistered leaves the work area permanently short by its height,
// with nothing on screen to explain why, until the user logs out.
class AppBar {
public:
    AppBar() = default;
    AppBar(const AppBar&) = delete;
    AppBar& operator=(const AppBar&) = delete;
    ~AppBar();

    // `callback` is the private message the shell posts for ABN_* notifications.
    bool Register(HWND hwnd, UINT callback);
    void Unregister();

    // Claims a band `heightPx` tall along the bottom of `monitor`. The shell
    // adjusts the request to fit around the taskbar and any other appbar, and
    // the rectangle it settles on is written back to `reserved`.
    bool Reserve(const RECT& monitor, int heightPx, RECT* reserved);

    // Gives the space back without unregistering, for when the setting is
    // turned off or auto-hide is turned on.
    void Release();

    bool registered() const { return registered_; }
    bool reserving() const { return reserving_; }

private:
    HWND hwnd_ = nullptr;
    UINT callback_ = 0;
    bool registered_ = false;
    bool reserving_ = false;
};

} // namespace liquidock
