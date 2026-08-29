#include "app/DockLayout.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "core/DesignTokens.h"

namespace liquidock {
namespace {

using namespace design;

// The springs are integrated at a fixed step rather than at the frame's own.
// Semi-implicit Euler on a critically damped spring goes unstable once the step
// approaches 2/omega, which at this stiffness is about 97 ms - well inside the
// range a stalled frame can reach. Substepping makes the motion identical at
// 60 Hz and 240 Hz, and identical again after a hitch.
constexpr float kSubstep = 1.0f / 240.0f;

// Below these the spring has visually arrived, and continuing to integrate it
// would keep the dock presenting frames forever for motion no one can see.
constexpr float kRestPosition = 0.001f;
constexpr float kRestVelocity = 0.01f;

// An icon has to be meaningfully raised before it earns a lens; a bulge that
// tracks a scale of 1.005 is invisible and just costs the shader an iteration.
constexpr float kLensThreshold = 1.02f;

float BounceOffset(float time) {
    if (time < 0.0f || time > magnify::kBounceSeconds) {
        return 0.0f;
    }
    const float phase = time / magnify::kBounceSeconds;
    // Two hops - sin over a full period peaks twice - fading out linearly, so
    // the icon lands rather than stopping mid-air.
    const float hop = std::fabs(std::sin(phase * 2.0f * std::numbers::pi_v<float>));
    return magnify::kBounceHeightPx * hop * (1.0f - phase);
}

// The magnification wave. A raised cosine: zero slope at the cursor and zero
// slope where it dies out, so neither the peak nor the edge of the wave has a
// crease in it.
float WaveScale(float distance) {
    const float t = distance / magnify::kInfluencePx;
    if (t >= 1.0f) {
        return 1.0f;
    }
    const float bell = 0.5f * (1.0f + std::cos(t * std::numbers::pi_v<float>));
    return 1.0f + (magnify::kMaxScale - 1.0f) * bell;
}

} // namespace

float DockLayout::bar_center_y() {
    return kBleed + kBarHeight * 0.5f;
}

float DockLayout::bar_half_height() {
    return kBarHeight * 0.5f;
}

void DockLayout::SetItems(const std::vector<DockItem>& items) {
    elements_.clear();
    icons_.clear();
    separators_.clear();
    lenses_.clear();

    bool separatorPlaced = false;
    bool first = true;
    for (size_t index = 0; index < items.size(); ++index) {
        const bool utility = items[index].group == ItemGroup::Utility;

        // The hairline goes in once, at the first utility item. If the list has
        // no utility items it never appears, which is the right answer for a
        // dock of ten applications and nothing else.
        if (utility && !separatorPlaced && !first) {
            Element hairline;
            hairline.itemIndex = -1;
            hairline.baseWidth = kSeparatorWidth;
            hairline.gapBefore = kGroupGap;
            elements_.push_back(hairline);
            separatorPlaced = true;
        }

        Element element;
        element.itemIndex = static_cast<int>(index);
        element.baseWidth = kIconSize;
        if (first) {
            element.gapBefore = 0.0f;
        } else if (separatorPlaced && elements_.back().itemIndex < 0) {
            element.gapBefore = kGroupGap; // the other side of the hairline
        } else {
            element.gapBefore = kIconGap;
        }
        elements_.push_back(element);
        first = false;
    }
}

void DockLayout::SetCursor(float x, bool inside) {
    // The last cursor position is kept even after the cursor leaves. It is what
    // the proportional anchor is measured against, so holding it lets the row
    // shrink back to rest around the place the cursor actually was instead of
    // snapping toward the middle.
    if (inside) {
        cursorX_ = x;
    }
    hovered_ = inside;
}

void DockLayout::Bounce(int itemIndex) {
    for (Element& element : elements_) {
        if (element.itemIndex == itemIndex) {
            element.bounceTime = 0.0f;
            return;
        }
    }
}

float DockLayout::ContentWidth() const {
    float total = 0.0f;
    for (const Element& element : elements_) {
        total += element.gapBefore + element.baseWidth * element.scale;
    }
    return total;
}

float DockLayout::TargetScale(const Element& element, float baseCenterX) const {
    if (element.itemIndex < 0 || !hovered_) {
        return 1.0f; // the hairline never magnifies
    }
    return WaveScale(std::fabs(cursorX_ - baseCenterX));
}

float DockLayout::RestingBarWidth() const {
    float total = 0.0f;
    for (const Element& element : elements_) {
        total += element.gapBefore + element.baseWidth;
    }
    return total + 2.0f * kPaddingX;
}

float DockLayout::MaxBarWidth() const {
    // Scales depend on cursor-to-icon distances, which do not care where the
    // row sits, so this can be probed in a coordinate space of its own - and
    // has to be, because the answer is what decides how wide the window is.
    std::vector<float> centers;
    centers.reserve(elements_.size());
    float x = 0.0f;
    for (const Element& element : elements_) {
        x += element.gapBefore;
        centers.push_back(x + element.baseWidth * 0.5f);
        x += element.baseWidth;
    }
    const float restingContent = x;
    if (elements_.empty()) {
        return RestingBarWidth();
    }

    // Closed form would need the wave's shape and the icon pitch to interact in
    // a way that changes whenever either is tuned. A sweep is exact for
    // whatever the numbers happen to be, and it runs once per layout change.
    float widest = restingContent;
    for (float cursor = -magnify::kInfluencePx; cursor <= restingContent + magnify::kInfluencePx;
         cursor += 2.0f) {
        float total = 0.0f;
        for (size_t i = 0; i < elements_.size(); ++i) {
            const float scale = (elements_[i].itemIndex < 0)
                                    ? 1.0f
                                    : WaveScale(std::fabs(cursor - centers[i]));
            total += elements_[i].gapBefore + elements_[i].baseWidth * scale;
        }
        widest = std::max(widest, total);
    }
    return widest + 2.0f * kPaddingX;
}

bool DockLayout::Advance(float deltaSeconds) {
    if (elements_.empty()) {
        icons_.clear();
        separators_.clear();
        lenses_.clear();
        barCenterX_ = windowWidth_ * 0.5f;
        barHalfWidth_ = 0.0f;
        return false;
    }

    // Resting centres, which is what the wave is measured against.
    const float restingBar = RestingBarWidth();
    float restLeft = (windowWidth_ - restingBar) * 0.5f + kPaddingX;
    std::vector<float> restCenters;
    restCenters.reserve(elements_.size());
    {
        float x = restLeft;
        for (const Element& element : elements_) {
            x += element.gapBefore;
            restCenters.push_back(x + element.baseWidth * 0.5f);
            x += element.baseWidth;
        }
    }

    const float omega = std::sqrt(magnify::kStiffness);
    bool moving = false;

    for (size_t i = 0; i < elements_.size(); ++i) {
        Element& element = elements_[i];
        const float target = TargetScale(element, restCenters[i]);

        float remaining = deltaSeconds;
        while (remaining > 0.0f) {
            const float step = std::min(remaining, kSubstep);
            remaining -= step;
            const float acceleration =
                -magnify::kStiffness * (element.scale - target) - 2.0f * omega * element.velocity;
            element.velocity += acceleration * step;
            element.scale += element.velocity * step;
        }

        if (std::fabs(element.scale - target) < kRestPosition &&
            std::fabs(element.velocity) < kRestVelocity) {
            element.scale = target;
            element.velocity = 0.0f;
        } else {
            moving = true;
        }

        if (element.bounceTime >= 0.0f) {
            element.bounceTime += deltaSeconds;
            if (element.bounceTime > magnify::kBounceSeconds) {
                element.bounceTime = -1.0f;
            } else {
                moving = true;
            }
        }
    }

    Place();
    return moving;
}

void DockLayout::Place() {
    icons_.clear();
    separators_.clear();
    lenses_.clear();

    const float restingBar = RestingBarWidth();
    const float restingContent = restingBar - 2.0f * kPaddingX;
    const float restLeft = (windowWidth_ - restingBar) * 0.5f + kPaddingX;
    const float content = ContentWidth();

    // The proportional anchor. u is where the cursor falls along the resting
    // row, in units of that row; holding it fixed as the row grows is what
    // keeps the hovered icon under the cursor. u is deliberately not clamped:
    // outside the row the scales are all 1, so growth is zero and the
    // expression collapses to the resting position with no seam.
    float left = restLeft;
    if (restingContent > 0.0f) {
        const float u = (cursorX_ - restLeft) / restingContent;
        left = restLeft - u * (content - restingContent);
    }

    barCenterX_ = left + content * 0.5f;
    barHalfWidth_ = content * 0.5f + kPaddingX;

    const float rowBottom = kBleed + kBarHeight - kPaddingY;
    const float barTop = kBleed;
    const float barBottom = kBleed + kBarHeight;

    float x = left;
    for (const Element& element : elements_) {
        x += element.gapBefore;
        const float width = element.baseWidth * element.scale;
        const float centerX = x + width * 0.5f;
        x += width;

        if (element.itemIndex < 0) {
            PlacedIcon hairline;
            hairline.itemIndex = -1;
            hairline.centerX = centerX;
            hairline.centerY = bar_center_y();
            hairline.size = kSeparatorHeight; // the drawing code takes width from the design
            hairline.scale = 1.0f;
            separators_.push_back(hairline);
            continue;
        }

        // Icons keep their bottom edge on the icon row and grow upward, so a
        // magnified icon rises out of the bar rather than swelling through it.
        const float iconSize = kIconSize * element.scale;
        const float lift = BounceOffset(element.bounceTime);

        PlacedIcon icon;
        icon.itemIndex = element.itemIndex;
        icon.centerX = centerX;
        icon.centerY = rowBottom - iconSize * 0.5f - lift;
        icon.size = iconSize;
        icon.scale = element.scale;
        icons_.push_back(icon);

        if (element.scale > kLensThreshold && lenses_.size() < design::kMaxLenses) {
            // The lens bottom stays on the bar's bottom edge and its top rises
            // with the icon. At scale 1 it is exactly inscribed in the bar, so
            // it contributes nothing until the icon actually lifts - which is
            // what keeps the resting silhouette the plain rounded rectangle the
            // design specifies.
            const float top = barTop - (element.scale - 1.0f) * kIconSize * magnify::kBulge;
            GlassLens lens;
            lens.centerX = centerX;
            lens.centerY = (top + barBottom) * 0.5f;
            lens.halfWidth = iconSize * 0.5f + kIconGap;
            lens.halfHeight = (barBottom - top) * 0.5f;
            lens.radius = std::min(lens.halfWidth, lens.halfHeight);
            lenses_.push_back(lens);
        }
    }
}

bool DockLayout::Contains(float x, float y) const {
    if (barHalfWidth_ <= 0.0f) {
        return false;
    }
    const float barTop = kBleed;
    const float barBottom = kBleed + kBarHeight;
    if (y >= barTop && y <= barBottom && std::fabs(x - barCenterX_) <= barHalfWidth_) {
        return true;
    }
    // A raised icon stands proud of the bar, and clicking the part that sticks
    // out has to work or the magnified icon is harder to hit than the resting
    // one, which would be exactly backwards.
    for (const PlacedIcon& icon : icons_) {
        const float half = icon.size * 0.5f;
        if (std::fabs(x - icon.centerX) <= half && y >= icon.centerY - half &&
            y <= icon.centerY + half) {
            return true;
        }
    }
    return false;
}

int DockLayout::ItemAt(float x, float y) const {
    const float barTop = kBleed;
    const float barBottom = kBleed + kBarHeight;

    int best = -1;
    float bestDistance = 0.0f;
    for (const PlacedIcon& icon : icons_) {
        // The hit slab is the icon's column, from the top of whichever is
        // higher - the icon or the bar - down to the bar's bottom edge. Clicking
        // the padding under an icon activates it, the way the macOS dock does.
        const float half = icon.size * 0.5f + kIconGap * 0.5f;
        const float top = std::min(barTop, icon.centerY - icon.size * 0.5f);
        if (y < top || y > barBottom) {
            continue;
        }
        const float distance = std::fabs(x - icon.centerX);
        if (distance <= half && (best < 0 || distance < bestDistance)) {
            best = icon.itemIndex;
            bestDistance = distance;
        }
    }
    return best;
}

} // namespace liquidock
