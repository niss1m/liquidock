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
inline constexpr float kBarTint[4] = {1.0f, 1.0f, 1.0f, 0.08f};

// separator, rgba(255,255,255,0.2)
inline constexpr float kSeparatorTint[4] = {1.0f, 1.0f, 1.0f, 0.20f};

// The glass, normalised to 0..1 from the 0..100 the Figma panel shows.
//
// These are the *shipping* defaults, not the panel's own numbers. The panel was
// authored against a flat grey artboard; on a real desktop, over a photograph,
// the same values turn the whole bar into a lens - the wallpaper is hauled
// across the pane rather than bending at its edge, and the dock reads as a blob
// of liquid instead of a sheet of glass. Each departure is noted with the value
// it replaced, so the design can still be argued with rather than guessed at.
//
// Every one of these is overridable in settings.txt, which is the honest place
// for a judgement call about how something looks.
namespace glass {
inline constexpr float kLightAngleDegrees = -45.0f; // Figma
inline constexpr float kLightIntensity = 0.55f;     // Figma 0.80; the specular blew out
inline constexpr float kRefraction = 0.45f;         // Figma 0.80; the rim should bend, not smear
inline constexpr float kDepth = 0.35f;              // Figma 0.20; a wider bevel reads as thicker
inline constexpr float kDispersion = 0.20f;         // Figma 0.50; the fringing was rainbow-bright
// Figma 0.04, which is essentially clear glass. Frosting is what separates the
// dock from the photograph behind it, and without it the bar has no surface of
// its own on a busy wallpaper.
inline constexpr float kFrost = 0.65f;
// Figma 1.00, which fans the bending all the way to the middle of the pane.
// Keeping it near the rim is what leaves the interior flat.
inline constexpr float kSplay = 0.60f;
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
inline constexpr float kBleed = kMaxIconRise + magnify::kBounceHeightPx + 20.0f;

static_assert(kMaxConfigurableScale >= magnify::kMaxScale,
              "The ceiling has to admit the default magnification");
static_assert(kBleed > kMaxIconRise + magnify::kBounceHeightPx,
              "The window must contain a raised icon at the top of its bounce");

} // namespace liquidock::design
