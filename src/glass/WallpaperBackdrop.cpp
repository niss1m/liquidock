#include "glass/WallpaperBackdrop.h"

#include <shlobj_core.h>
#include <shobjidl_core.h>
#include <wincodec.h>

#include <algorithm>
#include <vector>

#include "core/Check.h"
#include "core/Log.h"

namespace liquidock {
namespace {

// Mirrors DESKTOP_WALLPAPER_POSITION so the arithmetic below reads clearly.
enum WallpaperPosition {
    kCenter = 0,
    kTile = 1,
    kStretch = 2,
    kFit = 3,
    kFill = 4,
    kSpan = 5,
};

const char* PositionName(int position) {
    switch (position) {
        case kCenter:  return "center";
        case kTile:    return "tile";
        case kStretch: return "stretch";
        case kFit:     return "fit";
        case kFill:    return "fill";
        case kSpan:    return "span";
    }
    return "unknown";
}

} // namespace

bool WallpaperBackdrop::Initialize(GraphicsDevice& device) {
    device_ = &device;
    return true;
}

void WallpaperBackdrop::Reset() {
    srv_.Reset();
    texture_.Reset();
    loadedKey_.clear();
}

bool WallpaperBackdrop::Update(HMONITOR monitor) {
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) {
        return false;
    }

    // Everything below this line is a COM activation plus several cross-process
    // calls into the shell, and it used to run on every single frame the dock
    // drew - about nine milliseconds of it, which is most of a frame at 100 Hz
    // and exactly the cursor lag it caused.
    //
    // The wallpaper cannot change without the SPI_SETDESKWALLPAPER broadcast
    // that calls Invalidate(), so once it is loaded there is nothing left to
    // ask anybody.
    if (srv_ && !loadedKey_.empty() && EqualRect(&monitorRect_, &info.rcMonitor)) {
        return false;
    }
    monitorRect_ = info.rcMonitor;
    const int monitorWidth = monitorRect_.right - monitorRect_.left;
    const int monitorHeight = monitorRect_.bottom - monitorRect_.top;

    ComPtr<IDesktopWallpaper> wallpaper;
    if (FAILED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&wallpaper)))) {
        LogWarn("IDesktopWallpaper unavailable; falling back to a solid backdrop");
        return CreateSolidColour(GetSysColor(COLOR_DESKTOP));
    }

    // IDesktopWallpaper is keyed by monitor device path, not HMONITOR. Matching
    // on the monitor rectangle is the documented way across.
    std::wstring monitorId;
    UINT count = 0;
    if (SUCCEEDED(wallpaper->GetMonitorDevicePathCount(&count))) {
        for (UINT i = 0; i < count; ++i) {
            LPWSTR path = nullptr;
            if (FAILED(wallpaper->GetMonitorDevicePathAt(i, &path)) || !path) {
                continue;
            }
            RECT rect{};
            if (SUCCEEDED(wallpaper->GetMonitorRECT(path, &rect)) &&
                rect.left == monitorRect_.left && rect.top == monitorRect_.top) {
                monitorId = path;
                CoTaskMemFree(path);
                break;
            }
            CoTaskMemFree(path);
        }
    }

    int position = kFill;
    DESKTOP_WALLPAPER_POSITION dwPosition{};
    if (SUCCEEDED(wallpaper->GetPosition(&dwPosition))) {
        position = static_cast<int>(dwPosition);
    }

    std::wstring path;
    LPWSTR raw = nullptr;
    if (SUCCEEDED(wallpaper->GetWallpaper(monitorId.empty() ? nullptr : monitorId.c_str(), &raw)) &&
        raw) {
        path = raw;
        CoTaskMemFree(raw);
    }

    if (path.empty()) {
        COLORREF colour = GetSysColor(COLOR_DESKTOP);
        wallpaper->GetBackgroundColor(&colour);
        const std::wstring key = L"#solid:" + std::to_wstring(colour);
        if (key == loadedKey_) {
            return false;
        }
        loadedKey_ = key;
        return CreateSolidColour(colour);
    }

    // Position and monitor size participate in the key: the same file at a
    // different fit or on a different-sized monitor needs a new UV transform.
    const std::wstring key = path + L"|" + std::to_wstring(position) + L"|" +
                             std::to_wstring(monitorWidth) + L"x" + std::to_wstring(monitorHeight);
    if (key == loadedKey_) {
        return false;
    }

    if (!LoadImage(path)) {
        return CreateSolidColour(GetSysColor(COLOR_DESKTOP));
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture_->GetDesc(&desc);
    ComputeUvTransform(static_cast<int>(desc.Width), static_cast<int>(desc.Height), monitorWidth,
                       monitorHeight, position);

    loadedKey_ = key;

    char utf8Path[512]{};
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8Path, sizeof(utf8Path) - 1, nullptr,
                        nullptr);
    LogInfo("Backdrop: {}x{} wallpaper, {} on a {}x{} monitor, uv scale ({:.3f},{:.3f}) offset "
            "({:.3f},{:.3f})",
            desc.Width, desc.Height, PositionName(position), monitorWidth, monitorHeight,
            uvScale_[0], uvScale_[1], uvOffset_[0], uvOffset_[1]);
    LogInfo("Backdrop source: {}", utf8Path);
    return true;
}

