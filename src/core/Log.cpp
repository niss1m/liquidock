#include "core/Log.h"

#include <windows.h>
#include <shlobj.h>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <share.h>
#include <string>

namespace liquidock {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;

constexpr const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

std::wstring LogFilePath() {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        return {};
    }
    std::wstring dir = local;
    CoTaskMemFree(local);
    dir += L"\\LiquiDock";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\liquidock.log";
}

} // namespace

void InitLogFile() {
    std::lock_guard lock(g_mutex);
    if (g_file) {
        return;
    }
    const std::wstring path = LogFilePath();
    if (path.empty()) {
        return;
    }
    // Truncate on each launch; a dock that has been running for a week should
    // not have accumulated an unbounded log.
    //
    // _wfsopen rather than _wfopen_s: the secure variants open a file for
    // *exclusive* access, so nothing can tail the log while the dock is
    // running - which is precisely when the log is worth reading.
    //
    // Binary mode with raw UTF-8 bytes rather than "ccs=UTF-8": that mode puts
    // the stream into Unicode translation, where narrow fwrite output comes
    // back out as mojibake.
    g_file = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
    if (g_file) {
        // Explicit BOM so Windows PowerShell's Get-Content decodes the file as
        // UTF-8 rather than guessing the active code page.
        static constexpr unsigned char kBom[] = {0xEF, 0xBB, 0xBF};
        fwrite(kBom, 1, sizeof(kBom), g_file);
    }
}

void ShutdownLogFile() {
    std::lock_guard lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

void LogWrite(LogLevel level, std::string_view message) {
    // GetLocalTime rather than std::chrono's time zone support: it is a single
    // syscall with no tzdb lookup, and logging must never be the reason a frame
    // is late.
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const std::string line = std::format("[{:02}:{:02}:{:02}.{:03}] {} {}\n", now.wHour,
                                         now.wMinute, now.wSecond, now.wMilliseconds,
                                         LevelName(level), message);

    OutputDebugStringA(line.c_str());

    std::lock_guard lock(g_mutex);
    if (g_file) {
        fwrite(line.data(), 1, line.size(), g_file);
        fflush(g_file);
    }
}

} // namespace liquidock
