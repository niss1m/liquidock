#include "app/DockWindow.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <thread>

#include "core/Check.h"
#include "core/ConfigPaths.h"
#include "core/DesignTokens.h"
#include "core/Log.h"
#include "gfx/TextureDump.h"

namespace liquidock {
namespace {

constexpr wchar_t kWindowClass[] = L"LiquiDock.Dock";
constexpr UINT_PTR kShaderWatchTimer = 1;
constexpr UINT_PTR kHideTimer = 2;
// One-shot, and only for --dump-backdrop. The dump has to wait for the capture
// thread to produce its first frame, or it writes out the wallpaper the dock was
// still showing while duplication was starting up.
constexpr UINT_PTR kDumpTimer = 3;
constexpr UINT kDumpDelayMs = 2500;

// Posted to ourselves after presenting an animation frame. Present blocks on
// vblank, so this self-paces at the monitor's refresh rate without a timer -
// and unlike a timer it stops the instant the animation settles.
constexpr UINT kAnimateMessage = WM_APP + 1;
// Posted by the icon loader thread as each icon becomes available.
constexpr UINT kIconMessage = WM_APP + 2;
// Posted by the capture thread when the screen behind the dock changed.
constexpr UINT kCaptureMessage = WM_APP + 3;

enum ItemMenuCommand : UINT {
    kCommandOpen = 1,
    kCommandRemove = 2,
    kCommandEditFile = 3,
};

constexpr int kTriggerThicknessPx = 2; // the strip that notices the cursor

constexpr float kCornerRadius = design::kCornerRadius;
constexpr float kBottomMargin = design::kScreenMargin;
constexpr float kBleed = design::kBleed;

// Blur radius at frost = 1.0, in logical pixels. The default frost of 0.65
// lands at 13 px, which softens the desktop behind the dock without erasing it -
// you can still tell what is back there, which is the difference between
// frosted glass and a grey rectangle.
constexpr float kMaxFrostSigmaPx = 20.0f;

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

// Icons are stored at the size a fully magnified one needs, so that the resting
// size is always a minification and the mip chain has something to work with.
// Two sizes rather than a continuum: the atlas is reallocated when this changes,
// and there is no visible difference between 128 and, say, 144.
int CellForDpi(UINT dpi) {
    const float scale = static_cast<float>(dpi) / 96.0f;
    const float needed = design::kIconSize * design::magnify::kMaxScale * scale;
    return (needed <= 128.0f) ? 128 : 256;
}

} // namespace

DockWindow::~DockWindow() {
    Destroy();
}

bool DockWindow::Create(GraphicsDevice& device, bool diagnostic,
                        std::optional<bool> autoHideOverride, bool dumpBackdrop) {
    device_ = &device;
    diagnostic_ = diagnostic;
    autoHideOverride_ = autoHideOverride;
    dumpBackdrop_ = dumpBackdrop;
    shaders_ = std::make_unique<ShaderCache>(device.d3d());

    QueryPerformanceFrequency(&frequency_);
    QueryPerformanceCounter(&startTime_);

    // Settings before items before placement: the magnification the settings
    // ask for and the number of items together decide how wide the dock can get,
    // and that is what decides how wide the window must be.
    settings_.Load();
    store_.Load();
    layout_.SetItems(store_.items());
    ApplySettings();

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
        //
        // NOACTIVATE is what lets the dock take clicks without stealing focus
        // from whatever the user is working in - launching an app from the dock
        // must not first deactivate the app they were reading.
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

    StartIconLoad();
    ApplyBackdropSource();
    if (dumpBackdrop_) {
        SetTimer(hwnd_, kDumpTimer, kDumpDelayMs, nullptr);
    }

#ifdef LIQUIDOCK_DEBUG
    shaders_->PollForChanges(); // establish the baseline stamp
    SetTimer(hwnd_, kShaderWatchTimer, 250, nullptr);
#endif

    // Created whether or not auto-hide is on, so turning it on from the
    // settings file while the dock is running has something to arm.
    trigger_.Create([this] { Reveal(); });
    trigger_.SetEnabled(false); // the dock starts on screen
    UpdatePlacement();          // now that the trigger exists, give it its bounds

    QueryPerformanceCounter(&lastFrameTime_);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    RequestRedraw();

    // Start visible, then tuck away. Launching straight into a hidden dock
    // looks indistinguishable from launching into a broken one.
    StartHideCountdown();

    LogInfo("Dock window created at {} DPI, auto-hide {}, icon bulge {}, {} items", dpi_,
            autoHide_ ? "on" : "off", settings_.iconBulge ? "on" : "off", store_.items().size());
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
    if (capture_) {
        capture_->SetActive(true);
    }
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
    SetTimer(hwnd_, kHideTimer, static_cast<UINT>(settings_.dwellSeconds * 1000.0f), nullptr);
}

void DockWindow::BeginHiding() {
    if (!autoHide_ || menuOpen_ || revealState_ == RevealState::Hidden) {
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

    const float step =
        (settings_.slideSeconds > 0.0f) ? (deltaSeconds / settings_.slideSeconds) : 1.0f;

    if (revealState_ == RevealState::Revealing) {
        revealProgress_ = std::min(1.0f, revealProgress_ + step);
        if (revealProgress_ >= 1.0f) {
            revealState_ = RevealState::Shown;
            animating_ = false;
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
            // Nothing behind a hidden dock is worth capturing.
            if (capture_) {
                capture_->SetActive(false);
            }
        }
    }

    return Eased(revealProgress_);
}

float DockWindow::SlideOffset(float reveal) const {
    const float scale = static_cast<float>(dpi_) / 96.0f;
    const float viewHeight = static_cast<float>(target_.height()) / scale;
    if (viewHeight <= 0.0f) {
        return 0.0f;
    }
    // At reveal 0 the bar's centre sits one half-height below the bottom of the
    // window, which is the bottom of the screen; at 1 it sits at kBleed. The
    // whole slide is this one offset applied to every piece of the dock, so the
    // window itself never moves and DWM never has to redo its layout.
    return (1.0f - reveal) * (viewHeight - kBleed);
}

bool DockWindow::CursorToLayout(POINT screen, float* x, float* y) const {
    if (!hwnd_) {
        return false;
    }
    POINT client = screen;
    if (!ScreenToClient(hwnd_, &client)) {
        return false;
    }
    const float scale = static_cast<float>(dpi_) / 96.0f;
    *x = static_cast<float>(client.x) / scale;
    *y = static_cast<float>(client.y) / scale - SlideOffset(Eased(revealProgress_));
    return true;
}

bool DockWindow::CreateResources() {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(GlassConstants);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    LD_CHECK(device_->d3d()->CreateBuffer(&desc, nullptr, &constantBuffer_));

    desc.ByteWidth = sizeof(IconConstants);
    LD_CHECK(device_->d3d()->CreateBuffer(&desc, nullptr, &iconConstantBuffer_));

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

    // Premultiplied source-over. The icons are the only thing in the dock that
    // blends: the glass writes an opaque reconstruction into a cleared target,
    // and the icons land on top of it.
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    LD_CHECK(device_->d3d()->CreateBlendState(&blend, &iconBlend_));

    atlasCell_ = CellForDpi(dpi_);
    if (!atlas_.Initialize(*device_, atlasCell_, design::kMaxItems)) {
        return false;
    }

    if (!wallpaper_.Initialize(*device_) || !frost_.Initialize(*device_, *shaders_)) {
        return false;
    }
    return frost_.Resize(target_.width(), target_.height());
}

void DockWindow::Destroy() {
    // Before the window goes: both the icon loader and the capture thread post
    // to this HWND, so they have to be stopped while the handle is still valid.
    iconLoader_.Stop();
    capture_.reset();
    trigger_.Destroy();
    if (hwnd_) {
        KillTimer(hwnd_, kShaderWatchTimer);
        KillTimer(hwnd_, kHideTimer);
        KillTimer(hwnd_, kDumpTimer);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void DockWindow::RequestRedraw() {
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void DockWindow::StartIconLoad() {
    std::vector<std::wstring> paths;
    paths.reserve(store_.items().size());
    for (DockItem& item : store_.items()) {
        item.atlasSlot = -1;
        paths.push_back(item.path);
    }
    iconLoader_.Start(paths, atlasCell_, hwnd_, kIconMessage);
}

void DockWindow::DrainLoadedIcons() {
    loadedIcons_.clear();
    iconLoader_.Collect(loadedIcons_);
    if (loadedIcons_.empty()) {
        return;
    }

    for (const IconBitmap& icon : loadedIcons_) {
        if (icon.slot < 0 || icon.slot >= static_cast<int>(store_.items().size())) {
            continue; // the list changed while this one was being extracted
        }
        if (icon.size == atlasCell_ && atlas_.Upload(icon.slot, icon.pixels)) {
            store_.items()[static_cast<size_t>(icon.slot)].atlasSlot = icon.slot;
        }
    }
    // One mip regeneration for the whole batch, not one per icon.
    atlas_.FinishUpdates();
    RequestRedraw();
}

void DockWindow::ApplySettings() {
    // The command line wins over the file, so a reload cannot turn auto-hide
    // back on under someone who started the dock with --no-autohide.
    autoHide_ = autoHideOverride_.value_or(settings_.autoHide);
    layout_.SetMagnification(settings_.magnification, settings_.maxScale, settings_.influencePx,
                             settings_.iconBulge);
    // The frost is a cached blur at a particular radius, and the radius is a
    // setting, so it is stale by definition after a reload.
    frostDirty_ = true;
    if (hwnd_) {
        ApplyBackdropSource();
    }
}

void DockWindow::ReloadSettings() {
    if (!hwnd_) {
        return;
    }
    const float previousScale = settings_.maxScale;
    const float previousInfluence = settings_.influencePx;
    const bool previousAutoHide = autoHide_;

    settings_.Load();
    ApplySettings();
    LogInfo("Settings reloaded (frost {:.2f}, refraction {:.2f}, icon bulge {})", settings_.frost,
            settings_.refraction, settings_.iconBulge ? "on" : "off");

    // How wide the dock can get is a function of the magnification, and the
    // window is sized for that once rather than resized per frame - so these two
    // are the only settings that have to reach all the way back to the window.
    if (settings_.maxScale != previousScale || settings_.influencePx != previousInfluence) {
        UpdatePlacement();
    }

    if (autoHide_ != previousAutoHide) {
        if (autoHide_) {
            StartHideCountdown();
        } else {
            // Turning auto-hide off has to be able to rescue a dock that is
            // already tucked away, or the setting appears to do nothing until
            // the user goes and finds the screen edge.
            KillTimer(hwnd_, kHideTimer);
            trigger_.SetEnabled(false);
            revealState_ = RevealState::Shown;
            revealProgress_ = 1.0f;
            animating_ = false;
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
    }

    RequestRedraw();
}

Backdrop& DockWindow::ActiveBackdrop() {
    if (capture_ && capture_->ready()) {
        return *capture_;
    }
    return wallpaper_;
}

void DockWindow::ApplyBackdropSource() {
    const bool wantCapture = (settings_.backdrop == BackdropSource::Screen);
    if (wantCapture == (capture_ != nullptr)) {
        return;
    }

    if (!wantCapture) {
        capture_.reset();
        // Put the dock back in the user's screenshots the moment capture stops.
        SetWindowDisplayAffinity(hwnd_, WDA_NONE);
        LogInfo("Backdrop source: wallpaper");
        frostDirty_ = true;
        return;
    }

    // The capture thread writes into the device context, so without the
    // runtime's multithread protection this mode would silently corrupt D3D
    // state rather than merely look wrong.
    if (!device_->multithread_safe()) {
        LogWarn("Live capture needs D3D11 multithread protection, which is unavailable");
        return;
    }

    // The exclusion has to be in place *before* the first frame is captured, or
    // the dock refracts its own previous frame into an infinite mirror.
    if (!SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE)) {
        LogWarn("Could not exclude the dock from capture ({}); staying on the wallpaper backdrop",
                GetLastError());
        return;
    }

    auto capture = std::make_unique<CaptureBackdrop>();
    if (!capture->Initialize(*device_, hwnd_, kCaptureMessage)) {
        SetWindowDisplayAffinity(hwnd_, WDA_NONE);
        return;
    }
    capture_ = std::move(capture);
    capture_->SetActive(revealState_ != RevealState::Hidden);
    LogInfo("Backdrop source: live screen capture");
    frostDirty_ = true;
}

void DockWindow::ReloadItems() {
    layout_.SetItems(store_.items());
    UpdatePlacement(); // the dock's width depends on how many items it holds
    StartIconLoad();
    RequestRedraw();
}

void DockWindow::Launch(int itemIndex) {
    if (itemIndex < 0 || itemIndex >= static_cast<int>(store_.items().size())) {
        return;
    }
    const DockItem& item = store_.items()[static_cast<size_t>(itemIndex)];
    const std::wstring path = ItemStore::ExpandPath(item.path);
    LogInfo("Launching item {}", itemIndex);

    layout_.Bounce(itemIndex);
    StartHideCountdown();
    RequestRedraw();

    // ShellExecuteEx can block for a noticeable time - it may load a shell
    // extension, resolve a stale shortcut, or wait on a slow network path - and
    // blocking here would freeze the magnification mid-wave. The thread is
    // detached because it touches nothing of ours and needs no result.
    std::thread([path] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
            return;
        }
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        // NOASYNC matters on a thread that is about to exit: without it the
        // shell may hand the work to this thread's apartment and then find it
        // gone. NO_UI keeps a bad path in the config file from putting a modal
        // error box on screen.
        info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"open";
        info.lpFile = path.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info)) {
            LogWarn("Could not launch a dock item: {}", GetLastError());
        }
        CoUninitialize();
    }).detach();
}

void DockWindow::ShowItemMenu(int itemIndex, POINT screen) {
    if (itemIndex < 0 || itemIndex >= static_cast<int>(store_.items().size())) {
        return;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }
    const std::wstring label = store_.items()[static_cast<size_t>(itemIndex)].label;
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, label.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCommandOpen, L"Open");
    AppendMenuW(menu, MF_STRING, kCommandRemove, L"Remove from Dock");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCommandEditFile, L"Edit the items file…");

    // The dwell timer must not fire while a menu is up, or the dock slides away
    // from underneath the thing the user is choosing from.
    menuOpen_ = true;
    KillTimer(hwnd_, kHideTimer);

    SetForegroundWindow(hwnd_);
    const int command = TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                         screen.x, screen.y, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);

    menuOpen_ = false;
    StartHideCountdown();

    switch (command) {
        case kCommandOpen:
            Launch(itemIndex);
            break;
        case kCommandRemove:
            if (store_.Remove(static_cast<size_t>(itemIndex))) {
                // Every item after this one shifted down, and atlas slots are
                // item indices, so the icons are re-extracted rather than
                // shuffled. It costs a few tens of milliseconds on a thread
                // nobody is waiting on.
                ReloadItems();
            }
            break;
        case kCommandEditFile: {
            const std::wstring path = ItemStore::ConfigPath();
            if (!path.empty()) {
                ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        }
        default:
            break;
    }
    RequestRedraw();
}

