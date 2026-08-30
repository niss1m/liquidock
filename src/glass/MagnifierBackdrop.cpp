#include "glass/MagnifierBackdrop.h"

#include <magnification.h>

#include <algorithm>
#include <cstring>

#include "core/Log.h"
#include "gfx/GraphicsDevice.h"

#pragma comment(lib, "Magnification.lib")

namespace liquidock {
namespace {

constexpr wchar_t kHostClass[] = L"LiquiDock.MagHost";

} // namespace

MagnifierBackdrop* MagnifierBackdrop::current_ = nullptr;

MagnifierBackdrop::~MagnifierBackdrop() {
    Shutdown();
}

BOOL CALLBACK MagnifierBackdrop::Scaled(HWND, void* source, MAGIMAGEHEADER header, void*,
                                        MAGIMAGEHEADER, RECT, RECT, HRGN) {
    MagnifierBackdrop* self = current_;
    if (!self || !source || header.width == 0 || header.height == 0) {
        return FALSE;
    }

    // Whatever the magnifier hands over, copied out before returning: the
    // buffer belongs to it and is only valid for the length of this call.
    const size_t stride = header.stride;
    const size_t bytes = stride * header.height;
    if (bytes == 0) {
        return FALSE;
    }
    self->pixels_.resize(bytes);
    std::memcpy(self->pixels_.data(), source, bytes);
    self->pixelWidth_ = static_cast<int>(header.width);
    self->pixelHeight_ = static_cast<int>(header.height);
    self->gotPixels_ = true;

    // TRUE says the scaling was handled, so the magnifier does not go on to
    // draw into its own window. Nothing is looking at that window.
    return TRUE;
}

bool MagnifierBackdrop::EnsureHost() {
    if (host_) {
        return true;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kHostClass;
    RegisterClassExW(&wc); // an existing class is fine

    // On screen, but layered at one part in 255 and click-through, so it is
    // there without being visible. Parking it off the working area was the
    // obvious thing and it is why nothing arrived: the magnifier renders when
    // its window paints, and a window at -32000 never does.
    host_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                                WS_EX_NOACTIVATE,
                            kHostClass, L"", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
    if (!host_) {
        LogWarn("Magnifier host window failed: {}", GetLastError());
        return false;
    }
    SetLayeredWindowAttributes(host_, 0, 1, LWA_ALPHA);
    ShowWindow(host_, SW_SHOWNA);

    magnifier_ = CreateWindowW(WC_MAGNIFIER, L"", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, host_,
                               nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!magnifier_) {
        LogWarn("Magnifier control failed: {}", GetLastError());
        DestroyWindow(host_);
        host_ = nullptr;
        return false;
    }

    // The host has to be in the list as well, or the magnifier reads its own
    // output back and we are straight back to the feedback loop.
    excluded_.push_back(host_);

    // The whole point. Without this the magnifier reads the desktop including
    // the dock, which is the feedback loop duplication has.
    if (!excluded_.empty()) {
        if (!MagSetWindowFilterList(magnifier_, MW_FILTERMODE_EXCLUDE,
                                    static_cast<int>(excluded_.size()), excluded_.data())) {
            LogWarn("Magnifier would not take an exclusion list: {}", GetLastError());
            return false;
        }
    }
    if (!MagSetImageScalingCallback(magnifier_, &MagnifierBackdrop::Scaled)) {
        // Deprecated since Windows 8 and still present; if it ever goes, this
        // is where we find out, and the dock falls back to duplication.
        LogWarn("Magnifier image callback unavailable: {}", GetLastError());
        return false;
    }
    return true;
}

bool MagnifierBackdrop::Initialize(GraphicsDevice& device, std::vector<HWND> excluded) {
    device_ = &device;
    excluded_ = std::move(excluded);

    if (!MagInitialize()) {
        LogWarn("MagInitialize failed: {}", GetLastError());
        failed_ = true;
        return false;
    }
    initialized_ = true;
    current_ = this;

    if (!EnsureHost()) {
        failed_ = true;
        return false;
    }
    LogInfo("Live backdrop reading the screen through the magnifier, dock excluded by name");
    return true;
}

void MagnifierBackdrop::Shutdown() {
    if (magnifier_) {
        MagSetImageScalingCallback(magnifier_, nullptr);
        DestroyWindow(magnifier_);
        magnifier_ = nullptr;
    }
    if (host_) {
        DestroyWindow(host_);
        host_ = nullptr;
    }
    if (initialized_) {
        MagUninitialize();
        initialized_ = false;
    }
    if (current_ == this) {
        current_ = nullptr;
    }
}

void MagnifierBackdrop::SetRegion(const RECT& region) {
    if (std::memcmp(&region, &region_, sizeof(RECT)) == 0) {
        return;
    }
    region_ = region;
    regionMoved_ = true;
}

void MagnifierBackdrop::SetActive(bool active) {
    active_ = active;
}

void MagnifierBackdrop::Invalidate() {
    regionMoved_ = true;
}

bool MagnifierBackdrop::EnsureTexture(int width, int height) {
    if (texture_ && width == textureWidth_ && height == textureHeight_) {
        return true;
    }
    srv_.Reset();
    texture_.Reset();
    textureWidth_ = 0;
    textureHeight_ = 0;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    // Written from the CPU every frame the screen changes, so dynamic with
    // write-discard: the alternative is a staging copy and an extra blit for a
    // texture measured in hundreds of kilobytes.
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(device_->d3d()->CreateTexture2D(&desc, nullptr, &texture_))) {
        LogWarn("Magnifier backdrop texture failed");
        return false;
    }
    if (FAILED(device_->d3d()->CreateShaderResourceView(texture_.Get(), nullptr, &srv_))) {
        LogWarn("Magnifier backdrop view failed");
        texture_.Reset();
        return false;
    }
    textureWidth_ = width;
    textureHeight_ = height;
    return true;
}

void MagnifierBackdrop::ComputeUvTransform() {
    const float monitorWidth = static_cast<float>(monitorRect_.right - monitorRect_.left);
    const float monitorHeight = static_cast<float>(monitorRect_.bottom - monitorRect_.top);
    if (monitorWidth <= 0.0f || monitorHeight <= 0.0f || textureWidth_ <= 0) {
        return;
    }
    // The texture holds only the dock's strip, so a point on the monitor maps
    // into it by scaling up and sliding the strip's origin to zero. Same
    // arithmetic the duplication source does, for the same reason.
    const float regionWidth = static_cast<float>(region_.right - region_.left);
    const float regionHeight = static_cast<float>(region_.bottom - region_.top);
    if (regionWidth <= 0.0f || regionHeight <= 0.0f) {
        return;
    }
    uvScale_[0] = monitorWidth / regionWidth;
    uvScale_[1] = monitorHeight / regionHeight;
    uvOffset_[0] = -static_cast<float>(region_.left) / regionWidth;
    uvOffset_[1] = -static_cast<float>(region_.top) / regionHeight;
}

bool MagnifierBackdrop::Update(HMONITOR monitor) {
    if (failed_ || !device_ || !monitor) {
        return false;
    }
    if (!active_) {
        return false; // tucked away; nothing to read and nobody to read it for
    }
    if (!EnsureHost()) {
        failed_ = true;
        return false;
    }

    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) {
        return false;
    }
    monitorRect_ = info.rcMonitor;

