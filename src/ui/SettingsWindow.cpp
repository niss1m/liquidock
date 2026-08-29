#include "ui/SettingsWindow.h"

#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "core/Check.h"
#include "core/DesignTokens.h"
#include "core/Log.h"

namespace liquidock {
namespace {

constexpr wchar_t kWindowClass[] = L"LiquiDock.Settings";
// The file is only rewritten once a drag settles. Dragging a slider produces a
// change per mouse report, and writing the file sixty times a second would also
// wake the dock's own config watcher sixty times a second.
constexpr UINT_PTR kSaveTimer = 1;
constexpr UINT kSaveDelayMs = 400;

namespace layout {
constexpr float kWidth = 880.0f;
constexpr float kPadding = 26.0f;
constexpr float kTitleHeight = 62.0f;
constexpr float kRowHeight = 54.0f;
constexpr float kSectionHeight = 46.0f;
constexpr float kColumnGap = 26.0f;
constexpr float kControlWidth = 120.0f;
constexpr float kValueWidth = 48.0f;
constexpr float kFooterHeight = 34.0f;
constexpr float kCorner = design::kCornerRadius;
} // namespace layout

D2D1_COLOR_F Grey(float level, float alpha) {
    return D2D1::ColorF(level, level, level, alpha);
}

// The panel is nearly opaque. The dock is glass because you are meant to look
// past it; preferences are meant to be read, and text over a moving desktop is
// the single most common way a beautiful settings panel becomes an unusable one.
const D2D1_COLOR_F kPanel = D2D1::ColorF(0.086f, 0.086f, 0.098f, 0.97f);
const D2D1_COLOR_F kPanelEdge = Grey(1.0f, 0.10f);
const D2D1_COLOR_F kTitle = Grey(1.0f, 0.95f);
const D2D1_COLOR_F kSection = Grey(1.0f, 0.42f);
const D2D1_COLOR_F kLabel = Grey(1.0f, 0.88f);
const D2D1_COLOR_F kHint = Grey(1.0f, 0.40f);
const D2D1_COLOR_F kValue = Grey(1.0f, 0.62f);
const D2D1_COLOR_F kTrack = Grey(1.0f, 0.14f);
const D2D1_COLOR_F kFill = Grey(1.0f, 0.70f);
const D2D1_COLOR_F kKnob = Grey(1.0f, 0.95f);
const D2D1_COLOR_F kRowHover = Grey(1.0f, 0.045f);
const D2D1_COLOR_F kOn = D2D1::ColorF(0.25f, 0.60f, 1.0f, 0.95f);

std::wstring FormatValue(float value, int decimals, const wchar_t* suffix) {
    wchar_t buffer[48];
    switch (decimals) {
        case 0: swprintf_s(buffer, L"%.0f", value); break;
        case 1: swprintf_s(buffer, L"%.1f", value); break;
        default: swprintf_s(buffer, L"%.2f", value); break;
    }
    std::wstring text(buffer);
    if (suffix) {
        text += suffix;
    }
    return text;
}

} // namespace

SettingsWindow::~SettingsWindow() {
    Destroy();
}

bool SettingsWindow::Create(GraphicsDevice& device, const Settings& settings,
                            ChangedCallback onChanged) {
    device_ = &device;
    settings_ = settings;
    onChanged_ = std::move(onChanged);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &SettingsWindow::WndProcThunk;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    // TOOLWINDOW keeps it out of the taskbar and Alt+Tab - it belongs to the
    // dock rather than standing on its own - but unlike the dock it is
    // deliberately activatable, because it has to take the keyboard for Escape.
    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                            kWindowClass, L"LiquiDock Preferences", WS_POPUP, 0, 0, 1, 1, nullptr,
                            nullptr, wc.hInstance, this);
    if (!hwnd_) {
        LogError("Preferences window creation failed: {}", GetLastError());
        return false;
    }

    BuildRows();
    return true;
}

