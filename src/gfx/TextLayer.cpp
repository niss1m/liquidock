#include "gfx/TextLayer.h"

#include <dwrite_1.h>

#include "core/Check.h"
#include "core/Log.h"

#include <algorithm>
#include <iterator>

namespace liquidock {

bool TextLayer::Initialize(GraphicsDevice& device, float fontSize, DWRITE_FONT_WEIGHT weight,
                           const wchar_t* family) {
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
    // Segoe UI Variable on Windows 11 by default, with DirectWrite falling back
    // on its own where it is missing. The dock's hover label asks for plain
    // Segoe UI Bold instead, because that is the face Nexus labels its icons
    // with and matching it was the point.
    LD_CHECK(dwrite_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, fontSize, L"en-us", &format_));
    // Rasterisation, which is where the remaining difference from Nexus lives -
    // the family, size and weight are the registry's own and are not in doubt.
    //
    // Not GDI_CLASSIC: hinting a bold face onto the pixel grid at this size
    // snaps some stems to whole pixels and leaves others half covered, and that
    // unevenness reads as the letters coming apart. NATURAL_SYMMETRIC is the
    // opposite end - no hinting, symmetric coverage, the smoothest DirectWrite
    // has.
    //
    // The contrast enhancement goes to zero. It exists to darken stems so dark
    // text holds up on a light page; white glyphs on a near-black pill are the
    // inverse case, where it only hardens an edge that is already at full
    // contrast. That hardness is what reads as too sharp.
    ComPtr<IDWriteRenderingParams> defaults;
    if (SUCCEEDED(dwrite_->CreateRenderingParams(&defaults)) && defaults) {
        ComPtr<IDWriteFactory1> dwrite1;
        ComPtr<IDWriteRenderingParams1> soft;
        if (SUCCEEDED(dwrite_.As(&dwrite1)) &&
            SUCCEEDED(dwrite1->CreateCustomRenderingParams(
                defaults->GetGamma(), 0.0f, 0.0f, defaults->GetClearTypeLevel(),
                defaults->GetPixelGeometry(), DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, &soft))) {
            d2d_->SetTextRenderingParams(soft.Get());
        }
    }

    format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // The face's vertical metrics, for DrawInkCentred. Walked from the format
    // rather than hard-coded: the family is a parameter and the size is a
    // setting, and a table of numbers for one font would be wrong for both.
    fontSize_ = fontSize;
    ComPtr<IDWriteFontCollection> collection;
    wchar_t familyName[128]{};
    if (SUCCEEDED(format_->GetFontCollection(&collection)) &&
        SUCCEEDED(format_->GetFontFamilyName(familyName, static_cast<UINT32>(std::size(familyName))))) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        ComPtr<IDWriteFontFamily> fontFamily;
        ComPtr<IDWriteFont> font;
        if (SUCCEEDED(collection->FindFamilyName(familyName, &index, &exists)) && exists &&
            SUCCEEDED(collection->GetFontFamily(index, &fontFamily)) &&
            SUCCEEDED(fontFamily->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL,
                                                       DWRITE_FONT_STYLE_NORMAL, &font))) {
            DWRITE_FONT_METRICS metrics{};
            font->GetMetrics(&metrics);
            const float perEm = static_cast<float>(metrics.designUnitsPerEm);
            if (perEm > 0.0f) {
                ascent_ = fontSize * static_cast<float>(metrics.ascent) / perEm;
                descent_ = fontSize * static_cast<float>(metrics.descent) / perEm;
                capHeight_ = fontSize * static_cast<float>(metrics.capHeight) / perEm;
                // The number this exists to fix: how far apart the space above
                // a capital and the space below its baseline are when the line
                // box is centred.
                // Pin the line box to the face's own ascent and descent.
                // Left on DEFAULT, DirectWrite derives the baseline from the
                // font's *recommended* line height - ascent, descent and the
                // line gap - so the baseline is not at `top + ascent` and
                // everything measured from it lands a little low. That gap is
                // exactly the residual asymmetry left over after centring the
                // ink, and it is invisible until you measure for it.
                format_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, ascent_ + descent_,
                                        ascent_);
                LogDebug("Face at {:.1f}px: ascent {:.2f} descent {:.2f} cap {:.2f}, "
                         "line-box centring is off by {:.2f}px",
                         fontSize, ascent_, descent_, capHeight_,
                         (ascent_ - capHeight_) - descent_);
            }
        }
    }
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

