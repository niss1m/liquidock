#include "gfx/ImageTexture.h"

#include <wincodec.h>

#include <vector>

#include "core/Check.h"
#include "core/Log.h"

namespace liquidock {

void ImageTexture::Reset() {
    srv_.Reset();
    texture_.Reset();
    path_.clear();
    width_ = 0;
    height_ = 0;
}

bool ImageTexture::Load(GraphicsDevice& device, const std::wstring& path) {
    Reset();
    if (path.empty()) {
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        LogWarn("No WIC factory; the image cannot be decoded");
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder))) {
        LogWarn("Could not decode the image named in the settings; ignoring it");
        return false;
    }

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
    LD_CHECK(
        converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data()));

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

    LD_CHECK(device.d3d()->CreateTexture2D(&desc, &initial, &texture_));
    LD_CHECK(device.d3d()->CreateShaderResourceView(texture_.Get(), nullptr, &srv_));

    path_ = path;
    width_ = static_cast<int>(width);
    height_ = static_cast<int>(height);
    LogInfo("Loaded image {}x{}", width_, height_);
    return true;
}

} // namespace liquidock
