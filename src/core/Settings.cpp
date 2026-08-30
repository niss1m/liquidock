#include "core/Settings.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <vector>

#include "core/ConfigPaths.h"
#include "core/DesignTokens.h"
#include "core/Log.h"

namespace liquidock {
namespace {

constexpr wchar_t kFileName[] = L"settings.txt";

std::wstring Trim(std::wstring_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == L' ' || text[begin] == L'\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == L' ' || text[end - 1] == L'\t' ||
                           text[end - 1] == L'\r' || text[end - 1] == L'\n')) {
        --end;
    }
    return std::wstring(text.substr(begin, end - begin));
}

bool ParseBool(const std::wstring& value, bool fallback) {
    if (_wcsicmp(value.c_str(), L"on") == 0 || _wcsicmp(value.c_str(), L"true") == 0 ||
        _wcsicmp(value.c_str(), L"yes") == 0 || value == L"1") {
        return true;
    }
    if (_wcsicmp(value.c_str(), L"off") == 0 || _wcsicmp(value.c_str(), L"false") == 0 ||
        _wcsicmp(value.c_str(), L"no") == 0 || value == L"0") {
        return false;
    }
    LogWarn("Unrecognised on/off value in settings.txt; keeping the default");
    return fallback;
}

// Clamped on the way in rather than at every use. A typo in the file should
// give a dock that looks wrong, not one that allocates a window the width of
// three monitors or divides by zero in the wave.
float ParseFloat(const std::wstring& value, float fallback, float low, float high) {
    wchar_t* end = nullptr;
    const float parsed = std::wcstof(value.c_str(), &end);
    if (end == value.c_str()) {
        LogWarn("Unrecognised number in settings.txt; keeping the default");
        return fallback;
    }
    return std::clamp(parsed, low, high);
}

} // namespace

std::wstring Settings::FilePath() {
    return ConfigFilePath(kFileName);
}

