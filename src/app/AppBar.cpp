#include "app/AppBar.h"

#include <shellapi.h>

#include "core/Log.h"

namespace liquidock {

AppBar::~AppBar() {
    Unregister();
}

bool AppBar::Register(HWND hwnd, UINT callback) {
    if (registered_) {
        return true;
    }
    hwnd_ = hwnd;
    callback_ = callback;

    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uCallbackMessage = callback_;
    if (!SHAppBarMessage(ABM_NEW, &data)) {
        LogWarn("Could not register the dock as an appbar");
        return false;
    }
    registered_ = true;
    LogInfo("Registered as an appbar");
    return true;
}

void AppBar::Unregister() {
    if (!registered_) {
        return;
    }
    Release();

    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    SHAppBarMessage(ABM_REMOVE, &data);
    registered_ = false;
    LogInfo("Unregistered as an appbar");
}

bool AppBar::Reserve(const RECT& monitor, int heightPx, RECT* reserved) {
    if (!registered_ || heightPx <= 0) {
        return false;
    }

    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uEdge = ABE_BOTTOM;
    data.rc = monitor;
    data.rc.top = monitor.bottom - heightPx;

    // QUERYPOS moves the proposed rectangle out of the way of the taskbar and
    // any other appbar already on this edge. It only tells us where the band may
    // start, so the height has to be re-applied afterwards - taking its answer
    // verbatim would let the band be squeezed to nothing.
    SHAppBarMessage(ABM_QUERYPOS, &data);
    data.rc.top = data.rc.bottom - heightPx;

    SHAppBarMessage(ABM_SETPOS, &data);
    reserving_ = true;
    if (reserved) {
        *reserved = data.rc;
    }
    return true;
}

void AppBar::Release() {
    if (!registered_ || !reserving_) {
        return;
    }
    // A zero-height band at the bottom edge: still registered, claiming nothing.
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uEdge = ABE_BOTTOM;
    SHAppBarMessage(ABM_SETPOS, &data);
    reserving_ = false;
}

} // namespace liquidock
