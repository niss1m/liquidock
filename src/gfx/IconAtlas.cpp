#include "gfx/IconAtlas.h"

#include <algorithm>

#include "core/Check.h"
#include "core/Log.h"

namespace liquidock {

bool IconAtlas::Initialize(GraphicsDevice& device, int cell, int capacity) {
    device_ = &device;
    cell_ = std::max(cell, 16);
    columns_ = std::min(capacity, 8);
    rows_ = (capacity + columns_ - 1) / columns_;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width());
    desc.Height = static_cast<UINT>(height());
    desc.MipLevels = 0; // full chain; the runtime works out how many that is
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    // RENDER_TARGET is not for drawing into: GenerateMips downsamples through
    // the render pipeline and refuses a texture that cannot be bound as one.
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    LD_CHECK(device_->d3d()->CreateTexture2D(&desc, nullptr, &texture_));
    LD_CHECK(device_->d3d()->CreateShaderResourceView(texture_.Get(), nullptr, &srv_));

    LogInfo("Icon atlas {}x{} ({} px cells, {} slots)", width(), height(), cell_,
            columns_ * rows_);
    return true;
}

bool IconAtlas::Upload(int slot, const std::vector<uint32_t>& pixels) {
    if (!texture_ || slot < 0 || slot >= columns_ * rows_) {
        return false;
    }
    if (pixels.size() != static_cast<size_t>(cell_) * cell_) {
        LogWarn("Icon for slot {} is the wrong size; ignoring it", slot);
        return false;
    }

    // Mip 0 only. The rest of the chain is derived, and writing into it by hand
    // would be overwritten by the next GenerateMips anyway.
    const D3D11_BOX box{
        static_cast<UINT>((slot % columns_) * cell_),
        static_cast<UINT>((slot / columns_) * cell_),
        0,
        static_cast<UINT>((slot % columns_) * cell_ + cell_),
        static_cast<UINT>((slot / columns_) * cell_ + cell_),
        1,
    };
    device_->context()->UpdateSubresource(texture_.Get(), 0, &box, pixels.data(),
                                          static_cast<UINT>(cell_ * sizeof(uint32_t)), 0);
    dirty_ = true;
    return true;
}

void IconAtlas::FinishUpdates() {
    if (!dirty_ || !srv_) {
        return;
    }
    device_->context()->GenerateMips(srv_.Get());
    dirty_ = false;
}

} // namespace liquidock
