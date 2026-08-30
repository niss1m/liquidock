#include "model/ProfileStore.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "core/Log.h"
#include "core/Settings.h"

namespace liquidock {
namespace {

constexpr wchar_t kExtension[] = L".txt";

} // namespace

std::wstring ProfileStore::Folder() {
    const std::wstring settings = Settings::FilePath();
    if (settings.empty()) {
        return {};
    }
    std::filesystem::path folder = std::filesystem::path(settings).parent_path() / L"profiles";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    return folder.wstring();
}

std::wstring ProfileStore::Clean(const std::wstring& name) {
    std::wstring out;
    out.reserve(name.size());
    for (const wchar_t ch : name) {
        // Anything the file system reserves, plus the separators - a profile
        // called "../autoexec" must not be able to write outside the folder.
        if (ch < 32 || wcschr(L"\\/:*?\"<>|", ch) != nullptr) {
            continue;
        }
        out.push_back(ch);
    }
    // Leading and trailing spaces and dots are legal to type and illegal to
    // store, which is a difference nobody should have to know about.
    while (!out.empty() && (out.front() == L' ' || out.front() == L'.')) {
        out.erase(0, 1);
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) {
        out.pop_back();
    }
    if (out.size() > 48) {
        out.resize(48);
    }
    return out;
}

std::wstring ProfileStore::PathFor(const std::wstring& name) {
    const std::wstring clean = Clean(name);
    const std::wstring folder = Folder();
    if (clean.empty() || folder.empty()) {
        return {};
    }
    return (std::filesystem::path(folder) / (clean + kExtension)).wstring();
}

std::vector<std::wstring> ProfileStore::List() {
    std::vector<std::wstring> names;
    const std::wstring folder = Folder();
    if (folder.empty()) {
        return names;
    }
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (error) {
            break;
        }
        if (!entry.is_regular_file(error) || entry.path().extension() != kExtension) {
            continue;
        }
        names.push_back(entry.path().stem().wstring());
    }
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    return names;
}

std::wstring ProfileStore::FileFor(const std::wstring& name) {
    return PathFor(name);
}

bool ProfileStore::Exists(const std::wstring& name) {
    const std::wstring path = PathFor(name);
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::exists(path, error);
}

bool ProfileStore::Save(const std::wstring& name) {
    const std::wstring path = PathFor(name);
    const std::wstring settings = Settings::FilePath();
    if (path.empty() || settings.empty()) {
        return false;
    }
    std::error_code error;
    std::filesystem::copy_file(settings, path,
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        LogWarn("Could not write the profile");
        return false;
    }
    return true;
}

bool ProfileStore::Load(const std::wstring& name) {
    const std::wstring path = PathFor(name);
    const std::wstring settings = Settings::FilePath();
    if (path.empty() || settings.empty()) {
        return false;
    }
    std::error_code error;
    // Copied over the live file rather than renamed onto it: the config watcher
    // is looking for a write, and a rename is not one.
    std::filesystem::copy_file(path, settings,
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        LogWarn("Could not read the profile");
        return false;
    }
    return true;
}

bool ProfileStore::Remove(const std::wstring& name) {
    const std::wstring path = PathFor(name);
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::remove(path, error);
}

} // namespace liquidock
