#pragma once

#include <windows.h>

namespace liquidock {

// Notices the settings file being saved.
//
// A polling timer would be the obvious way and is the one thing this dock is
// not allowed to do: the whole point of the architecture is that an idle dock
// costs no wakeups, and two file stats a second is exactly the sort of cost
// that never shows up in a benchmark and always shows up in a battery graph.
//
// FindFirstChangeNotification hands back a waitable handle instead, so the
// message loop waits on the handle and the message queue together with
// MsgWaitForMultipleObjectsEx and blocks indefinitely until one of them has
// something. Nothing changing means nothing runs.
//
// The handle signals for any write anywhere in the config directory, the log
// file included, so the loop still has to ask Settings whether the settings
// file itself actually changed before acting.
class ConfigWatcher {
public:
    ConfigWatcher() = default;
    ConfigWatcher(const ConfigWatcher&) = delete;
    ConfigWatcher& operator=(const ConfigWatcher&) = delete;
    ~ConfigWatcher() { Stop(); }

    bool Start();
    void Stop();

    // INVALID_HANDLE_VALUE if the watch could not be established, in which case
    // the dock simply reads its settings once at startup.
    HANDLE handle() const { return handle_; }
    bool valid() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

    // Re-arms the notification. Must be called after every signal or the handle
    // stays signalled and the wait returns immediately, forever.
    void Acknowledge();

private:
    HANDLE handle_ = nullptr;
};

} // namespace liquidock
