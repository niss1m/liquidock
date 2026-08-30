#pragma once

#include <string>

#include "gfx/GraphicsDevice.h"

namespace liquidock {

// One image file on disk, decoded to a premultiplied BGRA texture.
//
// Deliberately general rather than a divider-specific loader: the icon pack
// picker in M4 needs exactly this, and so does any future theme asset. It
// decodes through WIC, which handles PNG, JPEG, BMP, GIF, TIFF and HEIF without
// us knowing which one we were handed - and it sniffs the container rather than
// trusting the extension, because a folder of user art always contains at least
// one file that is not what it is called.
class ImageTexture {
public:
    ImageTexture() = default;
    ImageTexture(const ImageTexture&) = delete;
    ImageTexture& operator=(const ImageTexture&) = delete;

    // Loads `path`, replacing whatever was held. An empty path, or a file that
    // will not decode, clears it and returns false - a bad path in the settings
    // should cost the user the image, not the dock.
    bool Load(GraphicsDevice& device, const std::wstring& path);

    void Reset();

    ID3D11ShaderResourceView* srv() const { return srv_.Get(); }
    bool ready() const { return srv_ != nullptr; }
    // What was successfully loaded, so a settings reload that names the same
    // file again does not decode it twice.
    const std::wstring& path() const { return path_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    ComPtr<ID3D11ShaderResourceView> srv_;
    ComPtr<ID3D11Texture2D> texture_;
    std::wstring path_;
    int width_ = 0;
    int height_ = 0;
};

} // namespace liquidock
