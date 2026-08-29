#include "glass/CaptureBackdrop.h"

#include <algorithm>
#include <vector>

#include "core/Check.h"
#include "core/Log.h"

namespace liquidock {
namespace {

// How long the worker blocks in AcquireNextFrame before looking up to see
// whether it has been asked to stop.
//
// The call sleeps in the kernel and returns the moment the desktop changes, so
// this timeout only fires when the screen is completely static - two wakeups a
// second, each of which re-blocks immediately, on a mode the user opted into and
// only while the dock is actually on screen. The alternative is an infinite wait
// with no way to cancel it, which would hold up quitting the app indefinitely.
//
// It can be generous precisely because this thread has its own device: blocking
// here no longer blocks anything the dock is drawing.
constexpr DWORD kAcquireTimeoutMs = 500;

// How long to wait before retrying a duplication that could not be created.
// DuplicateOutput fails with E_ACCESSDENIED across a secure-desktop switch or a
// fullscreen-exclusive game, and both of those end on their own.
constexpr DWORD kRetryDelayMs = 500;

// How long either side waits for the shared texture. The copy is one small
// region and takes well under a millisecond, so this is a deadlock guard rather
// than a real wait.
constexpr DWORD kLockTimeoutMs = 40;

// The backdrop is refreshed at most this often.
//
// It is a heavily blurred image seen through glass; nobody can tell a 30 Hz
// blur from a 240 Hz one. Without the cap the dock repaints once per captured
// frame, and on a high-refresh monitor that is a lot of GPU for something no
// one can see - the sort of cost that never shows up in a benchmark and always
// shows up in a battery graph.
constexpr double kMinFrameIntervalMs = 33.0;

} // namespace

CaptureBackdrop::~CaptureBackdrop() {
    Shutdown();
}

bool CaptureBackdrop::Initialize(GraphicsDevice& device, HWND notify, UINT message) {
    device_ = &device;
    notify_ = notify;
    message_ = message;

    // Manual-reset both: the worker asks "are we running" and "should we stop"
    // as states, not as one-shot signals.
    resume_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!resume_ || !stopEvent_) {
        LogError("Could not create the capture events: {}", GetLastError());
        return false;
    }

    QueryPerformanceFrequency(&tickFrequency_);
    QueryPerformanceCounter(&lastFrameTick_);

    stop_ = false;
    worker_ = std::thread([this] { Run(); });
    LogInfo("Live screen capture started");
    return true;
}