void DockWindow::UpdatePlacement() {
    const float scale = static_cast<float>(dpi_) / 96.0f;

    // The window has to be wide enough for the dock at its widest, which is the
    // magnification wave sitting wherever produces the largest total. Sizing it
    // to the resting width and growing it per frame would mean a SetWindowPos
    // on every animation frame, and DWM redoing its layout each time.
    const float maxBar = layout_.MaxBarWidth();
    const int width = static_cast<int>(std::lround((maxBar + 2.0f * kBleed) * scale));
    // The window reaches from above the bar's resting position all the way down
    // to the screen edge, so the bar can slide entirely out of sight *within*
    // it. That makes the animation a constant fed to the shader rather than a
    // SetWindowPos on every frame.
    const int height =
        static_cast<int>(std::lround((kBottomMargin + design::kBarHeight + kBleed) * scale));

    layout_.SetWindowWidth(static_cast<float>(width) / scale);

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

    // The trigger strip spans the resting bar rather than the whole window: the
    // window is much wider than the dock looks, and an edge trigger that
    // extends well past the visible bar feels like the dock is being summoned
    // by nothing.
    const int barWidth = static_cast<int>(std::lround(layout_.RestingBarWidth() * scale));
    trigger_.SetBounds(x + (width - barWidth) / 2, work.bottom - kTriggerThicknessPx, barWidth,
                       kTriggerThicknessPx);

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

    // The capture source only copies the part of the screen the dock covers,
    // and only wakes for changes that land in it, so it has to be told where
    // that is before it is asked for a frame.
    RECT windowRect{};
    GetWindowRect(hwnd_, &windowRect);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (GetMonitorInfoW(monitor, &monitorInfo) && capture_) {
        const RECT& bounds = monitorInfo.rcMonitor;
        const RECT region{windowRect.left - bounds.left, windowRect.top - bounds.top,
                          windowRect.right - bounds.left, windowRect.bottom - bounds.top};
        capture_->SetRegion(region);
    }

    if (wallpaper_.Update(monitor)) {
        frostDirty_ = true;
    }
    if (capture_ && capture_->Update(monitor)) {
        frostDirty_ = true;
    }
    // A capture that failed outright - no duplication on this adapter - is
    // dropped here rather than retried forever, and the dock carries on with
    // the wallpaper it already has.
    if (capture_ && capture_->failed()) {
        LogWarn("Live capture unavailable; falling back to the wallpaper backdrop");
        capture_.reset();
        SetWindowDisplayAffinity(hwnd_, WDA_NONE);
        frostDirty_ = true;
    }

    Backdrop& backdrop = ActiveBackdrop();
    const RECT monitorRect = backdrop.monitor_rect();
    const POINT windowOrigin{windowRect.left - monitorRect.left, windowRect.top - monitorRect.top};
    const SIZE windowSize{static_cast<LONG>(target_.width()),
                          static_cast<LONG>(target_.height())};

    const float frostSigma = settings_.frost * kMaxFrostSigmaPx * scale;
    if (frostDirty_ || !frost_.ready()) {
        if (frost_.Build(backdrop, windowOrigin, windowSize, frostSigma)) {
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
    const float slide = SlideOffset(reveal);

    // The cursor is read here rather than tracked from WM_MOUSEMOVE. A move
    // message says where the cursor *was* when it was posted, and the dock also
    // moves underneath a stationary cursor during the slide - so the honest
    // question every frame is "where is the cursor now, relative to the dock as
    // it is being drawn".
    POINT cursor{};
    float cursorX = 0.0f;
    float cursorY = 0.0f;
    bool hovered = false;
    if (!menuOpen_ && GetCursorPos(&cursor) && CursorToLayout(cursor, &cursorX, &cursorY)) {
        hovered = layout_.Contains(cursorX, cursorY);
    }
    layout_.SetCursor(cursorX, hovered);
    const bool layoutMoving = layout_.Advance(deltaSeconds);

    GlassConstants constants{};
    constants.viewportCenter[0] = viewWidth;
    constants.viewportCenter[1] = viewHeight;
    constants.viewportCenter[2] = layout_.bar_center_x() * scale;
    constants.viewportCenter[3] = (DockLayout::bar_center_y() + slide) * scale;
    constants.shape[0] = layout_.bar_half_width() * scale;
    constants.shape[1] = DockLayout::bar_half_height() * scale;
    constants.shape[2] = kCornerRadius * scale;
    constants.shape[3] = elapsed;

    constants.light[0] = Radians(settings_.lightAngleDegrees);
    constants.light[1] = settings_.lightIntensity;
    constants.light[2] = settings_.refraction;
    constants.light[3] = settings_.depth;
    constants.material[0] = settings_.dispersion;
    constants.material[1] = settings_.frost;
    constants.material[2] = settings_.splay;
    constants.material[3] = backdrop.tiled() ? 1.0f : 0.0f;

    constants.windowOrigin[0] = static_cast<float>(windowOrigin.x);
    constants.windowOrigin[1] = static_cast<float>(windowOrigin.y);
    constants.windowOrigin[2] = static_cast<float>(monitorRect.right - monitorRect.left);
    constants.windowOrigin[3] = static_cast<float>(monitorRect.bottom - monitorRect.top);

    float uvScale[2]{};
    float uvOffset[2]{};
    backdrop.uv_scale(uvScale);
    backdrop.uv_offset(uvOffset);
    constants.backdropUv[0] = uvScale[0];
    constants.backdropUv[1] = uvScale[1];
    constants.backdropUv[2] = uvOffset[0];
    constants.backdropUv[3] = uvOffset[1];

    // The bulges under raised icons. At rest there are none, and the shader's
    // loop does not run at all.
    const auto& lenses = layout_.lenses();
    const int lensCount =
        std::min(static_cast<int>(lenses.size()), static_cast<int>(design::kMaxLenses));
    constants.lensInfo[0] = static_cast<float>(lensCount);
    constants.lensInfo[1] = design::magnify::kFuse * scale;
    for (int i = 0; i < lensCount; ++i) {
        constants.lens[i][0] = lenses[static_cast<size_t>(i)].centerX * scale;
        constants.lens[i][1] = (lenses[static_cast<size_t>(i)].centerY + slide) * scale;
        constants.lens[i][2] = lenses[static_cast<size_t>(i)].halfWidth * scale;
        constants.lens[i][3] = lenses[static_cast<size_t>(i)].halfHeight * scale;
    }

    if (diagnostic_) {
        constants.tint[0] = 1.0f;
        constants.tint[1] = 0.0f;
        constants.tint[2] = 1.0f;
        constants.tint[3] = 1.0f;
    } else {
        memcpy(constants.tint, design::kBarTint, sizeof(constants.tint));
        constants.tint[3] = settings_.tintAlpha;
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

    ID3D11ShaderResourceView* resources[2] = {backdrop.srv(), frost_.srv()};
    ctx->PSSetShaderResources(0, 2, resources);
    ID3D11SamplerState* samplers[1] = {sampler_.Get()};
    ctx->PSSetSamplers(0, 1, samplers);

    // No blend state: the target was just cleared to zero and the shader writes
    // premultiplied colour, so blending would only cost bandwidth.
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullResources[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, nullResources);

    RenderIcons(scale, slide);

    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
    ctx->PSSetShaderResources(0, 2, nullResources);

    device_->DrainDebugMessages();

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
    // itself at the refresh rate; when the slide and the springs have both
    // settled the dock falls silent again and presents nothing at all.
    if ((animating_ || layoutMoving) && !deviceLost_) {
        PostMessageW(hwnd_, kAnimateMessage, 0, 0);
    }
}

void DockWindow::RenderIcons(float scale, float slideLogical) {
    ComPtr<ID3D11VertexShader> vs = shaders_->VertexShader("Icon", "VSMain");
    ComPtr<ID3D11PixelShader> ps = shaders_->PixelShader("Icon", "PSMain");
    if (!vs || !ps || !atlas_.srv()) {
        return;
    }

    IconConstants constants{};
    const float viewWidth = static_cast<float>(target_.width());
    const float viewHeight = static_cast<float>(target_.height());
    constants.viewport[0] = viewWidth;
    constants.viewport[1] = viewHeight;
    constants.viewport[2] = 1.0f / std::max(viewWidth, 1.0f);
    constants.viewport[3] = 1.0f / std::max(viewHeight, 1.0f);
    constants.cell[0] = static_cast<float>(atlas_.cell()) / static_cast<float>(atlas_.width());
    constants.cell[1] = static_cast<float>(atlas_.cell()) / static_cast<float>(atlas_.height());

    int instances = 0;
    for (const PlacedIcon& icon : layout_.icons()) {
        if (instances >= kMaxIconInstances) {
            break;
        }
        const size_t index = static_cast<size_t>(icon.itemIndex);
        if (index >= store_.items().size()) {
            continue;
        }
        const int slot = store_.items()[index].atlasSlot;
        if (slot < 0) {
            continue; // still being extracted; the dock draws without it
        }

        const float half = icon.size * 0.5f * scale;
        constants.rect[instances][0] = icon.centerX * scale;
        constants.rect[instances][1] = (icon.centerY + slideLogical) * scale;
        constants.rect[instances][2] = half;
        constants.rect[instances][3] = half;

        constants.source[instances][0] =
            static_cast<float>((slot % atlas_.columns()) * atlas_.cell()) /
            static_cast<float>(atlas_.width());
        constants.source[instances][1] =
            static_cast<float>((slot / atlas_.columns()) * atlas_.cell()) /
            static_cast<float>(atlas_.height());
        constants.source[instances][2] = 1.0f;
        constants.source[instances][3] = 0.0f;
        ++instances;
    }

    for (const PlacedIcon& hairline : layout_.separators()) {
        if (instances >= kMaxIconInstances) {
            break;
        }
        // A one-logical-pixel rule has to be snapped to the physical grid or it
        // lands across two columns and renders as two half-lit ones, which at
        // 20% white is a line that looks like it failed to draw.
        const float width = std::max(1.0f, std::round(design::kSeparatorWidth * scale));
        const float left = std::round(hairline.centerX * scale - width * 0.5f);
        constants.rect[instances][0] = left + width * 0.5f;
        constants.rect[instances][1] = (hairline.centerY + slideLogical) * scale;
        constants.rect[instances][2] = width * 0.5f;
        constants.rect[instances][3] = design::kSeparatorHeight * 0.5f * scale;

        constants.source[instances][2] = design::kSeparatorTint[3];
        constants.source[instances][3] = 1.0f; // solid fill, not an atlas sample
        ++instances;
    }

    if (instances == 0) {
        return;
    }

    ID3D11DeviceContext1* ctx = device_->context();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(iconConstantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    memcpy(mapped.pData, &constants, sizeof(constants));
    ctx->Unmap(iconConstantBuffer_.Get(), 0);

    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    ID3D11Buffer* cb = iconConstantBuffer_.Get();
    ctx->VSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetConstantBuffers(0, 1, &cb);

    ID3D11ShaderResourceView* resources[1] = {atlas_.srv()};
    ctx->PSSetShaderResources(0, 1, resources);
    ctx->OMSetBlendState(iconBlend_.Get(), nullptr, 0xFFFFFFFF);
    ctx->DrawInstanced(6, static_cast<UINT>(instances), 0, 0);
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

        case WM_NCHITTEST: {
            // The window is far larger than the dock looks - it carries bleed on
            // every side for the rim, the bulge and raised icons - so anything
            // outside the dock's actual silhouette has to fall through to
            // whatever is underneath, or the dock would swallow desktop clicks
            // across a band the user cannot see.
            if (revealState_ == RevealState::Hidden) {
                return HTTRANSPARENT;
            }
            const POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            float x = 0.0f;
            float y = 0.0f;
            if (CursorToLayout(screen, &x, &y) && layout_.Contains(x, y)) {
                return HTCLIENT;
            }
            return HTTRANSPARENT;
        }

        case WM_MOUSEACTIVATE:
            // Take the click, but do not become the active window and do not
            // eat the click doing it. WS_EX_NOACTIVATE already stops the
            // activation; saying so here is what stops the system trying, which
            // otherwise flickers focus away from whatever the user is in.
            return MA_NOACTIVATE;

        case WM_MOUSEMOVE: {
            if (!mouseTracking_) {
                TRACKMOUSEEVENT track{sizeof(track)};
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                mouseTracking_ = TrackMouseEvent(&track) != 0;
            }
            // The move message is only a nudge: Render reads the cursor's
            // actual position. What matters here is that something changed and
            // a frame is now worth presenting.
            RequestRedraw();
            return 0;
        }

        case WM_MOUSELEAVE:
            mouseTracking_ = false;
            pressedItem_ = -1;
            StartHideCountdown();
            RequestRedraw();
            return 0;

        case WM_LBUTTONDOWN: {
            float x = 0.0f;
            float y = 0.0f;
            POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd, &screen);
            pressedItem_ = CursorToLayout(screen, &x, &y) ? layout_.ItemAt(x, y) : -1;
            return 0;
        }

        case WM_LBUTTONUP: {
            float x = 0.0f;
            float y = 0.0f;
            POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd, &screen);
            const int released = CursorToLayout(screen, &x, &y) ? layout_.ItemAt(x, y) : -1;
            // Press and release have to agree, so dragging off an icon cancels
            // the launch the way a button does.
            if (released >= 0 && released == pressedItem_) {
                Launch(released);
            }
            pressedItem_ = -1;
            return 0;
        }

        case WM_RBUTTONUP: {
            float x = 0.0f;
            float y = 0.0f;
            POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd, &screen);
            if (CursorToLayout(screen, &x, &y)) {
                const int item = layout_.ItemAt(x, y);
                if (item >= 0) {
                    ShowItemMenu(item, screen);
                }
            }
            return 0;
        }

        case kIconMessage:
            DrainLoadedIcons();
            return 0;

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam);
            UpdatePlacement();
            // Icons are stored at the size a magnified one needs, so a DPI
            // change can mean a different atlas and a fresh extraction.
            const int cell = CellForDpi(dpi_);
            if (cell != atlasCell_) {
                atlasCell_ = cell;
                if (atlas_.Initialize(*device_, atlasCell_, design::kMaxItems)) {
                    StartIconLoad();
                }
            }
            return 0;
        }

        case WM_DISPLAYCHANGE:
            UpdatePlacement();
            return 0;

        case WM_SETTINGCHANGE:
            // Only two settings matter here. Reacting to every broadcast would
            // resize the swap chain for things like a theme or locale change.
            if (wParam == SPI_SETWORKAREA) {
                UpdatePlacement();
            } else if (wParam == SPI_SETDESKWALLPAPER) {
                wallpaper_.Invalidate();
                frostDirty_ = true;
                RequestRedraw();
            }
            return 0;

        case kCaptureMessage:
            // The screen behind the dock changed. The frost is a blur of it, so
            // it is stale; the glass samples the capture texture directly and
            // simply picks up the new contents.
            frostDirty_ = true;
            RequestRedraw();
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
            } else if (wParam == kDumpTimer) {
                KillTimer(hwnd_, kDumpTimer);
                DumpTextureToBmp(*device_, ActiveBackdrop().srv(),
                                 ConfigFilePath(L"backdrop.bmp"));
            } else if (wParam == kHideTimer) {
                KillTimer(hwnd_, kHideTimer);
                // One cursor check when the dwell expires, rather than polling
                // for the whole three seconds to learn the same thing.
                POINT cursor{};
                float x = 0.0f;
                float y = 0.0f;
                const bool hovering = !menuOpen_ && GetCursorPos(&cursor) &&
                                      CursorToLayout(cursor, &x, &y) && layout_.Contains(x, y);
                if (hovering || menuOpen_) {
                    StartHideCountdown(); // still in use, so stay out
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
