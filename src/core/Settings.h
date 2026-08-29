#pragma once

#include <string>

namespace liquidock {

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

    // --- Magnification ----------------------------------------------------
    bool magnification = true;
    float maxScale = 0.0f;
    float influencePx = 0.0f;

    // Whether the glass swells around a raised icon. Off by default: it fuses
    // the bar's silhouette to the icons, which reads as liquid clinging to them
    // rather than as a pane of glass, and that is a much stronger effect than
    // this dock is going for.
    bool iconBulge = false;

    // --- Auto-hide --------------------------------------------------------
    bool autoHide = true;
    float dwellSeconds = 0.0f;
    float slideSeconds = 0.0f;

    // Reads the file, filling in defaults for anything absent, and writes the
    // file with its explanatory comments if it does not exist yet. Always
    // succeeds: an unreadable file leaves every default in place.
    void Load();

    // The file's path, for the tray menu's "Preferences" command.
    static std::wstring FilePath();

    // True when the file changed on disk since the last call. Used to reload
    // while the dock is running so the glass can be tuned by saving the file.
    static bool PollForChanges();

private:
    bool ReadFile(const std::wstring& path);
    bool WriteDefaults(const std::wstring& path) const;
};

} // namespace liquidock
