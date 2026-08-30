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

float Radians(float degrees) {
    return degrees * std::numbers::pi_v<float> / 180.0f;
}

} // namespace

GlassMenu::~GlassMenu() {
    Destroy();
}

bool GlassMenu::Initialize(GraphicsDevice& device, ShaderCache& shaders) {
    device_ = &device;
    shaders_ = &shaders;
    backdrop_.Initialize(device);

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

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(GlassConstants);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    LD_CHECK(device_->d3d()->CreateBuffer(&desc, nullptr, &constantBuffer_));

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    LD_CHECK(device_->d3d()->CreateSamplerState(&sampler, &sampler_));

    if (!frost_.Initialize(*device_, *shaders_) ||
        !text_.Initialize(*device_, layout::kFontSize)) {
        return false;
    }
    text_.SetAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    return true;
}

void GlassMenu::Destroy() {
    text_.Reset();
    frost_.Reset();
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
    const int windowHeight =
        static_cast<int>(std::lround((height_ + 2.0f * layout::kBleed) * scale));

    // The dock lives at the bottom of the screen, so the menu almost always has
    // to open upward. Flipping rather than clamping keeps the cursor at the
    // corner the menu grows from, which is what makes it feel anchored.
    int x = screen.x - static_cast<int>(layout::kBleed * scale);
    int y = screen.y - static_cast<int>(layout::kBleed * scale);
    if (screen.y + windowHeight > work.bottom) {
        y = screen.y - windowHeight + static_cast<int>(layout::kBleed * scale);
    }
    if (screen.x + windowWidth > work.right) {
        x = screen.x - windowWidth + static_cast<int>(layout::kBleed * scale);
    }
    x = std::clamp(x, static_cast<int>(work.left), static_cast<int>(work.right) - windowWidth);
    y = std::clamp(y, static_cast<int>(work.top), static_cast<int>(work.bottom) - windowHeight);

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, windowWidth, windowHeight, SWP_NOACTIVATE);

    // Taken now, while the window is positioned but still hidden, so it copies
    // what is actually behind the menu rather than the menu itself.
    const RECT screenRect{x, y, x + windowWidth, y + windowHeight};
    backdrop_.Capture(monitor, screenRect);

    if (target_.width() == 0) {
        target_.Initialize(*device_, hwnd_, static_cast<UINT>(windowWidth),
                           static_cast<UINT>(windowHeight));
    } else {
        target_.Resize(static_cast<UINT>(windowWidth), static_cast<UINT>(windowHeight));
        text_.Invalidate();
    }
    frost_.Resize(static_cast<UINT>(windowWidth), static_cast<UINT>(windowHeight));
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
    ComPtr<ID3D11VertexShader> vs = shaders_->VertexShader("Glass", "VSMain");
    ComPtr<ID3D11PixelShader> ps = shaders_->PixelShader("Glass", "PSMain");
    if (!vs || !ps || !target_.width()) {
        return;
    }

    const float scale = static_cast<float>(dpi_) / 96.0f;
    if (!backdrop_.srv()) {
        return;
    }

    RECT windowRect{};
    GetWindowRect(hwnd_, &windowRect);
    const RECT monitorRect = backdrop_.monitor_rect();
    const POINT origin{windowRect.left - monitorRect.left, windowRect.top - monitorRect.top};
    const SIZE size{static_cast<LONG>(target_.width()), static_cast<LONG>(target_.height())};

    // Rebuilt every time the menu opens rather than cached: it is on screen for
    // a second and a half, and it has moved since last time.
    frost_.Build(backdrop_, origin, size, design::menu::kFrost * design::glass::kMaxFrostSigmaPx * scale);

    const float viewWidth = static_cast<float>(target_.width());
    const float viewHeight = static_cast<float>(target_.height());

    GlassConstants constants{};
    constants.viewportCenter[0] = viewWidth;
    constants.viewportCenter[1] = viewHeight;
    constants.viewportCenter[2] = viewWidth * 0.5f;
    constants.viewportCenter[3] = viewHeight * 0.5f;
    constants.shape[0] = width_ * 0.5f * scale;
    constants.shape[1] = height_ * 0.5f * scale;
    constants.shape[2] = layout::kCorner * scale;
    constants.light[0] = Radians(design::glass::kLightAngleDegrees);
    constants.light[1] = design::menu::kLightIntensity;
    constants.light[2] = design::menu::kRefraction;
    constants.light[3] = design::menu::kDepth;
    constants.material[0] = design::menu::kDispersion;
    constants.material[1] = design::menu::kFrost;
    constants.material[2] = design::menu::kSplay;
    constants.material[3] = 0.0f;
    // No window origin or backdrop transform: the glass pass reads one texture,
    // the frost chain's output, which the menu builds over its own window.
    constants.lensInfo[2] = scale;
    constants.tint[0] = 1.0f;
    constants.tint[1] = 1.0f;
    constants.tint[2] = 1.0f;
    constants.tint[3] = design::menu::kTintAlpha;

    ID3D11DeviceContext1* ctx = device_->context();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    memcpy(mapped.pData, &constants, sizeof(constants));
    ctx->Unmap(constantBuffer_.Get(), 0);

    ID3D11RenderTargetView* rtv = target_.BeginFrame();
    if (!rtv) {
        return;
    }

    const D3D11_VIEWPORT viewport{0.0f, 0.0f, viewWidth, viewHeight, 0.0f, 1.0f};
    constexpr float kTransparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ctx->ClearRenderTargetView(rtv, kTransparent);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->RSSetViewports(1, &viewport);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    ID3D11Buffer* cb = constantBuffer_.Get();
    ctx->VSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView* resources[2] = {backdrop_.srv(), frost_.srv()};
    ctx->PSSetShaderResources(0, 2, resources);
    ID3D11SamplerState* samplers[1] = {sampler_.Get()};
    ctx->PSSetSamplers(0, 1, samplers);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullResources[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, nullResources);
    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);

    // The text, straight onto the finished glass.
    if (text_.Begin(target_.swap_chain(), scale)) {
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
            text_.Draw(item.label,
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

    Measure();
    Place(screen);

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    // Capture rather than activation: the dock is a no-activate window and a
    // menu that stole the foreground would deactivate whatever the user was
    // working in just to show them four commands. Capture gets every mouse
    // message wherever it lands, which is all a menu actually needs.
    SetCapture(hwnd_);
    running_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);

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

        case WM_MOUSEMOVE: {
            // With the mouse captured these arrive in our client coordinates
            // however far outside the window the cursor is, which is exactly
            // what both the hover and the dismiss test want.
            const float x = static_cast<float>(GET_X_LPARAM(lParam)) / scale;
            const float y = static_cast<float>(GET_Y_LPARAM(lParam)) / scale;
            const int hit = ItemAt(x, y);
            if (hit != hover_) {
                hover_ = hit;
                InvalidateRect(hwnd, nullptr, FALSE);
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
