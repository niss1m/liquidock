#include "core/ConfigPaths.h"

#include <windows.h>
#include <shlobj.h>

#include <filesystem>

namespace liquidock {

std::wstring ConfigDirectory() {
    PWSTR raw = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw)) &&
        raw) {
        base = raw;
    }
    if (raw) {
        CoTaskMemFree(raw);
    }
    if (base.empty()) {
        return {};
    }

    const std::filesystem::path directory = std::filesystem::path(base) / L"LiquiDock";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    return directory.wstring();
}

std::wstring ConfigFilePath(const wchar_t* fileName) {
    const std::wstring directory = ConfigDirectory();
    if (directory.empty()) {
        return {};
    }
    return (std::filesystem::path(directory) / fileName).wstring();
}

} // namespace liquidock
