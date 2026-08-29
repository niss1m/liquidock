#include "app/DockWindow.h"

#include <shellapi.h>
#include <shobjidl_core.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
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
// Debounces the running-indicator rescan. Opening a folder creates a burst of
// windows and one scan afterwards answers for all of them.
constexpr UINT_PTR kRunningTimer = 4;
constexpr UINT kRunningDebounceMs = 250;
// Retries after the graphics adapter goes away. A driver reset (a TDR, or an
// update installing while the dock runs) takes a couple of seconds to come
// back, so the first few attempts are close together and then it gives up
// rather than spinning a timer for the rest of the session.
// The hover label's idle dismissal. Restarted on every cursor movement, so it
// only ever fires once the cursor has actually been still.
constexpr UINT_PTR kLabelTimer = 6;

constexpr UINT_PTR kDeviceRetryTimer = 5;
constexpr UINT kDeviceRetryMs = 1500;
constexpr int kMaxDeviceRetries = 10;

// Posted to ourselves after presenting an animation frame. Present blocks on
// vblank, so this self-paces at the monitor's refresh rate without a timer -
// and unlike a timer it stops the instant the animation settles.
constexpr UINT kAnimateMessage = WM_APP + 1;
// Posted by the icon loader thread as each icon becomes available.
constexpr UINT kIconMessage = WM_APP + 2;
// Posted by the capture thread when the screen behind the dock changed.
constexpr UINT kCaptureMessage = WM_APP + 3;
// Posted by the WinEvent hook when a top-level window appeared or vanished.
constexpr UINT kRunningMessage = WM_APP + 4;
// The shell's appbar notifications - another appbar appeared, the taskbar moved.
constexpr UINT kAppBarMessage = WM_APP + 5;
// Posted by the tray when Preferences is chosen.
constexpr UINT kShowSettingsMessage = WM_APP + 7;
// --simulate-device-loss, posted once at startup.
constexpr UINT kSimulateDeviceLossMessage = WM_APP + 6;
constexpr UINT_PTR kSimulateLossTimer = 6;
constexpr UINT kSimulateLossDelayMs = 3000;
// --stats only.
constexpr UINT_PTR kStatsTimer = 7;

enum ItemMenuCommand : UINT {
    kCommandOpen = 1,
    kCommandRemove = 2,
    kCommandAdd = 3,
    kCommandPreferences = 4,
    kCommandEditFile = 5,
    kCommandQuit = 6,
};

constexpr int kTriggerThicknessPx = 2; // the strip that notices the cursor

constexpr float kCornerRadius = design::kCornerRadius;
constexpr float kBottomMargin = design::kScreenMargin;
constexpr float kBleed = design::kBleed;