bool Settings::PollForChanges() {
    static unsigned long long lastStamp = 0;

    const std::wstring path = FilePath();
    if (path.empty()) {
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return false;
    }
    const auto stamp = (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                       data.ftLastWriteTime.dwLowDateTime;

    const bool first = (lastStamp == 0);
    if (stamp == lastStamp) {
        return false;
    }
    lastStamp = stamp;
    return !first; // the first poll only establishes the baseline
}

void Settings::Load() {
    // Start from the design tokens every time, so a key deleted from the file
    // goes back to its default rather than keeping whatever was loaded before.
    refraction = design::glass::kRefraction;
    depth = design::glass::kDepth;
    dispersion = design::glass::kDispersion;
    frost = design::glass::kFrost;
    splay = design::glass::kSplay;
    lightAngleDegrees = design::glass::kLightAngleDegrees;
    lightIntensity = design::glass::kLightIntensity;
    tintAlpha = design::kBarTint[3];
    innerShadow = design::glass::kInnerShadow;
    rimOpacity = design::glass::kRimOpacity;
    backdrop = BackdropSource::Screen;

    magnification = true;
    followCursor = false;
    separatorImage.clear();
    dividerGap = design::kGroupGap;
    maxScale = design::magnify::kMaxScale;
    influencePx = design::magnify::kInfluencePx;
    iconSize = design::kDefaultIconSize;
    labelFontSize = design::label::kFontSize;
    labelBold = design::label::kBold;
    iconBulge = false;

    monitorIndex = 0;
    reserveSpace = false;

    autoHide = true;
    hideWhenCovered = true;
    dwellSeconds = design::kDwellSeconds;
    slideSeconds = design::kSlideSeconds;

    const std::wstring path = FilePath();
    if (path.empty()) {
        return;
    }
    if (!ReadFile(path)) {
        WriteDefaults(path);
    }
}

bool Settings::ReadFile(const std::wstring& path) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rt, ccs=UTF-8") != 0 || !file) {
        return false;
    }

    wchar_t line[1024];
    while (fgetws(line, static_cast<int>(std::size(line)), file)) {
        const std::wstring text = Trim(line);
        if (text.empty() || text[0] == L'#') {
            continue;
        }
        const size_t equals = text.find(L'=');
        if (equals == std::wstring::npos) {
            continue;
        }
        const std::wstring key = Trim(text.substr(0, equals));
        const std::wstring value = Trim(text.substr(equals + 1));

        if (key == L"refraction") {
            refraction = ParseFloat(value, refraction, 0.0f, 1.0f);
        } else if (key == L"depth") {
            depth = ParseFloat(value, depth, 0.0f, 1.0f);
        } else if (key == L"dispersion") {
            dispersion = ParseFloat(value, dispersion, 0.0f, 1.0f);
        } else if (key == L"frost") {
            frost = ParseFloat(value, frost, 0.0f, 1.0f);
        } else if (key == L"splay") {
            splay = ParseFloat(value, splay, 0.0f, 1.0f);
        } else if (key == L"light-angle") {
            lightAngleDegrees = ParseFloat(value, lightAngleDegrees, -360.0f, 360.0f);
        } else if (key == L"light-intensity") {
            lightIntensity = ParseFloat(value, lightIntensity, 0.0f, 1.0f);
        } else if (key == L"tint-alpha") {
            tintAlpha = ParseFloat(value, tintAlpha, 0.0f, 1.0f);
        } else if (key == L"inner-shadow") {
            innerShadow = ParseFloat(value, innerShadow, 0.0f, 1.0f);
        } else if (key == L"rim-opacity") {
            rimOpacity = ParseFloat(value, rimOpacity, 0.0f, 1.0f);
        } else if (key == L"backdrop") {
            if (_wcsicmp(value.c_str(), L"screen") == 0) {
                backdrop = BackdropSource::Screen;
            } else if (_wcsicmp(value.c_str(), L"wallpaper") == 0) {
                backdrop = BackdropSource::Wallpaper;
            } else {
                LogWarn("backdrop must be `wallpaper` or `screen`; keeping the default");
            }
        } else if (key == L"magnification") {
            magnification = ParseBool(value, magnification);
        } else if (key == L"follow-cursor") {
            followCursor = ParseBool(value, followCursor);
        } else if (key == L"separator-image") {
            separatorImage = value;
        } else if (key == L"divider-gap") {
            dividerGap = ParseFloat(value, dividerGap, 0.0f, 120.0f);
        } else if (key == L"max-scale") {
            maxScale = ParseFloat(value, maxScale, 1.0f, design::kMaxConfigurableScale);
        } else if (key == L"icon-size") {
            iconSize = ParseFloat(value, iconSize, design::kMinIconSize, design::kMaxIconSize);
        } else if (key == L"influence") {
            influencePx = ParseFloat(value, influencePx, 16.0f, 600.0f);
        } else if (key == L"label-font-size") {
            labelFontSize = ParseFloat(value, labelFontSize, 9.0f, design::label::kMaxFontSize);
        } else if (key == L"label-bold") {
            labelBold = ParseBool(value, labelBold);
        } else if (key == L"icon-bulge") {
            iconBulge = ParseBool(value, iconBulge);
        } else if (key == L"monitor") {
            if (_wcsicmp(value.c_str(), L"primary") == 0) {
                monitorIndex = 0;
            } else {
                monitorIndex = static_cast<int>(ParseFloat(value, 0.0f, 0.0f, 16.0f));
            }
        } else if (key == L"reserve-space") {
            reserveSpace = ParseBool(value, reserveSpace);
        } else if (key == L"auto-hide") {
            autoHide = ParseBool(value, autoHide);
        } else if (key == L"hide-when-covered") {
            hideWhenCovered = ParseBool(value, hideWhenCovered);
        } else if (key == L"dwell-seconds") {
            dwellSeconds = ParseFloat(value, dwellSeconds, 0.2f, 120.0f);
        } else if (key == L"slide-seconds") {
            slideSeconds = ParseFloat(value, slideSeconds, 0.0f, 2.0f);
        } else {
            LogWarn("Unknown key in settings.txt; ignoring it");
        }
    }

    fclose(file);
    return true;
}

