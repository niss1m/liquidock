#pragma once

#include <windows.h>
#include <magnification.h>

#include <d3d11_1.h>
#include <wrl/client.h>

#include <vector>

#include "glass/Backdrop.h"

namespace liquidock {

class GraphicsDevice;

// The live screen, read in a way that can leave the dock out of it.
//
// Desktop Duplication hands back the finished desktop with the dock already
// composited into it, so a dock refracting that image refracts its own last
// frame, and the feedback compounds every frame. The only lever Windows offers
// against that is SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE), which is
// all or nothing: it hides the window from *every* reader, so the price of a
// live backdrop was disappearing from the user's own screenshots and screen
// shares. That is a bad trade and it was never the point of the setting.
//
// The Magnification API is the one screen reader on Windows that takes a list
// of windows to leave out. MagSetWindowFilterList with MW_FILTERMODE_EXCLUDE
// names the dock, MagSetWindowSource then renders the region without it, and
// an image-scaling callback hands over the bits. No display affinity, so
// nothing else loses sight of the dock.
//
// The callback has been marked deprecated since Windows 8 and still works on
// 10 and 11; if it ever stops, this source reports failed() and the dock falls
// back to duplication, which is why that one is still here.
class MagnifierBackdrop : public Backdrop {
public:
    MagnifierBackdrop() = default;
    ~MagnifierBackdrop() override;
    MagnifierBackdrop(const MagnifierBackdrop&) = delete;
    MagnifierBackdrop& operator=(const MagnifierBackdrop&) = delete;

    // `excluded` are the windows to keep out of the image - the dock and
    // anything of ours that floats over it.
    bool Initialize(GraphicsDevice& device, std::vector<HWND> excluded);
    void Shutdown();

    bool Update(HMONITOR monitor) override;
    void SetRegion(const RECT& region) override;
    void SetActive(bool active) override;
    void Invalidate() override;

    bool failed() const override { return failed_; }
    ID3D11ShaderResourceView* srv() const override { return srv_.Get(); }
    void uv_scale(float out[2]) const override;
    void uv_offset(float out[2]) const override;
    bool tiled() const override { return false; }
    RECT monitor_rect() const override { return monitorRect_; }

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    bool EnsureHost();
    bool EnsureTexture(int width, int height);
    void ComputeUvTransform();

    // The magnifier answers on the thread that asked, into this. One reader at
    // a time by construction: Update is only ever called from the dock's own
    // thread.
    static BOOL CALLBACK Scaled(HWND window, void* source, MAGIMAGEHEADER header, void* dest,
                                MAGIMAGEHEADER destHeader, RECT unclipped, RECT clipped,
                                HRGN dirty);
    static MagnifierBackdrop* current_;

    GraphicsDevice* device_ = nullptr;
    std::vector<HWND> excluded_;

    HWND host_ = nullptr;
    HWND magnifier_ = nullptr;
    bool initialized_ = false;
    bool failed_ = false;
    bool active_ = true;

    RECT region_{};
    RECT monitorRect_{};
    bool regionMoved_ = true;

    // Where the callback puts what it was given, and what shape it is in.
    std::vector<uint8_t> pixels_;
    int pixelWidth_ = 0;
    int pixelHeight_ = 0;
    bool gotPixels_ = false;

    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> srv_;
    int textureWidth_ = 0;
    int textureHeight_ = 0;

    float uvScale_[2] = {1.0f, 1.0f};
    float uvOffset_[2] = {0.0f, 0.0f};
};

} // namespace liquidock