    const int width = static_cast<int>(region_.right - region_.left);
    const int height = static_cast<int>(region_.bottom - region_.top);
    if (width <= 0 || height <= 0) {
        return false;
    }

    // The magnifier renders one to one into a control the size of the source,
    // so the callback is handed the region at its own resolution. The host has
    // to be at least that big or the control is clipped to nothing.
    SetWindowPos(host_, HWND_BOTTOM, 0, 0, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
    SetWindowPos(magnifier_, nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE);

    const RECT source{monitorRect_.left + region_.left, monitorRect_.top + region_.top,
                      monitorRect_.left + region_.right, monitorRect_.top + region_.bottom};

    gotPixels_ = false;
    current_ = this;
    // Synchronous: the callback runs inside this call, before it returns.
    if (!MagSetWindowSource(magnifier_, source)) {
        LogWarn("MagSetWindowSource failed: {}", GetLastError());
        failed_ = true;
        return false;
    }
    // Setting the source marks the control dirty; the render - and with it the
    // callback - happens when it paints. UpdateWindow makes that happen now
    // rather than whenever the message loop next gets round to it.
    InvalidateRect(magnifier_, nullptr, FALSE);
    UpdateWindow(magnifier_);
    if (!gotPixels_ || pixelWidth_ <= 0 || pixelHeight_ <= 0) {
        return false;
    }

    if (!EnsureTexture(pixelWidth_, pixelHeight_)) {
        failed_ = true;
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(device_->context()->Map(texture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    const size_t sourceStride = pixels_.size() / static_cast<size_t>(pixelHeight_);
    const size_t row = std::min<size_t>(sourceStride, mapped.RowPitch);
    for (int y = 0; y < pixelHeight_; ++y) {
        std::memcpy(static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                    pixels_.data() + static_cast<size_t>(y) * sourceStride, row);
    }
    device_->context()->Unmap(texture_.Get(), 0);

    ComputeUvTransform();
    regionMoved_ = false;
    return true;
}

void MagnifierBackdrop::uv_scale(float out[2]) const {
    out[0] = uvScale_[0];
    out[1] = uvScale_[1];
}

void MagnifierBackdrop::uv_offset(float out[2]) const {
    out[0] = uvOffset_[0];
    out[1] = uvOffset_[1];
}

} // namespace liquidock
