#pragma once

#include <windows.h>

#include <string>

#include "gfx/GraphicsDevice.h"
#include "glass/Backdrop.h"

namespace liquidock {

// Supplies the image the glass refracts.
//
// Windows gives desktop apps no live backdrop brush - CreateBackdropBrush is
// UWP-only, and TryCreateBlurredWallpaperBackdropBrush only ever returns the
// wallpaper - so the two honest options are to read the wallpaper ourselves or
// to capture the screen. This is the first, and it is the default because it
// costs nothing once loaded and is exactly correct whenever nothing is behind
// the dock. The capture-based source arrives in M3 for the case where something
// is.
//
// The wallpaper is uploaded once at its native size. Rather than pre-composing
// a monitor-sized copy, the fit mode is reduced to a UV transform the shader
// applies while sampling, which keeps this to a single texture and no
// per-frame work at all.
class WallpaperBackdrop : public Backdrop {
public:
    WallpaperBackdrop() = default;
    WallpaperBackdrop(const WallpaperBackdrop&) = delete;
    WallpaperBackdrop& operator=(const WallpaperBackdrop&) = delete;

    bool Initialize(GraphicsDevice& device);

    // Loads the wallpaper for `monitor` if it is not already loaded. Returns
    // true when the backdrop changed and dependent caches (the frost blur) must
    // be rebuilt.
    bool Update(HMONITOR monitor) override;

    // Forces a reload on the next Update, for WM_SETTINGCHANGE/SPI_SETDESKWALLPAPER.
    void Invalidate() override { loadedKey_.clear(); }

    ID3D11ShaderResourceView* srv() const override { return srv_.Get(); }

    // Maps a point in monitor-relative pixels to a UV in the wallpaper:
    //     t  = pixel / monitorSize
    //     uv = t * uvScale + uvOffset
    // With `tiled`, the shader takes frac(uv) so a clamp sampler still tiles.
    void uv_scale(float out[2]) const override { out[0] = uvScale_[0]; out[1] = uvScale_[1]; }
    void uv_offset(float out[2]) const override { out[0] = uvOffset_[0]; out[1] = uvOffset_[1]; }
    bool tiled() const override { return tiled_; }

    RECT monitor_rect() const override { return monitorRect_; }

private:
    bool LoadImage(const std::wstring& path);
    bool CreateSolidColour(COLORREF colour);
    void ComputeUvTransform(int imageWidth, int imageHeight, int monitorWidth, int monitorHeight,
                            int position);

    GraphicsDevice* device_ = nullptr;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> srv_;

    std::wstring loadedKey_;
    RECT monitorRect_{};
    float uvScale_[2] = {1.0f, 1.0f};
    float uvOffset_[2] = {0.0f, 0.0f};
    bool tiled_ = false;
};

} // namespace liquidock
