#include "ui/GlassMenu.h"

#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

#include "core/Check.h"
#include "core/DesignTokens.h"
#include "core/Log.h"
#include "glass/GlassConstants.h"

namespace liquidock {
namespace {

constexpr wchar_t kWindowClass[] = L"LiquiDock.Menu";

namespace layout {
constexpr float kItemHeight = 32.0f;
constexpr float kHeaderHeight = 30.0f;
constexpr float kSeparatorHeight = 9.0f;
constexpr float kPaddingX = 14.0f;
constexpr float kPaddingY = 8.0f;
constexpr float kMinWidth = 180.0f;
constexpr float kCorner = design::kCornerRadius;
// Room around the glass for the rim and its antialiasing, the same reason the
// dock's window is bigger than the dock.
constexpr float kBleed = 10.0f;
constexpr float kFontSize = 13.5f;
} // namespace layout

const D2D1_COLOR_F kText = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.94f);
const D2D1_COLOR_F kTextDim = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.42f);
const D2D1_COLOR_F kHover = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f);
const D2D1_COLOR_F kRule = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.13f);

// The design tokens are plain float[4]; this is the one place they meet D2D.
D2D1_COLOR_F Colour(const float rgba[4]) {
    return D2D1::ColorF(rgba[0], rgba[1], rgba[2], rgba[3]);
}

} // namespace

GlassMenu::~GlassMenu() {
    Destroy();
}

bool GlassMenu::Initialize(GraphicsDevice& device, ShaderCache& shaders) {
    device_ = &device;
    shaders_ = &shaders;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &GlassMenu::WndProcThunk;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                            kWindowClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                            wc.hInstance, this);
    if (!hwnd_) {
        LogError("Menu window creation failed: {}", GetLastError());
        return false;
    }

    if (!text_.Initialize(*device_, layout::kFontSize)) {
        return false;
    }
    text_.SetAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    return true;
}

void GlassMenu::Destroy() {
    text_.Reset();
    target_.Reset();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void GlassMenu::Measure() {
    tops_.clear();
    float widest = layout::kMinWidth;
    float y = layout::kPaddingY;

    for (const Item& item : items_) {
        tops_.push_back(y);
        if (item.separator) {
            y += layout::kSeparatorHeight;
            continue;
        }
        y += item.header ? layout::kHeaderHeight : layout::kItemHeight;
        widest = std::max(widest, text_.MeasureWidth(item.label) + 2.0f * layout::kPaddingX + 20.0f);
    }

    width_ = widest;
    height_ = y + layout::kPaddingY;
}

void GlassMenu::Place(POINT screen) {
    HMONITOR monitor = MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        dpi_ = dpiX;
    }
    const float scale = static_cast<float>(dpi_) / 96.0f;

    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    const RECT& work = info.rcWork;

    const int windowWidth =
        static_cast<int>(std::lround((width_ + 2.0f * layout::kBleed) * scale));
    const int windowHeight = static_cast<int>(
        std::lround((height_ + 2.0f * layout::kBleed + design::label::kTailHeight) * scale));

    // Centred over the point it was opened from and sitting above it, with the
    // tail pointing back down at it - the dock is at the bottom of the screen,
    // so the menu always opens upward and the tail always points down.
    const float tailPx = design::label::kTailHeight * scale;
    int x = screen.x - windowWidth / 2;
    int y = screen.y - windowHeight - static_cast<int>(std::lround(tailPx));

    // Clamped horizontally rather than flipped: the tail moves along the bottom
    // edge to stay over the anchor, so the menu can slide sideways to fit and
    // still point at the thing it came from.
    x = std::clamp(x, static_cast<int>(work.left), static_cast<int>(work.right) - windowWidth);
    if (y < work.top) {
        y = static_cast<int>(work.top);
    }

    // Where the tail meets the panel, in the panel's own logical space. Kept
    // clear of the corners, or the tail grows out of the rounding.
    const float anchorLocal = (static_cast<float>(screen.x - x) / scale) - layout::kBleed;
    const float margin = layout::kCorner + design::label::kTailWidth;
    tailCenterX_ = std::clamp(anchorLocal, margin, std::max(margin, width_ - margin));

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, windowWidth, windowHeight, SWP_NOACTIVATE);

    if (target_.width() == 0) {
        target_.Initialize(*device_, hwnd_, static_cast<UINT>(windowWidth),
                           static_cast<UINT>(windowHeight));
    } else {
        // The text layer's cached bitmap wraps the back buffer, so it has to go
        // before the resize, not after. Released afterwards, ResizeBuffers
        // fails with DXGI_ERROR_INVALID_CALL, the swap chain keeps the previous
        // menu's size, and a taller menu is drawn into a buffer too small for
        // it - which is a menu with its last two commands cut off. The failed
        // resize also leaves the frame-latency semaphore unbalanced, so the
        // next few BeginFrames each wait out their full one-second timeout:
        // the same bug was both the clipping and the several-second delay.
        text_.Invalidate();
        target_.Resize(static_cast<UINT>(windowWidth), static_cast<UINT>(windowHeight));
    }
}

