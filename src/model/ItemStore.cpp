#include "model/ItemStore.h"

#include <shlobj.h>
#include <shobjidl_core.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "core/ConfigPaths.h"
#include "core/DesignTokens.h"
#include "core/Log.h"

namespace liquidock {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kFileName[] = L"items.txt";

const wchar_t kFileHeader[] =
    L"# LiquiDock items.\n"
    L"#\n"
    L"# One block per entry:\n"
    L"#\n"
    L"#     [item]\n"
    L"#     group   = main | utility     utility items sit right of the hairline\n"
    L"#     path    = what to launch     a program, shortcut, folder, document, a\n"
    L"#                                  shell:AppsFolder\\... moniker for a Store app,\n"
    L"#                                  or a ::{guid} parsing name\n"
    L"#     label   = what to call it    optional; defaults to the file name\n"
    L"#     args    = arguments          optional\n"
    L"#     workdir = starting folder    optional\n"
    L"#     icon    = an image file      optional; png, jpg, bmp, ico, gif, tiff\n"
    L"#\n"
    L"# Environment variables are expanded. Blank lines and # lines are ignored.\n"
    L"# The old `group | path | label` one-liners are still read.\n"
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
    return ConfigFilePath(kFileName);
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

    DockItem current;
    bool building = false;
    auto flush = [this, &current, &building]() {
        if (!building || current.path.empty()) {
            building = false;
            current = DockItem{};
            return;
        }
        if (current.label.empty()) {
            current.label = LabelFromPath(current.path);
        }
        if (static_cast<int>(items_.size()) < design::kMaxItems) {
            items_.push_back(std::move(current));
        }
        current = DockItem{};
        building = false;
    };

    wchar_t line[2048];
    while (fgetws(line, static_cast<int>(std::size(line)), file)) {
        const std::wstring text = Trim(line);
        if (text.empty() || text[0] == L'#') {
            continue;
        }

        if (_wcsicmp(text.c_str(), L"[item]") == 0) {
            flush();
            building = true;
            continue;
        }

        // The old one-line form, kept because a file someone already has must
        // not stop working when the format grows.
        if (!building && text.find(L'|') != std::wstring::npos) {
            const size_t first = text.find(L'|');
            const size_t second = text.find(L'|', first + 1);
            DockItem item;
            item.group = (_wcsicmp(Trim(text.substr(0, first)).c_str(), L"utility") == 0)
                             ? ItemGroup::Utility
                             : ItemGroup::Main;
            if (second == std::wstring::npos) {
                item.path = Trim(text.substr(first + 1));
            } else {
                item.path = Trim(text.substr(first + 1, second - first - 1));
                item.label = Trim(text.substr(second + 1));
            }
            if (!item.path.empty()) {
                if (item.label.empty()) {
                    item.label = LabelFromPath(item.path);
                }
                if (static_cast<int>(items_.size()) < design::kMaxItems) {
                    items_.push_back(std::move(item));
                }
            }
            continue;
        }

        const size_t equals = text.find(L'=');
        if (equals == std::wstring::npos || !building) {
            continue;
        }
        const std::wstring key = Trim(text.substr(0, equals));
        const std::wstring value = Trim(text.substr(equals + 1));

        if (key == L"group") {
            current.group = (_wcsicmp(value.c_str(), L"utility") == 0) ? ItemGroup::Utility
                                                                       : ItemGroup::Main;
        } else if (key == L"path") {
            current.path = value;
        } else if (key == L"label") {
            current.label = value;
        } else if (key == L"args") {
            current.arguments = value;
        } else if (key == L"workdir") {
            current.workingDirectory = value;
        } else if (key == L"icon") {
            current.iconPath = value;
        } else {
            LogWarn("Unknown key in items.txt; ignoring it");
        }
    }
    flush();

    fclose(file);

    if (static_cast<int>(items_.size()) >= design::kMaxItems) {
        LogWarn("Dock item limit ({}) reached; the rest of the file was ignored",
                design::kMaxItems);
    }

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

