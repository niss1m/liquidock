#pragma once

#include <windows.h>

#include <memory>

#include "gfx/CompositionTarget.h"
#include "gfx/GraphicsDevice.h"
#include "gfx/ShaderCache.h"

namespace liquidock {

// Mirrors GlassConstants in shaders/Glass.hlsl. Both sides are float4-packed so
// the layouts cannot drift apart silently.
struct GlassConstants {
    float viewportCenter[4]; // xy = viewport size (px), zw = rect centre (px)
    float shape[4];          // xy = rect half size (px), z = corner radius (px), w = time (s)
    float light[4];          // x = angle (rad), y = intensity, z = refraction, w = depth
    float material[4];       // x = dispersion, y = frost, z = splay, w = reserved
    float tint[4];           // straight alpha
};
static_assert(sizeof(GlassConstants) == 80, "GlassConstants must match the HLSL cbuffer");
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

    bool Create(GraphicsDevice& device);
    void Destroy();

    HWND hwnd() const { return hwnd_; }

    // Marks the window dirty. The dock never renders on a timer - a frame is
    // presented only in response to something that actually changed.
    void RequestRedraw();

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    bool CreateResources();
    void UpdatePlacement();
    void Render();

    GraphicsDevice* device_ = nullptr;
    std::unique_ptr<ShaderCache> shaders_;
    CompositionTarget target_;

    HWND hwnd_ = nullptr;
    ComPtr<ID3D11Buffer> constantBuffer_;
    UINT dpi_ = 96;
    LARGE_INTEGER startTime_{};
    LARGE_INTEGER frequency_{};
};

} // namespace liquidock
