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

// dock-bar fill, rgba(255,255,255,0.05). Deliberately almost nothing: the dock
// reads as glass because of what the shader does to the backdrop behind it, not
// because of this fill. Raising it is how the design gets muddy.
inline constexpr float kBarTint[4] = {1.0f, 1.0f, 1.0f, 0.05f};

// separator, rgba(255,255,255,0.2)
inline constexpr float kSeparatorTint[4] = {1.0f, 1.0f, 1.0f, 0.20f};

// The Figma glass panel, normalised to 0..1 from the 0..100 the panel shows.
namespace glass {
inline constexpr float kLightAngleDegrees = -45.0f;
inline constexpr float kLightIntensity = 0.80f;
inline constexpr float kRefraction = 0.80f;
inline constexpr float kDepth = 0.20f;
inline constexpr float kDispersion = 0.50f;
inline constexpr float kFrost = 0.04f;
inline constexpr float kSplay = 1.00f;
} // namespace glass

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

} // namespace liquidock::design