void WallpaperBackdrop::ComputeUvTransform(int imageWidth, int imageHeight, int monitorWidth,
                                           int monitorHeight, int position) {
    tiled_ = false;
    uvScale_[0] = 1.0f;
    uvScale_[1] = 1.0f;
    uvOffset_[0] = 0.0f;
    uvOffset_[1] = 0.0f;

    if (imageWidth <= 0 || imageHeight <= 0 || monitorWidth <= 0 || monitorHeight <= 0) {
        return;
    }

    const float iw = static_cast<float>(imageWidth);
    const float ih = static_cast<float>(imageHeight);
    const float mw = static_cast<float>(monitorWidth);
    const float mh = static_cast<float>(monitorHeight);

    // Each mode reduces to "how many image pixels does the monitor span, and
    // where does that span start". Expressed as a UV scale and offset, the
    // shader needs no branches beyond the tile case.
    float scale = 1.0f;
    switch (position) {
        case kStretch:
            uvScale_[0] = 1.0f;
            uvScale_[1] = 1.0f;
            return;

        case kTile:
            tiled_ = true;
            uvScale_[0] = mw / iw;
            uvScale_[1] = mh / ih;
            return;

        case kCenter:
            scale = 1.0f;
            break;

        case kFit:
            scale = std::min(mw / iw, mh / ih);
            break;

        case kSpan:
            // TODO(M3): span stretches one image across the whole virtual
            // desktop, so it needs the virtual bounds rather than this
            // monitor's. Treated as fill until multi-monitor lands.
        case kFill:
        default:
            scale = std::max(mw / iw, mh / ih);
            break;
    }

    uvScale_[0] = (mw / scale) / iw;
    uvScale_[1] = (mh / scale) / ih;
    uvOffset_[0] = (1.0f - uvScale_[0]) * 0.5f;
    uvOffset_[1] = (1.0f - uvScale_[1]) * 0.5f;
}

bool WallpaperBackdrop::LoadImage(const std::wstring& path) {
    ComPtr<IWICImagingFactory> factory;
    LD_CHECK(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory)));

    ComPtr<IWICBitmapDecoder> decoder;
    LD_CHECK(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnDemand, &decoder));

    ComPtr<IWICBitmapFrameDecode> frame;
    LD_CHECK(decoder->GetFrame(0, &frame));

    // Premultiplied BGRA matches the swap chain and the blend maths downstream.
    ComPtr<IWICFormatConverter> converter;
    LD_CHECK(factory->CreateFormatConverter(&converter));
    LD_CHECK(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom));

    UINT width = 0;
    UINT height = 0;
    LD_CHECK(converter->GetSize(&width, &height));
    if (width == 0 || height == 0) {
        return false;
    }

    const UINT stride = width * 4;
    std::vector<BYTE> pixels(static_cast<size_t>(stride) * height);
    LD_CHECK(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()),
                                   pixels.data()));

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = stride;

    texture_.Reset();
    srv_.Reset();
    LD_CHECK(device_->d3d()->CreateTexture2D(&desc, &initial, &texture_));
    LD_CHECK(device_->d3d()->CreateShaderResourceView(texture_.Get(), nullptr, &srv_));
    return true;
}

bool WallpaperBackdrop::CreateSolidColour(COLORREF colour) {
    const BYTE pixel[4] = {GetBValue(colour), GetGValue(colour), GetRValue(colour), 0xFF};

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixel;
    initial.SysMemPitch = sizeof(pixel);

    texture_.Reset();
    srv_.Reset();
    LD_CHECK(device_->d3d()->CreateTexture2D(&desc, &initial, &texture_));
    LD_CHECK(device_->d3d()->CreateShaderResourceView(texture_.Get(), nullptr, &srv_));

    uvScale_[0] = 1.0f;
    uvScale_[1] = 1.0f;
    uvOffset_[0] = 0.0f;
    uvOffset_[1] = 0.0f;
    tiled_ = false;
    return true;
}

} // namespace liquidock
