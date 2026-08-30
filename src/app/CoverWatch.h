#pragma once

#include <windows.h>

namespace liquidock {

// Whether an ordinary window is sitting where the dock wants to be.
//
// This is what makes auto-hide behave the way people expect: a dock that gets
// out of the way of your work, not one that hides from an empty desktop. Hiding
// unconditionally is the behaviour that makes an auto-hidden dock feel like a
// chore - you minimise everything to get at the desktop and the one thing you
// wanted is the thing that just left.
//
// Event-driven, like everything else here. A WinEvent hook covering the system
// range - foreground changes, minimise, the end of a window drag - is enough to
// catch every way the answer can change, and all of those are rare. Nothing
// polls, and the scan itself only runs when one of them fires.
class CoverWatch {
public:
    CoverWatch() = default;
    CoverWatch(const CoverWatch&) = delete;
    CoverWatch& operator=(const CoverWatch&) = delete;
    ~CoverWatch();

    // `notify` is posted `message` whenever the answer may have changed.
    bool Initialize(HWND notify, UINT message);
    void Shutdown();

    // True if any ordinary top-level window overlaps `screenRect`.
    //
    // "Ordinary" excludes our own windows, the shell's own furniture (the
    // taskbar sits at the same screen edge and would otherwise cover the dock
    // permanently), tool windows and overlays, minimised windows, and the
    // cloaked ghosts that suspended packaged apps leave behind - all of which
    // are windows the user is emphatically not working in.
    // `culprit`, when given, receives the class name of the window that
    // decided it - which is the difference between "the dock will not come out
    // and I do not know why" and reading one line of the log.
    static bool IsCovered(const RECT& screenRect, char* culprit = nullptr,
                          size_t culpritChars = 0);

    // True when the thing in front is the desktop rather than an application -
    // no foreground window, the shell's own desktop, or one of our own windows.
    //
    // This is the question people actually mean by "am I on the desktop", and
    // geometry alone answers it badly: an app window that stops short of the
    // bottom of the screen overlaps nothing, so a purely geometric check leaves
    // the dock sitting on top of the work you just switched to.
    static bool DesktopIsForeground(char* culprit = nullptr, size_t culpritChars = 0);

private:
    static void CALLBACK EventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject,
                                   LONG idChild, DWORD thread, DWORD time);

    // One hook per event rather than one hook over a range. The range from
    // FOREGROUND to MINIMIZEEND also contains CAPTURESTART/END and
    // SCROLLINGSTART/END, which fire continuously while anything on the desktop
    // is scrolled or grabs the mouse - so a single range hook turned an
    // event-driven check into a scan several times a second, which is precisely
    // the polling this was written to avoid.
    HWINEVENTHOOK hooks_[3]{};
    HWND notify_ = nullptr;
    UINT message_ = 0;
};

} // namespace liquidock
