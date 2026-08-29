#pragma once

#include <windows.h>

#include <string>

#include "core/Log.h"

namespace liquidock {

inline std::string FormatHResult(HRESULT hr) {
    char* buffer = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char*>(&buffer), 0, nullptr);

    std::string text;
    if (length && buffer) {
        text.assign(buffer, length);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '.')) {
            text.pop_back();
        }
    }
    if (buffer) {
        LocalFree(buffer);
    }
    return std::format("0x{:08X}{}{}", static_cast<unsigned>(hr), text.empty() ? "" : ": ", text);
}

} // namespace liquidock

// Logs and returns false on failure. For call sites that can degrade gracefully.
#define LD_CHECK(expr)                                                            \
    do {                                                                          \
        const HRESULT ld_hr_ = (expr);                                            \
        if (FAILED(ld_hr_)) {                                                     \
            ::liquidock::LogError("{}({}) failed - {}", #expr, __LINE__,           \
                                  ::liquidock::FormatHResult(ld_hr_));            \
            return false;                                                         \
        }                                                                         \
    } while (false)

// Logs and propagates the HRESULT. For call sites that return HRESULT.
#define LD_RETURN_IF_FAILED(expr)                                                 \
    do {                                                                          \
        const HRESULT ld_hr_ = (expr);                                            \
        if (FAILED(ld_hr_)) {                                                     \
            ::liquidock::LogError("{}({}) failed - {}", #expr, __LINE__,           \
                                  ::liquidock::FormatHResult(ld_hr_));            \
            return ld_hr_;                                                        \
        }                                                                         \
    } while (false)
