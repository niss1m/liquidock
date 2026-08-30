#pragma once

#include <windows.h>

#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>

namespace liquidock {

// One thing the shell knows how to launch.
struct CatalogEntry {
    std::wstring label;
    // What ShellExecute would be handed: a `shell:AppsFolder\...` moniker. It
    // is the same string for a packaged app and a desktop one, which is the
    // reason this reads the AppsFolder rather than walking Start Menu folders.
    std::wstring path;
};

// Everything installed, as the Start menu sees it.
//
// Read from the AppsFolder shell namespace rather than by scanning directories.
// The Start Menu's .lnk files miss every packaged app - the Store ones, the
// system ones like Settings and the Calculator - and enumerating both sources
// means reconciling two lists that name the same program differently. The
// AppsFolder is the list the shell itself shows, and each child hands back a
// parsing name that launches it, whichever kind it is.
//
// Enumerated on a worker thread: it touches the shell, and the shell is
// entitled to take its time.
class AppCatalog {
public:
    AppCatalog() = default;
    AppCatalog(const AppCatalog&) = delete;
    AppCatalog& operator=(const AppCatalog&) = delete;
    ~AppCatalog();

    // Starts reading. `notify` is posted `message` when the list is ready.
    void Start(HWND notify, UINT message);
    void Stop();

    // The entries, sorted by name. Empty until the read finishes.
    std::vector<CatalogEntry> Take();
    bool ready() const { return ready_.load(std::memory_order_relaxed); }

private:
    void Run(HWND notify, UINT message);

    std::thread worker_;
    std::mutex mutex_;
    std::vector<CatalogEntry> entries_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> stop_{false};
};

} // namespace liquidock
