#include <windows.h>
#include <objbase.h>

#include "app/DockWindow.h"
#include "app/TrayIcon.h"
#include "core/Log.h"
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

int Run() {
    InitLogFile();
    LogInfo("LiquiDock {} starting", LIQUIDOCK_VERSION);

    GraphicsDevice device;
    if (!device.Initialize()) {
        MessageBoxW(nullptr,
                    L"LiquiDock could not create a Direct3D 11 device.\n\n"
                    L"Check that your graphics driver is up to date.",
                    L"LiquiDock", MB_ICONERROR | MB_OK);
        return 1;
    }

    DockWindow dock;
    if (!dock.Create(device)) {
        MessageBoxW(nullptr, L"LiquiDock could not create its window.", L"LiquiDock",
                    MB_ICONERROR | MB_OK);
        return 1;
    }

    TrayIcon tray;
    tray.Create();

    // Strictly event-driven. GetMessage blocks, so an idle dock costs no CPU,
    // no GPU and no timer wakeups. The animation clock added in M2 pumps frames
    // only while a spring is still settling.
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    tray.Destroy();
    dock.Destroy();

    LogInfo("LiquiDock exiting");
    ShutdownLogFile();
    return static_cast<int>(message.wParam);
}

} // namespace
} // namespace liquidock

int APIENTRY wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) {
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

    const int result = liquidock::Run();
    CoUninitialize();
    return result;
}
