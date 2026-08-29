#include "gfx/TextLayer.h"

#include "core/Check.h"
#include "core/Log.h"

namespace liquidock {

bool TextLayer::Initialize(GraphicsDevice& device, float fontSize) {
    Reset();
    device_ = &device;

    D2D1_FACTORY_OPTIONS options{};
#ifdef LIQUIDOCK_DEBUG
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    LD_CHECK(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                               reinterpret_cast<void**>(factory_.GetAddressOf())));
    LD_CHECK(factory_->CreateDevice(device_->dxgi(), &d2dDevice_));
    LD_CHECK(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_));
    LD_CHECK(d2d_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush_));

    LD_CHECK(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())));
    // Segoe UI Variable on Windows 11, with DirectWrite falling back on its own
    // where it is missing.
    LD_CHECK(dwrite_->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
                                       DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, fontSize, L"en-us", &format_));
    format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return true;
}

void TextLayer::Reset() {
    if (drawing_ && d2d_) {
        d2d_->EndDraw();
        drawing_ = false;
    }
    target_.Reset();
    format_.Reset();
    dwrite_.Reset();
    brush_.Reset();
    d2d_.Reset();
    d2dDevice_.Reset();
    factory_.Reset();
}

bool TextLayer::Begin(IDXGISwapChain1* swapChain, float scale) {
    if (!d2d_ || !swapChain || drawing_) {
        return false;
    }

    if (!target_) {
        ComPtr<IDXGISurface> surface;
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&surface)))) {
            return false;
        }
        // CANNOT_DRAW is deliberate: D2D must not clear or otherwise take
        // ownership of a buffer that already holds the finished frame.
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f,
            96.0f);
        if (FAILED(d2d_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &target_))) {
            return false;
        }
    }

    d2d_->SetTarget(target_.Get());
    d2d_->BeginDraw();
    d2d_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    drawing_ = true;
    return true;
}

void TextLayer::End() {
    if (!drawing_) {
        return;
    }
    d2d_->SetTransform(D2D1::Matrix3x2F::Identity());
    const HRESULT hr = d2d_->EndDraw();
    d2d_->SetTarget(nullptr);
    drawing_ = false;
    if (FAILED(hr)) {
        // Usually the device going away underneath us. Drop the bitmap; the
        // dock's own recovery path rebuilds everything else.
        LogWarn("Text layer draw failed: {}", FormatHResult(hr));
        target_.Reset();
    }
}

void TextLayer::SetAlignment(DWRITE_TEXT_ALIGNMENT alignment) {
    if (format_) {
        format_->SetTextAlignment(alignment);
    }
}

float TextLayer::MeasureWidth(std::wstring_view text) {
    if (!dwrite_ || !format_ || text.empty()) {
        return 0.0f;
    }
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
                                         format_.Get(), 4096.0f, 256.0f, &layout))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) {
        return 0.0f;
    }
    return metrics.widthIncludingTrailingWhitespace;
}

void TextLayer::FillRounded(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& colour) {
    if (!drawing_) {
        return;
    }
    brush_->SetColor(colour);
    d2d_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get());
}

void TextLayer::StrokeRounded(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& colour,
                              float width) {
    if (!drawing_) {
        return;
    }
    brush_->SetColor(colour);
    d2d_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get(), width);
}

void TextLayer::Draw(std::wstring_view text, const D2D1_RECT_F& rect, const D2D1_COLOR_F& colour) {
    if (!drawing_ || text.empty()) {
        return;
    }
    brush_->SetColor(colour);
    d2d_->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format_.Get(), rect,
                    brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

} // namespace liquidock