void CaptureBackdrop::Shutdown() {
    stop_ = true;
    if (stopEvent_) {
        SetEvent(stopEvent_);
    }
    if (resume_) {
        SetEvent(resume_); // so a parked worker wakes up to see the stop
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    if (resume_) {
        CloseHandle(resume_);
        resume_ = nullptr;
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    duplication_.Reset();
    srv_.Reset();
    sharedLock_.Reset();
    sharedRegion_.Reset();
    regionLock_.Reset();
    region_.Reset();
    captureContext_.Reset();
    captureDevice_.Reset();
    if (sharedHandle_) {
        CloseHandle(sharedHandle_);
        sharedHandle_ = nullptr;
    }
}

bool CaptureBackdrop::BeginRead() {
    if (!sharedLock_) {
        return true; // nothing shared yet; the caller is using the wallpaper
    }
    return SUCCEEDED(sharedLock_->AcquireSync(0, kLockTimeoutMs));
}

void CaptureBackdrop::EndRead() {
    if (sharedLock_) {
        sharedLock_->ReleaseSync(0);
    }
}

void CaptureBackdrop::SetActive(bool active) {
    if (active_.exchange(active) == active) {
        return;
    }
    if (!resume_) {
        return;
    }
    if (active) {
        SetEvent(resume_);
    } else {
        ResetEvent(resume_);
    }
}

void CaptureBackdrop::SetRegion(const RECT& region) {
    // Clamped to the monitor so the copy box is always inside the desktop image.
    // The dock lives in the work area and the work area is inside the monitor,
    // so this only ever bites for the window's bleed at a screen edge.
    const int monitorWidth = monitorRect_.right - monitorRect_.left;
    const int monitorHeight = monitorRect_.bottom - monitorRect_.top;
    if (monitorWidth <= 0 || monitorHeight <= 0) {
        return;
    }

    RECT clamped;
    clamped.left = std::clamp<LONG>(region.left, 0, monitorWidth);
    clamped.top = std::clamp<LONG>(region.top, 0, monitorHeight);
    clamped.right = std::clamp<LONG>(region.right, clamped.left + 1, monitorWidth);
    clamped.bottom = std::clamp<LONG>(region.bottom, clamped.top + 1, monitorHeight);

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (EqualRect(&clamped, &region_rect_) && region_) {
            return;
        }
        region_rect_ = clamped;
    }

    if (!EnsureRegionTexture(clamped.right - clamped.left, clamped.bottom - clamped.top)) {
        return;
    }
    ComputeUvTransform();
}

bool CaptureBackdrop::EnsureRegionTexture(int width, int height) {
    if (region_ && width == regionWidth_ && height == regionHeight_) {
        return true;
    }
    if (!CreateCaptureDevice()) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    // The desktop image is BGRA and CopySubresourceRegion will not convert, so
    // this has to match it exactly.
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // Shared between the capture device that writes it and the render device
    // that samples it, with a keyed mutex so neither sees the other half-done.
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(captureDevice_->CreateTexture2D(&desc, nullptr, &texture))) {
        LogError("Could not allocate the shared capture texture");
        return false;
    }

    ComPtr<IDXGIResource1> resource;
    HANDLE handle = nullptr;
    if (FAILED(texture.As(&resource)) ||
        FAILED(resource->CreateSharedHandle(nullptr,
                                            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                            nullptr, &handle))) {
        LogError("Could not share the capture texture");
        return false;
    }

    ComPtr<ID3D11Texture2D> opened;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<IDXGIKeyedMutex> writeLock;
    ComPtr<IDXGIKeyedMutex> readLock;
    const bool ok = SUCCEEDED(device_->d3d()->OpenSharedResource1(handle, IID_PPV_ARGS(&opened))) &&
                    SUCCEEDED(device_->d3d()->CreateShaderResourceView(opened.Get(), nullptr,
                                                                       &view)) &&
                    SUCCEEDED(texture.As(&writeLock)) && SUCCEEDED(opened.As(&readLock));
    if (!ok) {
        LogError("Could not open the shared capture texture on the render device");
        CloseHandle(handle);
        return false;
    }

    if (sharedHandle_) {
        CloseHandle(sharedHandle_);
    }
    sharedHandle_ = handle;

    // Published under the lock: the worker copies into whatever it reads here,
    // and must never hold a pointer to a texture that has been replaced.
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        region_ = texture;
        regionLock_ = writeLock;
        regionWidth_ = width;
        regionHeight_ = height;
    }
    sharedRegion_ = opened;
    sharedLock_ = readLock;
    srv_ = view;
    // The old texture's contents are gone, so the next frame is the first one
    // again as far as the dock is concerned.
    frameReady_ = false;
    hasFrame_ = false;
    LogDebug("Capture region texture {}x{}", width, height);
    return true;
}

void CaptureBackdrop::ComputeUvTransform() {
    RECT region;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        region = region_rect_;
    }
    const float monitorWidth = static_cast<float>(monitorRect_.right - monitorRect_.left);
    const float monitorHeight = static_cast<float>(monitorRect_.bottom - monitorRect_.top);
    const float regionWidth = static_cast<float>(region.right - region.left);
    const float regionHeight = static_cast<float>(region.bottom - region.top);
    if (regionWidth <= 0.0f || regionHeight <= 0.0f) {
        return;
    }

    // The shader works in monitor pixels: t = monitorPx / monitorSize, then
    // uv = t * scale + offset. Solving that for uv = (monitorPx - origin) /
    // regionSize gives these, which is how a texture holding one small crop of
    // the screen slots into the same sampling path as a whole wallpaper.
    uvScale_[0] = monitorWidth / regionWidth;
    uvScale_[1] = monitorHeight / regionHeight;
    uvOffset_[0] = -static_cast<float>(region.left) / regionWidth;
    uvOffset_[1] = -static_cast<float>(region.top) / regionHeight;
}