int GlassMenu::ItemAt(float x, float y) const {
    if (x < layout::kBleed || x > layout::kBleed + width_) {
        return -1;
    }
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].separator || items_[i].header || !items_[i].enabled) {
            continue;
        }
        const float top = layout::kBleed + tops_[i];
        if (y >= top && y < top + layout::kItemHeight) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void GlassMenu::Render() {
    if (!target_.width() || !text_.ready()) {
        return;
    }
    const float scale = static_cast<float>(dpi_) / 96.0f;

    ID3D11RenderTargetView* rtv = target_.BeginFrame();
    if (!rtv) {
        return;
    }
    constexpr float kTransparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    device_->context()->ClearRenderTargetView(rtv, kTransparent);
    ID3D11RenderTargetView* nullRtv = nullptr;
    device_->context()->OMSetRenderTargets(1, &nullRtv, nullptr);

    if (text_.Begin(target_.swap_chain(), scale)) {
        // The same shape as the hover label, for the same reason: a menu that
        // came from the dock should say so, and a tail pointing back at the
        // icon says it without a word. It is also why this is flat black rather
        // than glass - the tooltip established that language first, and two
        // different materials for two things that both hang off the dock reads
        // as two different programs.
        const D2D1_RECT_F panel = D2D1::RectF(layout::kBleed, layout::kBleed,
                                              layout::kBleed + width_, layout::kBleed + height_);
        text_.FillTooltip(panel, layout::kCorner, tailCenterX_, design::label::kTailWidth,
                          design::label::kTailHeight, Colour(design::label::kFill),
                          Colour(design::label::kEdge));

        for (size_t i = 0; i < items_.size(); ++i) {
            const Item& item = items_[i];
            const float top = layout::kBleed + tops_[i];

            if (item.separator) {
                const float y = top + layout::kSeparatorHeight * 0.5f;
                text_.FillRounded(D2D1::RectF(layout::kBleed + layout::kPaddingX, y - 0.5f,
                                              layout::kBleed + width_ - layout::kPaddingX, y + 0.5f),
                                  0.5f, kRule);
                continue;
            }

            const float rowHeight = item.header ? layout::kHeaderHeight : layout::kItemHeight;
            if (static_cast<int>(i) == hover_) {
                text_.FillRounded(D2D1::RectF(layout::kBleed + 5.0f, top + 1.0f,
                                              layout::kBleed + width_ - 5.0f, top + rowHeight - 1.0f),
                                  7.0f, kHover);
            }
            text_.DrawInkCentred(item.label,
                       D2D1::RectF(layout::kBleed + layout::kPaddingX, top,
                                   layout::kBleed + width_ - layout::kPaddingX, top + rowHeight),
                       (item.header || !item.enabled) ? kTextDim : kText);
        }
        text_.End();
    }

    target_.EndFrame();
}

