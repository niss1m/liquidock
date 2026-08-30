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

// node 2:107, size-[48px]. This is the design's own icon size and the unit the
// rest of the layout is expressed in; the size actually drawn is this times the
// `icon-size` setting's share of it, so every other measurement scales with it
// rather than having to be restated.
inline constexpr float kIconSize = 48.0f;
// What the dock actually draws at by default. Nexus runs at 32 and it is a
// better size for a dock that holds forty things.
inline constexpr float kDefaultIconSize = 32.0f;
// The range the setting allows. The ceiling is what the window's height is
// reserved for, so raising it costs everyone a taller window.
inline constexpr float kMinIconSize = 20.0f;
inline constexpr float kMaxIconSize = 48.0f;
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
// The Figma glass panel's own numbers, and this time they are the numbers the
// renderer is calibrated against rather than a starting point that got tuned
// away from. Measuring the design's render - node 3:5, a 2.25x export - settles
// every one of them:
//
//   * its interior is a quarter as sharp as the backdrop just outside the bar,
//     which is a Gaussian of about 1.8 logical pixels. That is frost 0.04 at
//     the calibration in DockWindow.cpp, and the panel says 4.
//   * its rim measures +100/255 at the top edge, +83 at both sides and +59 at
//     the bottom. A light coming from -45 degrees would put the two sides far
//     apart; equal sides with a bright top is light from straight above, and
//     -90 is what our own convention spells that.
//   * its edge compresses the background over about four logical pixels, which
//     is depth 0.20 against the bend the shader now applies.
// The Figma panel's own number. It reads as -45 there and it is -45 here,
// because the offset below makes the two conventions agree rather than asking
// anyone to translate between them.
inline constexpr float kLightAngleDegrees = -45.0f;

// What to add before the shader uses it. Figma's angle and ours are not the
// same zero: the design's render, at its stated -45, has its bright edges along
// the top and the bottom with the sides dark, and reproducing that in our own
// terms takes -90. One calibration point only fixes an offset, not a direction,
// so this is exactly that - the amount that makes the panel's number produce
// the panel's picture.
inline constexpr float kLightAngleOffset = -45.0f;
inline constexpr float kLightIntensity = 0.80f;
inline constexpr float kRefraction = 0.80f;
inline constexpr float kDepth = 0.20f;
inline constexpr float kDispersion = 0.50f;
inline constexpr float kFrost = 0.04f;
// Full splay: the bending reaches the middle of the pane. This was 0.64 while
// the band was measured in pixels, where anything above about a third already
// covered a 45 px bar and the control had nothing left to move.
inline constexpr float kSplay = 1.00f;

// The dark shoulder inside the rim, as a fraction of the depth measured off the
// design. 1.00 is the design's own; 0 removes it, leaving the bright line on
// its own.
inline constexpr float kInnerShadow = 1.00f;

// How opaque the rim is. Separate from the light's intensity, which is about
// how hard the light falls; this is about how much of the edge you want to see.
inline constexpr float kRimOpacity = 1.00f;

// Blur radius at frost = 1.0, in logical pixels. Calibrated: the design's
// render is a quarter as sharp inside the bar as immediately outside it, which
// is a Gaussian of about 1.8 logical pixels, and the Figma panel calls that
// frost 4 of 100. Shared so the dock and its menu cannot drift apart.
inline constexpr float kMaxFrostSigmaPx = 44.0f;
} // namespace glass