bool CaptureBackdrop::Update(HMONITOR monitor) {
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) {
        return false;
    }

    if (monitor != monitor_ || !EqualRect(&monitorRect_, &info.rcMonitor)) {
        monitorRect_ = info.rcMonitor;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            monitor_ = monitor;
        }
        // The duplication is bound to one output; a different monitor needs a
        // different one.
        duplicationStale_ = true;
        ComputeUvTransform();
    }

    return frameReady_.exchange(false);
}

bool CaptureBackdrop::CreateCaptureDevice() {
    if (captureDevice_) {
        return true;
    }
    // The same adapter as the dock's device, because duplication only works for
    // an output on the adapter the device was created on - and because a shared
    // texture has to live on one adapter to be opened on the other device.
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(device_->dxgi()->GetAdapter(&adapter))) {
        return false;
    }

    static constexpr D3D_FEATURE_LEVEL kLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    const HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT, kLevels,
                                         ARRAYSIZE(kLevels), D3D11_SDK_VERSION, &captureDevice_,
                                         nullptr, &captureContext_);
    if (FAILED(hr)) {
        LogWarn("Could not create the capture device: {}", FormatHResult(hr));
        failed_ = true;
        return false;
    }
    return true;
}

bool CaptureBackdrop::CreateDuplication() {
    if (!CreateCaptureDevice()) {
        return false;
    }

    HMONITOR monitor = nullptr;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        monitor = monitor_;
    }
    if (!monitor || !device_) {
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(device_->dxgi()->GetAdapter(&adapter))) {
        return false;
    }

    // Duplication is only available for an output on the same adapter the device
    // was created on. On a hybrid laptop the dock's device can easily be on the
    // other one, which is a real reason for this mode to fail on a real machine
    // rather than a theoretical one.
    ComPtr<IDXGIOutput> match;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIOutput> output;
        if (adapter->EnumOutputs(i, &output) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_OUTPUT_DESC desc{};
        if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
            match = output;
            break;
        }
    }
    if (!match) {
        LogWarn("The dock's monitor is not on the graphics device's adapter; "
                "live capture is unavailable");
        failed_ = true;
        return false;
    }

    ComPtr<IDXGIOutput1> output1;
    if (FAILED(match.As(&output1))) {
        failed_ = true;
        return false;
    }

    const HRESULT hr = output1->DuplicateOutput(captureDevice_.Get(), &duplication_);
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_UNSUPPORTED) {
            // No duplication on this adapter at all. Retrying will not help.
            LogWarn("Desktop duplication is unsupported here; staying on the wallpaper backdrop");
            failed_ = true;
        } else {
            // E_ACCESSDENIED across a secure desktop or a fullscreen-exclusive
            // game. Both end by themselves, so this is worth retrying.
            LogDebug("DuplicateOutput deferred: {}", FormatHResult(hr));
        }
        duplication_.Reset();
        return false;
    }

    duplicationStale_ = false;
    LogInfo("Desktop duplication active");
    return true;
}

bool CaptureBackdrop::TouchesRegion(const DXGI_OUTDUPL_FRAME_INFO& info, const RECT& region) {
    if (info.TotalMetadataBufferSize == 0) {
        // No change metadata at all. Assume the worst rather than skipping a
        // frame that might have been the one that mattered.
        return true;
    }
    metadata_.resize(info.TotalMetadataBufferSize);

    UINT moveBytes = info.TotalMetadataBufferSize;
    if (FAILED(duplication_->GetFrameMoveRects(
            moveBytes, reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(metadata_.data()), &moveBytes))) {
        return true;
    }
    const auto* moves = reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT*>(metadata_.data());
    const UINT moveCount = moveBytes / sizeof(DXGI_OUTDUPL_MOVE_RECT);
    RECT scratch{};
    for (UINT i = 0; i < moveCount; ++i) {
        if (IntersectRect(&scratch, &region, &moves[i].DestinationRect)) {
            return true;
        }
    }

    UINT dirtyBytes = info.TotalMetadataBufferSize - moveBytes;
    if (FAILED(duplication_->GetFrameDirtyRects(
            dirtyBytes, reinterpret_cast<RECT*>(metadata_.data() + moveBytes), &dirtyBytes))) {
        return true;
    }
    const auto* dirty = reinterpret_cast<const RECT*>(metadata_.data() + moveBytes);
    const UINT dirtyCount = dirtyBytes / sizeof(RECT);
    for (UINT i = 0; i < dirtyCount; ++i) {
        if (IntersectRect(&scratch, &region, &dirty[i])) {
            return true;
        }
    }
    return false;
}

