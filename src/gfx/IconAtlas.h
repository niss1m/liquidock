#pragma once

#include <cstdint>
#include <vector>

#include "gfx/GraphicsDevice.h"

namespace liquidock {

// Every dock icon in one texture, laid out on a grid.
//
// One texture means one shader resource binding and one draw call for the whole
// icon row, however many items there are. The alternative - a texture per icon -
// would cost a bind and a draw each, which is the shape of thing that turns a
// dock into a frame-rate problem when someone pins thirty apps.
//
// The atlas is mipmapped. Icons are stored at the size a fully magnified icon
// needs, so the resting size is a minification, and without mips that would
// shimmer visibly as the magnification wave moves across it.
class IconAtlas {
public:
    IconAtlas() = default;
    IconAtlas(const IconAtlas&) = delete;
    IconAtlas& operator=(const IconAtlas&) = delete;

    // `cell` is the edge of one icon in pixels; `capacity` how many fit.
    bool Initialize(GraphicsDevice& device, int cell, int capacity);

    // Drops the texture. The icons themselves are re-extracted afterwards, so
    // there is nothing here worth trying to preserve across a device loss.
    void Reset();

    // Copies one icon into its cell. `pixels` must be cell*cell premultiplied
    // BGRA. Mips are regenerated, so batch a frame's worth of icons and call
    // FinishUpdates once rather than calling this in a loop.
    bool Upload(int slot, const std::vector<uint32_t>& pixels);

    // Regenerates the mip chain after one or more Upload calls.
    void FinishUpdates();

    ID3D11ShaderResourceView* srv() const { return srv_.Get(); }
    int cell() const { return cell_; }
    int columns() const { return columns_; }
    int rows() const { return rows_; }
    int width() const { return columns_ * cell_; }
    int height() const { return rows_ * cell_; }

private:
    GraphicsDevice* device_ = nullptr;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> srv_;
    int cell_ = 0;
    int columns_ = 0;
    int rows_ = 0;
    bool dirty_ = false;
};

} // namespace liquidock
