#include "app/DockWindow.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

#include "core/Check.h"
#include "core/DesignTokens.h"
#include "core/Log.h"

namespace liquidock {
namespace {

constexpr wchar_t kWindowClass[] = L"LiquiDock.Dock";
constexpr UINT_PTR kShaderWatchTimer = 1;
constexpr UINT_PTR kHideTimer = 2;

// Posted to ourselves after presenting an animation frame. Present blocks on
// vblank, so this self-paces at the monitor's refresh rate without a timer -
// and unlike a timer it stops the instant the animation settles.
constexpr UINT kAnimateMessage = WM_APP + 1;

// TODO(M2): these belong in the config file once it exists, and in the
// preferences UI in M4. They are the numbers a user actually wants to change.
constexpr float kDwellSeconds = 3.0f;  // how long the dock stays out
constexpr float kSlideSeconds = 0.22f; // how long the slide itself takes
constexpr int kTriggerThicknessPx = 2; // the strip that notices the cursor

// M0 has no item list yet, so the bar is sized as the design's own dock: ten
// main icons, a separator, two utility icons. That makes an M0 screenshot
// directly comparable against Figma frame 3:5 instead of merely dock-shaped.
// M2 replaces this with the real item count.
constexpr float kDockWidth = design::BarWidth(10, 2);
constexpr float kDockHeight = design::kBarHeight;
constexpr float kCornerRadius = design::kCornerRadius;
constexpr float kBottomMargin = design::kScreenMargin;

// The window is grown past the glass on every side so the rim highlight, and
// later the drop shadow and the icons that overshoot the bar while magnified,
// have somewhere to land.
constexpr float kBleed = 40.0f;

// Blur radius at frost = 1.0, in logical pixels. The design's frost of 0.04
// lands near a pixel and a half, which is the barely-there scattering the
// Figma render shows rather than a milky panel.
constexpr float kMaxFrostSigmaPx = 40.0f;

float Radians(float degrees) {
    return degrees * std::numbers::pi_v<float> / 180.0f;
}

// Smootherstep. Zero first *and* second derivative at both ends, so the slide
// has no perceptible corner where it starts or stops - a plain lerp reads as
// mechanical at this duration.
float Eased(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

} // namespace

DockWindow::~DockWindow() {
    Destroy();
}

bool DockWindow::Create(GraphicsDevice& device, bool diagnostic, bool autoHide) {
    device_ = &device;
    diagnostic_ = diagnostic;
    autoHide_ = autoHide;
    shaders_ = std::make_unique<ShaderCache>(device.d3d());

    QueryPerformanceFrequency(&frequency_);
    QueryPerformanceCounter(&startTime_);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &DockWindow::WndProcThunk;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) {
        LogError("RegisterClassExW failed: {}", GetLastError());
        return false;
    }

    hwnd_ = CreateWindowExW(
        // NOREDIRECTIONBITMAP is the load-bearing flag: without it DWM
        // allocates an opaque redirection surface and the window can never be
        // per-pixel translucent no matter what the swap chain does.
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kWindowClass, L"LiquiDock", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, this);

    if (!hwnd_) {
        LogError("CreateWindowExW failed: {}", GetLastError());
        return false;
    }

    dpi_ = GetDpiForWindow(hwnd_);
    UpdatePlacement();

    RECT client{};
    GetClientRect(hwnd_, &client);
    if (!target_.Initialize(*device_, hwnd_, static_cast<UINT>(client.right),
                            static_cast<UINT>(client.bottom))) {
        return false;
    }
    if (!CreateResources()) {
        return false;
    }

#ifdef LIQUIDOCK_DEBUG
    shaders_->PollForChanges(); // establish the baseline stamp
    SetTimer(hwnd_, kShaderWatchTimer, 250, nullptr);
#endif

    if (autoHide_) {
        trigger_.Create([this] { Reveal(); });
        trigger_.SetEnabled(false); // the dock starts on screen
    }
    UpdatePlacement(); // now that the trigger exists, give it its bounds

    QueryPerformanceCounter(&lastFrameTime_);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    RequestRedraw();

    // Start visible, then tuck away. Launching straight into a hidden dock
    // looks indistinguishable from launching into a broken one.
    StartHideCountdown();

