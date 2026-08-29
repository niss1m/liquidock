#pragma once

#include <windows.h>

#include <memory>
#include <optional>
#include <vector>

#include "app/AppBar.h"
#include "app/DockLayout.h"
#include "app/EdgeTrigger.h"
#include "core/DesignTokens.h"
#include "core/Settings.h"
#include "gfx/CompositionTarget.h"
#include "gfx/GraphicsDevice.h"
#include "gfx/IconAtlas.h"
#include "gfx/ShaderCache.h"
#include "gfx/TextLayer.h"
#include "glass/CaptureBackdrop.h"
#include "glass/FrostChain.h"
#include "glass/GlassConstants.h"
#include "glass/WallpaperBackdrop.h"
#include "model/IconLoader.h"
#include "model/ItemStore.h"
#include "model/RunningState.h"
#include "ui/GlassMenu.h"
#include "ui/SettingsWindow.h"

namespace liquidock {

// Mirrors IconConstants in shaders/Icon.hlsl. Worst case is every item drawn
// with a running indicator under it, plus the hairline between the two groups -
// all of it one instanced draw.
inline constexpr int kMaxIconInstances = design::kMaxItems * 2 + 1;
struct IconConstants {
    float viewport[4];                     // xy = viewport size (px), zw = 1 / size
    float cell[4];                         // xy = one atlas cell in uv
    float rect[kMaxIconInstances][4];      // xy = centre (px), zw = half size (px)
    float source[kMaxIconInstances][4];    // xy = uv origin, z = opacity, w = solid
};
static_assert(sizeof(IconConstants) % 16 == 0, "Constant buffers must be 16-byte aligned");

// The dock's own window: an always-on-top, per-pixel-alpha surface that owns
// the glass, the icon row and the input that drives both.
class DockWindow {
public:
    DockWindow() = default;
    DockWindow(const DockWindow&) = delete;
    DockWindow& operator=(const DockWindow&) = delete;
    ~DockWindow();

    // `diagnostic` renders the bar in flat opaque magenta instead of glass. The
    // design fill is 5% white, which is correct but nearly invisible on its
    // own - so if the composition path were broken on a given driver, a normal
    // render would look much the same as a failed one. This makes the two
    // impossible to confuse.
    // `autoHideOverride` is the command line having its say; empty means the
    // settings file decides. Kept, so a settings reload cannot quietly turn
    // auto-hide back on under someone who launched with --no-autohide.
    bool Create(GraphicsDevice& device, bool diagnostic = false,
                std::optional<bool> autoHideOverride = std::nullopt, bool dumpBackdrop = false,
                bool simulateDeviceLoss = false, bool stats = false);
    void Destroy();

    HWND hwnd() const { return hwnd_; }

    // Marks the window dirty. The dock never renders on a timer - a frame is
    // presented only in response to something that actually changed.
    void RequestRedraw();

    // Slides the dock into view and restarts its dwell timer. Called when the
    // cursor reaches the screen edge, and safe to call repeatedly.
    void Reveal();

    // Re-reads settings.txt and applies it. Called from the message loop when
    // the config directory watch reports the file was saved.
    void ReloadSettings();

    // The private message the tray posts to open the preferences window.
    static UINT show_settings_message();

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    // Takes the HWND as a parameter rather than reading hwnd_: WM_DESTROY
    // clears the member, and WM_NCDESTROY arrives afterwards and still has to
    // reach DefWindowProc with a valid handle.
    LRESULT WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool CreateResources();
    // Which screen the dock lives on, from the settings.
    HMONITOR TargetMonitor() const;
    void UpdatePlacement();
    void Render();
    void RenderIcons(float scale, float slideLogical);
    // The name of the icon under the cursor, drawn above it. Returns true while
    // the fade is still running, so the dock keeps presenting frames until it
    // has settled.
    bool RenderHoverLabel(float scale, float slideLogical, float deltaSeconds);

    // Advances the slide animation. Returns the eased 0..1 position, where 0 is
    // fully tucked below the screen edge and 1 is fully out.
    float AdvanceReveal(float deltaSeconds);
    void StartHideCountdown();
    // Hides if the dwell has run out and nothing is using the dock. Called both
    // from the dwell timer and from the render loop, because a WM_TIMER is only
    // synthesised when the message queue is empty - and while live capture is
    // running the queue very often is not.
    void CheckHideDeadline();
    void BeginHiding();

    // How far, in logical pixels, the dock is currently pushed below its
    // revealed position. 0 when fully out.
    float SlideOffset(float reveal) const;
    // Screen point to logical dock space, with the slide taken back out, so
    // hit tests are against the layout's own coordinates.
    bool CursorToLayout(POINT screen, float* x, float* y) const;

    // Drops everything bound to the graphics device, rebuilds the device, and
    // builds it all again. Returns false while the adapter is still gone.
    void ReleaseDeviceResources();
    bool RecoverDevice();
    // Called when the adapter goes away: parks rendering and starts retrying.
    void HandleDeviceLost();

