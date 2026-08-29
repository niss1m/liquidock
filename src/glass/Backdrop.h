#pragma once

#include <windows.h>
#include <d3d11_1.h>

namespace liquidock {

// The image the glass refracts.
//
// Windows gives desktop apps no live backdrop brush - CreateBackdropBrush is
// UWP-only, and TryCreateBlurredWallpaperBackdropBrush only ever returns the
// wallpaper - so there are exactly two honest ways to get one, and LiquiDock
// ships both behind this interface. Everything downstream of here, the frost
// chain and the glass shader included, works the same either way: a texture
// plus the transform that maps a point on the monitor into it.
class Backdrop {
public:
    virtual ~Backdrop() = default;

    // Brings the backdrop up to date for `monitor`. True when the content
    // changed and the caches built from it - the frost blur - are now stale.
    virtual bool Update(HMONITOR monitor) = 0;

    // The dock's footprint, in monitor-relative pixels. A source that holds the
    // whole monitor ignores this; one that captures only what it needs uses it
    // to decide both what to copy and what changes are worth waking up for.
    virtual void SetRegion(const RECT& region) { (void)region; }

    // Forces a reload on the next Update.
    virtual void Invalidate() {}

    // Whether the dock is on screen. A source with a cost while running uses
    // this to stop entirely while the dock is tucked away.
    virtual void SetActive(bool active) { (void)active; }

    // True once the source has given up - no duplication available on this
    // adapter, say. The dock falls back to the wallpaper rather than rendering
    // nothing.
    virtual bool failed() const { return false; }

    virtual ID3D11ShaderResourceView* srv() const = 0;

    // Maps a point in monitor-relative pixels to a UV in the backdrop texture:
    //     t  = pixel / monitorSize
    //     uv = t * uvScale + uvOffset
    // With `tiled`, the shader takes frac(uv) so a clamp sampler still tiles.
    virtual void uv_scale(float out[2]) const = 0;
    virtual void uv_offset(float out[2]) const = 0;
    virtual bool tiled() const = 0;

    virtual RECT monitor_rect() const = 0;
};

} // namespace liquidock
