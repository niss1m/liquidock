#include "gfx/GraphicsDevice.h"

#include "core/Check.h"
#include "core/Log.h"

namespace liquidock {

bool GraphicsDevice::Initialize() {
    // BGRA support is required for the Direct2D interop the settings UI needs in
    // M4. SINGLETHREADED is deliberately not set: Windows.Graphics.Capture
    // touches the device from its own threads in M3, and a device created
    // single-threaded turns that into rare, near-undebuggable corruption.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

#ifdef LIQUIDOCK_DEBUG
    // Only ask for the debug layer if the Graphics Tools optional feature is
    // actually present; otherwise device creation fails outright on machines
    // that never installed it.
    {
        ComPtr<ID3D11Device> probe;
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                             flags | D3D11_CREATE_DEVICE_DEBUG, nullptr, 0,
                                             D3D11_SDK_VERSION, &probe, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            flags |= D3D11_CREATE_DEVICE_DEBUG;
            LogDebug("D3D11 debug layer enabled");
        }
    }
#endif

    // 11_0 is the floor, not 10_1: ShaderCache compiles vs_5_0/ps_5_0
    // exclusively, and those profiles cannot be created on a 10_1 device.
    // Accepting 10_1 here would produce a device that comes up cleanly and then
    // fails every shader creation, leaving a permanently blank dock.
    static constexpr D3D_FEATURE_LEVEL kLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, kLevels,
                                   ARRAYSIZE(kLevels), D3D11_SDK_VERSION, &device, &featureLevel_,
                                   &context);

    if (FAILED(hr)) {
        LogWarn("Hardware D3D11 device unavailable ({}), falling back to WARP",
                FormatHResult(hr));
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, kLevels,
                               ARRAYSIZE(kLevels), D3D11_SDK_VERSION, &device, &featureLevel_,
                               &context);
    }
    LD_CHECK(hr);

    LD_CHECK(device.As(&d3d_));
    LD_CHECK(context.As(&context_));
    LD_CHECK(d3d_.As(&dxgi_));

    // Frame latency is set per swap chain in CompositionTarget, not here:
    // IDXGIDevice1::SetMaximumFrameLatency has no effect on a swap chain
    // created with FRAME_LATENCY_WAITABLE_OBJECT, which is the only kind this
    // process creates.

    ComPtr<IDXGIAdapter> adapter;
    LD_CHECK(dxgi_->GetAdapter(&adapter));
    LD_CHECK(adapter->GetParent(IID_PPV_ARGS(&factory_)));

    LD_CHECK(DCompositionCreateDevice(dxgi_.Get(), IID_PPV_ARGS(&composition_)));

    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(adapter->GetDesc(&desc))) {
        char name[128]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr,
                            nullptr);
        LogInfo("Graphics device ready: {} (feature level {:x})", name,
                static_cast<unsigned>(featureLevel_));
    }
    return true;
}

bool GraphicsDevice::Commit() {
    LD_CHECK(composition_->Commit());
    return true;
}

bool GraphicsDevice::IsDeviceLost() const {
    if (!d3d_) {
        return true;
    }
    const HRESULT reason = d3d_->GetDeviceRemovedReason();
    return reason != S_OK;
}

} // namespace liquidock
