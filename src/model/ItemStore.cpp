#include "model/ItemStore.h"

#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "core/DesignTokens.h"
#include "core/Log.h"

namespace liquidock {
namespace {

constexpr wchar_t kFileName[] = L"items.txt";

const wchar_t kFileHeader[] =
    L"# LiquiDock items. One per line:\n"
    L"#\n"
    L"#     group | path | label\n"
    L"#\n"
    L"# group is `main` or `utility` - utility items sit to the right of the\n"
    L"# hairline. label is optional and defaults to the file's own name. path is\n"
    L"# anything the shell can open: a program, a shortcut, a folder, a\n"
    L"# `shell:AppsFolder\\...` moniker, or a `::{guid}` parsing name.\n"
    L"#\n"
    L"# Environment variables are expanded. Blank lines and # lines are ignored.\n"
    L"# LiquiDock rereads this file when it starts.\n"
    L"\n";

std::wstring Trim(std::wstring_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == L' ' || text[begin] == L'\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == L' ' || text[end - 1] == L'\t' ||
                           text[end - 1] == L'\r' || text[end - 1] == L'\n')) {
        --end;
    }
    return std::wstring(text.substr(begin, end - begin));
}

// A shell parsing name is not a path and must not be touched: expanding it is
// harmless, but testing it for existence would wrongly reject it.
bool IsShellMoniker(const std::wstring& path) {
    if (path.rfind(L"::", 0) == 0) {
        return true;
    }
    return _wcsnicmp(path.c_str(), L"shell:", 6) == 0;
}

std::wstring LabelFromPath(const std::wstring& path) {
    if (IsShellMoniker(path)) {
        return path;
    }
    const std::filesystem::path parsed(path);
    std::wstring stem = parsed.stem().wstring();
    if (stem.empty()) {
        stem = parsed.filename().wstring();
    }
    return stem.empty() ? path : stem;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) {
        result = raw;
    }
    if (raw) {
        CoTaskMemFree(raw);
    }
    return result;
}

} // namespace

