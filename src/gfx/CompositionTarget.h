#pragma once

#include <windows.h>

#include "gfx/GraphicsDevice.h"

namespace liquidock {

// Binds one HWND to a DirectComposition visual backed by a flip-model swap
// chain with per-pixel alpha.
//
// This is the modern replacement for UpdateLayeredWindow: the window is created
// with WS_EX_NOREDIRECTIONBITMAP so DWM never allocates a redirection surface,
// and everything we draw is composited straight from the GPU with no CPU blit
// on the path. It is the reason the dock can hold refresh rate while refracting
// a live backdrop.
class CompositionTarget {
public:
    CompositionTarget() = default;
    CompositionTarget(const CompositionTarget&) = delete;
    CompositionTarget& operator=(const CompositionTarget&) = delete;
    ~CompositionTarget() { Reset(); }

    bool Initialize(GraphicsDevice& device, HWND hwnd, UINT width, UINT height);
    bool Resize(UINT width, UINT height);

    // Blocks until the composition engine is ready for another frame, then
    // returns the back buffer view. Returns nullptr if the frame should be
    // skipped, in which case no wait was consumed.
    //
    // Every successful BeginFrame MUST be followed by EndFrame. The wait takes
    // a slot from the frame-latency semaphore and only Present gives it back;
    // bailing out in between stalls the next frame for a full timeout.
    ID3D11RenderTargetView* BeginFrame();
    bool EndFrame();

    // Releases the swap chain, the composition tree and the waitable handle.
    void Reset();

    UINT width() const { return width_; }
    UINT height() const { return height_; }

    // Signalled when the swap chain can accept another frame. The animation
    // clock waits on this rather than on a timer.
    HANDLE frame_latency_waitable() const { return waitable_; }

private:
    bool CreateBuffers();

    GraphicsDevice* device_ = nullptr;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<IDCompositionTarget> target_;
    ComPtr<IDCompositionVisual> visual_;
    HANDLE waitable_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
};

} // namespace liquidock
