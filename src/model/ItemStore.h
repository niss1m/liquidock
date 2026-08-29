#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "model/DockItem.h"

namespace liquidock {

// The dock's contents, and the file they live in.
//
// The file is one item per line - `group | path | label` - because it has to be
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
    // Loads the config file, seeding and writing it first if it does not exist.
    // Always succeeds: a missing or unreadable file yields the built-in
    // defaults rather than an empty dock.
    void Load();

    // Writes the current list back out. Called after a context-menu removal.
    bool Save() const;

    const std::vector<DockItem>& items() const { return items_; }
    std::vector<DockItem>& items() { return items_; }

    int MainCount() const;
    int UtilityCount() const;

    // Removes one item and rewrites the file. Returns false if the index is out
    // of range.
    bool Remove(size_t index);

    // Adds `item` to the end of its group and writes the file. False if the
    // dock is already full.
    bool Add(DockItem item);

    // Swaps an item with its neighbour, staying inside its own group so a move
    // can never shuffle an app across the hairline into the utility run.
    // Returns the item's new index, or -1 if it could not move.
    int Move(size_t index, int direction);

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

    std::vector<DockItem> items_;
};

} // namespace liquidock
