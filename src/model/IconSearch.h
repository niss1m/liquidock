#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace liquidock {

// One icon offered by macOSicons.com.
//
// `file` is where the PNG landed on disk. Downloading it *is* the pick: an
// entry's icon is a path to an image file, so the cached download is the final
// artefact and there is no second copy made when one is chosen.
struct IconHit {
    std::wstring appName;
    std::wstring pngUrl;
    // Who made it. The API's terms ask that the site and the author both be
    // credited wherever an icon is used, so it is carried with the icon rather
    // than thrown away at the point of choosing - which is the only place it
    // would still have been known.
    std::wstring credit;
    std::wstring creditUrl;
    int downloads = 0;
    std::wstring file;
};

// Searching macOSicons.com, off the UI thread.
//
// The one place LiquiDock talks to the network, and it does so only while
// somebody is typing into the icon search. Nothing here runs at startup,
// nothing polls, and a dock that never opens this panel never opens a socket.
//
// The key is the user's own. macOSicons issues one per account and its terms
// forbid sharing it, so there is none to ship - and a key embedded in a
// source-available repository would not be a secret anyway. It lives in a file
// of its own rather than in settings.txt, which is what keeps it out of a
// shared config token and out of a saved profile by construction rather than by
// remembering to exclude it.
class IconSearch {
public:
    IconSearch() = default;
    IconSearch(const IconSearch&) = delete;
    IconSearch& operator=(const IconSearch&) = delete;
    ~IconSearch();

    // %LOCALAPPDATA%\LiquiDock\macosicons.key, and what is in it.
    static std::wstring KeyPath();
    static std::wstring LoadKey();
    static bool SaveKey(const std::wstring& key);
    static void ForgetKey();
    static bool HasKey();

    // Where downloaded PNGs live. Created on demand.
    static std::wstring CacheDirectory();

    // Runs a search and then fetches every result's PNG. `notify` is posted
    // `message` twice: once when the names are known, once when the images
    // are on disk. Cancels and replaces whatever was already running.
    void Start(std::wstring query, HWND notify, UINT message);
    void Stop();

    // What the last search produced. Safe to call at any point; the hits gain
    // their `file` between the two notifications.
    std::vector<IconHit> Take();
    // Empty unless the last search failed, in which case it is something worth
    // putting on screen.
    std::wstring error();
    bool running() const { return running_.load(std::memory_order_relaxed); }

private:
    void Run(std::wstring query, HWND notify, UINT message, unsigned generation);

    std::thread worker_;
    std::mutex mutex_;
    std::vector<IconHit> hits_;
    std::wstring error_;
    std::atomic<unsigned> generation_{0};
    std::atomic<bool> running_{false};
};

} // namespace liquidock