    // Text mode turns every newline into a CRLF on the way out, so the file
    // opens cleanly in Notepad without the source here being littered with
    // carriage returns.
    fputws(kFileHeader, file);
    for (const DockItem& item : items_) {
        fwprintf(file, L"[item]\n");
        fwprintf(file, L"group   = %s\n",
                 item.group == ItemGroup::Utility ? L"utility" : L"main");
        fwprintf(file, L"path    = %s\n", item.path.c_str());
        fwprintf(file, L"label   = %s\n", item.label.c_str());
        // Only written when set, so the common entry stays four short lines
        // rather than seven with three of them empty.
        if (!item.arguments.empty()) {
            fwprintf(file, L"args    = %s\n", item.arguments.c_str());
        }
        if (!item.workingDirectory.empty()) {
            fwprintf(file, L"workdir = %s\n", item.workingDirectory.c_str());
        }
        if (!item.iconPath.empty()) {
            fwprintf(file, L"icon    = %s\n", item.iconPath.c_str());
        }
        fwprintf(file, L"\n");
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

bool ItemStore::Add(DockItem item) {
    if (static_cast<int>(items_.size()) >= design::kMaxItems) {
        LogWarn("The dock is full ({} items); not adding another", design::kMaxItems);
        return false;
    }
    if (item.label.empty()) {
        item.label = LabelFromPath(item.path);
    }
    // At the end of its own group, so a new app does not land on the far side of
    // the hairline among the folders.
    const size_t at = (item.group == ItemGroup::Main) ? static_cast<size_t>(MainCount())
                                                      : items_.size();
    items_.insert(items_.begin() + static_cast<ptrdiff_t>(at), std::move(item));
    Save();
    return true;
}

int ItemStore::Move(size_t index, int direction) {
    if (index >= items_.size() || direction == 0) {
        return -1;
    }
    const ptrdiff_t target = static_cast<ptrdiff_t>(index) + (direction > 0 ? 1 : -1);
    if (target < 0 || target >= static_cast<ptrdiff_t>(items_.size())) {
        return -1;
    }
    // Groups are contiguous and the hairline sits between them, so refusing to
    // swap across a group boundary is what keeps the two runs meaningful.
    if (items_[static_cast<size_t>(target)].group != items_[index].group) {
        return -1;
    }
    std::swap(items_[index], items_[static_cast<size_t>(target)]);
    Save();
    return static_cast<int>(target);
}

bool ItemStore::PickProgram(HWND owner, DockItem* out) {
    if (!out) {
        return false;
    }
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return false;
    }

    static const COMDLG_FILTERSPEC kTypes[] = {
        {L"Programs and shortcuts", L"*.lnk;*.exe;*.url"},
        {L"All files", L"*.*"},
    };
    dialog->SetFileTypes(ARRAYSIZE(kTypes), kTypes);
    dialog->SetTitle(L"Add to the dock");

    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    // NODEREFERENCELINKS keeps a shortcut a shortcut: a .lnk carries the
    // arguments and working directory its target needs, and resolving it here
    // would quietly throw both away.
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_NODEREFERENCELINKS |
                       FOS_FORCEFILESYSTEM);

    if (FAILED(dialog->Show(owner))) {
        return false; // cancelled
    }

    ComPtr<IShellItem> result;
    PWSTR path = nullptr;
    if (FAILED(dialog->GetResult(&result)) ||
        FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) {
        return false;
    }
    out->path = path;
    out->group = ItemGroup::Main;
    out->label = LabelFromPath(out->path);
    CoTaskMemFree(path);
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
            DockItem item;
            item.path = shortcut.wstring();
            item.label = shortcut.stem().wstring();
            items_.push_back(std::move(item));
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
                DockItem item;
                item.path = candidate;
                item.label = LabelFromPath(expanded);
                items_.push_back(std::move(item));
            }
        }
    }

    // The standing shortcuts, mirroring the right-hand run in the design.
    DockItem downloads;
    downloads.path = L"%USERPROFILE%\\Downloads";
    downloads.label = L"Downloads";
    downloads.group = ItemGroup::Utility;
    items_.push_back(std::move(downloads));

    DockItem bin;
    bin.path = L"::{645FF040-5081-101B-9F08-00AA002F954E}";
    bin.label = L"Recycle Bin";
    bin.group = ItemGroup::Utility;
    items_.push_back(std::move(bin));
}

} // namespace liquidock