std::wstring Settings::ValueFor(const std::wstring& key) const {
    wchar_t buffer[64];
    auto number = [&buffer](const wchar_t* format, double value) {
        swprintf_s(buffer, format, value);
        return std::wstring(buffer);
    };

    if (key == L"refraction") return number(L"%.2f", refraction);
    if (key == L"depth") return number(L"%.2f", depth);
    if (key == L"dispersion") return number(L"%.2f", dispersion);
    if (key == L"frost") return number(L"%.2f", frost);
    if (key == L"splay") return number(L"%.2f", splay);
    if (key == L"light-angle") return number(L"%.0f", lightAngleDegrees);
    if (key == L"light-intensity") return number(L"%.2f", lightIntensity);
    if (key == L"tint-alpha") return number(L"%.2f", tintAlpha);
    if (key == L"inner-shadow") return number(L"%.2f", innerShadow);
    if (key == L"rim-opacity") return number(L"%.2f", rimOpacity);
    if (key == L"backdrop") return backdrop == BackdropSource::Screen ? L"screen" : L"wallpaper";
    if (key == L"magnification") return magnification ? L"on" : L"off";
    if (key == L"follow-cursor") return followCursor ? L"on" : L"off";
    if (key == L"separator-image") return separatorImage;
    if (key == L"divider-gap") return number(L"%.0f", dividerGap);
    if (key == L"max-scale") return number(L"%.2f", maxScale);
    if (key == L"influence") return number(L"%.0f", influencePx);
    if (key == L"icon-size") return number(L"%.0f", iconSize);
    if (key == L"label-font-size") return number(L"%.1f", labelFontSize);
    if (key == L"label-bold") return labelBold ? L"on" : L"off";
    if (key == L"icon-bulge") return iconBulge ? L"on" : L"off";
    if (key == L"monitor") {
        if (monitorIndex <= 0) {
            return L"primary";
        }
        swprintf_s(buffer, L"%d", monitorIndex);
        return std::wstring(buffer);
    }
    if (key == L"reserve-space") return reserveSpace ? L"on" : L"off";
    if (key == L"auto-hide") return autoHide ? L"on" : L"off";
    if (key == L"hide-when-covered") return hideWhenCovered ? L"on" : L"off";
    if (key == L"dwell-seconds") return number(L"%.1f", dwellSeconds);
    if (key == L"slide-seconds") return number(L"%.2f", slideSeconds);
    return {};
}

bool Settings::Save() const {
    const std::wstring path = FilePath();
    if (path.empty()) {
        return false;
    }
    // Nothing to rewrite yet: write the annotated default file instead, which
    // already contains every current value.
    if (!std::filesystem::exists(path)) {
        return WriteDefaults(path);
    }

    std::vector<std::wstring> lines;
    {
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rt, ccs=UTF-8") != 0 || !file) {
            return false;
        }
        wchar_t line[1024];
        while (fgetws(line, static_cast<int>(std::size(line)), file)) {
            std::wstring text(line);
            while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) {
                text.pop_back();
            }
            lines.push_back(std::move(text));
        }
        fclose(file);
    }

    // Rewrite in place, so the user's own comments and ordering survive.
    std::vector<std::wstring> written;
    for (std::wstring& text : lines) {
        const std::wstring trimmed = Trim(text);
        if (trimmed.empty() || trimmed[0] == L'#') {
            continue;
        }
        const size_t equals = trimmed.find(L'=');
        if (equals == std::wstring::npos) {
            continue;
        }
        const std::wstring key = Trim(trimmed.substr(0, equals));
        const std::wstring value = ValueFor(key);
        if (value.empty()) {
            continue; // a key this build does not know; leave it untouched
        }
        text = key + L" = " + value;
        written.push_back(key);
    }

    // A key the file predates - it was written by an older build - is appended
    // rather than lost, so upgrading never silently drops a setting.
    static const wchar_t* const kAllKeys[] = {
        L"refraction", L"depth",        L"dispersion",    L"frost",         L"splay",
        L"light-angle", L"light-intensity", L"tint-alpha", L"inner-shadow", L"rim-opacity", L"backdrop", L"magnification",
        L"follow-cursor", L"separator-image", L"divider-gap",
        L"max-scale",  L"influence",    L"icon-size",    L"icon-bulge",    L"monitor",       L"reserve-space",
        L"auto-hide",  L"hide-when-covered", L"dwell-seconds", L"slide-seconds",
        L"label-font-size", L"label-bold",
    };
    for (const wchar_t* key : kAllKeys) {
        if (std::find(written.begin(), written.end(), key) == written.end()) {
            lines.push_back(std::wstring(key) + L" = " + ValueFor(key));
        }
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wt, ccs=UTF-8") != 0 || !file) {
        LogWarn("Could not write the settings file");
        return false;
    }
    for (const std::wstring& text : lines) {
        fwprintf(file, L"%s\n", text.c_str());
    }
    fclose(file);
    return true;
}

