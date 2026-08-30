#include "core/ConfigWatcher.h"

#include "core/ConfigPaths.h"
#include "core/Log.h"

namespace liquidock {

bool ConfigWatcher::Start() {
    Stop();

    const std::wstring directory = ConfigDirectory();
    if (directory.empty()) {
        return false;
    }

    // LAST_WRITE alone is not enough. Plenty of editors - and every tool that
    // wants a save to be atomic - write a temporary file and rename it over the
    // original, which changes no file's contents and so raises no write
    // notification at all. The dock would then keep running against settings
    // the user had already saved and watched the file not take effect.
    handle_ = FindFirstChangeNotificationW(
        directory.c_str(), FALSE,
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE);
    if (!valid()) {
        LogWarn("Could not watch the config directory: {}", GetLastError());
        handle_ = nullptr;
        return false;
    }
    LogInfo("Watching the config directory for edits");
    return true;
}

void ConfigWatcher::Stop() {
    if (valid()) {
        FindCloseChangeNotification(handle_);
    }
    handle_ = nullptr;
}

void ConfigWatcher::Acknowledge() {
    if (valid() && !FindNextChangeNotification(handle_)) {
        // The directory was deleted or the handle went bad. Drop the watch
        // rather than spinning on a handle that will signal forever.
        LogWarn("Config directory watch lapsed: {}", GetLastError());
        Stop();
    }
}

} // namespace liquidock