std::wstring ItemStore::ExpandPath(const std::wstring& raw) {
    if (raw.find(L'%') == std::wstring::npos) {
        return raw;
    }
    wchar_t buffer[MAX_PATH * 2];
    const DWORD written =
        ExpandEnvironmentStringsW(raw.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
    // The return counts the terminator, and is 0 on failure or larger than the
    // buffer when it did not fit. Both mean the original string is the best
    // answer available.
    if (written == 0 || written > std::size(buffer)) {
        return raw;
    }
    return std::wstring(buffer, written - 1);
}

std::wstring ItemStore::ConfigPath() {
    const std::wstring base = KnownFolder(FOLDERID_LocalAppData);
    if (base.empty()) {
        return {};
    }
    const std::filesystem::path directory = std::filesystem::path(base) / L"LiquiDock";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    return (directory / kFileName).wstring();
}

int ItemStore::MainCount() const {
    return static_cast<int>(
        std::count_if(items_.begin(), items_.end(),
                      [](const DockItem& item) { return item.group == ItemGroup::Main; }));
}

int ItemStore::UtilityCount() const {
    return static_cast<int>(items_.size()) - MainCount();
}

void ItemStore::Load() {
    items_.clear();

    const std::wstring path = ConfigPath();
    if (!path.empty() && ReadFile(path) && !items_.empty()) {
        LogInfo("Loaded {} dock items from the config file", items_.size());
        return;
    }

    SeedDefaults();
    if (!path.empty()) {
        Save(); // so the user has something to edit
    }
    LogInfo("Seeded {} dock items ({} pinned)", items_.size(), MainCount());
}

bool ItemStore::ReadFile(const std::wstring& path) {
    FILE* file = nullptr;
    // ccs=UTF-8 makes the CRT honour a BOM if there is one and treat the file
    // as UTF-8 either way, so an entry typed into Notepad survives.
    if (_wfopen_s(&file, path.c_str(), L"rt, ccs=UTF-8") != 0 || !file) {
        return false;
    }

    wchar_t line[2048];
    while (fgetws(line, static_cast<int>(std::size(line)), file)) {
        const std::wstring text = Trim(line);
        if (text.empty() || text[0] == L'#') {
            continue;
        }

        // group | path | label, with the label optional. Splitting on the first
        // two separators only means a label containing a pipe still survives.
        const size_t first = text.find(L'|');
        if (first == std::wstring::npos) {
            LogWarn("Skipping a malformed item line (no separator)");
            continue;
        }
        const size_t second = text.find(L'|', first + 1);

        DockItem item;
        const std::wstring group = Trim(text.substr(0, first));
        item.group =
            (_wcsicmp(group.c_str(), L"utility") == 0) ? ItemGroup::Utility : ItemGroup::Main;
        if (second == std::wstring::npos) {
            item.path = Trim(text.substr(first + 1));
        } else {
            item.path = Trim(text.substr(first + 1, second - first - 1));
            item.label = Trim(text.substr(second + 1));
        }

        if (item.path.empty()) {
            continue;
        }
        if (item.label.empty()) {
            item.label = LabelFromPath(item.path);
        }
        if (static_cast<int>(items_.size()) >= design::kMaxItems) {
            LogWarn("Dock item limit ({}) reached; ignoring the rest of the file",
                    design::kMaxItems);
            break;
        }
        items_.push_back(std::move(item));
    }

    fclose(file);

    // Utility items always sit to the right of the hairline, whatever order the
    // file lists them in. Stable, so the file's order survives within a group.
    std::stable_partition(items_.begin(), items_.end(),
                          [](const DockItem& item) { return item.group == ItemGroup::Main; });
    return true;
}

bool ItemStore::Save() const {
    const std::wstring path = ConfigPath();
    if (path.empty()) {
        return false;
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wt, ccs=UTF-8") != 0 || !file) {
        LogWarn("Could not write the dock config file");
        return false;
    }

    // Text mode turns every \n into a CRLF on the way out, so the file opens
    // cleanly in Notepad without the source here being littered with \r.
    fputws(kFileHeader, file);
    for (const DockItem& item : items_) {
        fwprintf(file, L"%s | %s | %s\n",
                 item.group == ItemGroup::Utility ? L"utility" : L"main", item.path.c_str(),
                 item.label.c_str());
    }
    fclose(file);
    return true;
}

bool ItemStore::Remove(size_t index) {
    if (index >= items_.size()) {
        return false;
    }
    items_.erase(items_.begin() + static_cast<ptrdiff_t>(index));
    Save();
    return true;
}

void ItemStore::SeedDefaults() {
    // The taskbar's pinned shortcuts. Windows keeps them as ordinary .lnk files
    // in this folder; their *order* lives in a binary registry value with no
    // documented layout, so these come out alphabetically. That is a reasonable
    // first arrangement, and reordering the file is one drag in Notepad.
    const std::wstring appData = KnownFolder(FOLDERID_RoamingAppData);
    if (!appData.empty()) {
        const std::filesystem::path pinned =
            std::filesystem::path(appData) /
            L"Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar";

        std::vector<std::filesystem::path> shortcuts;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(pinned, ec)) {
            if (entry.is_regular_file(ec) &&
                _wcsicmp(entry.path().extension().c_str(), L".lnk") == 0) {
                shortcuts.push_back(entry.path());
            }
        }
        std::sort(shortcuts.begin(), shortcuts.end());

        for (const auto& shortcut : shortcuts) {
            if (items_.size() >= 10) {
                break; // a first run should look like a dock, not like a taskbar
            }
            items_.push_back(
                DockItem{shortcut.wstring(), shortcut.stem().wstring(), ItemGroup::Main, -1});
        }
    }

    if (items_.empty()) {
        // Nothing pinned. Fall back to programs every Windows install has.
        const wchar_t* fallbacks[] = {
            L"%WINDIR%\\explorer.exe",
            L"%WINDIR%\\system32\\notepad.exe",
            L"%WINDIR%\\system32\\mspaint.exe",
            L"%WINDIR%\\system32\\control.exe",
        };
        for (const wchar_t* candidate : fallbacks) {
            const std::wstring expanded = ExpandPath(candidate);
            std::error_code ec;
            if (std::filesystem::exists(expanded, ec)) {
                items_.push_back(
                    DockItem{candidate, LabelFromPath(expanded), ItemGroup::Main, -1});
            }
        }
    }

    // The standing shortcuts, mirroring the right-hand run in the design.
    items_.push_back(DockItem{L"%USERPROFILE%\\Downloads", L"Downloads", ItemGroup::Utility, -1});
    items_.push_back(DockItem{L"::{645FF040-5081-101B-9F08-00AA002F954E}", L"Recycle Bin",
                              ItemGroup::Utility, -1});
}

} // namespace liquidock
