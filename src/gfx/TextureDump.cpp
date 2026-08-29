#include "gfx/TextureDump.h"

#include <windows.h>

#include <cstdio>
#include <vector>

#include "core/Log.h"

namespace liquidock {

bool DumpTextureToBmp(GraphicsDevice& device, ID3D11ShaderResourceView* view,
                      const std::wstring& path) {
    if (!view || path.empty()) {
        return false;
    }

    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture))) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.Width == 0 || desc.Height == 0) {
        LogWarn("Backdrop dump skipped: unexpected texture format");
        return false;
    }

    // The GPU cannot map a DEFAULT texture, so it goes through a staging copy.
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    staging.MipLevels = 1;
    staging.ArraySize = 1;

    ComPtr<ID3D11Texture2D> copy;
    if (FAILED(device.d3d()->CreateTexture2D(&staging, nullptr, &copy))) {
        return false;
    }
    device.context()->CopyResource(copy.Get(), texture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(device.context()->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }

    const int width = static_cast<int>(desc.Width);
    const int height = static_cast<int>(desc.Height);
    const size_t rowBytes = static_cast<size_t>(width) * 4;

    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4D42; // "BM"
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize =
        fileHeader.bfOffBits + static_cast<DWORD>(rowBytes * static_cast<size_t>(height));

    BITMAPINFOHEADER infoHeader{};
    infoHeader.biSize = sizeof(infoHeader);
    infoHeader.biWidth = width;
    // Negative height writes the rows top-down, which is the order the GPU has
    // them in; the alternative is flipping every row on the way out.
    infoHeader.biHeight = -height;
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = BI_RGB;

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) {
        device.context()->Unmap(copy.Get(), 0);
        return false;
    }

    fwrite(&fileHeader, sizeof(fileHeader), 1, file);
    fwrite(&infoHeader, sizeof(infoHeader), 1, file);
    // RowPitch is whatever the driver chose and is usually wider than the image,
    // so the rows are written one at a time rather than as one block.
    const auto* rows = static_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < height; ++y) {
        fwrite(rows + static_cast<size_t>(y) * mapped.RowPitch, 1, rowBytes, file);
    }
    fclose(file);

    device.context()->Unmap(copy.Get(), 0);
    LogInfo("Wrote a {}x{} backdrop dump", width, height);
    return true;
}

} // namespace liquidock
