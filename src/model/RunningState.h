#pragma once

#include <windows.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace liquidock {

// Which dock items are currently running.
//
// "Running" means the item's executable owns at least one ordinary top-level
// window. That is a narrower definition than "the process exists" and a better
// one for a dock: a browser's background updater is not something the user
// wants a light under, and an app with a window is the thing they might be
// trying to switch to.
//
// Event-driven, not polled. A WinEvent hook for top-level window creation and
// destruction is the only signal that matters, and both are rare; the scan
// itself is then debounced, because opening a folder can create a dozen windows
// in a burst and one scan afterwards answers for all of them.
class RunningState {
public:
    RunningState() = default;
    RunningState(const RunningState&) = delete;
    RunningState& operator=(const RunningState&) = delete;
    ~RunningState();

    // `notify` is posted `message` when the set of running windows may have
    // changed. The caller decides when to act on it.
    bool Initialize(HWND notify, UINT message);
    void Shutdown();

    // The executables to watch, one per dock item, in item order. An empty
    // string means the item has nothing to watch - a folder, or the recycle bin.
    void SetTargets(std::vector<std::wstring> executables);

    // Rebuilds the running set. Returns true if it changed, so a rescan that
    // finds nothing new costs no redraw.
    bool Refresh();

    bool IsRunning(size_t index) const {
        return index < running_.size() && running_[index];
    }

    // One of the item's windows, or null. Which one is unspecified beyond
    // "ordinary and visible" - an app with several windows has no canonical
    // one, and the first found is as good an answer as any.
    HWND WindowFor(size_t index) const {
        if (index >= targets_.size() || targets_[index].empty()) {
            return nullptr;
        }
        const auto found = windows_.find(targets_[index]);
        return (found == windows_.end()) ? nullptr : found->second;
    }

private:
    static void CALLBACK EventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject,
                                   LONG idChild, DWORD thread, DWORD time);

    HWINEVENTHOOK hook_ = nullptr;
    HWND notify_ = nullptr;
    UINT message_ = 0;

    // Lowercased full paths, so comparison is a hash lookup rather than a
    // case-insensitive compare against every item.
    std::vector<std::wstring> targets_;
    std::vector<bool> running_;
    std::unordered_set<std::wstring> live_;
    // One window per executable, so a click can switch to what is already open
    // rather than starting another copy of it.
    std::unordered_map<std::wstring, HWND> windows_;
};

} // namespace liquidock
