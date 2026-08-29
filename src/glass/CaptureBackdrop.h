#pragma once

#include <windows.h>
#include <dxgi1_2.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "gfx/GraphicsDevice.h"
#include "glass/Backdrop.h"

namespace liquidock {

// The live screen behind the dock, via DXGI desktop duplication.
//
// Windows.Graphics.Capture is the more modern API and was the original plan,
// but on Windows 11 a WGC session draws a yellow border around whatever it is
// capturing unless the app has been granted borderless access - and a permanent
// yellow rectangle around the monitor is not something a dock can ship.
// Duplication has no border, needs no permission, is plain D3D11/DXGI like the
// rest of this codebase, and hands back dirty rectangles, which is what makes
// the whole thing affordable.
//
// Three properties make this cheap enough to live with:
//
//  - It copies the dock's footprint, not the monitor. The glass never samples
//    further than the refraction offset outside its own bar, and the window
//    already carries far more bleed than that.
//  - It ignores changes that miss that footprint. A video playing in the corner
//    of the screen produces a frame every 16 ms and none of them reach the dock,
//    so the dock never wakes.
//  - It stops completely while the dock is hidden, parked on an event rather
//    than on a timeout.
//
// The dock's own window must be excluded from capture by the caller, or it
// refracts its own previous frame into an infinite mirror. That exclusion is
// also why this is not the default: it hides the dock from the user's own
// screenshots and screen shares.
class CaptureBackdrop : public Backdrop {
public:
    CaptureBackdrop() = default;
    CaptureBackdrop(const CaptureBackdrop&) = delete;
    CaptureBackdrop& operator=(const CaptureBackdrop&) = delete;
    ~CaptureBackdrop() override;

    // `notify` is posted `message` whenever a frame lands that actually touches
    // the dock's region.
    bool Initialize(GraphicsDevice& device, HWND notify, UINT message);
    void Shutdown();

    bool Update(HMONITOR monitor) override;

    // The capture texture is shared between two devices, so the render thread
    // has to take the keyed mutex around every frame that samples it. Begin
    // returns false if it could not, in which case the frame is skipped rather
    // than drawn from a half-written texture.
    bool BeginRead();
    void EndRead();
    void SetRegion(const RECT& region) override;
    void SetActive(bool active) override;
    bool failed() const override { return failed_.load(std::memory_order_relaxed); }

    // True once a frame has actually landed. Until then the dock keeps showing
    // the wallpaper, so switching modes never flashes an empty backdrop.
    bool ready() const { return hasFrame_.load(std::memory_order_relaxed) && srv_ != nullptr; }

    // Counters for --stats. Acquired is every frame duplication handed over,
    // copied is the subset that actually touched the dock. The gap between them
    // is the whole argument for filtering on dirty rectangles.
    unsigned acquired() const { return acquired_.load(std::memory_order_relaxed); }
    unsigned copied() const { return copied_.load(std::memory_order_relaxed); }
    unsigned pointerOnly() const { return pointerOnly_.load(std::memory_order_relaxed); }
    unsigned throttled() const { return throttled_.load(std::memory_order_relaxed); }

    ID3D11ShaderResourceView* srv() const override { return srv_.Get(); }
    void uv_scale(float out[2]) const override { out[0] = uvScale_[0]; out[1] = uvScale_[1]; }
    void uv_offset(float out[2]) const override { out[0] = uvOffset_[0]; out[1] = uvOffset_[1]; }
    bool tiled() const override { return false; }
    RECT monitor_rect() const override { return monitorRect_; }

private:
    void Run();
    bool CreateCaptureDevice();
    bool CreateDuplication();
    bool EnsureRegionTexture(int width, int height);
    // True when any of the frame's dirty or move rectangles overlaps the dock.
    bool TouchesRegion(const DXGI_OUTDUPL_FRAME_INFO& info, const RECT& region);
    void ComputeUvTransform();

    GraphicsDevice* device_ = nullptr;
    HWND notify_ = nullptr;
    UINT message_ = 0;

    // Desktop duplication gets a device of its own rather than borrowing the
    // dock's. Sharing one was the obvious thing and it was wrong: duplication
    // does substantial work inside the device, and with the immediate context
    // serialised between the two threads the dock's own frames queued behind it
    // - measured at three hundred milliseconds a frame while idle. The two
    // devices now meet only at a shared texture with a keyed mutex, which is
    // sub-millisecond.
    ComPtr<ID3D11Device> captureDevice_;
    ComPtr<ID3D11DeviceContext> captureContext_;
    ComPtr<IDXGIOutputDuplication> duplication_;

    // Written by the capture device, sampled by the render device. Two views of
    // one allocation, each with its own handle to the same keyed mutex.
    ComPtr<ID3D11Texture2D> region_;         // capture side
    ComPtr<IDXGIKeyedMutex> regionLock_;     // capture side
    ComPtr<ID3D11Texture2D> sharedRegion_;   // render side
    ComPtr<IDXGIKeyedMutex> sharedLock_;     // render side
    HANDLE sharedHandle_ = nullptr;
    ComPtr<ID3D11ShaderResourceView> srv_;

    std::thread worker_;
    // Set under `mutex_` and read by the worker: the dock can move, and a copy
    // box computed from a stale rectangle reads the wrong part of the screen.
    std::mutex mutex_;
    RECT region_rect_{};
    HMONITOR monitor_ = nullptr;
    RECT monitorRect_{};
    int regionWidth_ = 0;
    int regionHeight_ = 0;

    // Both manual-reset: the worker asks these as states, not as one-shot
    // signals. `resume_` is set while the dock is on screen; `stopEvent_` is set
    // once, at shutdown, and is what lets every wait in the worker be cancelled.
    HANDLE resume_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> failed_{false};
    // Set by the worker when a frame has been copied, cleared by the render
    // thread when it has acted on it.
    std::atomic<bool> frameReady_{false};
    std::atomic<bool> hasFrame_{false};
    // The dock moved to another monitor, so the worker has to rebuild its
    // duplication against a different output.
    std::atomic<bool> duplicationStale_{false};
    std::atomic<unsigned> acquired_{0};
    std::atomic<unsigned> copied_{0};
    std::atomic<unsigned> pointerOnly_{0};
    std::atomic<unsigned> throttled_{0};

    // Reused across frames rather than reallocated: this is touched on every
    // desktop change, which on a busy screen is sixty times a second.
    std::vector<uint8_t> metadata_;
    // When the last frame was actually taken, for the refresh cap below.
    LARGE_INTEGER lastFrameTick_{};
    LARGE_INTEGER tickFrequency_{};

    float uvScale_[2] = {1.0f, 1.0f};
    float uvOffset_[2] = {0.0f, 0.0f};
};

} // namespace liquidock
