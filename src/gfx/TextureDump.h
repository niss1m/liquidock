#pragma once

#include <d3d11_1.h>

#include <string>

#include "gfx/GraphicsDevice.h"

namespace liquidock {

// Writes whatever a shader resource view points at to a 32-bit BMP.
//
// This exists because live capture mode excludes the dock from screen capture,
// which means the one thing you cannot do while debugging it is take a
// screenshot of the dock. Without a way to look at the texture directly there
// is no way to tell a correct capture from one that is reading the wrong region,
// upside down, or of the dock itself.
//
// BMP rather than PNG: the header is fourteen bytes plus forty, the pixel order
// is already the BGRA the GPU handed over, and it saves pulling WIC into a
// debugging path.
bool DumpTextureToBmp(GraphicsDevice& device, ID3D11ShaderResourceView* view,
                      const std::wstring& path);

} // namespace liquidock