// --- Magnification --------------------------------------------------------
// Not in the Figma file: the wave is motion, and motion is not something a
// static frame can specify. These are tuned against the macOS dock, which is
// the thing everyone is actually comparing this to.
namespace magnify {

// How big the icon under the cursor gets. Double, which is what a dock at 32 px
// needs for the magnified icon to be worth looking at - at 1.6 the difference
// between the icon you are pointing at and its neighbours is too slight to
// register at this size.
inline constexpr float kMaxScale = 2.00f;

// Half-width of the wave in logical pixels - Nexus's DockMagPixels, which is
// worth copying exactly. It was 132 here, which at any icon size reaches so far
// either side that the wave feels slow and vague: the cursor arrives somewhere
// and half the dock is already moving. Sixty is about two icons, and that is
// what makes the swell feel attached to the pointer.
inline constexpr float kInfluencePx = 60.0f;

// The spring now runs in one place only: relaxing back to rest once the cursor
// has left. While the cursor is on the dock the magnification is computed
// directly from its position, because anything that converges necessarily lags.
// w = sqrt(1250) = 35.4 rad/s, so leaving settles in about 0.11 s.
inline constexpr float kStiffness = 1250.0f;

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
inline constexpr float kSlideSeconds = 0.07f; // how long the slide itself takes

// The name that appears above the icon under the cursor. Not in the Figma
// file - the design has no hover state - so these follow the macOS dock, which
// is what everyone will compare it to: a small dark pill directly above the
// icon, close enough to belong to it.
namespace label {
// Nexus's own: DockFontName1 "Segoe UI", DockFontSize1 12, DockFontBold1 True.
// 12 pt at 96 dpi is 16 px, and the weight is what most of the difference in
// character was - a semibold variable face next to a plain bold one does not
// read as the same label however close the metrics get.
// 12 pt at 96 dpi is 16 px. I dropped this to 15 and to semibold on a theory
// about light glyphs blooming into a dark pill; against a photograph of the
// real thing the weight was plainly too light, so the registry's own numbers
// stand. Both remain settings - this is the one thing here that cannot be
// measured off the running Nexus, whose tooltip window will not show for
// synthetic input.
inline constexpr float kFontSize = 16.0f;
inline constexpr bool kBold = true;
// The ceiling for the label-font-size setting. The window reserves headroom for
// a label this tall at startup, so the pill cannot grow past its own bleed.
inline constexpr float kMaxFontSize = 22.0f;
inline constexpr float kPaddingX = 11.0f;
inline constexpr float kPaddingY = 5.0f;
inline constexpr float kRadius = 8.0f;
// The tail. Measured to the point, not to the pill's flat bottom edge, so the
// gap below stays the same whatever the tail's height is.
inline constexpr float kTailWidth = 13.0f;
inline constexpr float kTailHeight = 7.0f;
// Between the top of the icon and the tip of the tail. Small, because the tail
// is what connects the name to the icon now - the wide gap that did that job
// before is no longer carrying it.
inline constexpr float kGap = 6.0f;
// Instant. Nexus has MagSmoothness 0 and DisableAnimations set, and a fade here
// is another tenth of a second between pointing at something and being told
// what it is.
inline constexpr float kFadeSeconds = 0.0f;
// Dismissed after this long without the cursor moving. Nexus does the same, and
// it is what stops a name you have finished reading from sitting over your work.
inline constexpr unsigned kIdleMs = 1000;

// Near-black and near-opaque. The label has to be readable over a photograph
// with no idea what colour it is, and the only thing that reliably survives
// that is black behind white.
inline constexpr float kFill[4] = {0.03f, 0.03f, 0.04f, 0.94f};
// None. Nexus's has no border at all - it is a flat black shape with the name
// on it, and the hairline was reading as a glow around the pill rather than as
// a definition of it. Kept as a token, at zero, because the tooltip is drawn
// with one call that takes both colours.
inline constexpr float kEdge[4] = {1.0f, 1.0f, 1.0f, 0.0f};
inline constexpr float kText[4] = {1.0f, 1.0f, 1.0f, 0.95f};
} // namespace label

// The context menu is glass too, but it is glass with words on it, and that
// changes what the material has to do. The dock is clear because you are meant
// to see the desktop through it; a menu is meant to be read, and text over a
// sharp photograph is unreadable however pretty the photograph. So the menu
// frosts hard and tints harder, and bends much less - there is no point
// refracting an image nobody is looking at.
namespace menu {
inline constexpr float kRefraction = 0.30f;
inline constexpr float kDepth = 0.22f;
inline constexpr float kDispersion = 0.15f;
inline constexpr float kFrost = 0.80f;
inline constexpr float kSplay = 0.30f;
inline constexpr float kLightIntensity = 0.55f;
// Enough white that the text has something to sit on whatever is behind.
inline constexpr float kTintAlpha = 0.16f;
} // namespace menu

// Hard ceiling on dock items. The magnified layout and the glass lenses both
// travel to the GPU in constant buffers, which want a fixed size. Raised from
// 32 after importing a real Nexus dock that had 43 - "far past any dock a person
// would actually use" turned out to be wrong about the first person who tried.
inline constexpr int kMaxItems = 64;

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
inline constexpr float kLabelHeight = label::kMaxFontSize + 2.0f * label::kPaddingY;
inline constexpr float kBleed = kMaxIconRise + magnify::kBounceHeightPx + label::kGap +
                                label::kTailHeight + kLabelHeight + 8.0f;

static_assert(kMaxConfigurableScale >= magnify::kMaxScale,
              "The ceiling has to admit the default magnification");
static_assert(kBleed > kMaxIconRise + magnify::kBounceHeightPx + kLabelHeight +
                          label::kTailHeight,
              "The window must contain a raised icon, its bounce, and its label");

} // namespace liquidock::design