void GlassMenu::Choose(int index) {
    if (index >= 0 && index < static_cast<int>(items_.size())) {
        chosen_ = items_[static_cast<size_t>(index)].id;
    }
    running_ = false;
}

UINT GlassMenu::Track(std::vector<Item> items, POINT screen) {
    if (!hwnd_ || items.empty()) {
        return 0;
    }
    items_ = std::move(items);
    chosen_ = 0;
    hover_ = -1;

    LARGE_INTEGER opened{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&opened);
    QueryPerformanceFrequency(&frequency);

    Measure();
    Place(screen);

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    // Drawn here rather than left to WM_PAINT. WM_PAINT is synthesised only when
    // the message queue is empty, and while a menu is up the dock is still being
    // driven by its capture thread - so on a busy desktop the first paint could
    // be seconds late. For those seconds the menu was invisible *and* holding
    // mouse capture, which is exactly the shape of "the menu takes ages and I
    // cannot click anything".
    Render();
    // Capture rather than activation: the dock is a no-activate window and a
    // menu that stole the foreground would deactivate whatever the user was
    // working in just to show them four commands. Capture gets every mouse
    // message wherever it lands, which is all a menu actually needs.
    SetCapture(hwnd_);
    running_ = true;

    // How long from the click to pixels on screen. Logged because this was
    // seconds once, and the failure was invisible: the window was up and had
    // the mouse, it just had not painted.
    LARGE_INTEGER shown{};
    QueryPerformanceCounter(&shown);
    LogDebug("Menu shown in {:.1f} ms",
             1000.0 * static_cast<double>(shown.QuadPart - opened.QuadPart) /
                 static_cast<double>(frequency.QuadPart));

    MSG message{};
    while (running_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
        // Escape has to be caught here: with no activation the window never
        // receives key messages of its own.
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            running_ = false;
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (message.message == WM_QUIT) {
            // Quit chosen from somewhere else; put it back for the main loop.
            PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
    }

    ReleaseCapture();
    ShowWindow(hwnd_, SW_HIDE);
    running_ = false;
    return chosen_;
}

LRESULT CALLBACK GlassMenu::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    GlassMenu* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<GlassMenu*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<GlassMenu*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->WndProc(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT GlassMenu::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    const float scale = static_cast<float>(dpi_) / 96.0f;

    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            Render();
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SETCURSOR: {
            // The menu holds capture, so this arrives wherever the cursor is;
            // ItemAt returns -1 outside the panel and the arrow is right there.
            POINT cursor{};
            int hit = -1;
            if (GetCursorPos(&cursor)) {
                ScreenToClient(hwnd, &cursor);
                hit = ItemAt(static_cast<float>(cursor.x) / scale,
                             static_cast<float>(cursor.y) / scale);
            }
            SetCursor(LoadCursorW(nullptr, hit >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }

        case WM_MOUSEMOVE: {
            // With the mouse captured these arrive in our client coordinates
            // however far outside the window the cursor is, which is exactly
            // what both the hover and the dismiss test want.
            const float x = static_cast<float>(GET_X_LPARAM(lParam)) / scale;
            const float y = static_cast<float>(GET_Y_LPARAM(lParam)) / scale;
            const int hit = ItemAt(x, y);
            if (hit != hover_) {
                hover_ = hit;
                Render(); // never through WM_PAINT; see Track
            }
            return 0;
        }

        case WM_LBUTTONUP:
        case WM_RBUTTONUP: {
            const float x = static_cast<float>(GET_X_LPARAM(lParam)) / scale;
            const float y = static_cast<float>(GET_Y_LPARAM(lParam)) / scale;
            const int hit = ItemAt(x, y);
            if (hit >= 0) {
                Choose(hit);
            } else {
                running_ = false; // clicked away
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
            running_ = false;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace liquidock
