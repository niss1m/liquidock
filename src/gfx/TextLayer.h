#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <string>
#include <string_view>

#include "gfx/GraphicsDevice.h"

namespace liquidock {

// Direct2D and DirectWrite drawing onto a composition swap chain the rest of
// the frame was rendered into with D3D.
//
// The dock is drawn with shaders because that is what a shader is for, but a
// line of text is not - and rasterising glyphs by hand to avoid mixing the two
// APIs would be a great deal of work to arrive somewhere worse than DirectWrite
// already is. D2D wraps the same back buffer as a bitmap, so this composites on
// top of the finished frame with no extra surface and no extra present.
//
// Everything is stated in logical pixels; Begin applies the DPI scale once.
class TextLayer {
public:
    TextLayer() = default;
    TextLayer(const TextLayer&) = delete;
    TextLayer& operator=(const TextLayer&) = delete;

    bool Initialize(GraphicsDevice& device, float fontSize);
    void Reset();

    // Drops the cached target bitmap. Call when the swap chain is resized: the
    // bitmap wraps a buffer that no longer exists.
    void Invalidate() { target_.Reset(); }

    bool ready() const { return d2d_ != nullptr; }

    // Wraps the swap chain's back buffer and opens a drawing batch. Every
    // successful Begin must be matched by an End.
    bool Begin(IDXGISwapChain1* swapChain, float scale);
    void End();

    // Width of `text` in logical pixels, for sizing something around it.
    float MeasureWidth(std::wstring_view text);

    void FillRounded(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& colour);
    void StrokeRounded(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& colour,
                       float width = 1.0f);
    void Draw(std::wstring_view text, const D2D1_RECT_F& rect, const D2D1_COLOR_F& colour);

    // Centred by default, which is what a label wants; a menu wants its items
    // left-aligned against a common edge.
    void SetAlignment(DWRITE_TEXT_ALIGNMENT alignment);

private:
    GraphicsDevice* device_ = nullptr;
    ComPtr<ID2D1Factory1> factory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> d2d_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IDWriteTextFormat> format_;
    bool drawing_ = false;
};

} // namespace liquidock
