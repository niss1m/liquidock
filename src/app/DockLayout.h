#pragma once

#include <vector>

#include "model/DockItem.h"

namespace liquidock {

// One icon, placed and sized for this frame. Logical pixels in window space,
// measured with the dock fully revealed; the slide offset is applied by the
// caller so the layout does not have to know about auto-hide.
struct PlacedIcon {
    int itemIndex = -1;
    float centerX = 0.0f;
    float centerY = 0.0f;
    float size = 0.0f;  // edge length, magnification included
    float scale = 1.0f; // 1 at rest
};

// A bulge in the glass under a raised icon, fused into the bar body with a
// smooth minimum. Logical pixels, same space as PlacedIcon.
struct GlassLens {
    float centerX = 0.0f;
    float centerY = 0.0f;
    float halfWidth = 0.0f;
    float halfHeight = 0.0f;
    float radius = 0.0f;
};

// The dock's geometry, and the physics that animates it.
//
// The magnification is the macOS wave: each icon's target scale falls off with
// its distance from the cursor, and the row is re-laid-out at those scales so
// icons push each other outward instead of overlapping. Two properties make it
// feel right rather than merely correct:
//
//  - The target is computed from each icon's *resting* position, never its
//    magnified one. Feeding a magnified position back into the distance that
//    produced it is a loop, and it oscillates.
//
//  - The run is anchored proportionally. The cursor's position as a fraction of
//    the resting row is held fixed as the row grows, so the icon under the
//    cursor stays under the cursor, and the growth still tapers off smoothly at
//    both ends instead of jumping when the cursor leaves the row.
//
// Every icon then chases its target on its own critically damped spring, which
// is what gives the wave weight and what keeps it smooth when the cursor jumps
// across the dock in one mouse report.
class DockLayout {
public:
    // Rebuilds the element list. Call whenever the item list changes.
    void SetItems(const std::vector<DockItem>& items);

    // Window width in logical pixels, used to centre the resting bar.
    void SetWindowWidth(float logicalWidth) { windowWidth_ = logicalWidth; }

    // Shrinks the row until it fits `availableLogical`, and returns the factor
    // applied. A dock of forty-odd items is wider than a screen at full size,
    // and a dock running off both edges is worse than a slightly smaller one.
    float FitWithin(float availableLogical);
    float item_scale() const { return itemScale_; }

    // The magnification the user asked for. `bulge` is what makes the glass
    // swell around a raised icon; with it off the bar keeps the plain rounded
    // silhouette the design specifies, and icons simply rise out of it.
    void SetMagnification(bool enabled, float maxScale, float influencePx, bool bulge);

    // `x` is in logical window space. `inside` false means the cursor is not
    // over the dock, which relaxes every icon back to rest.
    void SetCursor(float x, bool inside);

    // Starts the launch bounce on one item.
    void Bounce(int itemIndex);

    // Steps the springs and rebuilds the placement. Returns true while anything
    // is still moving, which is what keeps the dock presenting frames.
    bool Advance(float deltaSeconds);

    // The widest the bar can ever get, for sizing the window once.
    float MaxBarWidth() const;
    // The bar at rest, which is what the window is centred on.
    float RestingBarWidth() const;

    float bar_center_x() const { return barCenterX_; }
    float bar_half_width() const { return barHalfWidth_; }
    static float bar_center_y();
    static float bar_half_height();

    const std::vector<PlacedIcon>& icons() const { return icons_; }
    const std::vector<GlassLens>& lenses() const { return lenses_; }
    const std::vector<PlacedIcon>& separators() const { return separators_; }

    // True if the point is over the bar or over any icon standing above it.
    // Logical window space, dock fully revealed.
    bool Contains(float x, float y) const;

    // Index into the item list of the icon under the point, or -1.
    int ItemAt(float x, float y) const;

private:
    // A slot in the row: an icon, or the hairline between the two groups.
    struct Element {
        int itemIndex = -1; // -1 marks the separator
        float baseWidth = 0.0f;
        float gapBefore = 0.0f;
        float scale = 1.0f;
        float velocity = 0.0f;
        float bounceTime = -1.0f; // seconds into the launch bounce, or < 0
    };

    float ContentWidth() const;
    float WaveScale(float distance) const;
    float TargetScale(const Element& element, float baseCenterX) const;
    // Lays out at the current spring scales. Shared by Advance and by the
    // width probe that sizes the window.
    void Place();

    std::vector<Element> elements_;
    std::vector<PlacedIcon> icons_;
    std::vector<PlacedIcon> separators_;
    std::vector<GlassLens> lenses_;

    float windowWidth_ = 0.0f;
    float cursorX_ = 0.0f;
    bool hovered_ = false;
    bool magnification_ = true;
    float maxScale_ = 1.0f;
    float influencePx_ = 1.0f;
    bool bulge_ = false;
    // 1 until the row would not fit the screen.
    float itemScale_ = 1.0f;
    float barCenterX_ = 0.0f;
    float barHalfWidth_ = 0.0f;
};

} // namespace liquidock
