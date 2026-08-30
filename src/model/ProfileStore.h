#pragma once

#include <string>
#include <vector>

namespace liquidock {

// Named copies of the settings file, kept beside it in a `profiles` folder.
//
// A profile is not a separate format or a subset - it is the whole settings
// file under a name. Switching copies it over the live one and lets the config
// watcher do the rest, which means a profile can be edited by hand, backed up,
// or sent to someone exactly like the file it came from.
class ProfileStore {
public:
    // The names on disk, sorted, without their extension.
    static std::vector<std::wstring> List();

    // Writes the live settings file into `name`. Overwrites silently: the
    // preferences window asks before it calls this.
    static bool Save(const std::wstring& name);

    // Copies `name` over the live settings file. The dock notices the write and
    // reloads, so nothing here has to know about the dock.
    static bool Load(const std::wstring& name);

    static bool Remove(const std::wstring& name);
    static bool Exists(const std::wstring& name);

    // A name that cannot escape the folder or collide with the file system's
    // own ideas. Empty if nothing usable is left after cleaning.
    static std::wstring Clean(const std::wstring& name);

private:
    static std::wstring Folder();
    static std::wstring PathFor(const std::wstring& name);
};

} // namespace liquidock
