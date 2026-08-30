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
    L"#     kind    = separator         a divider; it launches nothing and needs\n"
    L"#                                  no path\n"
    L"#     path    = what to launch     a program, shortcut, folder, document, a\n"
    L"#                                  shell:AppsFolder\\... moniker for a Store app,\n"
    L"#                                  or a ::{guid} parsing name\n"
    L"#     label   = what to call it    optional; defaults to the file name\n"
    L"#     args    = arguments          optional\n"
    L"#     workdir = starting folder    optional\n"
    L"#     icon    = an image file      optional; png, jpg, bmp, ico, gif, tiff\n"
    L"#\n"
    L"# Environment variables are expanded. Blank lines and # lines are ignored.\n"
    L"# The old `group | path | label` one-liners are still read; the group they\n"
    L"# start with is ignored.\n"
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
    LogInfo("Seeded {} dock items", items_.size());
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
        if (!building) {
            current = DockItem{};
            return;
        }
        // An entry with no path launches nothing, so a divider is the only
        // thing it can sensibly be. This has to come *before* the test that
        // discards a half-written block, or the block is gone before anything
        // gets to decide what it was - which is what happened when this was
        // written the other way round, and the dividers stayed missing.
        if (current.path.empty()) {
            current.kind = ItemKind::Separator;
        }
        if (current.kind == ItemKind::Separator) {
            if (static_cast<int>(items_.size()) < design::kMaxItems) {
                items_.push_back(std::move(current));
            }
            current = DockItem{};
            building = false;
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
            // The leading field named a group. Skipped rather than parsed:
            // it no longer means anything, and the two after it are what the
            // line was really for.
            DockItem item;
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

        if (key == L"kind") {
            current.kind = (value == L"separator") ? ItemKind::Separator : ItemKind::App;
        } else if (key == L"group") {
            // Read and dropped, so a file written by an older build still loads
            // without warning about a key this one does not know.
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
        } else if (key == L"show") {
            if (_wcsicmp(value.c_str(), L"minimized") == 0) {
                current.runState = RunState::Minimized;
            } else if (_wcsicmp(value.c_str(), L"maximized") == 0) {
                current.runState = RunState::Maximized;
            } else {
                current.runState = RunState::Normal;
            }
        } else if (key == L"admin") {
            current.runAsAdmin = (_wcsicmp(value.c_str(), L"on") == 0 ||
                                  _wcsicmp(value.c_str(), L"true") == 0 || value == L"1");
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

    // The file's order is the dock's order. This used to re-sort utility items
    // to the end on every load, which quietly undid any drag that moved one
    // earlier - the dock obeyed until the next reload and then sprang back.
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
        if (item.kind == ItemKind::Separator) {
            // A divider has nothing else worth writing, and a block of empty
            // keys would only invite someone to fill them in.
            //
            // This branch was missing, so every save wrote a divider as an app
            // with no path - and a divider therefore survived exactly until the
            // next time anything rewrote the file, which adding a second one
            // does immediately.
            fwprintf(file, L"kind    = separator\n");
            fwprintf(file, L"\n");
            continue;
        }
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
        if (item.runState != RunState::Normal) {
            fwprintf(file, L"show    = %s\n",
                     item.runState == RunState::Minimized ? L"minimized" : L"maximized");
        }
        if (item.runAsAdmin) {
            fwprintf(file, L"admin   = on\n");
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
    Remember();
    items_.erase(items_.begin() + static_cast<ptrdiff_t>(index));
    Save();
    return true;
}

void ItemStore::Remember() {
    // Deep enough to cover a bad few seconds and shallow enough that nobody
    // notices the memory. Undoing thirty steps is not a thing anyone does; the
    // step that matters is the one just taken by accident.
    constexpr size_t kDepth = 32;
    undo_.push_back(items_);
    if (undo_.size() > kDepth) {
        undo_.erase(undo_.begin());
    }
}

bool ItemStore::Undo() {
    if (undo_.empty()) {
        return false;
    }
    items_ = std::move(undo_.back());
    undo_.pop_back();
    Save();
    return true;
}

int ItemStore::MoveTo(size_t from, size_t to) {
    if (from >= items_.size() || to > items_.size()) {
        return -1;
    }
    Remember();
    DockItem moving = items_[from];
    items_.erase(items_.begin() + static_cast<ptrdiff_t>(from));
    if (to > from) {
        --to; // the erase shifted everything after it down one
    }
    to = std::min(to, items_.size());

    items_.insert(items_.begin() + static_cast<ptrdiff_t>(to), std::move(moving));
    Save();
    return static_cast<int>(to);
}

bool ItemStore::Replace(size_t index, DockItem item) {
    if (index >= items_.size()) {
        return false;
    }
    Remember();
    if (item.label.empty()) {
        item.label = LabelFromPath(item.path);
    }
    items_[index] = std::move(item);
    Save();
    return true;
}

bool ItemStore::Add(DockItem item) {
    if (static_cast<int>(items_.size()) >= design::kMaxItems) {
        LogWarn("The dock is full ({} items); not adding another", design::kMaxItems);
        return false;
    }
    Remember();
    if (item.label.empty()) {
        item.label = LabelFromPath(item.path);
    }
    items_.push_back(std::move(item));
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
    Remember();
    // One step is one step. A boundary used to sit in here that a move could
    // only convert an item *across* - costing a click and moving nothing, which
    // became invisible the moment the boundary stopped being drawn.
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
    out->label = LabelFromPath(out->path);
    CoTaskMemFree(path);
    return true;
}

namespace {

// Both pickers are the same dialog with different options, so the shared part
// is written once. Returns the chosen path, or false on cancel.
bool PickPath(HWND owner, const wchar_t* title, const COMDLG_FILTERSPEC* types, UINT typeCount,
              bool folders, std::wstring* out) {
    if (!out) {
        return false;
    }
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return false;
    }
    if (types && typeCount > 0) {
        dialog->SetFileTypes(typeCount, types);
    }
    dialog->SetTitle(title);

    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM |
                       (folders ? FOS_PICKFOLDERS : 0));

    if (FAILED(dialog->Show(owner))) {
        return false; // cancelled
    }
    ComPtr<IShellItem> result;
    PWSTR path = nullptr;
    if (FAILED(dialog->GetResult(&result)) ||
        FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) {
        return false;
    }
    *out = path;
    CoTaskMemFree(path);
    return true;
}

} // namespace

bool ItemStore::PickImage(HWND owner, std::wstring* out) {
    // Everything WIC decodes. The extension list is a convenience for the
    // dialog, not a check: the loader sniffs the file itself.
    static const COMDLG_FILTERSPEC kTypes[] = {
        {L"Images", L"*.png;*.ico;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.webp"},
        {L"All files", L"*.*"},
    };
    return PickPath(owner, L"Choose an icon", kTypes, ARRAYSIZE(kTypes), false, out);
}

bool ItemStore::PickFolder(HWND owner, std::wstring* out) {
    return PickPath(owner, L"Start in", nullptr, 0, true, out);
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
    items_.push_back(std::move(downloads));

    DockItem bin;
    bin.path = L"::{645FF040-5081-101B-9F08-00AA002F954E}";
    bin.label = L"Recycle Bin";
    items_.push_back(std::move(bin));
}

} // namespace liquidock
