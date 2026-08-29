#pragma once

#include <d3d11_1.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

namespace liquidock {

using Microsoft::WRL::ComPtr;

// Owns the single D3D11 device shared by every window LiquiDock puts on screen
// (the dock and, later, the settings window), plus the DirectComposition device
// that composites them.
class GraphicsDevice {
public:
    GraphicsDevice() = default;
    GraphicsDevice(const GraphicsDevice&) = delete;
    GraphicsDevice& operator=(const GraphicsDevice&) = delete;

    bool Initialize();

    ID3D11Device1* d3d() const { return d3d_.Get(); }
    ID3D11DeviceContext1* context() const { return context_.Get(); }
    IDXGIDevice* dxgi() const { return dxgi_.Get(); }
    IDXGIFactory2* factory() const { return factory_.Get(); }
    IDCompositionDevice* composition() const { return composition_.Get(); }

    // Publishes every composition change made since the last call. Cheap, but
    // it is a cross-process call to DWM, so batch a frame's worth of visual
    // changes before committing rather than committing per visual.
    bool Commit();

    // True once the adapter has been removed or reset. Everything device-bound
    // has to be rebuilt; the caller decides when.
    bool IsDeviceLost() const;

private:
    ComPtr<ID3D11Device1> d3d_;
    ComPtr<ID3D11DeviceContext1> context_;
    ComPtr<IDXGIDevice> dxgi_;
    ComPtr<IDXGIFactory2> factory_;
    ComPtr<IDCompositionDevice> composition_;
    D3D_FEATURE_LEVEL featureLevel_ = D3D_FEATURE_LEVEL_11_0;
};

} // namespace liquidock
