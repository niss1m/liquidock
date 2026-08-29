#include "app/TrayIcon.h"

#include <shellapi.h>

#include <cwchar>

#include "core/Log.h"
#include "core/Settings.h"

namespace liquidock {
namespace {

constexpr wchar_t kWindowClass[] = L"LiquiDock.Tray";
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kTrayIconId = 1;

enum MenuCommand : UINT {
    kCommandSettings = 100,
    kCommandExit = 101,
};

} // namespace

TrayIcon::~TrayIcon() {
    Destroy();
}

bool TrayIcon::Create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TrayIcon::WndProcThunk;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    // Deliberately NOT a message-only (HWND_MESSAGE) window. Message-only
    // windows are excluded from broadcasts, and TaskbarCreated - the whole
    // reason this window exists beyond the icon callback - is a broadcast. It
    // is an ordinary top-level window that is simply never shown.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"LiquiDock", WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) {
        LogError("Tray window creation failed: {}", GetLastError());
        return false;
    }

    // Explorer can restart out from under us; it broadcasts this to tell every
    // tray client to re-register. Without it the dock survives an explorer
    // crash but loses its only exit affordance.
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");

    // Vista and later filter broadcasts to lower-integrity processes. The dock
    // runs asInvoker so this is normally a no-op, but it costs nothing and
    // makes the re-registration reliable if that ever changes.
    ChangeWindowMessageFilterEx(hwnd_, taskbarCreatedMessage_, MSGFLT_ALLOW, nullptr);

    return AddIcon();
}

bool TrayIcon::AddIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayCallbackMessage;
    // TODO(M4): ship a real icon resource once the app mark is drawn.
    data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(data.szTip, L"LiquiDock " LIQUIDOCK_VERSION_W);

    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        LogError("Shell_NotifyIcon(NIM_ADD) failed: {}", GetLastError());
        return false;
    }

    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    iconAdded_ = true;
    return true;
}

void TrayIcon::Destroy() {
    if (iconAdded_) {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd_;
        data.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &data);
        iconAdded_ = false;
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void TrayIcon::ShowMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"LiquiDock " LIQUIDOCK_VERSION_W);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCommandSettings, L"Preferences…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCommandExit, L"Quit LiquiDock");

    POINT cursor{};
    GetCursorPos(&cursor);

    // Required so the menu dismisses when the user clicks away from it.
    SetForegroundWindow(hwnd_);
    const int command = TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                         cursor.x, cursor.y, hwnd_, nullptr);
    // The other half of the SetForegroundWindow dance: without a message posted
    // to our own queue afterwards, the menu can linger after the user clicks
    // away from it.
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (command == kCommandExit) {
        PostQuitMessage(0);
    } else if (command == kCommandSettings) {
        // Until the preferences UI lands in M4, the settings file is the
        // preferences UI. The dock watches it, so saving takes effect without
        // a restart and this is a complete answer rather than a placeholder.
        const std::wstring path = Settings::FilePath();
        if (!path.empty()) {
            ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
}

LRESULT CALLBACK TrayIcon::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    TrayIcon* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<TrayIcon*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->WndProc(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT TrayIcon::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == taskbarCreatedMessage_ && taskbarCreatedMessage_ != 0) {
        iconAdded_ = false;
        AddIcon();
        return 0;
    }

    switch (message) {
        case kTrayCallbackMessage:
            switch (LOWORD(lParam)) {
                case WM_CONTEXTMENU:
                case WM_RBUTTONUP:
                    ShowMenu();
                    return 0;
                default:
                    break;
            }
            return 0;

        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace liquidock
