#include "model/RunningState.h"

#include <algorithm>

#include "core/Log.h"

namespace liquidock {
namespace {

// The hook is process-wide state with a C callback, so the instance it belongs
// to has to be reachable from it. There is exactly one dock in a process.
RunningState* g_instance = nullptr;

std::wstring Lowercase(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return text;
}

struct ScanContext {
    std::unordered_set<std::wstring>* paths;
    std::unordered_map<std::wstring, HWND>* windows;
    // One process can own many windows, and QueryFullProcessImageName is a
    // syscall each time.
    std::unordered_set<DWORD>* seenPids;
};

BOOL CALLBACK ScanWindow(HWND hwnd, LPARAM param) {
    auto* context = reinterpret_cast<ScanContext*>(param);

    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }
    // Tool windows are palettes and helpers, not the app being "open". Owned
    // windows are dialogs belonging to a window already counted.
    if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) {
        return TRUE;
    }
    if (GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }
    if (GetWindowTextLengthW(hwnd) == 0) {
        return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || !context->seenPids->insert(pid).second) {
        return TRUE;
    }

    // QUERY_LIMITED_INFORMATION rather than QUERY_INFORMATION: it is the one
    // that works against processes at a higher integrity level without the dock
    // needing to be elevated, which is most of them on a normal desktop.
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return TRUE;
    }
    wchar_t path[MAX_PATH * 2];
    DWORD length = static_cast<DWORD>(std::size(path));
    if (QueryFullProcessImageNameW(process, 0, path, &length) && length > 0) {
        const std::wstring key = Lowercase(std::wstring(path, length));
        context->paths->insert(key);
        // First window wins. An app with several has no canonical one, and the
        // alternative - keeping all of them and picking later - is a list per
        // process for a decision that only ever needs one answer.
        context->windows->emplace(key, hwnd);
    }
    CloseHandle(process);
    return TRUE;
}

} // namespace

RunningState::~RunningState() {
    Shutdown();
}

bool RunningState::Initialize(HWND notify, UINT message) {
    notify_ = notify;
    message_ = message;
    g_instance = this;

    // OUTOFCONTEXT keeps our code out of every other process; the callback is
    // delivered to this thread while it pumps messages, which is exactly where
    // it should run. SKIPOWNPROCESS so the dock's own windows never trigger it.
    hook_ = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_DESTROY, nullptr, &EventProc, 0, 0,
                            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hook_) {
        LogWarn("Could not hook window events; running indicators will not update");
        return false;
    }
    return true;
}

void RunningState::Shutdown() {
    if (hook_) {
        UnhookWinEvent(hook_);
        hook_ = nullptr;
    }
    if (g_instance == this) {
        g_instance = nullptr;
    }
}

void CALLBACK RunningState::EventProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG idChild,
                                      DWORD, DWORD) {
    // Only whole top-level windows. Without this filter the hook fires for every
    // menu item, tooltip and caret in the session.
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) {
        return;
    }
    if (g_instance && g_instance->notify_) {
        PostMessageW(g_instance->notify_, g_instance->message_, 0, 0);
    }
}

void RunningState::SetTargets(std::vector<std::wstring> executables) {
    targets_.clear();
    targets_.reserve(executables.size());
    for (std::wstring& path : executables) {
        targets_.push_back(Lowercase(std::move(path)));
    }
    running_.assign(targets_.size(), false);
}

bool RunningState::Refresh() {
    if (targets_.empty()) {
        return false;
    }

    live_.clear();
    windows_.clear();
    std::unordered_set<DWORD> seenPids;
    ScanContext context{&live_, &windows_, &seenPids};
    EnumWindows(&ScanWindow, reinterpret_cast<LPARAM>(&context));

    bool changed = false;
    for (size_t i = 0; i < targets_.size(); ++i) {
        const bool running = !targets_[i].empty() && live_.count(targets_[i]) > 0;
        if (running != running_[i]) {
            running_[i] = running;
            changed = true;
        }
    }
    return changed;
}

} // namespace liquidock
