#pragma once

#include <string>

namespace liquidock {

// Where the glass gets the image it refracts.
enum class BackdropSource {
    // The desktop wallpaper, decoded once. Costs nothing after load and is
    // exactly right whenever nothing is behind the dock.
    Wallpaper,
    // The live screen. Correct over a window, but it requires excluding the
    // dock from capture to break the feedback loop, and that same exclusion
    // hides the dock from the user's own screenshots and screen shares.
    Screen,
};

// Everything about the dock a person is allowed to change, and the file it
// lives in.
//
// `key = value`, one per line, because the preferences UI does not exist until
// M4 and until then the file *is* the UI - it has to be readable and editable
// in Notepad without a schema to consult. Unknown keys are ignored and missing
// keys keep their default, so a file written by an older build still loads.
//
// The file is written once, with a comment on every setting, and never
// rewritten: overwriting it would throw away whatever the user had typed around
// their edits.
struct Settings {
    // --- Glass ------------------------------------------------------------
    // The defaults here are the shipping look, not the Figma glass panel's own
    // numbers; see the comments in DesignTokens.h for which ones moved and why.
    float refraction = 0.0f;
    float depth = 0.0f;
    float dispersion = 0.0f;
    float frost = 0.0f;
    float splay = 0.0f;
    float lightAngleDegrees = 0.0f;
    float lightIntensity = 0.0f;
    float tintAlpha = 0.0f;
    BackdropSource backdrop = BackdropSource::Screen;

    // --- Magnification ----------------------------------------------------
    bool magnification = true;
    float maxScale = 0.0f;
    float influencePx = 0.0f;

    // The icon size the dock draws at, in logical pixels. Everything else in
    // the layout is proportional to it, so this is the one number that makes
    // the whole dock bigger or smaller.
    float iconSize = 0.0f;

    // Whether the glass swells around a raised icon. Off by default: it fuses
    // the bar's silhouette to the icons, which reads as liquid clinging to them
    // rather than as a pane of glass, and that is a much stronger effect than
    // this dock is going for.
    bool iconBulge = false;

    // --- Placement --------------------------------------------------------
    // Which monitor the dock lives on. 0 means the primary one; otherwise it is
    // a 1-based index into the display order, and an index that no longer
    // exists falls back to the primary rather than putting the dock nowhere.
    int monitorIndex = 0;

    // Whether to reserve screen space so maximised windows stop above the dock.
    // Ignored while auto-hide is on, where it would make no sense.
    bool reserveSpace = false;

    // --- Auto-hide --------------------------------------------------------
    bool autoHide = true;
    float dwellSeconds = 0.0f;
    float slideSeconds = 0.0f;

    // Reads the file, filling in defaults for anything absent, and writes the
    // file with its explanatory comments if it does not exist yet. Always
    // succeeds: an unreadable file leaves every default in place.
    void Load();

    // Writes the current values back, rewriting only the value half of each
    // `key = value` line and leaving every comment, blank line and unknown key
    // exactly where it was. A settings file the user has annotated must survive
    // being edited from the preferences window.
    bool Save() const;

    // The file's path, for the tray menu's "Preferences" command.
    static std::wstring FilePath();

    // True when the file changed on disk since the last call. Used to reload
    // while the dock is running so the glass can be tuned by saving the file.
    static bool PollForChanges();

    // Formats one setting the way the file spells it. Public so the writer and
    // any future exporter agree on the representation.
    std::wstring ValueFor(const std::wstring& key) const;

private:
    bool ReadFile(const std::wstring& path);
    bool WriteDefaults(const std::wstring& path) const;
};

} // namespace liquidock
