#include "model/SystemItems.h"

#include <knownfolders.h>
#include <shellapi.h>
#include <shldisp.h>
#include <shlobj.h>
#include <shobjidl_core.h>
#include <wrl/client.h>

#include <filesystem>

#include "core/Log.h"
#include "model/ItemStore.h"

namespace liquidock {
namespace {

using Microsoft::WRL::ComPtr;

// One row of the hand-written table, before this machine has been asked which
// of its candidates it actually has.
struct Candidate {
    const wchar_t* id;
    const wchar_t* label;
    const wchar_t* hint;
    // Tried in order; the first that resolves is the one offered. Two are
    // enough: the packaged app people actually have, and the executable that
    // has been sitting in System32 since long before it for when they do not.
    const wchar_t* paths[2];
    SystemAction action;
    bool live;
    int stockIcon;
};

// The list. Deliberately short, and deliberately not a directory listing: these
// are the things a dock gets asked for by name, and every one of them is either
// impossible to add today or fiddly enough that nobody would.
const Candidate kCandidates[] = {
    {L"recycle-bin", L"Recycle Bin", L"What you have deleted, until you empty it",
     {L"::{645FF040-5081-101B-9F08-00AA002F954E}", nullptr},
     SystemAction::Open, true, -1},
    {L"this-pc", L"This PC", L"Every drive on this machine",
     {L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", nullptr},
     SystemAction::Open, false, -1},
    {L"explorer", L"File Explorer", L"A new window, at the folder it opens on",
     {L"%WINDIR%\\explorer.exe", nullptr},
     SystemAction::Open, false, -1},
    {L"downloads", L"Downloads", L"Where the browser puts things",
     {L"shell:Downloads", nullptr},
     SystemAction::Open, false, -1},
    {L"documents", L"Documents", L"Your documents folder",
     {L"shell:Personal", nullptr},
     SystemAction::Open, false, -1},
    {L"pictures", L"Pictures", L"Your pictures folder",
     {L"shell:My Pictures", nullptr},
     SystemAction::Open, false, -1},
    {L"network", L"Network", L"The machines and shares this one can see",
     {L"::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}", nullptr},
     SystemAction::Open, false, -1},
    {L"calculator", L"Calculator", L"The Windows calculator",
     {L"shell:AppsFolder\\Microsoft.WindowsCalculator_8wekyb3d8bbwe!App",
      L"%WINDIR%\\System32\\calc.exe"},
     SystemAction::Open, false, -1},
    {L"settings", L"Settings", L"The Windows settings app",
     {L"shell:AppsFolder\\windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows."
      L"immersivecontrolpanel",
      nullptr},
     SystemAction::Open, false, -1},
    {L"control-panel", L"Control Panel", L"The older settings, the ones that never moved",
     {L"%WINDIR%\\System32\\control.exe", nullptr},
     SystemAction::Open, false, -1},
    {L"task-manager", L"Task Manager", L"What is running, and what it is costing",
     {L"%WINDIR%\\System32\\Taskmgr.exe", nullptr},
     SystemAction::Open, false, -1},
    {L"show-desktop", L"Show Desktop", L"Sweeps every window aside, and puts them back",
     {nullptr, nullptr},
     SystemAction::ShowDesktop, false, SIID_DESKTOPPC},
    {L"lock", L"Lock", L"Locks the session, leaving everything running",
     {nullptr, nullptr},
     SystemAction::Lock, false, SIID_LOCK},
};

bool IsShellMoniker(const std::wstring& path) {
    return path.rfind(L"::", 0) == 0 || _wcsnicmp(path.c_str(), L"shell:", 6) == 0;
}

// Whether this machine has the thing the candidate names.
//
// A parsing name is asked of the shell, because "does this exist" is not a
// question the file system can answer about the Recycle Bin or about a packaged
// app; a file path is asked of the file system, because putting the shell
// through a namespace parse to stat an executable is a lot of machinery for a
// test with one right answer.
bool Resolves(const std::wstring& candidate) {
    if (candidate.empty()) {
        return false;
    }
    const std::wstring expanded = ItemStore::ExpandPath(candidate);
    if (IsShellMoniker(expanded)) {
        ComPtr<IShellItem> item;
        return SUCCEEDED(
            SHCreateItemFromParsingName(expanded.c_str(), nullptr, IID_PPV_ARGS(&item)));
    }
    std::error_code ec;
    return std::filesystem::exists(expanded, ec);
}

std::vector<SystemEntry> Build() {
    std::vector<SystemEntry> entries;
    entries.reserve(std::size(kCandidates));
    for (const Candidate& candidate : kCandidates) {
        SystemEntry entry;
        entry.id = candidate.id;
        entry.label = candidate.label;
        entry.hint = candidate.hint;
        entry.action = candidate.action;
        entry.live = candidate.live;
        entry.stockIcon = candidate.stockIcon;

        if (candidate.action == SystemAction::Open) {
            for (const wchar_t* path : candidate.paths) {
                if (path && Resolves(path)) {
                    entry.path = path;
                    break;
                }
            }
            if (entry.path.empty()) {
                // Not on this machine. Left out rather than listed and broken:
                // an entry that does nothing when clicked is a bug report.
                LogInfo("A system entry is not available on this machine; not offering it");
                continue;
            }
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace

const std::vector<SystemEntry>& SystemEntries() {
    // Built once, on whichever thread asks first. The initialisation guard is
    // what makes that safe, and the answer cannot change without an install.
    static const std::vector<SystemEntry> entries = Build();
    return entries;
}

DockItem MakeSystemItem(const SystemEntry& entry) {
    DockItem item;
    item.systemId = entry.id;
    item.label = entry.label;
    item.path = entry.path;
    return item;
}

const SystemEntry* FindSystemEntry(const std::wstring& id) {
    if (id.empty()) {
        return nullptr;
    }
    for (const SystemEntry& entry : SystemEntries()) {
        if (_wcsicmp(entry.id.c_str(), id.c_str()) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

const SystemEntry* FindSystemEntryByPath(const std::wstring& path) {
    if (path.empty()) {
        return nullptr;
    }
    // Expanded on both sides, because the table spells things with %WINDIR% and
    // a config file written by a person spells them out. The two name the same
    // program and the point of asking is to find out whether they do.
    const std::wstring wanted = ItemStore::ExpandPath(path);
    for (const SystemEntry& entry : SystemEntries()) {
        if (entry.path.empty()) {
            continue;
        }
        if (_wcsicmp(ItemStore::ExpandPath(entry.path).c_str(), wanted.c_str()) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

int SystemStateIcon(const SystemEntry& entry) {
    if (!entry.live) {
        return -1;
    }
    if (entry.id == L"recycle-bin") {
        SHQUERYRBINFO info{};
        info.cbSize = sizeof(info);
        // A null root asks about every drive, which is the only answer that
        // matches what the icon is claiming: a bin that is empty on C: and full
        // on D: is a full bin.
        if (FAILED(SHQueryRecycleBinW(nullptr, &info))) {
            return SIID_RECYCLER;
        }
        return info.i64NumItems > 0 ? SIID_RECYCLERFULL : SIID_RECYCLER;
    }
    return -1;
}

void RunSystemAction(SystemAction action) {
    switch (action) {
        case SystemAction::Lock:
            LockWorkStation();
            return;

        case SystemAction::ShowDesktop: {
            // The same call the taskbar corner makes, which is why it toggles:
            // once to clear the screen and again to get everything back.
            // Posting MIN_ALL to the tray window is the older trick and only
            // goes one way, so the second click would do nothing.
            ComPtr<IShellDispatch4> shell;
            if (FAILED(CoCreateInstance(CLSID_Shell, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&shell)))) {
                LogWarn("Show Desktop: the shell would not answer");
                return;
            }
            shell->ToggleDesktop();
            return;
        }

        case SystemAction::Open:
            return; // handled by the ordinary launch path
    }
}

ULONG WatchLiveItems(HWND window, UINT message) {
    if (!window) {
        return 0;
    }
    PIDLIST_ABSOLUTE bin = nullptr;
    if (FAILED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, 0, nullptr, &bin)) || !bin) {
        return 0;
    }

    SHChangeNotifyEntry watched{};
    watched.pidl = bin;
    watched.fRecursive = TRUE;

    // NewDelivery hands the notification over in shared memory that the
    // receiver locks and releases, which is more work at the other end and is
    // the only form that is safe when the sender can outlive the message.
    // ShellLevel and InterruptLevel together are what catch both a file being
    // deleted and the bin itself being emptied: those are raised by different
    // parts of the shell, and registering for one misses half of what the icon
    // exists to show.
    const ULONG registration = SHChangeNotifyRegister(
        window,
        SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_RecursiveInterrupt |
            SHCNRF_NewDelivery,
        SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR | SHCNE_UPDATEDIR |
            SHCNE_UPDATEITEM | SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER | SHCNE_UPDATEIMAGE,
        message, 1, &watched);
    CoTaskMemFree(bin);

    if (registration == 0) {
        LogWarn("The shell would not report Recycle Bin changes; its icon will only be re-read "
                "when the dock reloads");
    }
    return registration;
}

void StopWatchingLiveItems(ULONG registration) {
    if (registration != 0) {
        SHChangeNotifyDeregister(registration);
    }
}

} // namespace liquidock
