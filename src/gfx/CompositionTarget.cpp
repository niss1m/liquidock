#include "gfx/CompositionTarget.h"

#include "core/Check.h"
#include "core/Log.h"

namespace liquidock {

bool CompositionTarget::Initialize(GraphicsDevice& device, HWND hwnd, UINT width, UINT height) {
    device_ = &device;
    width_ = (width > 0) ? width : 1;
    height_ = (height > 0) ? height : 1;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    // Composition swap chains must declare premultiplied alpha; straight alpha
    // is not a legal option here, so every shader writes rgb pre-multiplied.
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    LD_CHECK(device_->factory()->CreateSwapChainForComposition(device_->d3d(), &desc, nullptr,
                                                               &swapChain_));

    ComPtr<IDXGISwapChain2> swapChain2;
    if (SUCCEEDED(swapChain_.As(&swapChain2))) {
        swapChain2->SetMaximumFrameLatency(1);
        waitable_ = swapChain2->GetFrameLatencyWaitableObject();
    }

    if (!CreateBuffers()) {
        return false;
    }

    LD_CHECK(device_->composition()->CreateTargetForHwnd(hwnd, TRUE, &target_));
    LD_CHECK(device_->composition()->CreateVisual(&visual_));
    LD_CHECK(visual_->SetContent(swapChain_.Get()));
    LD_CHECK(target_->SetRoot(visual_.Get()));
    if (!device_->Commit()) {
        return false;
    }

    LogInfo("Composition target ready ({}x{})", width_, height_);
    return true;
}

bool CompositionTarget::CreateBuffers() {
    rtv_.Reset();

    ComPtr<ID3D11Texture2D> backBuffer;
    LD_CHECK(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
    LD_CHECK(device_->d3d()->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv_));
    return true;
}

bool CompositionTarget::Resize(UINT width, UINT height) {
    width = (width > 0) ? width : 1;
    height = (height > 0) ? height : 1;
    if (width == width_ && height == height_) {
        return true;
    }

    // The context can still hold a reference to the old back buffer; the flip
    // model refuses to resize until every reference is released.
    device_->context()->OMSetRenderTargets(0, nullptr, nullptr);
    device_->context()->Flush();
    rtv_.Reset();

    LD_CHECK(swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
                                       DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT));
    width_ = width;
    height_ = height;
    return CreateBuffers();
}

ID3D11RenderTargetView* CompositionTarget::BeginFrame() {
    if (waitable_) {
        WaitForSingleObjectEx(waitable_, 1000, TRUE);
    }
    return rtv_.Get();
}

bool CompositionTarget::EndFrame() {
    DXGI_PRESENT_PARAMETERS params{};
    const HRESULT hr = swapChain_->Present1(1, 0, &params);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        LogError("Swap chain lost the device: {}", FormatHResult(hr));
        return false;
    }
    LD_CHECK(hr);
    return true;
}

} // namespace liquidock
