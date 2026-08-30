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

    // The dark shoulder just inside the rim, as a fraction of the depth the
    // design's own render has. Its whole job is to give the bright line
    // something to stand on so the edge reads as raised rather than drawn -
    // which is a strong effect, and one worth being able to turn off.
    float innerShadow = 0.0f;

    // How opaque the bright edge is. Distinct from the light's intensity: that
    // is how hard the light falls on the glass, this is how much of the edge
    // you want to see at all.
    float rimOpacity = 0.0f;
    BackdropSource backdrop = BackdropSource::Screen;

    // --- Magnification ----------------------------------------------------
    bool magnification = true;
    float maxScale = 0.0f;
    float influencePx = 0.0f;

    // Whether the row slides sideways as it swells so the icon under the cursor
    // stays under it. That is what the macOS dock does, and it is also why the
    // whole bar appears to drift left and right as the cursor moves along it.
    // Off by default: the bar holds its centre and grows evenly to both sides,
    // which costs the hovered icon a few pixels of drift and buys a dock that
    // sits still.
    bool followCursor = false;

    // The icon size the dock draws at, in logical pixels. Everything else in
    // the layout is proportional to it, so this is the one number that makes
    // the whole dock bigger or smaller.
    float iconSize = 0.0f;

    // The hover label's face. Nexus's registry says Segoe UI 12 bold, but that
    // is what its GDI rendering makes of those numbers, and DirectWrite drawing
    // light glyphs onto a dark pill does not land in the same place. These are
    // settings so the match can be dialled in rather than guessed at.
    float labelFontSize = 0.0f;
    bool labelBold = false;

    // Whether the glass swells around a raised icon. Off by default: it fuses
    // the bar's silhouette to the icons, which reads as liquid clinging to them
    // rather than as a pane of glass, and that is a much stronger effect than
    // this dock is going for.
    bool iconBulge = false;

    // The divider between the two groups. Empty draws the built-in rule; a path
    // to an image draws that instead, stretched to the divider's height, so a
    // Nexus theme's `sep.png` or anything else can be dropped in.
    std::wstring separatorImage;

    // The air on each side of a divider. The design's own value is a group gap;
    // raising it is how a divider stops being a hairline between neighbours and
    // starts being a break between runs.
    float dividerGap = 0.0f;

    // The space between neighbouring icons, in design units, so it scales with
    // the dock like everything else in the layout.
    float iconGap = 0.0f;

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

    // Whether auto-hide only applies while something is actually in the way.
    // On - the default - the dock stays out for as long as you are looking at
    // the desktop, and tucks away as soon as you are not. "In the way" means
    // either that an application has the foreground or that a window overlaps
    // the dock's strip; the first alone would ignore a window sitting over the
    // dock unfocused, and the second alone leaves the dock on top of an app
    // whose window happens to stop above it.
    bool hideWhenCovered = true;

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
