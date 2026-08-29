#pragma once

#include <windows.h>

#include "gfx/GraphicsDevice.h"
#include "gfx/ShaderCache.h"
#include "glass/Backdrop.h"

namespace liquidock {

// Produces the blurred copy of the backdrop that the glass mixes in as frost.
//
// Three passes at quarter resolution: crop the window's footprint out of the
// wallpaper, then a separable Gaussian across and down. For a 745x148 dock that
// is a 186x37 target, so the whole chain is a rounding error.
//
// The important property is that it is *cached*. Build() is called only when
// the wallpaper changes, the dock moves or resizes, or the frost amount is
// edited - never per frame. That is what lets the dock refract a blurred
// backdrop while still costing nothing at idle.
class FrostChain {
public:
    FrostChain() = default;
    FrostChain(const FrostChain&) = delete;
    FrostChain& operator=(const FrostChain&) = delete;

    bool Initialize(GraphicsDevice& device, ShaderCache& shaders);

    // Drops every target and the constant buffer, for a device rebuild.
    void Reset();

    // Reallocates the intermediate targets. Safe to call with an unchanged size.
    bool Resize(UINT windowWidth, UINT windowHeight);

    // Runs the three passes. `windowOrigin` is the window's top-left in
    // monitor-relative pixels; `sigmaPx` is the blur radius in full-resolution
    // pixels.
    bool Build(const Backdrop& backdrop, POINT windowOrigin, SIZE windowSize, float sigmaPx);

    ID3D11ShaderResourceView* srv() const { return frost_.srv.Get(); }
    bool ready() const { return frost_.srv != nullptr; }

private:
    struct Target {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
        void Reset() {
            texture.Reset();
            rtv.Reset();
            srv.Reset();
        }
    };

    // Mirrors BlurConstants in shaders/Blur.hlsl.
    struct BlurConstants {
        float target[4];    // xy = target size, zw = 1 / target size
        float window[4];    // xy = window origin (monitor px), zw = window size
        float monitor[4];   // xy = monitor size, z = sigma (texels), w = tiled
        float backdropUv[4]; // xy = uv scale, zw = uv offset
        float direction[4]; // xy = per-tap uv step
    };
    static_assert(sizeof(BlurConstants) == 80, "BlurConstants must match the HLSL cbuffer");

    bool CreateTarget(Target& target, UINT width, UINT height);
    void RunPass(Target& destination, ID3D11ShaderResourceView* source, const char* entryPoint,
                 const BlurConstants& constants);

    GraphicsDevice* device_ = nullptr;
    ShaderCache* shaders_ = nullptr;
    ComPtr<ID3D11Buffer> constantBuffer_;
    ComPtr<ID3D11SamplerState> sampler_;

    Target quarter_;
    Target temp_;
    Target frost_;
    UINT width_ = 0;
    UINT height_ = 0;
};

} // namespace liquidock
