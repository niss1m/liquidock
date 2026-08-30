#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "model/DockItem.h"

namespace liquidock {

// The dock's contents, and the file they live in.
//
// The file was once one item per line - `group | path | label` - because it has to be
// hand-editable long before the preferences UI in M4 exists, and because a
// dock's contents are a list, not an object graph. Anything richer would be
// inventing a config format to hold twelve strings.
//
// On a first run there is no file, so the list is seeded from whatever the user
// has pinned to their taskbar. That is the closest thing Windows has to "the
// apps this person actually uses", and it means the dock is useful the moment
// it starts rather than empty until configured.
class ItemStore {
public:
    // Puts the list back the way it was before the last change. Returns false
    // if there is nothing to undo.
    //
    // A stack of whole lists rather than a log of reversible operations. A dock
    // holds a few dozen short records, so a snapshot is a few kilobytes and
    // costs nothing to take - and it is right by construction, where an undo
    // log has to get every operation's inverse right and is wrong the first
    // time someone adds an operation and forgets.
    bool Undo();
    bool can_undo() const { return !undo_.empty(); }

    // Loads the config file, seeding and writing it first if it does not exist.
    // Always succeeds: a missing or unreadable file yields the built-in
    // defaults rather than an empty dock.
    void Load();

    // Writes the current list back out. Called after a context-menu removal.
    bool Save() const;

    const std::vector<DockItem>& items() const { return items_; }
    std::vector<DockItem>& items() { return items_; }


    // Removes one item and rewrites the file. Returns false if the index is out
    // of range.
    bool Remove(size_t index);

    // Adds `item` to the end of the list and writes the file. False if the
    // dock is already full.
    bool Add(DockItem item);

    // Swaps an item with its neighbour so a move
    // can never shuffle an app across the hairline into the utility run.
    // Returns the item's new index, or -1 if it could not move.
    int Move(size_t index, int direction);

    // Moves an item to a new index.
    // Distinct from Move: that is the up/down buttons stepping one place, this
    // is a drag dropping somewhere entirely. Returns the resulting index.
    int MoveTo(size_t from, size_t to);

    // Rewrites one item in place and saves. Used by the preferences editor,
    // which edits a copy so a half-typed path is never what gets written.
    bool Replace(size_t index, DockItem item);

    // The same dialog, asked for an image and for a folder. Both live here for
    // the same reason PickProgram does: one place for the flags to be right.
    static bool PickImage(HWND owner, std::wstring* out);
    static bool PickFolder(HWND owner, std::wstring* out);

    // Asks for a program with the standard file dialog. False if cancelled.
    // Lives here because acquiring an item is the item store's business, and
    // because both the dock's context menu and the preferences window need it -
    // two copies of a file dialog is two places for the flags to drift.
    static bool PickProgram(HWND owner, DockItem* out);

    // %LOCALAPPDATA%\LiquiDock\items.txt, created on demand. Empty if the
    // directory could not be resolved.
    static std::wstring ConfigPath();

    // Expands environment variables, and nothing else. Shell parsing names
    // (`::{...}`, `shell:...`) are passed through untouched.
    static std::wstring ExpandPath(const std::wstring& raw);

private:
    bool ReadFile(const std::wstring& path);
    void SeedDefaults();
    // Snapshots the current list. Called by every mutation before it mutates.
    void Remember();

    std::vector<DockItem> items_;
    std::vector<std::vector<DockItem>> undo_;
};

} // namespace liquidock
