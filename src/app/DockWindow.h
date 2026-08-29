#pragma once

#include <windows.h>

#include <memory>

#include "gfx/CompositionTarget.h"
#include "gfx/GraphicsDevice.h"
#include "gfx/ShaderCache.h"
#include "glass/FrostChain.h"
#include "glass/WallpaperBackdrop.h"

namespace liquidock {

// Mirrors GlassConstants in shaders/Glass.hlsl. Both sides are float4-packed so
// the layouts cannot drift apart silently.
struct GlassConstants {
    float viewportCenter[4]; // xy = viewport size (px), zw = rect centre (px)
    float shape[4];          // xy = rect half size (px), z = corner radius (px), w = time (s)
    float light[4];          // x = angle (rad), y = intensity, z = refraction, w = depth
    float material[4];       // x = dispersion, y = frost, z = splay, w = tiled flag
    float tint[4];           // straight alpha
    float windowOrigin[4];   // xy = window origin (monitor px), zw = monitor size (px)
    float backdropUv[4];     // xy = uv scale, zw = uv offset
};
static_assert(sizeof(GlassConstants) == 112, "GlassConstants must match the HLSL cbuffer");
static_assert(sizeof(GlassConstants) % 16 == 0, "Constant buffers must be 16-byte aligned");

// The dock's own window: a click-through, always-on-top, per-pixel-alpha
// surface that owns nothing but the glass. Items, input and animation arrive in
// M2; for now this exists to prove the composition path works on real hardware.
class DockWindow {
public:
    DockWindow() = default;
    DockWindow(const DockWindow&) = delete;
    DockWindow& operator=(const DockWindow&) = delete;
    ~DockWindow();

    // `diagnostic` renders the bar in flat opaque magenta instead of glass. The
    // design fill is 5% white, which is correct but nearly invisible on its
    // own - so if the composition path were broken on a given driver, a normal
    // render would look much the same as a failed one. This makes the two
    // impossible to confuse.
    bool Create(GraphicsDevice& device, bool diagnostic = false);
    void Destroy();

    HWND hwnd() const { return hwnd_; }

    // Marks the window dirty. The dock never renders on a timer - a frame is
    // presented only in response to something that actually changed.
    void RequestRedraw();

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    // Takes the HWND as a parameter rather than reading hwnd_: WM_DESTROY
    // clears the member, and WM_NCDESTROY arrives afterwards and still has to
    // reach DefWindowProc with a valid handle.
    LRESULT WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool CreateResources();
    void UpdatePlacement();
    void Render();

    GraphicsDevice* device_ = nullptr;
    std::unique_ptr<ShaderCache> shaders_;
    CompositionTarget target_;
    WallpaperBackdrop backdrop_;
    FrostChain frost_;

    HWND hwnd_ = nullptr;
    ComPtr<ID3D11Buffer> constantBuffer_;
    ComPtr<ID3D11SamplerState> sampler_;
    // Set whenever the cached frost is stale: the dock moved or resized, the
    // wallpaper changed, or a shader edit may have changed the frost amount.
    bool frostDirty_ = true;
    UINT dpi_ = 96;
    bool diagnostic_ = false;
    bool deviceLost_ = false;
    LARGE_INTEGER startTime_{};
    LARGE_INTEGER frequency_{};
};

} // namespace liquidock
