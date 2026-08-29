#pragma once

#include <string>

namespace liquidock {

// %LOCALAPPDATA%\LiquiDock, created on demand. Empty if the known folder could
// not be resolved, which the callers treat as "run with defaults and do not
// try to persist anything".
std::wstring ConfigDirectory();

// A file inside that directory. Empty if the directory is.
std::wstring ConfigFilePath(const wchar_t* fileName);

} // namespace liquidock