bool Settings::WriteDefaults(const std::wstring& path) const {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wt, ccs=UTF-8") != 0 || !file) {
        LogWarn("Could not write the settings file");
        return false;
    }

    wchar_t monitorBuffer[16];
    const wchar_t* monitorText = L"primary";
    if (monitorIndex > 0) {
        wsprintfW(monitorBuffer, L"%d", monitorIndex);
        monitorText = monitorBuffer;
    }

    // Text mode turns every newline into a CRLF on the way out, so the file
    // opens cleanly in Notepad without the source here being littered with
    // carriage returns.
    fwprintf(file,
             L"# LiquiDock settings. `key = value`, one per line; # starts a comment.\n"
             L"# Delete a line to go back to its default. LiquiDock picks up changes\n"
             L"# while it is running, so you can tune the glass by saving this file.\n"
             L"\n"
             L"# --- Glass ---------------------------------------------------------\n"
             L"# How far the rim bends the desktop behind it. This is what makes the\n"
             L"# edge read as a thick pane; past about 0.6 it starts to read as a\n"
             L"# fisheye lens instead.\n"
             L"refraction = %.2f\n"
             L"\n"
             L"# How wide the bevel is - how thick the glass looks.\n"
             L"depth = %.2f\n"
             L"\n"
             L"# Colour fringing at the rim, where the bending splits by wavelength.\n"
             L"dispersion = %.2f\n"
             L"\n"
             L"# Frosting. 0 is clear glass; this is the setting that decides whether\n"
             L"# the desktop behind the dock is sharp or softened.\n"
             L"frost = %.2f\n"
             L"\n"
             L"# How far inward from the rim the bending fans before the surface\n"
             L"# flattens off. Lower keeps the middle of the pane calm.\n"
             L"splay = %.2f\n"
             L"\n"
             L"# Where the light comes from: 0 is from the right, -90 from\n"
             L"# directly above. The rim is brightest on the edges square to it\n"
             L"# and fades where they run parallel, so this decides which edges\n"
             L"# catch the light. -90 is the design's own look; the Figma panel\n"
             L"# quotes -45 for the same frame, on a convention 45 degrees from\n"
             L"# this one.\n"
             L"light-angle = %.0f\n"
             L"light-intensity = %.2f\n"
             L"\n"
             L"# The white the glass is tinted with. Small on purpose: the dock reads\n"
             L"# as glass because of what the shader does to the backdrop, not\n"
             L"# because of this. Raising it is how the design gets muddy.\n"
             L"tint-alpha = %.2f\n"
             L"\n"
             L"# The dark shoulder just inside the rim, as a fraction of the\n"
             L"# depth the design's own render has. Its job is to give the\n"
             L"# bright line something to stand on, so the edge reads as raised\n"
             L"# rather than drawn on. 0 removes it and leaves the line alone.\n"
             L"inner-shadow = %.2f\n"
             L"\n"
             L"# How opaque the bright edge is. Distinct from the light's own\n"
             L"# intensity: that is how hard the light falls, this is how much\n"
             L"# of the edge you want to see at all.\n"
             L"rim-opacity = %.2f\n"
             L"\n"
             L"# What the glass refracts. `wallpaper` decodes your desktop background\n"
             L"# once and costs nothing after that - it is exactly right whenever\n"
             L"# nothing is behind the dock, and wrong-looking over a window.\n"
             L"# `screen` refracts what is actually there.\n"
             L"#\n"
             L"# The catch with `screen` is not performance - it copies only the strip\n"
             L"# the dock covers and ignores changes that miss it. It is that the dock\n"
             L"# has to be excluded from screen capture to stop it refracting its own\n"
             L"# last frame forever, and that same exclusion makes the dock invisible\n"
             L"# in your own screenshots and screen shares.\n"
             L"backdrop = %s\n"
             L"\n"
             L"# --- Magnification -------------------------------------------------\n"
             L"magnification = %s\n"
             L"\n"
             L"# How big the icons are, in pixels. Everything else in the dock is\n"
             L"# proportional to this, so it is the one number that resizes the lot.\n"
             L"icon-size = %.0f\n"
             L"\n"
             L"# How big the icon under the cursor gets (2.00 is the ceiling - the\n"
             L"# window reserves headroom for it at startup), and how far either side\n"
             L"# of it the swell reaches, in pixels.\n"
             L"max-scale = %.2f\n"
             L"influence = %.0f\n"
             L"\n"
             L"# Whether the row slides sideways as it swells, so the icon under\n"
             L"# the cursor stays exactly under it. That is what macOS does, and\n"
             L"# it is also why the whole bar appears to drift left and right as\n"
             L"# you move along it. Off keeps the bar's centre still.\n"
             L"follow-cursor = %s\n"
             L"\n"
             L"# The divider between the two groups. Empty draws the built-in\n"
             L"# rule; give it a path to an image - a Nexus theme's sep.png, or\n"
             L"# anything else - and that is drawn instead, stretched to height.\n"
             L"separator-image = %s\n"
             L"\n"
             L"# The air on each side of a divider. The design's own value is 8;\n"
             L"# raising it is how a divider stops being a hairline between two\n"
             L"# neighbours and starts being a break between runs. Scales with\n"
             L"# the dock, like every other measurement in the layout.\n"
             L"divider-gap = %.0f\n"
             L"\n"
             L"# The hover label's face, in pixels, and whether it is bold. Nexus asks\n"
             L"# GDI for Segoe UI 12 bold, but light text on a near-black pill blooms,\n"
             L"# so matching those numbers exactly comes out heavier than Nexus looks.\n"
             L"label-font-size = %.1f\n"
             L"label-bold = %s\n"
             L"\n"
             L"# Whether the glass swells around a raised icon. Off by default: it\n"
             L"# fuses the bar's outline to the icons, which reads as liquid clinging\n"
             L"# to them rather than as a pane of glass.\n"
             L"icon-bulge = %s\n"
             L"\n"
             L"# --- Placement -----------------------------------------------------\n"
             L"# Which monitor to live on: `primary`, or a 1-based number. An index\n"
             L"# that does not exist falls back to the primary one.\n"
             L"monitor = %s\n"
             L"\n"
             L"# Reserve screen space so maximised windows stop above the dock, the\n"
             L"# way the taskbar does. Ignored while auto-hide is on, where a dock\n"
             L"# that is not on screen has no business holding room.\n"
             L"reserve-space = %s\n"
             L"\n"
             L"# --- Auto-hide -----------------------------------------------------\n"
             L"auto-hide = %s\n"
             L"\n"
             L"# Whether auto-hide only applies while something is actually in the\n"
             L"# way. On, the dock tucks under a window and comes straight back\n"
             L"# when the desktop is clear, so minimising everything shows it. Off\n"
             L"# hides it on its dwell whatever is behind.\n"
             L"hide-when-covered = %s\n"
             L"\n"
             L"# How long the dock stays out once nothing is using it, and how long\n"
             L"# the slide itself takes.\n"
             L"dwell-seconds = %.1f\n"
             L"slide-seconds = %.2f\n",
             refraction, depth, dispersion, frost, splay, lightAngleDegrees, lightIntensity,
             tintAlpha, innerShadow, rimOpacity,
             backdrop == BackdropSource::Screen ? L"screen" : L"wallpaper",
             magnification ? L"on" : L"off", iconSize, maxScale, influencePx,
             followCursor ? L"on" : L"off", separatorImage.c_str(), dividerGap, labelFontSize,
             labelBold ? L"on" : L"off",
             iconBulge ? L"on" : L"off", monitorText, reserveSpace ? L"on" : L"off",
             autoHide ? L"on" : L"off", hideWhenCovered ? L"on" : L"off", dwellSeconds,
             slideSeconds);

    fclose(file);
    LogInfo("Wrote a default settings file");
    return true;
}

} // namespace liquidock