void SettingsWindow::Destroy() {
    if (hwnd_) {
        KillTimer(hwnd_, kSaveTimer);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    backBuffer_.Reset();
    target_.Reset();
}

void SettingsWindow::BuildRows() {
    rows_.clear();
    backdropChoice_ = settings_.backdrop == BackdropSource::Screen ? 1 : 0;

    auto section = [this](const wchar_t* label, int column) {
        Row row;
        row.kind = Row::Kind::Section;
        row.label = label;
        row.column = column;
        rows_.push_back(std::move(row));
    };
    auto slider = [this](const wchar_t* label, const wchar_t* hint, float* value, float low,
                         float high, int decimals, int column, const wchar_t* suffix = nullptr) {
        Row row;
        row.kind = Row::Kind::Slider;
        row.label = label;
        row.hint = hint;
        row.number = value;
        row.minimum = low;
        row.maximum = high;
        row.decimals = decimals;
        row.suffix = suffix;
        row.column = column;
        rows_.push_back(std::move(row));
    };
    auto toggle = [this](const wchar_t* label, const wchar_t* hint, bool* flag, int column) {
        Row row;
        row.kind = Row::Kind::Toggle;
        row.label = label;
        row.hint = hint;
        row.flag = flag;
        row.column = column;
        rows_.push_back(std::move(row));
    };
    auto choice = [this](const wchar_t* label, const wchar_t* hint, int* value,
                         std::vector<std::wstring> options, int column) {
        Row row;
        row.kind = Row::Kind::Choice;
        row.label = label;
        row.hint = hint;
        row.choice = value;
        row.options = std::move(options);
        row.column = column;
        rows_.push_back(std::move(row));
    };

    section(L"Glass", 0);
    choice(L"Backdrop", L"Screen mode hides it from screenshots",
           &backdropChoice_, {L"Wallpaper", L"Screen"}, 0);
    slider(L"Frost", L"How softened the desktop behind is", &settings_.frost, 0.0f,
           1.0f, 2, 0);
    slider(L"Refraction", L"How far the rim bends what is behind", &settings_.refraction, 0.0f,
           1.0f, 2, 0);
    slider(L"Depth", L"How thick the glass edge reads", &settings_.depth, 0.0f, 1.0f, 2, 0);
    slider(L"Splay", L"How far inward the bending reaches", &settings_.splay, 0.0f,
           1.0f, 2, 0);
    slider(L"Dispersion", L"Colour fringing at the rim", &settings_.dispersion,
           0.0f, 1.0f, 2, 0);
    slider(L"Light angle", L"Where the highlight falls", &settings_.lightAngleDegrees, -180.0f,
           180.0f, 0, 0, L"°");
    slider(L"Light intensity", L"How hard the highlight hits", &settings_.lightIntensity, 0.0f,
           1.0f, 2, 0);
    slider(L"Tint", L"The white the glass is tinted with",
           &settings_.tintAlpha, 0.0f, 0.4f, 2, 0);

    section(L"Magnification", 1);
    toggle(L"Magnify under the cursor", L"The macOS swell", &settings_.magnification, 1);
    slider(L"Maximum size", L"How big the hovered icon gets", &settings_.maxScale, 1.0f,
           design::kMaxConfigurableScale, 2, 1, L"x");
    slider(L"Reach", L"How far the swell carries",
           &settings_.influencePx, 40.0f, 320.0f, 0, 1, L" px");
    toggle(L"Glass follows the icons",
           L"The bar swells around a raised icon", &settings_.iconBulge, 1);

    section(L"Placement", 1);
    toggle(L"Reserve screen space", L"Maximised windows stop above it",
           &settings_.reserveSpace, 1);

    section(L"Behaviour", 1);
    toggle(L"Hide until needed", L"Comes back at the screen edge",
           &settings_.autoHide, 1);
    slider(L"Stay out for", L"Before sliding away again", &settings_.dwellSeconds, 0.5f, 15.0f, 1,
           1, L" s");
    slider(L"Slide time", L"How long the dock takes to arrive", &settings_.slideSeconds, 0.0f,
           0.8f, 2, 1, L" s");
}

void SettingsWindow::LayoutRows() {
    const float columnWidth = (layout::kWidth - 2.0f * layout::kPadding - layout::kColumnGap) * 0.5f;
    float y[2] = {layout::kTitleHeight, layout::kTitleHeight};

    for (Row& row : rows_) {
        const int column = row.column;
        const float left = layout::kPadding + column * (columnWidth + layout::kColumnGap);
        const float height =
            (row.kind == Row::Kind::Section) ? layout::kSectionHeight : layout::kRowHeight;

        row.bounds = D2D1::RectF(left, y[column], left + columnWidth, y[column] + height);
        // The control hugs the right-hand edge of its column, with the numeric
        // readout beyond it, so every control in a column shares one baseline.
        const float controlRight = row.bounds.right - layout::kValueWidth;
        row.control = D2D1::RectF(controlRight - layout::kControlWidth, y[column] + 12.0f,
                                  controlRight, y[column] + 12.0f + 22.0f);
        y[column] += height;
    }

    height_ = static_cast<int>(std::lround(
        (std::max(y[0], y[1]) + layout::kFooterHeight) * (static_cast<float>(dpi_) / 96.0f)));
    width_ = static_cast<int>(std::lround(layout::kWidth * (static_cast<float>(dpi_) / 96.0f)));
}

bool SettingsWindow::CreateDeviceResources() {
    if (d2d_) {
        return true;
    }

    D2D1_FACTORY_OPTIONS options{};
#ifdef LIQUIDOCK_DEBUG
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    LD_CHECK(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                               reinterpret_cast<void**>(d2dFactory_.GetAddressOf())));
    LD_CHECK(d2dFactory_->CreateDevice(device_->dxgi(), &d2dDevice_));
    LD_CHECK(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_));
    LD_CHECK(d2d_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush_));

    LD_CHECK(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())));

    // Segoe UI Variable on Windows 11, Segoe UI everywhere else - DirectWrite
    // falls back on its own when the first is missing.
    auto format = [this](float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out) {
        return dwrite_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                         L"en-us", out);
    };
    LD_CHECK(format(19.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &titleFormat_));
    LD_CHECK(format(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &sectionFormat_));
    LD_CHECK(format(14.0f, DWRITE_FONT_WEIGHT_NORMAL, &labelFormat_));
    LD_CHECK(format(11.5f, DWRITE_FONT_WEIGHT_NORMAL, &hintFormat_));

    // Labels and hints are one line each. Left to wrap they spill into the row
    // below, which reads as a rendering bug rather than as a long sentence -
    // and a settings panel where the rows do not line up is worse than one
    // where a hint is a word shorter.
    ComPtr<IDWriteInlineObject> ellipsis;
    dwrite_->CreateEllipsisTrimmingSign(hintFormat_.Get(), &ellipsis);
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    labelFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    hintFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    labelFormat_->SetTrimming(&trimming, ellipsis.Get());
    hintFormat_->SetTrimming(&trimming, ellipsis.Get());
    LD_CHECK(format(12.5f, DWRITE_FONT_WEIGHT_NORMAL, &valueFormat_));
    valueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    return true;
}

