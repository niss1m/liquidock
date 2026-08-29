#pragma once

namespace liquidock::design {

// Every number here is read straight off the Figma file
// (n1c0jARvaXboukVbP5Z9Xq, frame 3:5 "dock-default-state"), with the source
// node noted beside it. The point is that the dock's proportions can be checked
// against the design rather than argued about, and that a design change is a
// change to one file.
//
// Logical pixels at 96 DPI. Everything is multiplied by the monitor's scale
// factor at use, never baked.

inline constexpr float kIconSize = 48.0f;      // node 2:107, size-[48px]
inline constexpr float kIconGap = 4.0f;        // node 3:7 main-apps, gap-[4px]
inline constexpr float kGroupGap = 8.0f;       // node 3:6 dock-bar, gap-[8px]
inline constexpr float kPaddingX = 16.0f;      // node 3:6, px-[16px]
inline constexpr float kPaddingY = 10.0f;      // node 3:6, py-[10px]
inline constexpr float kCornerRadius = 16.0f;  // node 3:6, rounded-[16px]
inline constexpr float kScreenMargin = 20.0f;  // node 3:5, pb-[20px]

inline constexpr float kSeparatorWidth = 1.0f;   // node 2:148, w-px
inline constexpr float kSeparatorHeight = 48.0f; // node 2:148, h-[48px]

// Running indicator: a dot in the bar's bottom padding, under the icon. Not in
// the Figma file - the design has no "running" state - so these are chosen to
// sit inside the 10 px of padding that is already there rather than to make the
// bar taller. It stays on the resting row while the icon above it magnifies,
// the way the macOS one does.
inline constexpr float kIndicatorDiameter = 4.0f;
inline constexpr float kIndicatorGap = 3.0f; // between the icon row and the dot
inline constexpr float kIndicatorTint[4] = {1.0f, 1.0f, 1.0f, 0.75f};

// dock-bar fill, Figma rgba(255,255,255,0.05), raised a little. Still almost
// nothing: the dock reads as glass because of what the shader does to the
// backdrop behind it, not because of this fill. Raising it further is how the
// design gets muddy.
inline constexpr float kBarTint[4] = {1.0f, 1.0f, 1.0f, 0.05f};

// separator, rgba(255,255,255,0.2)
inline constexpr float kSeparatorTint[4] = {1.0f, 1.0f, 1.0f, 0.20f};

// The glass, normalised to 0..1 from the 0..100 the Figma panel shows.
//
// These are the panel's own values. They were overridden for a while with a
// heavier, frostier, desaturated set on the theory that the Figma numbers were
// authored against a flat artboard and would not survive a photograph. Reading
// the actual frame settles it: the design is nearly *clear* glass - the
// wallpaper behind the bar is still sharp, and the bar is legible almost
// entirely from its rim. The heavier set is what made it look muddy.
namespace glass {
inline constexpr float kLightAngleDegrees = -45.0f;
inline constexpr float kLightIntensity = 0.80f;
inline constexpr float kRefraction = 0.80f;
inline constexpr float kDepth = 0.20f;
inline constexpr float kDispersion = 0.50f;
// Essentially clear. This is the one that was most wrong: at 0.82 the dock is a
// frosted slab and the desktop behind it is gone.
inline constexpr float kFrost = 0.04f;
// Full splay, which now means something - it is how far the edge optics reach
// across the face, and the design wants them reaching a long way in.
inline constexpr float kSplay = 1.00f;
} // namespace glass

// --- Magnification --------------------------------------------------------
// Not in the Figma file: the wave is motion, and motion is not something a
// static frame can specify. These are tuned against the macOS dock, which is
// the thing everyone is actually comparing this to.
namespace magnify {

// How big the icon under the cursor gets. macOS's own slider tops out near 2x;
// this sits just under it, because the dock also has to stay a reasonable
// height on a 1080p screen.
inline constexpr float kMaxScale = 1.75f;

// Half-width of the wave in logical pixels. At the 52 px icon pitch this reaches
// about two and a half icons either side, which is what makes it read as a
// swell rather than one icon popping.
inline constexpr float kInfluencePx = 132.0f;

// Critically damped spring, in the usual w^2 form. w = sqrt(430) = 20.7 rad/s,
// so a step settles in roughly 4/w = 0.19 s: quick enough to feel attached to
// the cursor, slow enough to read as mass rather than a jump cut.
inline constexpr float kStiffness = 430.0f;

// A magnified icon keeps its bottom edge on the icon row and grows upward, the
// way the macOS dock does. The glass follows it by this fraction of the growth,
// so the body bulges under a raised icon instead of the icon simply floating
// away from a flat bar.
inline constexpr float kBulge = 0.50f;

// How hard the bulge fuses into the bar, in logical pixels of smooth-min
// radius. Below about 8 the join shows as a crease; far above it the bulge
// stops being localised and the whole bar swells.
inline constexpr float kFuse = 16.0f;

// Launch bounce: two hops over this long, this tall.
inline constexpr float kBounceSeconds = 0.62f;
inline constexpr float kBounceHeightPx = 16.0f;

} // namespace magnify

// Auto-hide timings. Both are overridable in settings.txt.
inline constexpr float kDwellSeconds = 3.0f;  // how long the dock stays out
inline constexpr float kSlideSeconds = 0.22f; // how long the slide itself takes

// The name that appears above the icon under the cursor. Not in the Figma
// file - the design has no hover state - so these follow the macOS dock, which
// is what everyone will compare it to: a small dark pill directly above the
// icon, close enough to belong to it.
namespace label {
inline constexpr float kFontSize = 14.5f;
inline constexpr float kPaddingX = 14.0f;
inline constexpr float kPaddingY = 7.5f;
inline constexpr float kRadius = 10.0f;
// Between the top of the icon and the bottom of the pill. Generous on purpose:
// a label that crowds the icon reads as part of it rather than as a name for it.
inline constexpr float kGap = 13.0f;
// A short fade rather than an instant appearance, so sweeping the cursor along
// the row does not strobe a different name every few milliseconds.
inline constexpr float kFadeSeconds = 0.09f;

// Near-black and near-opaque. The label has to be readable over a photograph
// with no idea what colour it is, and the only thing that reliably survives
// that is black behind white.
inline constexpr float kFill[4] = {0.03f, 0.03f, 0.04f, 0.94f};
// Barely there: invisible over a bright wallpaper, and just enough to keep the
// pill from dissolving into a dark one.
inline constexpr float kEdge[4] = {1.0f, 1.0f, 1.0f, 0.10f};
inline constexpr float kText[4] = {1.0f, 1.0f, 1.0f, 0.95f};
} // namespace label

// Hard ceiling on dock items. The magnified layout and the glass lenses both
// travel to the GPU in constant buffers, which want a fixed size; 32 is far
// past any dock a person would actually use and still only 1 KB of constants.
inline constexpr int kMaxItems = 32;

// Lenses are only emitted for icons that are actually raised, and the wave
// reaches about five icons wide, so eight is generous.
inline constexpr int kMaxLenses = 8;

inline constexpr float kBarHeight = kIconSize + 2.0f * kPaddingY; // 68

// Width of one run of icons, gaps included.
constexpr float GroupWidth(int count) {
    if (count <= 0) {
        return 0.0f;
    }
    return count * kIconSize + (count - 1) * kIconGap;
}

// Width of a bar holding a main run, then a separator, then a utility run.
// The design's own layout - 10 main icons and 2 utility icons - comes out at
// 665, which is the number to check any layout change against.
constexpr float BarWidth(int mainCount, int utilityCount) {
    float content = GroupWidth(mainCount);
    if (utilityCount > 0) {
        content += kGroupGap + kSeparatorWidth + kGroupGap + GroupWidth(utilityCount);
    }
    return content + 2.0f * kPaddingX;
}

static_assert(BarWidth(10, 2) == 665.0f, "Layout no longer matches Figma frame 3:5");
static_assert(kBarHeight == 68.0f, "Bar height no longer matches Figma node 3:6");

// The largest magnification settings.txt will accept.
//
// The window's vertical bleed is a compile-time constant - the window is sized
// once and never resized to animate - so the headroom has to be reserved for
// the largest magnification a user could ask for, not for the default. Raising
// this ceiling costs a taller window for everybody, which is why it stops at
// twice size rather than at whatever the parser would swallow.
inline constexpr float kMaxConfigurableScale = 2.0f;

// How far an icon at that ceiling rises above the top of the bar. The icon keeps
// its bottom edge on the icon row, so all of its growth goes upward, and the
// row already starts kPaddingY below the bar top.
inline constexpr float kMaxIconRise =
    kIconSize * (kMaxConfigurableScale - 1.0f) - kPaddingY;

// The window is grown past the glass on every side so the rim highlight, the
// bulge and the raised icons all have somewhere to land. Derived rather than
// picked, so raising the scale ceiling or the bounce cannot silently start
// clipping icons against the top of the window.
// The hover label sits above the icon, and the icon is at its highest exactly
// when the label is showing, so this is the case the window has to fit.
inline constexpr float kLabelHeight = label::kFontSize + 2.0f * label::kPaddingY;
inline constexpr float kBleed =
    kMaxIconRise + magnify::kBounceHeightPx + label::kGap + kLabelHeight + 8.0f;

static_assert(kMaxConfigurableScale >= magnify::kMaxScale,
              "The ceiling has to admit the default magnification");
static_assert(kBleed > kMaxIconRise + magnify::kBounceHeightPx + kLabelHeight,
              "The window must contain a raised icon, its bounce, and its label");

} // namespace liquidock::design