    LogInfo("Dock window created at {} DPI, auto-hide {}", dpi_, autoHide_ ? "on" : "off");
    return true;
}

void DockWindow::Reveal() {
    if (!autoHide_ || !hwnd_) {
        return;
    }

    if (revealState_ == RevealState::Shown || revealState_ == RevealState::Revealing) {
        StartHideCountdown(); // already out; just reset the dwell
        return;
    }

    revealState_ = RevealState::Revealing;
    animating_ = true;
    trigger_.SetEnabled(false);
    QueryPerformanceCounter(&lastFrameTime_);

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    // Re-assert topmost on every reveal. Other always-on-top shells - Nexus
    // included - re-assert theirs periodically, and whoever asked last wins.
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    RequestRedraw();
}

void DockWindow::StartHideCountdown() {
    if (!autoHide_ || !hwnd_) {
        return;
    }
    revealState_ = (revealProgress_ >= 1.0f) ? RevealState::Shown : revealState_;
    SetTimer(hwnd_, kHideTimer, static_cast<UINT>(kDwellSeconds * 1000.0f), nullptr);
    LogDebug("dwell timer armed ({} s)", kDwellSeconds);
}

void DockWindow::BeginHiding() {
    if (!autoHide_ || revealState_ == RevealState::Hidden) {
        return;
    }
    revealState_ = RevealState::Hiding;
    animating_ = true;
    QueryPerformanceCounter(&lastFrameTime_);
    RequestRedraw();
}

float DockWindow::AdvanceReveal(float deltaSeconds) {
    if (!autoHide_) {
        return 1.0f;
    }

    const float step = (kSlideSeconds > 0.0f) ? (deltaSeconds / kSlideSeconds) : 1.0f;

    if (revealState_ == RevealState::Revealing) {
        revealProgress_ = std::min(1.0f, revealProgress_ + step);
        if (revealProgress_ >= 1.0f) {
            revealState_ = RevealState::Shown;
            animating_ = false;
            LogDebug("slide out complete");
            StartHideCountdown();
        }
    } else if (revealState_ == RevealState::Hiding) {
        revealProgress_ = std::max(0.0f, revealProgress_ - step);
        if (revealProgress_ <= 0.0f) {
            revealState_ = RevealState::Hidden;
            animating_ = false;
            // Hide the window outright rather than leaving an invisible one
            // composited every frame, and re-arm the edge strip.
            LogDebug("slide in complete, dock hidden");
            ShowWindow(hwnd_, SW_HIDE);
            trigger_.SetEnabled(true);
        }
    }

    return Eased(revealProgress_);
}

bool DockWindow::CreateResources() {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(GlassConstants);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    LD_CHECK(device_->d3d()->CreateBuffer(&desc, nullptr, &constantBuffer_));

    // Clamp rather than wrap: refraction pushes samples past the dock's own
    // footprint, and near a screen edge that can land outside the wallpaper.
    // Tiled wallpapers are handled with frac() in the shader instead.
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    LD_CHECK(device_->d3d()->CreateSamplerState(&sampler, &sampler_));

    if (!backdrop_.Initialize(*device_) || !frost_.Initialize(*device_, *shaders_)) {
        return false;
    }
    return frost_.Resize(target_.width(), target_.height());
}

void DockWindow::Destroy() {
    trigger_.Destroy();
    if (hwnd_) {
        KillTimer(hwnd_, kShaderWatchTimer);
        KillTimer(hwnd_, kHideTimer);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void DockWindow::RequestRedraw() {
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void DockWindow::UpdatePlacement() {
    const float scale = static_cast<float>(dpi_) / 96.0f;
    const int width = static_cast<int>(std::lround((kDockWidth + 2.0f * kBleed) * scale));
    // The window reaches from above the bar's resting position all the way down
    // to the screen edge, so the bar can slide entirely out of sight *within*
    // it. That makes the animation a constant fed to the shader rather than a
    // SetWindowPos on every frame, which would make DWM redo its work each time.
    const int height =
        static_cast<int>(std::lround((kBottomMargin + kDockHeight + kBleed) * scale));

    HMONITOR monitor = hwnd_ ? MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY)
                             : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) {
        return;
    }

    // Position against the work area rather than the monitor bounds so the dock
    // does not sit underneath the taskbar before M3 teaches it to reserve its
    // own space.
    const RECT& work = info.rcWork;
    const int x = work.left + ((work.right - work.left) - width) / 2;
    const int y = work.bottom - height;

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW);

    // The trigger strip spans the dock's full width so the cursor does not have
    // to find the bar exactly, only the right stretch of the edge.
    trigger_.SetBounds(x, work.bottom - kTriggerThicknessPx, width, kTriggerThicknessPx);

    // Moving the dock invalidates the cached frost even at an unchanged size:
    // it is a crop of the wallpaper at a particular position.
    frostDirty_ = true;

    if (target_.width() > 0) {
        target_.Resize(static_cast<UINT>(width), static_cast<UINT>(height));
        frost_.Resize(static_cast<UINT>(width), static_cast<UINT>(height));
        RequestRedraw();
    }
}

void DockWindow::Render() {
    if (deviceLost_) {
        return;
    }

    // Everything that can fail is done before BeginFrame. BeginFrame takes a
    // slot from the frame-latency semaphore that only Present hands back, so
    // returning early after it stalls the *next* frame for a full timeout.
    ComPtr<ID3D11VertexShader> vs = shaders_->VertexShader("Glass", "VSMain");
    ComPtr<ID3D11PixelShader> ps = shaders_->PixelShader("Glass", "PSMain");
    if (!vs || !ps) {
        return; // Compile error already logged; the next save fixes it.
    }

    ID3D11DeviceContext1* ctx = device_->context();

    const float scale = static_cast<float>(dpi_) / 96.0f;

    // The backdrop reloads only when the wallpaper actually changes, and the
    // frost chain re-runs only when something it depends on moved. Both return
    // immediately in the common case, which is why a static desktop presents no
    // frames at all.
    HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    if (backdrop_.Update(monitor)) {
        frostDirty_ = true;
    }

    RECT windowRect{};
    GetWindowRect(hwnd_, &windowRect);
    const RECT monitorRect = backdrop_.monitor_rect();
    const POINT windowOrigin{windowRect.left - monitorRect.left, windowRect.top - monitorRect.top};
    const SIZE windowSize{static_cast<LONG>(target_.width()),
                          static_cast<LONG>(target_.height())};

    const float frostSigma = design::glass::kFrost * kMaxFrostSigmaPx * scale;
    if (frostDirty_ || !frost_.ready()) {
        if (frost_.Build(backdrop_, windowOrigin, windowSize, frostSigma)) {
            frostDirty_ = false;
        }
    }
    const float viewWidth = static_cast<float>(target_.width());
    const float viewHeight = static_cast<float>(target_.height());

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const float elapsed = static_cast<float>(now.QuadPart - startTime_.QuadPart) /
                          static_cast<float>(frequency_.QuadPart);
    float deltaSeconds = static_cast<float>(now.QuadPart - lastFrameTime_.QuadPart) /
                         static_cast<float>(frequency_.QuadPart);
    lastFrameTime_ = now;
    // A frame can be arbitrarily late if the machine stalled; clamping keeps a
    // hitch from teleporting the dock through its whole slide.
    deltaSeconds = std::clamp(deltaSeconds, 0.0f, 0.1f);

    const float reveal = AdvanceReveal(deltaSeconds);
    const float shownCentreY = (kBleed + kDockHeight * 0.5f) * scale;
    const float hiddenCentreY = viewHeight + kDockHeight * 0.5f * scale;

    GlassConstants constants{};
    constants.viewportCenter[0] = viewWidth;
    constants.viewportCenter[1] = viewHeight;
    constants.viewportCenter[2] = viewWidth * 0.5f;
    constants.viewportCenter[3] = hiddenCentreY + (shownCentreY - hiddenCentreY) * reveal;
    constants.shape[0] = kDockWidth * 0.5f * scale;
    constants.shape[1] = kDockHeight * 0.5f * scale;
    constants.shape[2] = kCornerRadius * scale;
    constants.shape[3] = elapsed;

    constants.light[0] = Radians(design::glass::kLightAngleDegrees);
    constants.light[1] = design::glass::kLightIntensity;
    constants.light[2] = design::glass::kRefraction;  // consumed in M1
    constants.light[3] = design::glass::kDepth;
    constants.material[0] = design::glass::kDispersion;
    constants.material[1] = design::glass::kFrost;
    constants.material[2] = design::glass::kSplay;
    constants.material[3] = backdrop_.tiled() ? 1.0f : 0.0f;

    constants.windowOrigin[0] = static_cast<float>(windowOrigin.x);
    constants.windowOrigin[1] = static_cast<float>(windowOrigin.y);
    constants.windowOrigin[2] = static_cast<float>(monitorRect.right - monitorRect.left);
    constants.windowOrigin[3] = static_cast<float>(monitorRect.bottom - monitorRect.top);

    float uvScale[2]{};
    float uvOffset[2]{};
    backdrop_.uv_scale(uvScale);
    backdrop_.uv_offset(uvOffset);
    constants.backdropUv[0] = uvScale[0];
    constants.backdropUv[1] = uvScale[1];
    constants.backdropUv[2] = uvOffset[0];
    constants.backdropUv[3] = uvOffset[1];

    if (diagnostic_) {
        constants.tint[0] = 1.0f;
        constants.tint[1] = 0.0f;
        constants.tint[2] = 1.0f;
        constants.tint[3] = 1.0f;
    } else {
        memcpy(constants.tint, design::kBarTint, sizeof(constants.tint));
    }

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

    // No blend state: the target was just cleared to zero and the shader writes
    // premultiplied colour, so blending would only cost bandwidth.
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->Draw(3, 0);

    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
    ID3D11ShaderResourceView* nullResources[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, nullResources);

    if (!target_.EndFrame()) {
        // A TDR or a driver update removed the adapter. Everything device-bound
        // is now invalid, so stop rendering rather than repainting a dead swap
        // chain on every message.
        // TODO(M3): rebuild the device and every device-bound resource instead
        // of parking here. Needs the recreate path GraphicsDevice does not have
        // yet, and is the same plumbing a monitor hot-plug will want.
        deviceLost_ = true;
        LogError("Device lost - the dock will stay blank until it is restarted");
    }

    // Drive the next animation frame. Present blocked on vblank, so this paces
    // itself at the refresh rate; when the slide settles animating_ goes false
    // and the dock falls silent again.
    if (animating_ && !deviceLost_) {
        PostMessageW(hwnd_, kAnimateMessage, 0, 0);
    }
}

LRESULT CALLBACK DockWindow::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    DockWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<DockWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<DockWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->WndProc(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT DockWindow::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            Render();
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_NCHITTEST:
            // M0 has nothing to click. Passing every hit through keeps the dock
            // from swallowing desktop input while it is only a backdrop test.
            return HTTRANSPARENT;

        case WM_DPICHANGED:
            dpi_ = HIWORD(wParam);
            UpdatePlacement();
            return 0;

        case WM_DISPLAYCHANGE:
            UpdatePlacement();
            return 0;

        case WM_SETTINGCHANGE:
            // Only two settings matter here. Reacting to every broadcast would
            // resize the swap chain for things like a theme or locale change.
            if (wParam == SPI_SETWORKAREA) {
                UpdatePlacement();
            } else if (wParam == SPI_SETDESKWALLPAPER) {
                backdrop_.Invalidate();
                frostDirty_ = true;
                RequestRedraw();
            }
            return 0;

        case kAnimateMessage:
            Render();
            return 0;

        case WM_TIMER:
            if (wParam == kShaderWatchTimer) {
                if (shaders_ && shaders_->PollForChanges()) {
                    // A shader edit can change the frost amount, and the cached
                    // blur would otherwise keep the old radius until the dock
                    // happened to move.
                    frostDirty_ = true;
                    RequestRedraw();
                }
            } else if (wParam == kHideTimer) {
                KillTimer(hwnd_, kHideTimer);
                // One cursor check when the dwell expires, rather than polling
                // for the whole three seconds to learn the same thing.
                POINT cursor{};
                RECT bounds{};
                const bool haveCursor = GetCursorPos(&cursor) != 0;
                const bool haveBounds = GetWindowRect(hwnd, &bounds) != 0;
                const bool hovering = haveCursor && haveBounds && PtInRect(&bounds, cursor);
                LogDebug("dwell expired: cursor ({},{}) bounds ({},{})-({},{}) hovering={}",
                         cursor.x, cursor.y, bounds.left, bounds.top, bounds.right, bounds.bottom,
                         hovering ? 1 : 0);
                if (hovering) {
                    StartHideCountdown(); // still hovering, so stay out
                } else {
                    BeginHiding();
                }
            }
            return 0;

        case WM_DESTROY:
            hwnd_ = nullptr;
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace liquidock