void SettingsWindow::Show(HMONITOR nearMonitor) {
    if (!hwnd_) {
        return;
    }

    // Re-read from disk on the way in. The file is editable while the dock is
    // running, so opening the window over a stale copy would silently revert
    // whatever was typed there.
    settings_.Load();
    BuildRows();

    UINT dpiX = 96;
    UINT dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(nearMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        dpi_ = dpiX;
    }
    LayoutRows();

    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(nearMonitor, &info);
    const RECT& work = info.rcWork;
    const int x = work.left + ((work.right - work.left) - width_) / 2;
    const int y = work.top + ((work.bottom - work.top) - height_) / 2;

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height_, SWP_NOACTIVATE);

    if (!target_.width()) {
        if (!target_.Initialize(*device_, hwnd_, static_cast<UINT>(width_),
                                static_cast<UINT>(height_))) {
            return;
        }
    } else {
        target_.Resize(static_cast<UINT>(width_), static_cast<UINT>(height_));
        backBuffer_.Reset();
    }
    if (!CreateDeviceResources()) {
        return;
    }

    visible_ = true;
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::Hide() {
    if (!hwnd_ || !visible_) {
        return;
    }
    visible_ = false;
    dragRow_ = -1;
    hoverRow_ = -1;
    ShowWindow(hwnd_, SW_HIDE);
    // Whatever was still pending is written now rather than on the next open.
    KillTimer(hwnd_, kSaveTimer);
    settings_.Save();
}

