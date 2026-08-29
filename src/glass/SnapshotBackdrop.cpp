#include "glass/SnapshotBackdrop.h"

#include <algorithm>
#include <vector>

#include "core/Log.h"

namespace liquidock {

bool SnapshotBackdrop::Initialize(GraphicsDevice& device) {
    device_ = &device;
    return true;
}

void SnapshotBackdrop::Reset() {
    srv_.Reset();
    texture_.Reset();
    width_ = 0;
    height_ = 0;
}

bool SnapshotBackdrop::Capture(HMONITOR monitor, const RECT& screenRect) {
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) {
        return false;
    }
    monitorRect_ = info.rcMonitor;

    const int width = std::max<int>(1, screenRect.right - screenRect.left);
    const int height = std::max<int>(1, screenRect.bottom - screenRect.top);

    HDC screen = GetDC(nullptr);
    if (!screen) {
        return false;
    }
    HDC memory = CreateCompatibleDC(screen);
    if (!memory) {
        ReleaseDC(nullptr, screen);
        return false;
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    // Negative height for a top-down DIB, matching the row order D3D wants.
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap =
        CreateDIBSection(memory, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bitmap && bits) {
        HGDIOBJ previous = SelectObject(memory, bitmap);
        // CAPTUREBLT so layered windows are included. The window this is for is
        // not shown yet, so it cannot capture itself.
        ok = BitBlt(memory, 0, 0, width, height, screen, screenRect.left, screenRect.top,
                    SRCCOPY | CAPTUREBLT) != 0;
        SelectObject(memory, previous);
    }

    if (ok) {
        // BitBlt leaves the top byte of each pixel at zero. The glass samples
        // only rgb, but a texture full of zero alpha is a trap for anything that
        // later blends with it, so it is filled in here.
        auto* pixels = static_cast<uint32_t*>(bits);
        const size_t count = static_cast<size_t>(width) * height;
        for (size_t i = 0; i < count; ++i) {
            pixels[i] |= 0xFF000000u;
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initial{};
        initial.pSysMem = bits;
        initial.SysMemPitch = static_cast<UINT>(width) * 4;

        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        ok = SUCCEEDED(device_->d3d()->CreateTexture2D(&desc, &initial, &texture)) &&
             SUCCEEDED(device_->d3d()->CreateShaderResourceView(texture.Get(), nullptr, &view));
        if (ok) {
            texture_ = texture;
            srv_ = view;
            width_ = width;
            height_ = height;
        }
    }

    if (bitmap) {
        DeleteObject(bitmap);
    }
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);

    if (!ok) {
        LogWarn("Could not snapshot the screen behind the menu");
        return false;
    }

    // The shader works in monitor pixels: t = monitorPx / monitorSize, then
    // uv = t * scale + offset. Solving for uv = (monitorPx - origin) / regionSize
    // gives these - the same arithmetic the live capture uses, because both are
    // one crop of the screen standing in for the whole of it.
    const float monitorWidth = static_cast<float>(monitorRect_.right - monitorRect_.left);
    const float monitorHeight = static_cast<float>(monitorRect_.bottom - monitorRect_.top);
    const float originX = static_cast<float>(screenRect.left - monitorRect_.left);
    const float originY = static_cast<float>(screenRect.top - monitorRect_.top);
    uvScale_[0] = monitorWidth / static_cast<float>(width);
    uvScale_[1] = monitorHeight / static_cast<float>(height);
    uvOffset_[0] = -originX / static_cast<float>(width);
    uvOffset_[1] = -originY / static_cast<float>(height);
    return true;
}

} // namespace liquidock