void TextLayer::FillTooltip(const D2D1_RECT_F& rect, float radius, float tailCenterX,
                            float tailWidth, float tailHeight, const D2D1_COLOR_F& fill,
                            const D2D1_COLOR_F& edge) {
    if (!drawing_ || !factory_ || !brush_) {
        return;
    }

    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    const float r = std::min(radius, std::min(width, height) * 0.5f);
    // The tail leaves the bottom edge between the two corner arcs, so its half
    // width is capped by whatever straight run is left between them.
    const float halfTail = std::min(tailWidth * 0.5f, std::max(0.0f, width * 0.5f - r));
    const float tailX =
        std::clamp(tailCenterX, rect.left + r + halfTail, rect.right - r - halfTail);

    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory_->CreatePathGeometry(&path))) {
        return;
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) {
        return;
    }

    auto arc = [](float x, float y, float sweep) {
        return D2D1::ArcSegment(D2D1::Point2F(x, y), D2D1::SizeF(sweep, sweep), 0.0f,
                                D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL);
    };

    sink->BeginFigure(D2D1::Point2F(rect.left + r, rect.top), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(rect.right - r, rect.top));
    sink->AddArc(arc(rect.right, rect.top + r, r));
    sink->AddLine(D2D1::Point2F(rect.right, rect.bottom - r));
    sink->AddArc(arc(rect.right - r, rect.bottom, r));
    sink->AddLine(D2D1::Point2F(tailX + halfTail, rect.bottom));
    sink->AddLine(D2D1::Point2F(tailX, rect.bottom + tailHeight));
    sink->AddLine(D2D1::Point2F(tailX - halfTail, rect.bottom));
    sink->AddLine(D2D1::Point2F(rect.left + r, rect.bottom));
    sink->AddArc(arc(rect.left, rect.bottom - r, r));
    sink->AddLine(D2D1::Point2F(rect.left, rect.top + r));
    sink->AddArc(arc(rect.left + r, rect.top, r));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) {
        return;
    }

    brush_->SetColor(fill);
    d2d_->FillGeometry(path.Get(), brush_.Get());
    // A borderless tooltip is the normal case rather than a special one, so the
    // stroke is skipped outright instead of drawn in a colour that happens to
    // be invisible.
    if (edge.a > 0.004f) {
        brush_->SetColor(edge);
        d2d_->DrawGeometry(path.Get(), brush_.Get(), 1.0f);
    }
}

void TextLayer::StrokeRounded(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& colour,
                              float width) {
    if (!drawing_) {
        return;
    }
    brush_->SetColor(colour);
    d2d_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get(), width);
}

void TextLayer::DrawInkCentred(std::wstring_view text, const D2D1_RECT_F& rect,
                               const D2D1_COLOR_F& colour) {
    if (!drawing_ || text.empty()) {
        return;
    }
    if (ascent_ <= 0.0f) {
        Draw(text, rect, colour); // metrics unavailable; the old behaviour
        return;
    }

    // Put the middle of the cap band on the middle of the box. Drawn from the
    // top, because centring is the thing being replaced.
    const float centre = (rect.top + rect.bottom) * 0.5f;
    const float top = centre - ascent_ + capHeight_ * 0.5f;
    const D2D1_RECT_F box =
        D2D1::RectF(rect.left, top, rect.right, top + ascent_ + descent_ + 1.0f);

    format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    brush_->SetColor(colour);
    d2d_->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format_.Get(), box, brush_.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_NONE);
    format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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
