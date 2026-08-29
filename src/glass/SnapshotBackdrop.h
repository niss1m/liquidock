#pragma once

#include <windows.h>

#include "gfx/GraphicsDevice.h"
#include "glass/Backdrop.h"

namespace liquidock {

// One frozen frame of whatever is on screen behind a given rectangle.
//
// For something transient - a context menu that lives for a second and a half -
// running a whole desktop duplication is far more machinery than the job needs,
// and pointing it at the wallpaper instead is worse than either: a menu that
// shows a photograph of the desert while sitting on top of a text editor reads
// as a bug, because it is not glass at all, it is a picture.
//
// So the region behind the window is copied once, with BitBlt, before the window
// is shown. Nothing has to be excluded from capture and there is no feedback
// loop to break, because at the moment of the copy the window is not on screen.
// It does not track changes behind it, which is exactly right for a menu: what
// was behind it when it opened is what a sheet of glass over that spot would
// show for as long as anyone is looking at it.
class SnapshotBackdrop : public Backdrop {
public:
    SnapshotBackdrop() = default;
    SnapshotBackdrop(const SnapshotBackdrop&) = delete;
    SnapshotBackdrop& operator=(const SnapshotBackdrop&) = delete;

    bool Initialize(GraphicsDevice& device);
    void Reset();

    // Grabs `screenRect` (in virtual-screen coordinates) from the desktop.
    bool Capture(HMONITOR monitor, const RECT& screenRect);

    bool Update(HMONITOR) override { return false; } // captured explicitly
    ID3D11ShaderResourceView* srv() const override { return srv_.Get(); }
    void uv_scale(float out[2]) const override { out[0] = uvScale_[0]; out[1] = uvScale_[1]; }
    void uv_offset(float out[2]) const override { out[0] = uvOffset_[0]; out[1] = uvOffset_[1]; }
    bool tiled() const override { return false; }
    RECT monitor_rect() const override { return monitorRect_; }

private:
    GraphicsDevice* device_ = nullptr;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> srv_;
    RECT monitorRect_{};
    int width_ = 0;
    int height_ = 0;
    float uvScale_[2] = {1.0f, 1.0f};
    float uvOffset_[2] = {0.0f, 0.0f};
};

} // namespace liquidock