// Blur radius at frost = 1.0, in logical pixels. The body of the panel is
// always fully this blurred image, so the number is now the whole story: the
// default of 0.82 lands at 26 px, heavy enough that what is behind the dock
// reads as depth rather than as detail, and light enough that you can still tell
// a window from a wallpaper.
constexpr float kMaxFrostSigmaPx = 32.0f;

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
                        std::optional<bool> autoHideOverride, bool dumpBackdrop,
                        bool simulateDeviceLoss, bool stats) {
    device_ = &device;
    diagnostic_ = diagnostic;
    autoHideOverride_ = autoHideOverride;
    dumpBackdrop_ = dumpBackdrop;
    simulateDeviceLoss_ = simulateDeviceLoss;
    stats_ = stats;
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
    running_.Initialize(hwnd_, kRunningMessage);
    menu_.Initialize(*device_, *shaders_);
    if (dumpBackdrop_) {
        SetTimer(hwnd_, kDumpTimer, kDumpDelayMs, nullptr);
    }
    if (simulateDeviceLoss_) {
        SetTimer(hwnd_, kSimulateLossTimer, kSimulateLossDelayMs, nullptr);
    }
    if (stats_) {
        QueryPerformanceCounter(&statsSince_);
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
    // The bar's own measurements, which are otherwise only checkable with a
    // screenshot - and a screenshot of a dock is exactly what another dock
    // sitting on the same screen edge will quietly ruin.
    LogInfo("Bar {:.0f}x{:.0f} logical at {:.0f}% of design size, icons {:.0f} px, "
            "bottom {:.0f} px above the edge",
            layout_.RestingBarWidth(), layout_.bar_half_height() * 2.0f,
            layout_.scale() * 100.0f, design::kIconSize * layout_.scale(),
            design::kScreenMargin);
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
    if (settingsWindow_ && settingsWindow_->visible()) {
        return; // the dock stays put while its own preferences are open
    }
    revealState_ = (revealProgress_ >= 1.0f) ? RevealState::Shown : revealState_;

    QueryPerformanceCounter(&hideDeadline_);
    hideDeadline_.QuadPart +=
        static_cast<LONGLONG>(settings_.dwellSeconds * static_cast<float>(frequency_.QuadPart));
    hidePending_ = true;

    // The timer is the backstop for when the dock is completely idle and never
    // renders again; the deadline above is what actually gets checked while it
    // is busy.
    SetTimer(hwnd_, kHideTimer, static_cast<UINT>(settings_.dwellSeconds * 1000.0f), nullptr);
}

void DockWindow::CheckHideDeadline() {
    if (!hidePending_ || !autoHide_ || !hwnd_) {
        return;
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (now.QuadPart < hideDeadline_.QuadPart) {
        return;
    }

    hidePending_ = false;
    KillTimer(hwnd_, kHideTimer);

    // One cursor check when the dwell expires, rather than polling for the
    // whole three seconds to learn the same thing.
    POINT cursor{};
    float x = 0.0f;
    float y = 0.0f;
    const bool hovering = !menuOpen_ && GetCursorPos(&cursor) && CursorToLayout(cursor, &x, &y) &&
                          layout_.Contains(x, y);
    const bool busy = menuOpen_ || (settingsWindow_ && settingsWindow_->visible());
    if (hovering || busy) {
        StartHideCountdown(); // still in use, so stay out
    } else {
        BeginHiding();
    }
}

void DockWindow::BeginHiding() {
    if (!autoHide_ || menuOpen_ || revealState_ == RevealState::Hidden) {
        return;
    }
    if (settingsWindow_ && settingsWindow_->visible()) {
        return;
    }
    revealState_ = RevealState::Hiding;
    hidePending_ = false;
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

    if (!text_.Initialize(*device_, settings_.labelFontSize,
                          settings_.labelBold ? DWRITE_FONT_WEIGHT_BOLD
                                              : DWRITE_FONT_WEIGHT_SEMI_BOLD,
                          L"Segoe UI")) {
        return false;
    }

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
    running_.Shutdown();
    settingsWindow_.reset();
    // Before anything else: an appbar registration that outlives its window
    // leaves the work area permanently short with nothing on screen to explain
    // why, until the user logs out.
    appBar_.Unregister();
    trigger_.Destroy();
    if (hwnd_) {
        KillTimer(hwnd_, kShaderWatchTimer);
        KillTimer(hwnd_, kHideTimer);
        KillTimer(hwnd_, kDumpTimer);
        KillTimer(hwnd_, kRunningTimer);
        KillTimer(hwnd_, kDeviceRetryTimer);
        KillTimer(hwnd_, kSimulateLossTimer);
        KillTimer(hwnd_, kStatsTimer);
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
    std::vector<DockItem> snapshot;
    snapshot.reserve(store_.items().size());
    for (DockItem& item : store_.items()) {
        item.atlasSlot = -1;
        snapshot.push_back(item);
    }
    iconLoader_.Start(std::move(snapshot), atlasCell_, hwnd_, kIconMessage);
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
        DockItem& item = store_.items()[static_cast<size_t>(icon.slot)];
        item.executable = icon.target;
        if (icon.size == atlasCell_ && atlas_.Upload(icon.slot, icon.pixels)) {
            item.atlasSlot = icon.slot;
        }
    }
    // One mip regeneration for the whole batch, not one per icon.
    atlas_.FinishUpdates();

    std::vector<std::wstring> executables;
    executables.reserve(store_.items().size());
    for (const DockItem& item : store_.items()) {
        executables.push_back(item.executable);
    }
    running_.SetTargets(std::move(executables));
    running_.Refresh();

    RequestRedraw();
}

void DockWindow::ApplySettings() {
    // The command line wins over the file, so a reload cannot turn auto-hide
    // back on under someone who started the dock with --no-autohide.
    autoHide_ = autoHideOverride_.value_or(settings_.autoHide);
    layout_.SetIconScale(settings_.iconSize / design::kIconSize);
    layout_.SetMagnification(settings_.magnification, settings_.maxScale, settings_.influencePx,
                             settings_.iconBulge);
    // The frost is a cached blur at a particular radius, and the radius is a
    // setting, so it is stale by definition after a reload.
    frostDirty_ = true;
    if (hwnd_) {
        ApplyBackdropSource();
    }
}

UINT DockWindow::show_settings_message() {
    return kShowSettingsMessage;
}

void DockWindow::ReloadSettings() {
    if (!hwnd_) {
        return;
    }
    const float previousScale = settings_.maxScale;
    const float previousInfluence = settings_.influencePx;
    const float previousIconSize = settings_.iconSize;
    const float previousFontSize = settings_.labelFontSize;
    const bool previousBold = settings_.labelBold;
    const bool previousAutoHide = autoHide_;

    settings_.Load();
    ApplySettings();
    LogInfo("Settings reloaded (frost {:.2f}, refraction {:.2f}, icon bulge {})", settings_.frost,
            settings_.refraction, settings_.iconBulge ? "on" : "off");

    // How wide the dock can get is a function of the magnification, and the
    // window is sized for that once rather than resized per frame - so these two
    // are the only settings that have to reach all the way back to the window.
    if (settings_.maxScale != previousScale || settings_.influencePx != previousInfluence ||
        settings_.iconSize != previousIconSize) {
        UpdatePlacement();
    }

    // The text format is built once and holds the size and weight, so a change
    // to either has to rebuild it - editing the file and watching the label
    // change is the whole point of these two being settings.
    if (device_ && (settings_.labelFontSize != previousFontSize ||
                    settings_.labelBold != previousBold)) {
        text_.Initialize(*device_, settings_.labelFontSize,
                         settings_.labelBold ? DWRITE_FONT_WEIGHT_BOLD
                                             : DWRITE_FONT_WEIGHT_SEMI_BOLD,
                         L"Segoe UI");
        RequestRedraw();
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

void DockWindow::ReleaseDeviceResources() {
    // The preferences window draws with the same device, so it goes first and
    // is simply rebuilt the next time it is asked for.
    settingsWindow_.reset();

    // The capture thread holds the device context, so it has to be stopped
    // before anything is released rather than merely told to stop.
    capture_.reset();

    menu_.Destroy();
    text_.Reset();
    frost_.Reset();
    atlas_.Reset();
    wallpaper_.Reset();
    target_.Reset();
    iconBlend_.Reset();
    sampler_.Reset();
    iconConstantBuffer_.Reset();
    constantBuffer_.Reset();
    // Compiled shaders belong to the dead device; the cache is rebuilt against
    // the new one, which also re-reads the .hlsl files.
    shaders_.reset();
}

void DockWindow::HandleDeviceLost() {
    if (deviceLost_) {
        return;
    }
    deviceLost_ = true;
    deviceRetries_ = 0;
    animating_ = false;
    LogWarn("Graphics device lost; the dock will keep trying to rebuild it");
    ReleaseDeviceResources();
    SetTimer(hwnd_, kDeviceRetryTimer, kDeviceRetryMs, nullptr);
}

bool DockWindow::RecoverDevice() {
    if (!device_->Initialize()) {
        return false;
    }

    // Everything below is exactly what Create does after the window exists. The
    // window itself survives - it is the one thing that was never the device's.
    shaders_ = std::make_unique<ShaderCache>(device_->d3d());

    RECT client{};
    GetClientRect(hwnd_, &client);
    if (!target_.Initialize(*device_, hwnd_, static_cast<UINT>(std::max<LONG>(client.right, 1)),
                            static_cast<UINT>(std::max<LONG>(client.bottom, 1)))) {
        return false;
    }
    if (!CreateResources()) {
        return false;
    }

    // The atlas is gone, so the icons have to be extracted again. They come
    // back over the next fraction of a second, exactly as at startup.
    StartIconLoad();
    ApplyBackdropSource();

    frostDirty_ = true;
    deviceLost_ = false;
    LogInfo("Graphics device rebuilt after {} attempt(s)", deviceRetries_);
    RequestRedraw();
    return true;
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

void DockWindow::AddItemViaDialog() {
    // The dwell timer must not slide the dock away while a modal dialog it
    // opened is on screen.
    menuOpen_ = true;
    KillTimer(hwnd_, kHideTimer);
    DockItem item;
    const bool picked = ItemStore::PickProgram(hwnd_, &item);
    menuOpen_ = false;
    StartHideCountdown();

    if (picked && store_.Add(std::move(item))) {
        ReloadItems();
    }
}

void DockWindow::ReloadItemsFromDisk() {
    store_.Load();
    ReloadItems();
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
    const std::wstring arguments = ItemStore::ExpandPath(item.arguments);
    const std::wstring directory = ItemStore::ExpandPath(item.workingDirectory);
    LogInfo("Launching item {}", itemIndex);

    layout_.Bounce(itemIndex);
    StartHideCountdown();
    RequestRedraw();

    // ShellExecuteEx can block for a noticeable time - it may load a shell
    // extension, resolve a stale shortcut, or wait on a slow network path - and
    // blocking here would freeze the magnification mid-wave. The thread is
    // detached because it touches nothing of ours and needs no result.
    std::thread([path, arguments, directory] {
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
        // Null rather than an empty string: ShellExecuteEx treats "" as a real
        // (empty) argument for some verbs, and as a working directory of "".
        info.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
        info.lpDirectory = directory.empty() ? nullptr : directory.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info)) {
            LogWarn("Could not launch a dock item: {}", GetLastError());
        }
        CoUninitialize();
    }).detach();
}

void DockWindow::ShowDockMenu(int itemIndex, POINT screen) {
    const bool onItem = itemIndex >= 0 && itemIndex < static_cast<int>(store_.items().size());

    std::vector<GlassMenu::Item> items;
    // The item commands are the top of the menu when there is an item, and
    // simply absent when the click landed on the bar - rather than a fixed menu
    // with half its entries greyed out, which tells the user nothing.
    if (onItem) {
        items.push_back({0, store_.items()[static_cast<size_t>(itemIndex)].label, false, false,
                         true});
        items.push_back({0, L"", false, true, false});
        items.push_back({kCommandOpen, L"Open", true, false, false});
        items.push_back({kCommandRemove, L"Remove from Dock", true, false, false});
        items.push_back({0, L"", false, true, false});
    }
    items.push_back({kCommandAdd, L"Add app…", true, false, false});
    items.push_back({kCommandPreferences, L"Preferences…", true, false, false});
    items.push_back({kCommandEditFile, L"Edit the items file…", true, false, false});
    items.push_back({0, L"", false, true, false});
    items.push_back({kCommandQuit, L"Quit LiquiDock", true, false, false});

    // The dwell timer must not fire while a menu the dock opened is up.
    menuOpen_ = true;
    KillTimer(hwnd_, kHideTimer);
    const UINT command = menu_.Track(std::move(items), screen);
    menuOpen_ = false;
    StartHideCountdown();

    switch (command) {
        case kCommandOpen:
            Launch(itemIndex);
            break;
        case kCommandRemove:
            if (onItem && store_.Remove(static_cast<size_t>(itemIndex))) {
                // Every item after this one shifted down, and atlas slots are
                // item indices, so the icons are re-extracted rather than
                // shuffled. It costs a few tens of milliseconds on a thread
                // nobody is waiting on.
                ReloadItems();
            }
            break;
        case kCommandAdd:
            AddItemViaDialog();
            break;
        case kCommandPreferences:
            PostMessageW(hwnd_, kShowSettingsMessage, 0, 0);
            break;
        case kCommandQuit:
            PostQuitMessage(0);
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

HMONITOR DockWindow::TargetMonitor() const {
    if (settings_.monitorIndex <= 0) {
        return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }

    struct Search {
        int wanted;
        int seen;
        HMONITOR found;
    } search{settings_.monitorIndex, 0, nullptr};

    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
            auto* state = reinterpret_cast<Search*>(param);
            if (++state->seen == state->wanted) {
                state->found = monitor;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));

    // An index that no longer exists - a monitor was unplugged - falls back to
    // the primary rather than leaving the dock on a screen that is not there.
    return search.found ? search.found : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

void DockWindow::UpdatePlacement() {
    // The monitor is resolved first and the DPI is read from *it*, not from the
    // window. Asking the window would answer for wherever it currently is,
    // which on a mixed-DPI desktop is the wrong scale to size it for the screen
    // it is about to move to.
    HMONITOR monitor = TargetMonitor();
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        dpi_ = dpiX;
    }
    const float scale = static_cast<float>(dpi_) / 96.0f;

    // How much room there is, before deciding how much is wanted. A dock of
    // forty-odd items is wider than a screen at full icon size, and one running
    // off both edges is worse than one drawn slightly smaller.
    MONITORINFO fit{sizeof(fit)};
    if (GetMonitorInfoW(monitor, &fit)) {
        const float availableLogical =
            static_cast<float>(fit.rcWork.right - fit.rcWork.left) / scale - 2.0f * kBleed;
        const float applied = layout_.FitWithin(availableLogical);
        if (applied < 1.0f) {
            LogInfo("Dock scaled to {:.0f}% to fit {} items on screen", applied * 100.0f,
                    store_.items().size());
        }
    }

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

    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) {
        return;
    }

    // Reserving space and being positioned by it are the same decision: an
    // appbar's own band is carved out of the work area, so a dock that both
    // reserved a band and then positioned itself against the work area would
    // walk up the screen a little further on every placement.
    const bool reserve = settings_.reserveSpace && !autoHide_;
    RECT area = info.rcWork;
    if (reserve) {
        appBar_.Register(hwnd_, kAppBarMessage);
        const int band = static_cast<int>(
            std::lround((design::kBarHeight + design::kScreenMargin) * scale));
        RECT reserved{};
        if (appBar_.Reserve(info.rcMonitor, band, &reserved)) {
            area = reserved;
        }
    } else {
        appBar_.Release();
    }

    const int x = area.left + ((area.right - area.left) - width) / 2;
    const int y = area.bottom - height;

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW);

    // The trigger strip spans the resting bar rather than the whole window: the
    // window is much wider than the dock looks, and an edge trigger that
    // extends well past the visible bar feels like the dock is being summoned
    // by nothing.
    const int barWidth = static_cast<int>(std::lround(layout_.RestingBarWidth() * scale));
    trigger_.SetBounds(x + (width - barWidth) / 2, area.bottom - kTriggerThicknessPx, barWidth,
                       kTriggerThicknessPx);

    // Moving the dock invalidates the cached frost even at an unchanged size:
    // it is a crop of the wallpaper at a particular position.
    frostDirty_ = true;

    if (target_.width() > 0) {
        target_.Resize(static_cast<UINT>(width), static_cast<UINT>(height));
        text_.Invalidate();
        frost_.Resize(static_cast<UINT>(width), static_cast<UINT>(height));
        RequestRedraw();
    }
}

void DockWindow::Render() {
    if (deviceLost_) {
        return;
    }
    LARGE_INTEGER renderStart{};
    if (stats_) {
        QueryPerformanceCounter(&renderStart);
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
    HMONITOR monitor = TargetMonitor();

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

    LARGE_INTEGER backdropStart{};
    if (stats_) {
        QueryPerformanceCounter(&backdropStart);
    }
    // The wallpaper is only the stand-in until live capture has a frame. Once
    // it does, asking about the wallpaper at all is pure cost.
    if (!capture_ || !capture_->ready()) {
        if (wallpaper_.Update(monitor)) {
            frostDirty_ = true;
        }
    }
    if (capture_ && capture_->Update(monitor)) {
        frostDirty_ = true;
    }
    if (stats_) {
        LARGE_INTEGER backdropEnd{};
        QueryPerformanceCounter(&backdropEnd);
        statsBackdropMs_ +=
            1000.0 * static_cast<double>(backdropEnd.QuadPart - backdropStart.QuadPart) /
            static_cast<double>(frequency_.QuadPart);
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
    // The capture texture belongs to another device. Everything that samples it
    // - the frost chain and the glass draw alike - has to sit inside the keyed
    // mutex, so it is taken for the whole frame and given back after Present.
    const bool sharedBackdrop = (&backdrop == static_cast<Backdrop*>(capture_.get()));
    if (sharedBackdrop && !capture_->BeginRead()) {
        RequestRedraw(); // the copy is still in flight; try again next message
        return;
    }
    const RECT monitorRect = backdrop.monitor_rect();
    const POINT windowOrigin{windowRect.left - monitorRect.left, windowRect.top - monitorRect.top};
    const SIZE windowSize{static_cast<LONG>(target_.width()),
                          static_cast<LONG>(target_.height())};

    const float frostSigma = settings_.frost * kMaxFrostSigmaPx * scale;
    LARGE_INTEGER frostStart{};
    if (stats_) {
        QueryPerformanceCounter(&frostStart);
    }
    if (frostDirty_ || !frost_.ready()) {
        if (frost_.Build(backdrop, windowOrigin, windowSize, frostSigma)) {
            frostDirty_ = false;
        }
    }
    if (stats_) {
        LARGE_INTEGER frostEnd{};
        QueryPerformanceCounter(&frostEnd);
        statsFrostMs_ += 1000.0 * static_cast<double>(frostEnd.QuadPart - frostStart.QuadPart) /
                         static_cast<double>(frequency_.QuadPart);
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

    CheckHideDeadline();

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
    constants.viewportCenter[3] = (layout_.bar_center_y() + slide) * scale;
    constants.shape[0] = layout_.bar_half_width() * scale;
    constants.shape[1] = layout_.bar_half_height() * scale;
    constants.shape[2] = layout_.corner_radius() * scale;
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
    // The shader works in physical pixels but the edge band and the rim are
    // quoted in logical ones, because a two-pixel rim at 200% DPI is a one-pixel
    // rim, which is to say no rim at all.
    constants.lensInfo[2] = scale;
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
        if (sharedBackdrop) {
            capture_->EndRead();
        }
        return;
    }
    memcpy(mapped.pData, &constants, sizeof(constants));
    ctx->Unmap(constantBuffer_.Get(), 0);

    LARGE_INTEGER waitStart{};
    if (stats_) {
        QueryPerformanceCounter(&waitStart);
    }
    ID3D11RenderTargetView* rtv = target_.BeginFrame();
    if (stats_) {
        LARGE_INTEGER waitEnd{};
        QueryPerformanceCounter(&waitEnd);
        statsWaitMs_ += 1000.0 * static_cast<double>(waitEnd.QuadPart - waitStart.QuadPart) /
                        static_cast<double>(frequency_.QuadPart);
    }
    if (!rtv) {
        if (sharedBackdrop) {
            capture_->EndRead();
        }
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

    // Text last, straight onto the finished frame with Direct2D. The pipeline
    // state is not restored afterwards because nothing else draws before the
    // present, and every stage sets what it needs on the way in.
    const bool labelMoving = RenderHoverLabel(scale, slide, deltaSeconds);

    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
    ctx->PSSetShaderResources(0, 2, nullResources);

    device_->DrainDebugMessages();

    LARGE_INTEGER presentStart{};
    if (stats_) {
        QueryPerformanceCounter(&presentStart);
    }
    const bool presented = target_.EndFrame();
    if (stats_) {
        LARGE_INTEGER presentEnd{};
        QueryPerformanceCounter(&presentEnd);
        statsPresentMs_ +=
            1000.0 * static_cast<double>(presentEnd.QuadPart - presentStart.QuadPart) /
            static_cast<double>(frequency_.QuadPart);
    }
    if (sharedBackdrop) {
        capture_->EndRead();
    }

    if (!presented) {
        // A TDR, or a driver updating itself while the dock runs. Everything
        // device-bound is invalid now, so tear it down and start trying to
        // build it again rather than repainting a dead swap chain forever.
        HandleDeviceLost();
        return;
    }

    if (stats_) {
        LARGE_INTEGER renderEnd{};
        QueryPerformanceCounter(&renderEnd);
        const double ms = 1000.0 * static_cast<double>(renderEnd.QuadPart - renderStart.QuadPart) /
                          static_cast<double>(frequency_.QuadPart);
        ++statsFrames_;
        statsRenderMs_ += ms;
        statsWorstMs_ = std::max(statsWorstMs_, ms);

        const double sinceMs =
            1000.0 * static_cast<double>(renderEnd.QuadPart - statsSince_.QuadPart) /
            static_cast<double>(frequency_.QuadPart);
        if (sinceMs >= 1000.0) {
            const unsigned acquired = capture_ ? capture_->acquired() : 0;
            const unsigned copied = capture_ ? capture_->copied() : 0;
            const unsigned pointer = capture_ ? capture_->pointerOnly() : 0;
            const unsigned throttled = capture_ ? capture_->throttled() : 0;
            LogInfo("STATS {} frames in {:.0f} ms, avg {:.2f} (wait {:.2f}, present {:.2f}), "
                    "worst {:.2f} | backdrop {:.2f} frost {:.2f} | capture acquired {} "
                    "pointer-only {} throttled {} copied {}",
                    statsFrames_, sinceMs, statsRenderMs_ / statsFrames_,
                    statsWaitMs_ / statsFrames_, statsPresentMs_ / statsFrames_, statsWorstMs_,
                    statsBackdropMs_ / statsFrames_, statsFrostMs_ / statsFrames_,
                    acquired - statsCaptureBase_, pointer - statsPointerBase_,
                    throttled - statsThrottledBase_, copied - statsCopiedBase_);
            statsFrames_ = 0;
            statsRenderMs_ = 0.0;
            statsWorstMs_ = 0.0;
            statsWaitMs_ = 0.0;
            statsPresentMs_ = 0.0;
            statsBackdropMs_ = 0.0;
            statsFrostMs_ = 0.0;
            statsCaptureBase_ = acquired;
            statsCopiedBase_ = copied;
            statsPointerBase_ = pointer;
            statsThrottledBase_ = throttled;
            statsSince_ = renderEnd;
        }
    }

    // Drive the next animation frame. Present blocked on vblank, so this paces
    // itself at the refresh rate; when the slide and the springs have both
    // settled the dock falls silent again and presents nothing at all.
    if ((animating_ || layoutMoving || labelMoving) && !deviceLost_) {
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

    // Running indicators. They sit on the resting icon row while the icon above
    // them magnifies, which is what keeps the row of dots reading as a row.
    const float indicatorY = layout_.icon_row_bottom() +
                             (design::kIndicatorGap + design::kIndicatorDiameter * 0.5f) *
                                 layout_.scale();
    for (const PlacedIcon& icon : layout_.icons()) {
        if (instances >= kMaxIconInstances) {
            break;
        }
        const size_t index = static_cast<size_t>(icon.itemIndex);
        if (index >= store_.items().size() || !running_.IsRunning(index)) {
            continue;
        }
        const float half = design::kIndicatorDiameter * 0.5f * layout_.scale() * scale;
        constants.rect[instances][0] = icon.centerX * scale;
        constants.rect[instances][1] = (indicatorY + slideLogical) * scale;
        constants.rect[instances][2] = half;
        constants.rect[instances][3] = half;

        constants.source[instances][2] = design::kIndicatorTint[3];
        constants.source[instances][3] = 2.0f; // solid, clipped to a disc
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
        constants.rect[instances][3] = hairline.size * 0.5f * scale;

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

bool DockWindow::RenderHoverLabel(float scale, float slideLogical, float deltaSeconds) {
    // Which icon is under the cursor, decided from the same placement the frame
    // was just drawn with rather than from a mouse message, so the label cannot
    // point at where an icon used to be.
    int wanted = -1;
    bool onDock = false;
    POINT cursor{};
    float x = 0.0f;
    float y = 0.0f;
    const bool haveCursor = GetCursorPos(&cursor) != 0;
    if (!menuOpen_ && revealState_ != RevealState::Hidden && haveCursor &&
        CursorToLayout(cursor, &x, &y)) {
        onDock = layout_.Contains(x, y);
        wanted = layout_.ItemAt(x, y);
    }

    // Movement is what wakes the label and what keeps it up. Every move
    // restarts the idle timer rather than polling for a second to learn the
    // same thing, and un-dismisses a label a click had put away.
    if (haveCursor && (cursor.x != lastCursor_.x || cursor.y != lastCursor_.y)) {
        lastCursor_ = cursor;
        labelSuppressed_ = false;
        if (hwnd_ && onDock) {
            SetTimer(hwnd_, kLabelTimer, design::label::kIdleMs, nullptr);
        }
    }

    // Between two icons ItemAt finds nothing, and letting that clear the label
    // made it blink out in every gap along the row - which is most of what
    // looked like it "going away". The name holds until another icon claims it
    // or the cursor leaves the dock altogether.
    if (wanted < 0 && onDock) {
        wanted = labelItem_;
    }
    if (!onDock || labelSuppressed_) {
        wanted = -1;
    }

    // The name changes the instant the cursor crosses onto another icon.
    // Handing over used to mean fading the old label out before the new one
    // could begin, which put two fades - nearly a fifth of a second - between
    // pointing at something and being told what it is. That was most of what
    // still read as lag once the magnification itself was instant.
    labelItem_ = wanted;

    const float target = (labelItem_ >= 0) ? 1.0f : 0.0f;
    const float step = (design::label::kFadeSeconds > 0.0f)
                           ? deltaSeconds / design::label::kFadeSeconds
                           : 1.0f;
    if (labelAlpha_ < target) {
        labelAlpha_ = std::min(target, labelAlpha_ + step);
    } else if (labelAlpha_ > target) {
        labelAlpha_ = std::max(target, labelAlpha_ - step);
    }

    const bool moving = (labelAlpha_ != target);
    if (labelAlpha_ <= 0.001f || labelItem_ < 0 ||
        labelItem_ >= static_cast<int>(store_.items().size()) || !text_.ready()) {
        // Nothing showing, so the next label starts where its icon is rather
        // than sliding in from wherever the last one happened to end up.
        labelPlaced_ = false;
        return moving;
    }

    // The icon this label belongs to, as placed in the frame just drawn.
    const PlacedIcon* icon = nullptr;
    for (const PlacedIcon& placed : layout_.icons()) {
        if (placed.itemIndex == labelItem_) {
            icon = &placed;
            break;
        }
    }
    if (!icon) {
        return moving;
    }

    const std::wstring& name = store_.items()[static_cast<size_t>(labelItem_)].label;
    if (!text_.Begin(target_.swap_chain(), scale)) {
        return moving;
    }

    const float width = text_.MeasureWidth(name) + 2.0f * design::label::kPaddingX;
    // From the setting rather than the token: kLabelHeight is sized for the
    // largest font the setting allows, because that is what the window has to
    // reserve headroom for, not what this label is actually drawn at.
    const float height = settings_.labelFontSize + 2.0f * design::label::kPaddingY;
    // Measured off where a *fully magnified* icon reaches, not off this icon's
    // current top. The icon under the cursor is at full size anyway, so the
    // tail still lands just above it - but the height no longer rides the swell,
    // and the label holds one line as the cursor travels the row.
    const float iconTop = layout_.magnified_icon_top() + slideLogical;

    // The pill travels to the next icon instead of teleporting. Exponential
    // approach, so it is frame-rate independent and has no overshoot to settle.
    const float targetX = icon->centerX;
    if (!labelPlaced_) {
        labelX_ = targetX;
        labelPlaced_ = true;
    } else if (deltaSeconds > 0.0f) {
        labelX_ += (targetX - labelX_) *
                   (1.0f - std::exp(-deltaSeconds / design::label::kSlideTau));
    }
    const bool sliding = std::fabs(targetX - labelX_) > 0.3f;

    const float viewWidth = static_cast<float>(target_.width()) / scale;
    float left = labelX_ - width * 0.5f;
    // Kept inside the window: an icon at either end would otherwise have half
    // its name clipped away by the swap chain.
    left = std::clamp(left, 4.0f, std::max(4.0f, viewWidth - width - 4.0f));
    // The gap is measured to the tip of the tail, so the pill itself sits a
    // tail's height further up.
    const float point = iconTop - design::label::kGap;
    const float bottom = point - design::label::kTailHeight;
    const D2D1_RECT_F pill = D2D1::RectF(left, bottom - height, left + width, bottom);

    auto colour = [](const float rgba[4], float alpha) {
        return D2D1::ColorF(rgba[0], rgba[1], rgba[2], rgba[3] * alpha);
    };
    // The tail points at the icon, not at the middle of the pill: at either end
    // of the row the pill is pushed inward to stay on screen, and a tail centred
    // on it would then be pointing at a neighbour.
    text_.FillTooltip(pill, design::label::kRadius, icon->centerX, design::label::kTailWidth,
                      design::label::kTailHeight, colour(design::label::kFill, labelAlpha_),
                      colour(design::label::kEdge, labelAlpha_));
    text_.Draw(name, pill, colour(design::label::kText, labelAlpha_));
    text_.End();

    return moving || sliding;
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
            // Once you have clicked, you know what the thing was called. The
            // next cursor movement brings the label back.
            labelSuppressed_ = true;
            KillTimer(hwnd, kLabelTimer);
            RequestRedraw();
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
                ShowDockMenu(layout_.ItemAt(x, y), screen);
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

        case kSimulateDeviceLossMessage:
            // --simulate-device-loss. A real TDR cannot be provoked without
            // hanging the whole GPU, which is not something to do on someone's
            // desktop, so the recovery path is exercised by taking the same
            // branch the failed Present takes.
            LogInfo("Simulating a lost graphics device");
            HandleDeviceLost();
            return 0;

        case kShowSettingsMessage: {
            if (deviceLost_) {
                return 0; // nothing to draw it with
            }
            if (!settingsWindow_) {
                settingsWindow_ = std::make_unique<SettingsWindow>();
                if (!settingsWindow_->Create(
                        *device_, settings_,
                        [this](const Settings& next) {
                        // Applied before the file is even written, so dragging a
                        // slider changes the dock under the cursor.
                            settings_ = next;
                            ApplySettings();
                            UpdatePlacement();
                            RequestRedraw();
                        },
                        [this] { ReloadItemsFromDisk(); })) {
                    settingsWindow_.reset();
                    return 0;
                }
            }
            settingsWindow_->Show(TargetMonitor());
            // The dock must not slide away while its own preferences are open.
            KillTimer(hwnd_, kHideTimer);
            return 0;
        }

        case kAppBarMessage:
            // ABN_POSCHANGED: the taskbar moved or another appbar appeared, so
            // the band we were given may no longer be where we think it is.
            if (wParam == ABN_POSCHANGED || wParam == ABN_FULLSCREENAPP) {
                UpdatePlacement();
            }
            return 0;

        case kRunningMessage:
            // Debounced rather than acted on directly: a single app launch can
            // fire this a dozen times as its windows appear.
            SetTimer(hwnd_, kRunningTimer, kRunningDebounceMs, nullptr);
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
            } else if (wParam == kLabelTimer) {
                // A second with the cursor still. Put the name away - it has
                // been read by now, and leaving it up parks a black pill over
                // whatever is behind the dock.
                KillTimer(hwnd_, kLabelTimer);
                if (labelItem_ >= 0) {
                    labelSuppressed_ = true;
                    RequestRedraw();
                }
            } else if (wParam == kSimulateLossTimer) {
                KillTimer(hwnd_, kSimulateLossTimer);
                PostMessageW(hwnd_, kSimulateDeviceLossMessage, 0, 0);
            } else if (wParam == kDeviceRetryTimer) {
                if (RecoverDevice()) {
                    KillTimer(hwnd_, kDeviceRetryTimer);
                } else if (++deviceRetries_ >= kMaxDeviceRetries) {
                    KillTimer(hwnd_, kDeviceRetryTimer);
                    LogError("Giving up on the graphics device after {} attempts; "
                             "the dock will stay blank until it is restarted",
                             deviceRetries_);
                } else {
                    // Initialize() leaves a partly-built device behind when it
                    // fails partway, and the next attempt has to start clean.
                    device_->Reset();
                }
            } else if (wParam == kRunningTimer) {
                KillTimer(hwnd_, kRunningTimer);
                // Only redraw if the answer actually changed; most window
                // events are something else opening and closing.
                if (running_.Refresh()) {
                    RequestRedraw();
                }
            } else if (wParam == kDumpTimer) {
                KillTimer(hwnd_, kDumpTimer);
                DumpTextureToBmp(*device_, ActiveBackdrop().srv(),
                                 ConfigFilePath(L"backdrop.bmp"));
            } else if (wParam == kHideTimer) {
                CheckHideDeadline();
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