void CaptureBackdrop::Run() {
    while (!stop_.load(std::memory_order_relaxed)) {
        if (!active_.load(std::memory_order_relaxed)) {
            // Parked, not polling. While the dock is tucked away this thread
            // costs exactly nothing.
            duplication_.Reset();
            HANDLE events[2] = {stopEvent_, resume_};
            WaitForMultipleObjects(2, events, FALSE, INFINITE);
            continue;
        }

        if (duplicationStale_.exchange(false)) {
            duplication_.Reset();
        }
        if (!duplication_) {
            if (!CreateDuplication()) {
                if (failed_.load(std::memory_order_relaxed)) {
                    return; // nothing here will ever work; the dock falls back
                }
                WaitForSingleObject(stopEvent_, kRetryDelayMs);
                continue;
            }
        }

        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        const HRESULT hr = duplication_->AcquireNextFrame(kAcquireTimeoutMs, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue; // the screen simply did not change
        }
        acquired_.fetch_add(1, std::memory_order_relaxed);
        if (FAILED(hr)) {
            // ACCESS_LOST is the normal way a fullscreen app or a mode change
            // ends a duplication. Rebuild it on the next pass.
            LogDebug("Capture frame failed: {}", FormatHResult(hr));
            duplication_.Reset();
            continue;
        }

        // LastPresentTime of zero means only the mouse pointer moved. The
        // backdrop deliberately does not include the pointer, so there is
        // nothing to do - and this is the single most common frame there is.
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double sinceMs = 1000.0 *
                               static_cast<double>(now.QuadPart - lastFrameTick_.QuadPart) /
                               static_cast<double>(tickFrequency_.QuadPart);

        if (info.LastPresentTime.QuadPart == 0) {
            pointerOnly_.fetch_add(1, std::memory_order_relaxed);
        } else if (sinceMs < kMinFrameIntervalMs) {
            // Too soon. Dropped rather than queued: the next frame carries the
            // accumulated change anyway, so nothing is lost by skipping this one.
            throttled_.fetch_add(1, std::memory_order_relaxed);
        } else {
            lastFrameTick_ = now;
            RECT region{};
            ComPtr<ID3D11Texture2D> target;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                region = region_rect_;
                target = region_;
            }

            ComPtr<ID3D11Texture2D> desktop;
            if (target && SUCCEEDED(resource.As(&desktop)) && TouchesRegion(info, region)) {
                const D3D11_BOX box{
                    static_cast<UINT>(region.left),  static_cast<UINT>(region.top),    0,
                    static_cast<UINT>(region.right), static_cast<UINT>(region.bottom), 1,
                };
                // Entirely on this thread's own device and context. The only
                // thing shared with the render thread is the destination
                // texture, and the keyed mutex is what makes that safe.
                if (regionLock_ && SUCCEEDED(regionLock_->AcquireSync(0, kLockTimeoutMs))) {
                    captureContext_->CopySubresourceRegion(target.Get(), 0, 0, 0, 0,
                                                           desktop.Get(), 0, &box);
                    // Flushed so the copy is actually in flight before the lock
                    // is handed over; without it the render thread can take the
                    // texture before this device has submitted anything.
                    captureContext_->Flush();
                    regionLock_->ReleaseSync(0);

                    hasFrame_ = true;
                    frameReady_ = true;
                    copied_.fetch_add(1, std::memory_order_relaxed);
                    if (notify_) {
                        PostMessageW(notify_, message_, 0, 0);
                    }
                }
            }
        }

        duplication_->ReleaseFrame();
    }
}

} // namespace liquidock
