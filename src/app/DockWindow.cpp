#include "app/DockWindow.h"

#include <cmath>
#include <cstring>
#include <numbers>

#include "core/Check.h"
#include "core/Log.h"

namespace liquidock {
namespace {

constexpr wchar_t kWindowClass[] = L"LiquiDock.Dock";
constexpr UINT_PTR kShaderWatchTimer = 1;

// Provisional geometry, in logical pixels at 96 DPI. M2 derives all of this
// from the item list and the configured icon size; the numbers here exist only
// to put something on screen that is the right shape.
constexpr float kDockWidth = 720.0f;
constexpr float kDockHeight = 92.0f;
constexpr float kCornerRadius = 26.0f;
constexpr float kBottomMargin = 16.0f;

// The window is grown past the glass on every side so the rim highlight, and
// later the drop shadow and the icons that overshoot the dock while magnified,
// have somewhere to land.
constexpr float kBleed = 40.0f;

float Radians(float degrees) {
    return degrees * std::numbers::pi_v<float> / 180.0f;
}

} // namespace

DockWindow::~DockWindow() {
    Destroy();
}

bool DockWindow::Create(GraphicsDevice& device) {
    device_ = &device;
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

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    RequestRedraw();
    LogInfo("Dock window created at {} DPI", dpi_);
    return true;
}

bool DockWindow::CreateResources() {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(GlassConstants);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    LD_CHECK(device_->d3d()->CreateBuffer(&desc, nullptr, &constantBuffer_));
    return true;
}

void DockWindow::Destroy() {
    if (hwnd_) {
        KillTimer(hwnd_, kShaderWatchTimer);
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
    const int height = static_cast<int>(std::lround((kDockHeight + 2.0f * kBleed) * scale));

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
    const int y = work.bottom - height - static_cast<int>(std::lround(kBottomMargin * scale)) +
                  static_cast<int>(std::lround(kBleed * scale));

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW);

    if (target_.width() > 0) {
        target_.Resize(static_cast<UINT>(width), static_cast<UINT>(height));
        RequestRedraw();
    }
}

void DockWindow::Render() {
    ID3D11RenderTargetView* rtv = target_.BeginFrame();
    if (!rtv) {
        return;
    }

    ComPtr<ID3D11VertexShader> vs = shaders_->VertexShader("Glass", "VSMain");
    ComPtr<ID3D11PixelShader> ps = shaders_->PixelShader("Glass", "PSMain");
    if (!vs || !ps) {
        return; // Compile error already logged; the next save fixes it.
    }

    ID3D11DeviceContext1* ctx = device_->context();

    const float scale = static_cast<float>(dpi_) / 96.0f;
    const float viewWidth = static_cast<float>(target_.width());
    const float viewHeight = static_cast<float>(target_.height());

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const float elapsed = static_cast<float>(now.QuadPart - startTime_.QuadPart) /
                          static_cast<float>(frequency_.QuadPart);

    GlassConstants constants{};
    constants.viewportCenter[0] = viewWidth;
    constants.viewportCenter[1] = viewHeight;
    constants.viewportCenter[2] = viewWidth * 0.5f;
    constants.viewportCenter[3] = viewHeight * 0.5f;
    constants.shape[0] = kDockWidth * 0.5f * scale;
    constants.shape[1] = kDockHeight * 0.5f * scale;
    constants.shape[2] = kCornerRadius * scale;
    constants.shape[3] = elapsed;

    // Defaults lifted straight from the Figma glass panel: light -45 degrees at
    // 80%, refraction 80, depth 20, dispersion 50, frost 4, splay 100.
    constants.light[0] = Radians(-45.0f);
    constants.light[1] = 0.80f;
    constants.light[2] = 0.80f; // refraction, consumed in M1
    constants.light[3] = 0.20f; // depth
    constants.material[0] = 0.50f; // dispersion, consumed in M1
    constants.material[1] = 0.04f; // frost, consumed in M1
    constants.material[2] = 1.00f; // splay
    constants.material[3] = 0.0f;

    constants.tint[0] = 0.10f;
    constants.tint[1] = 0.11f;
    constants.tint[2] = 0.13f;
    constants.tint[3] = 0.55f;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    memcpy(mapped.pData, &constants, sizeof(constants));
    ctx->Unmap(constantBuffer_.Get(), 0);

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
    // No blend state: the target was just cleared to zero and the shader writes
    // premultiplied colour, so blending would only cost bandwidth.
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->Draw(3, 0);

    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);

    target_.EndFrame();
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
        return self->WndProc(message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT DockWindow::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd_, &ps);
            Render();
            EndPaint(hwnd_, &ps);
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
            // Only the work area matters here. Reacting to every broadcast
            // setting change would repeatedly resize the swap chain for things
            // like a theme or locale change.
            if (wParam == SPI_SETWORKAREA) {
                UpdatePlacement();
            }
            return 0;

        case WM_TIMER:
            if (wParam == kShaderWatchTimer && shaders_ && shaders_->PollForChanges()) {
                RequestRedraw();
            }
            return 0;

        case WM_DESTROY:
            hwnd_ = nullptr;
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

} // namespace liquidock