    // Pushes the current settings into everything that caches them.
    void ApplySettings();
    // Starts or stops live screen capture to match the settings.
    void ApplyBackdropSource();
    // Whichever source is actually usable this frame. Capture only wins once it
    // has produced a frame, so switching modes never flashes an empty backdrop
    // and a capture that cannot start is simply never used.
    Backdrop& ActiveBackdrop();

    void ReloadItems();
    void StartIconLoad();
    void DrainLoadedIcons();
    void Launch(int itemIndex);
    // One menu for the whole dock, which grows the item commands when the click
    // landed on an icon. `itemIndex` of -1 means the bar itself.
    void ShowDockMenu(int itemIndex, POINT screen);
    // Asks for a program with the standard file dialog and appends it.
    void AddItemViaDialog();
    // Re-reads items.txt and rebuilds everything that depends on it.
    void ReloadItemsFromDisk();

    GraphicsDevice* device_ = nullptr;
    std::unique_ptr<ShaderCache> shaders_;
    CompositionTarget target_;
    WallpaperBackdrop wallpaper_;
    std::unique_ptr<CaptureBackdrop> capture_;
    FrostChain frost_;

    HWND hwnd_ = nullptr;
    ComPtr<ID3D11Buffer> constantBuffer_;
    ComPtr<ID3D11Buffer> iconConstantBuffer_;
    ComPtr<ID3D11SamplerState> sampler_;
    // Premultiplied source-over. The glass writes straight into a cleared
    // target and needs no blending; the icons land on top of it and do.
    ComPtr<ID3D11BlendState> iconBlend_;
    TextLayer text_;
    // Which item the label is for, and how far it has faded in. The index is
    // held through the fade-out so the name does not vanish the instant the
    // cursor leaves the icon.
    int labelItem_ = -1;
    float labelAlpha_ = 0.0f;
    // Dismissed by a click, or by the cursor sitting still - and put back by
    // the next movement.
    bool labelSuppressed_ = false;
    POINT lastCursor_{};
    // Set whenever the cached frost is stale: the dock moved or resized, the
    // wallpaper changed, or a shader edit may have changed the frost amount.
    bool frostDirty_ = true;
    UINT dpi_ = 96;
    bool diagnostic_ = false;
    bool deviceLost_ = false;
    // How many times we have tried to get the device back. A driver reset takes
    // a few seconds; a driver that is gone for good should not be retried
    // forever, or the dock spins a timer for the rest of the session.
    int deviceRetries_ = 0;
    // --dump-backdrop: write the backdrop texture out once, then stop. The only
    // way to inspect a live capture, which by construction cannot be screenshot.
    bool dumpBackdrop_ = false;
    // --simulate-device-loss: takes the device-lost branch a few seconds in, so
    // the recovery path can be tested without provoking a real TDR.
    bool simulateDeviceLoss_ = false;
    // --stats: once a second, how many frames were presented and what they
    // cost. Guessing at where a frame goes is how you end up optimising the
    // wrong thing.
    bool stats_ = false;
    unsigned statsFrames_ = 0;
    double statsRenderMs_ = 0.0;
    double statsWorstMs_ = 0.0;
    double statsWaitMs_ = 0.0;
    double statsPresentMs_ = 0.0;
    double statsBackdropMs_ = 0.0;
    double statsFrostMs_ = 0.0;
    LARGE_INTEGER statsSince_{};
    unsigned statsCaptureBase_ = 0;
    unsigned statsCopiedBase_ = 0;
    unsigned statsPointerBase_ = 0;
    unsigned statsThrottledBase_ = 0;

    // Items, their icons and their geometry.
    Settings settings_;
    std::optional<bool> autoHideOverride_;
    ItemStore store_;
    DockLayout layout_;
    IconLoader iconLoader_;
    IconAtlas atlas_;
    RunningState running_;
    std::unique_ptr<SettingsWindow> settingsWindow_;
    GlassMenu menu_;
    std::vector<IconBitmap> loadedIcons_;
    int atlasCell_ = 0;
    int pressedItem_ = -1;
    bool menuOpen_ = false;
    bool mouseTracking_ = false;

    // Auto-hide. The dock is a window that is mostly not there: it sits tucked
    // below the screen edge until EdgeTrigger reports the cursor arriving, then
    // slides out, dwells, and slides back.
    enum class RevealState { Hidden, Revealing, Shown, Hiding };
    EdgeTrigger trigger_;
    AppBar appBar_;
    RevealState revealState_ = RevealState::Shown;
    float revealProgress_ = 1.0f; // 0 = tucked away, 1 = fully out
    // When the dock is due to slide away, as a timestamp rather than only as a
    // timer, so a busy message queue cannot postpone it indefinitely.
    LARGE_INTEGER hideDeadline_{};
    bool hidePending_ = false;
    bool autoHide_ = true;
    // True only while a slide is in flight. The animation drives itself by
    // posting a message after each Present rather than running on a timer, so
    // it paces at the monitor's refresh rate and stops dead when it settles.
    bool animating_ = false;
    LARGE_INTEGER lastFrameTime_{};
    LARGE_INTEGER startTime_{};
    LARGE_INTEGER frequency_{};
};

} // namespace liquidock