int SettingsWindow::RowAt(float x, float y) const {
    for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        if (row.kind == Row::Kind::Section) {
            continue;
        }
        if (x >= row.bounds.left && x <= row.bounds.right && y >= row.bounds.top &&
            y <= row.bounds.bottom) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool SettingsWindow::ApplyPointer(int index, float x, bool dragging) {
    if (index < 0 || index >= static_cast<int>(rows_.size())) {
        return false;
    }
    Row& row = rows_[static_cast<size_t>(index)];

    switch (row.kind) {
        case Row::Kind::Slider: {
            const float span = std::max(row.control.right - row.control.left, 1.0f);
            const float t = std::clamp((x - row.control.left) / span, 0.0f, 1.0f);
            const float value = row.minimum + t * (row.maximum - row.minimum);
            if (std::fabs(value - *row.number) < 1e-4f) {
                return false;
            }
            *row.number = value;
            return true;
        }
        case Row::Kind::Toggle:
            if (dragging) {
                return false; // a toggle flips once, not once per mouse report
            }
            *row.flag = !*row.flag;
            return true;
        case Row::Kind::Choice: {
            if (dragging) {
                return false;
            }
            const int count = static_cast<int>(row.options.size());
            const float span = (row.control.right - row.control.left) / std::max(count, 1);
            const int picked =
                std::clamp(static_cast<int>((x - row.control.left) / std::max(span, 1.0f)), 0,
                           count - 1);
            if (picked == *row.choice) {
                return false;
            }
            *row.choice = picked;
            return true;
        }
        default:
            return false;
    }
}

void SettingsWindow::CommitChange() {
    settings_.backdrop =
        backdropChoice_ == 1 ? BackdropSource::Screen : BackdropSource::Wallpaper;
    if (onChanged_) {
        onChanged_(settings_);
    }
    // Debounced: dragging a slider produces a change per mouse report, and the
    // dock is watching this file.
    SetTimer(hwnd_, kSaveTimer, kSaveDelayMs, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::DrawText(const std::wstring& text, IDWriteTextFormat* format,
                              const D2D1_RECT_F& rect, const D2D1_COLOR_F& colour) {
    if (text.empty() || !format) {
        return;
    }
    brush_->SetColor(colour);
    d2d_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush_.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void SettingsWindow::DrawSlider(const Row& row, bool hovered) {
    const float centreY = (row.control.top + row.control.bottom) * 0.5f;
    const float radius = 2.0f;
    const D2D1_ROUNDED_RECT track = D2D1::RoundedRect(
        D2D1::RectF(row.control.left, centreY - radius, row.control.right, centreY + radius),
        radius, radius);
    brush_->SetColor(kTrack);
    d2d_->FillRoundedRectangle(track, brush_.Get());

    const float span = std::max(row.maximum - row.minimum, 1e-5f);
    const float t = std::clamp((*row.number - row.minimum) / span, 0.0f, 1.0f);
    const float knobX = row.control.left + t * (row.control.right - row.control.left);

    if (t > 0.0f) {
        brush_->SetColor(kFill);
        d2d_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(row.control.left, centreY - radius, knobX,
                                          centreY + radius),
                              radius, radius),
            brush_.Get());
    }

    brush_->SetColor(kKnob);
    const float knobRadius = hovered ? 7.0f : 6.0f;
    d2d_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, centreY), knobRadius, knobRadius),
                      brush_.Get());
}

void SettingsWindow::DrawToggle(const Row& row, bool hovered) {
    const float centreY = (row.control.top + row.control.bottom) * 0.5f;
    const float height = 22.0f;
    const float width = 40.0f;
    // Right-aligned in the control slot so toggles line up with slider knobs.
    const float right = row.control.right;
    const D2D1_RECT_F pill =
        D2D1::RectF(right - width, centreY - height * 0.5f, right, centreY + height * 0.5f);

    brush_->SetColor(*row.flag ? kOn : kTrack);
    d2d_->FillRoundedRectangle(D2D1::RoundedRect(pill, height * 0.5f, height * 0.5f), brush_.Get());

    const float knobRadius = height * 0.5f - 3.0f;
    const float knobX = *row.flag ? (pill.right - knobRadius - 3.0f) : (pill.left + knobRadius + 3.0f);
    brush_->SetColor(Grey(1.0f, hovered ? 1.0f : 0.92f));
    d2d_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, centreY), knobRadius, knobRadius),
                      brush_.Get());
}

