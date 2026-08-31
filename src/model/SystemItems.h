#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "model/DockItem.h"

namespace liquidock {

// What clicking one of these does. Most of them open something and go through
// exactly the same ShellExecute path every other dock item does; two of them
// are commands with nothing to open, and those are the only reason this enum
// exists.
enum class SystemAction {
    Open,        // launch `path`, like any other item
    ShowDesktop, // sweep the windows aside, or put them back
    Lock,        // the session, not the screen saver
};

// One of the things Windows can do that is not an application.
//
// A dock is a launcher, and the shell has plenty worth launching that no Start
// menu entry points at: the Recycle Bin, This PC, the network, Show Desktop.
// The installed-app list cannot offer any of them, because none of them are
// installed - they are places and commands inside the shell itself. So they are
// enumerated here, by hand, which is also the only way to say the two things a
// list of executables cannot: what a thing is for, and whether its icon changes
// while you are looking at it.
//
// `id` is what the config file stores - `system = recycle-bin` rather than a
// GUID nobody can read - and it is what lets the dock recognise an entry later.
// `path` is still written alongside it, so a file written by a newer build,
// naming an id this one has never heard of, still launches the right thing.
struct SystemEntry {
    std::wstring id;
    std::wstring label;
    // One line, for the tooltip. Half the point of offering these is that
    // "This PC" and "Network" mean nothing until someone says what they open.
    std::wstring hint;
    // What the shell is handed. Empty for the two commands.
    std::wstring path;
    SystemAction action = SystemAction::Open;
    // Its icon depends on something that changes while the dock is running, so
    // it is re-read whenever the shell says that something moved. Only the
    // Recycle Bin, so far.
    bool live = false;
    // An SHSTOCKICONID, for the entries with no path to ask about. -1 when the
    // shell can be asked directly, which is the better answer whenever it can.
    int stockIcon = -1;
};

// The entries this machine can actually offer, resolved once.
//
// Every candidate is checked before it is listed: a Store app can be
// uninstalled, and an entry that launches nothing is worse than one that was
// never offered. Built on first use and cached, because the checks touch the
// shell and the answer cannot change without an install.
const std::vector<SystemEntry>& SystemEntries();

// A dock entry for `entry`, ready to be added to the list.
DockItem MakeSystemItem(const SystemEntry& entry);

// The entry `id` names, or null - which is what an id from a newer build looks
// like, and is why nothing here is allowed to assume it found one.
const SystemEntry* FindSystemEntry(const std::wstring& id);

// The entry that launches `path`, or null.
//
// For recognising an entry that was added before it had a name to be added
// under: the seeded Recycle Bin has been a bare GUID in items.txt since the
// first build, and matching it here is what turns it into the live one on the
// next load rather than leaving it a picture of an empty bin forever.
const SystemEntry* FindSystemEntryByPath(const std::wstring& path);

// The stock icon a live entry should be wearing right now: the full bin or the
// empty one. -1 for anything whose icon does not depend on the machine's state.
int SystemStateIcon(const SystemEntry& entry);

// Runs one of the commands. Blocking - ToggleDesktop talks to Explorer - so
// call it off the UI thread.
void RunSystemAction(SystemAction action);

// Asks the shell to post `message` to `window` whenever anything a live entry
// depends on changes. Returns a registration to hand back to
// StopWatchingLiveItems, or 0 if the shell would not take it.
//
// A notification rather than a timer: the bin fills and empties a handful of
// times a day, and a dock that wakes up every second to ask has given up the
// one thing this project promised.
ULONG WatchLiveItems(HWND window, UINT message);
void StopWatchingLiveItems(ULONG registration);

} // namespace liquidock
