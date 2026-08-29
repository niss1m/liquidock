#pragma once

namespace liquidock {

// Writes a stack trace to %LOCALAPPDATA%\LiquiDock\crash.txt when the process
// dies of an unhandled exception.
//
// A dock crashes on someone else's machine, on someone else's graphics driver,
// with someone else's shell extension loaded into its icon thread. "It
// disappeared" is not a bug report; a named frame is. The handler writes with
// plain Win32 file calls rather than the logger, because the faulting thread may
// well be the one holding the logger's mutex.
void InstallCrashHandler();

} // namespace liquidock
