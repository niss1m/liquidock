#pragma once

#include <format>
#include <string_view>

namespace liquidock {

enum class LogLevel { Debug, Info, Warn, Error };

// Writes to the debugger output, and to %LOCALAPPDATA%\LiquiDock\liquidock.log
// once InitLogFile() has been called. Safe to call before initialisation.
void LogWrite(LogLevel level, std::string_view message);

void InitLogFile();
void ShutdownLogFile();

template <typename... Args>
void LogDebug(std::format_string<Args...> fmt, Args&&... args) {
#ifdef LIQUIDOCK_DEBUG
    LogWrite(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
#else
    // Compiled out of release builds, but the parameters still have to be
    // referenced or every call site warns about unused arguments.
    (void)fmt;
    ((void)args, ...);
#endif
}

template <typename... Args>
void LogInfo(std::format_string<Args...> fmt, Args&&... args) {
    LogWrite(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void LogWarn(std::format_string<Args...> fmt, Args&&... args) {
    LogWrite(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void LogError(std::format_string<Args...> fmt, Args&&... args) {
    LogWrite(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace liquidock