void SettingsWindow::DrawChoice(const Row& row, float pointerX) {
    const int count = static_cast<int>(row.options.size());
    if (count <= 0) {
        return;
    }
    const float height = 24.0f;
    const float centreY = (row.control.top + row.control.bottom) * 0.5f;
    const D2D1_RECT_F frame = D2D1::RectF(row.control.left, centreY - height * 0.5f,
                                          row.control.right, centreY + height * 0.5f);

    brush_->SetColor(kTrack);
    d2d_->FillRoundedRectangle(D2D1::RoundedRect(frame, 6.0f, 6.0f), brush_.Get());

    const float segment = (frame.right - frame.left) / count;
    for (int i = 0; i < count; ++i) {
        const D2D1_RECT_F cell = D2D1::RectF(frame.left + i * segment + 2.0f, frame.top + 2.0f,
                                             frame.left + (i + 1) * segment - 2.0f,
                                             frame.bottom - 2.0f);
        const bool selected = (i == *row.choice);
        const bool under = pointerX >= cell.left && pointerX <= cell.right;
        if (selected) {
            brush_->SetColor(kOn);
            d2d_->FillRoundedRectangle(D2D1::RoundedRect(cell, 5.0f, 5.0f), brush_.Get());
        } else if (under) {
            brush_->SetColor(Grey(1.0f, 0.08f));
            d2d_->FillRoundedRectangle(D2D1::RoundedRect(cell, 5.0f, 5.0f), brush_.Get());
        }
        auto centred = D2D1::RectF(cell.left, cell.top + 3.0f, cell.right, cell.bottom);
        valueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(row.options[static_cast<size_t>(i)], valueFormat_.Get(), centred,
                 selected ? Grey(1.0f, 1.0f) : kValue);
        valueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

void SettingsWindow::Render() {
    if (!d2d_ || !target_.width()) {
        return;
    }

    if (!backBuffer_) {
        ComPtr<IDXGISurface> surface;
        if (FAILED(target_.swap_chain()->GetBuffer(0, IID_PPV_ARGS(&surface)))) {
            return;
        }
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f,
            96.0f);
        if (FAILED(d2d_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &backBuffer_))) {
            return;
        }
    }

    if (!target_.BeginFrame()) {
        return;
    }

    const float scale = static_cast<float>(dpi_) / 96.0f;
    d2d_->SetTarget(backBuffer_.Get());
    d2d_->BeginDraw();
    // One transform for the whole panel: everything below is written in the
    // same logical units as the design tokens, at any DPI.
    d2d_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    d2d_->Clear(D2D1::ColorF(0, 0.0f));

    const D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(
        D2D1::RectF(0.0f, 0.0f, layout::kWidth, static_cast<float>(height_) / scale),
        layout::kCorner, layout::kCorner);
    brush_->SetColor(kPanel);
    d2d_->FillRoundedRectangle(panel, brush_.Get());
    brush_->SetColor(kPanelEdge);
    d2d_->DrawRoundedRectangle(panel, brush_.Get(), 1.0f);

    DrawText(L"LiquiDock", titleFormat_.Get(),
             D2D1::RectF(layout::kPadding, 22.0f, 400.0f, 56.0f), kTitle);
    DrawText(L"Every change applies straight away", hintFormat_.Get(),
             D2D1::RectF(layout::kPadding, 44.0f, 460.0f, 62.0f), kHint);

    for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        const bool hovered = (static_cast<int>(i) == hoverRow_);

        if (row.kind == Row::Kind::Section) {
            DrawText(row.label, sectionFormat_.Get(),
                     D2D1::RectF(row.bounds.left, row.bounds.top + 18.0f, row.bounds.right,
                                 row.bounds.bottom),
                     kSection);
            continue;
        }

        if (hovered) {
            brush_->SetColor(kRowHover);
            d2d_->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(row.bounds.left - 8.0f, row.bounds.top + 2.0f,
                                              row.bounds.right + 8.0f, row.bounds.bottom - 2.0f),
                                  8.0f, 8.0f),
                brush_.Get());
        }

        const float textRight = row.control.left - 14.0f;
        DrawText(row.label, labelFormat_.Get(),
                 D2D1::RectF(row.bounds.left, row.bounds.top + 8.0f, textRight, row.bounds.top + 30.0f),
                 kLabel);
        if (row.hint) {
            DrawText(row.hint, hintFormat_.Get(),
                     D2D1::RectF(row.bounds.left, row.bounds.top + 27.0f, textRight,
                                 row.bounds.bottom),
                     kHint);
        }

        switch (row.kind) {
            case Row::Kind::Slider:
                DrawSlider(row, hovered);
                DrawText(FormatValue(*row.number, row.decimals, row.suffix), valueFormat_.Get(),
                         D2D1::RectF(row.control.right + 8.0f, row.control.top,
                                     row.bounds.right, row.control.bottom + 4.0f),
                         kValue);
                break;
            case Row::Kind::Toggle:
                DrawToggle(row, hovered);
                break;
            case Row::Kind::Choice:
                DrawChoice(row, hovered ? pointerX_ : -1.0f);
                break;
            default:
                break;
        }
    }

    DrawText(L"Esc to close  ·  these are the same values as settings.txt", hintFormat_.Get(),
             D2D1::RectF(layout::kPadding, static_cast<float>(height_) / scale - 28.0f,
                         layout::kWidth - layout::kPadding,
                         static_cast<float>(height_) / scale - 8.0f),
             Grey(1.0f, 0.28f));

    d2d_->SetTransform(D2D1::Matrix3x2F::Identity());
    const HRESULT hr = d2d_->EndDraw();
    d2d_->SetTarget(nullptr);
    if (FAILED(hr)) {
        // The device went away underneath us. The dock rebuilds it; this window
        // simply closes, because a preferences panel is not worth recovering.
        LogWarn("Preferences drawing failed: {}", FormatHResult(hr));
        backBuffer_.Reset();
        Hide();
        return;
    }
    target_.EndFrame();
}

