#include "app/CoverWatch.h"

#include <dwmapi.h>

#include <iterator>

#include "core/Log.h"

#pragma comment(lib, "dwmapi.lib")

namespace liquidock {
namespace {

// The hook is a global callback with no user pointer, so where to send the
// notification has to live outside the instance. There is exactly one dock in a
// process, and the single-instance guard in main enforces that, so a pair of
// file statics is the honest representation rather than a hidden singleton.
HWND g_notify = nullptr;
UINT g_message = 0;

// The shell's own windows sit at the screen edges by definition. The taskbar
// overlaps the dock's strip on almost every machine, so counting it as coverage
// would mean the dock never came out at all.
bool IsShellWindow(HWND hwnd) {
    wchar_t name[64]{};
    if (GetClassNameW(hwnd, name, static_cast<int>(std::size(name))) == 0) {
        return false;
    }
    return wcscmp(name, L"Shell_TrayWnd") == 0 || wcscmp(name, L"Shell_SecondaryTrayWnd") == 0 ||
           wcscmp(name, L"Progman") == 0 || wcscmp(name, L"WorkerW") == 0;
}

// A suspended packaged app keeps a window that is `IsWindowVisible` but is not
// on screen. Without this test every UWP app the user has ever opened counts as
// covering the dock forever.
bool IsCloaked(HWND hwnd) {
    BOOL cloaked = FALSE;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        return false;
    }
    return cloaked != FALSE;
}

struct ScanState {
    RECT target{};
    DWORD ownProcess = 0;
    bool covered = false;
    char* culprit = nullptr;
    size_t culpritChars = 0;
};

BOOL CALLBACK ScanProc(HWND hwnd, LPARAM param) {
    auto* state = reinterpret_cast<ScanState*>(param);

    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return TRUE;
    }

    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    if (process == state->ownProcess) {
        return TRUE; // the dock, its menu and its preferences do not count
    }

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    // The alt-tab test, near enough: a window someone is working in is one they
    // could switch to. Tool windows, overlays and owned popups are not - and
    // that deliberately includes other docks and always-on-top widgets, which
    // are not "in the way" in any sense the user means.
    if ((exStyle & WS_EX_TOOLWINDOW) != 0) {
        return TRUE;
    }
    if (GetWindow(hwnd, GW_OWNER) != nullptr && (exStyle & WS_EX_APPWINDOW) == 0) {
        return TRUE;
    }
    if (IsShellWindow(hwnd) || IsCloaked(hwnd)) {
        return TRUE;
    }

    RECT bounds{};
    if (!GetWindowRect(hwnd, &bounds)) {
        return TRUE;
    }
    RECT overlap{};
    if (IntersectRect(&overlap, &bounds, &state->target)) {
        state->covered = true;
        if (state->culprit && state->culpritChars > 0) {
            // Narrow on purpose: it goes straight into the log, and a class
            // name is ASCII in every case that matters.
            GetClassNameA(hwnd, state->culprit, static_cast<int>(state->culpritChars));
        }
        return FALSE; // one is enough
    }
    return TRUE;
}

} // namespace

CoverWatch::~CoverWatch() {
    Shutdown();
}

bool CoverWatch::Initialize(HWND notify, UINT message) {
    notify_ = notify;
    message_ = message;
    g_notify = notify;
    g_message = message;

    // EVENT_SYSTEM_FOREGROUND through EVENT_SYSTEM_MINIMIZEEND covers every way
    // the answer changes without a window being created or destroyed: switching
    // apps, minimising and restoring, and the end of a move or resize. Window
    // creation and destruction arrive separately, through RunningState's hook,
    // which the dock already listens to.
    //
    // Deliberately *not* EVENT_OBJECT_LOCATIONCHANGE: that fires continuously
    // while a window is being dragged, and re-scanning every top-level window
    // on each of those would be the one polling loop in the whole program.
    hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, nullptr, &EventProc,
                            0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hook_) {
        LogWarn("Cover watch hook failed; the dock will hide on its dwell regardless");
        return false;
    }
    return true;
}

void CoverWatch::Shutdown() {
    if (hook_) {
        UnhookWinEvent(hook_);
        hook_ = nullptr;
    }
    if (g_notify == notify_) {
        g_notify = nullptr;
        g_message = 0;
    }
}

void CALLBACK CoverWatch::EventProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG idChild,
                                    DWORD, DWORD) {
    // Only whole windows. A foreground change reports the window; the accessible
    // objects inside it are somebody else's business.
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || hwnd == nullptr) {
        return;
    }
    if (g_notify && g_message) {
        PostMessageW(g_notify, g_message, 0, 0);
    }
}

bool CoverWatch::IsCovered(const RECT& screenRect, char* culprit, size_t culpritChars) {
    ScanState state{};
    state.target = screenRect;
    state.ownProcess = GetCurrentProcessId();
    state.culprit = culprit;
    state.culpritChars = culpritChars;
    EnumWindows(&ScanProc, reinterpret_cast<LPARAM>(&state));
    return state.covered;
}

} // namespace liquidock
