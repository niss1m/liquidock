#include "gfx/GraphicsDevice.h"

#include <string_view>
#include <vector>

#include "core/Check.h"
#include "core/Log.h"

namespace liquidock {

bool GraphicsDevice::Initialize() {
    // BGRA support is required for the Direct2D interop the settings UI needs in
    // M4. SINGLETHREADED is deliberately not set: the capture thread touches the
    // device context, and a device created single-threaded turns that into rare,
    // near-undebuggable corruption.
    //
    // Leaving that flag off is necessary but NOT sufficient, which cost an
    // afternoon: the runtime still does no locking until it is asked to, via
    // ID3D10Multithread below. Without that call the context is a free-for-all
    // and D3D happily corrupts itself.
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

    // The switch that makes the immediate context safe to touch from the
    // capture thread. D3D11 takes an internal critical section around every
    // context call once this is on; without it the debug layer reports "Two
    // threads were found to be executing functions associated with the same
    // Device[Context] at the same time", and release builds simply corrupt.
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(context_.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    } else {
        LogWarn("Could not enable D3D11 multithread protection; live capture will stay off");
        multithreadSafe_ = false;
    }

    LD_CHECK(DCompositionCreateDevice(dxgi_.Get(), IID_PPV_ARGS(&composition_)));

#ifdef LIQUIDOCK_DEBUG
    if (SUCCEEDED(d3d_.As(&infoQueue_))) {
        // The debug layer breaks into the debugger on a corruption or error
        // message by raising an exception, which without a debugger attached
        // simply kills the process with no explanation. Reading the messages
        // ourselves is strictly more useful than being killed by them.
        infoQueue_->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        infoQueue_->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, FALSE);
    }
#endif

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

void GraphicsDevice::DrainDebugMessages() {
#ifdef LIQUIDOCK_DEBUG
    if (!infoQueue_) {
        return;
    }
    const UINT64 count = infoQueue_->GetNumStoredMessages();
    std::vector<char> buffer;
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T length = 0;
        if (FAILED(infoQueue_->GetMessage(i, nullptr, &length)) || length == 0) {
            continue;
        }
        buffer.resize(length);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(buffer.data());
        if (FAILED(infoQueue_->GetMessage(i, message, &length))) {
            continue;
        }
        const std::string_view text(message->pDescription, message->DescriptionByteLength);
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
            LogWarn("D3D11: {}", text);
        } else {
            LogDebug("D3D11: {}", text);
        }
    }
    infoQueue_->ClearStoredMessages();
#endif
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
