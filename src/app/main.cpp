#include <windows.h>
#include <objbase.h>

#include <cwchar>
#include <optional>

#include "app/DockWindow.h"
#include "app/TrayIcon.h"
#include "core/ConfigWatcher.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "gfx/GraphicsDevice.h"

namespace liquidock {
namespace {

constexpr wchar_t kSingleInstanceMutex[] = L"Local\\LiquiDock.SingleInstance";

// Two docks fighting over the same screen edge is never what anyone wants, and
// with appbar space reservation in M3 it would leave the work area permanently
// wrong if one of them were killed.
class SingleInstanceGuard {
public:
    bool Acquire() {
        handle_ = CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
        return handle_ != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
    }

    ~SingleInstanceGuard() {
        if (handle_) {
            ReleaseMutex(handle_);
            CloseHandle(handle_);
        }
    }

private:
    HANDLE handle_ = nullptr;
};

int Run(bool diagnostic, std::optional<bool> autoHideOverride) {
    InitLogFile();
    LogInfo("LiquiDock {} starting{}", LIQUIDOCK_VERSION,
            diagnostic ? " (diagnostic render)" : "");

    GraphicsDevice device;
    if (!device.Initialize()) {
        MessageBoxW(nullptr,
                    L"LiquiDock could not create a Direct3D 11 device.\n\n"
                    L"Check that your graphics driver is up to date.",
                    L"LiquiDock", MB_ICONERROR | MB_OK);
        return 1;
    }

    DockWindow dock;
    if (!dock.Create(device, diagnostic, autoHideOverride)) {
        MessageBoxW(nullptr, L"LiquiDock could not create its window.", L"LiquiDock",
                    MB_ICONERROR | MB_OK);
        return 1;
    }

    TrayIcon tray;
    tray.Create();

    // Watching the settings file rather than polling it. The wait below blocks
    // on the message queue and the directory handle together, so an idle dock
    // still costs no CPU, no GPU and no timer wakeups - it just has one more
    // thing it can be woken by.
    ConfigWatcher watcher;
    watcher.Start();
    Settings::PollForChanges(); // establish the baseline after the first write

    // Strictly event-driven. The animation clock pumps frames only while a
    // spring is still settling or a slide is still running.
    MSG message{};
    bool running = true;
    while (running) {
        HANDLE handles[1] = {watcher.handle()};
        const DWORD handleCount = watcher.valid() ? 1u : 0u;
        // MWMO_INPUTAVAILABLE so a message that arrived between the last
        // PeekMessage and this wait is not slept through.
        const DWORD signalled = MsgWaitForMultipleObjectsEx(
            handleCount, handleCount ? handles : nullptr, INFINITE, QS_ALLINPUT,
            MWMO_INPUTAVAILABLE);

        if (handleCount > 0 && signalled == WAIT_OBJECT_0) {
            watcher.Acknowledge();
            // The handle signals for any write in the config directory, the log
            // file included, so ask whether the settings file itself moved
            // before doing anything about it. Without this the dock reloads
            // every time it writes its own log.
            if (Settings::PollForChanges()) {
                dock.ReloadSettings();
            }
            continue;
        }

        // PeekMessage rather than GetMessage: the wait has already told us the
        // queue is non-empty, and GetMessage would block again if the only
        // thing that woke us was the directory handle.
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    watcher.Stop();
    tray.Destroy();
    dock.Destroy();

    LogInfo("LiquiDock exiting");
    ShutdownLogFile();
    return static_cast<int>(message.wParam);
}

} // namespace
} // namespace liquidock

int APIENTRY wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR commandLine, _In_ int) {
    const bool diagnostic = wcsstr(commandLine, L"--diagnostic") != nullptr;
    // Auto-hide lives in settings.txt. The command line only overrides it, and
    // only in the one direction anybody needs while testing.
    std::optional<bool> autoHideOverride;
    if (wcsstr(commandLine, L"--no-autohide") != nullptr) {
        autoHideOverride = false;
    }

    liquidock::SingleInstanceGuard guard;
    if (!guard.Acquire()) {
        return 0; // Already running; the existing instance owns the screen edge.
    }

    // Apartment-threaded: the shell interfaces M2 uses for icon extraction and
    // launching expect an STA on the thread that calls them.
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        return 1;
    }

    const int result = liquidock::Run(diagnostic, autoHideOverride);
    CoUninitialize();
    return result;
}