LRESULT CALLBACK SettingsWindow::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                              LPARAM lParam) {
    SettingsWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<SettingsWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->WndProc(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT SettingsWindow::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    const float scale = static_cast<float>(dpi_) / 96.0f;

    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            Render();
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_NCHITTEST: {
            // The panel has no title bar, so the header area is the grab handle.
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &point);
            if (static_cast<float>(point.y) / scale < layout::kTitleHeight) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_MOUSEMOVE: {
            if (!mouseTracking_) {
                TRACKMOUSEEVENT track{sizeof(track)};
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                mouseTracking_ = TrackMouseEvent(&track) != 0;
            }
            pointerX_ = static_cast<float>(GET_X_LPARAM(lParam)) / scale;
            pointerY_ = static_cast<float>(GET_Y_LPARAM(lParam)) / scale;

            if (dragRow_ >= 0) {
                if (ApplyPointer(dragRow_, pointerX_, true)) {
                    CommitChange();
                }
                return 0;
            }
            const int row = RowAt(pointerX_, pointerY_);
            if (row != hoverRow_) {
                hoverRow_ = row;
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (row >= 0 && rows_[static_cast<size_t>(row)].kind == Row::Kind::Choice) {
                InvalidateRect(hwnd, nullptr, FALSE); // segment hover follows the pointer
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            mouseTracking_ = false;
            hoverRow_ = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN: {
            const float x = static_cast<float>(GET_X_LPARAM(lParam)) / scale;
            const float y = static_cast<float>(GET_Y_LPARAM(lParam)) / scale;
            const int row = RowAt(x, y);
            if (row >= 0) {
                if (ApplyPointer(row, x, false)) {
                    CommitChange();
                }
                if (rows_[static_cast<size_t>(row)].kind == Row::Kind::Slider) {
                    dragRow_ = row;
                    SetCapture(hwnd);
                }
            }
            return 0;
        }

        case WM_LBUTTONUP:
            if (dragRow_ >= 0) {
                dragRow_ = -1;
                ReleaseCapture();
            }
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                Hide();
            }
            return 0;

        case WM_ACTIVATE:
            // Clicking away is how a preferences panel is dismissed; it has no
            // business staying on top of whatever the user turned to next.
            if (LOWORD(wParam) == WA_INACTIVE) {
                Hide();
            }
            return 0;

        case WM_TIMER:
            if (wParam == kSaveTimer) {
                KillTimer(hwnd, kSaveTimer);
                settings_.Save();
            }
            return 0;

        case WM_CLOSE:
            Hide();
            return 0;

        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace liquidock
