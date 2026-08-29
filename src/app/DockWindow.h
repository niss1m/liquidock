#pragma once

#include <windows.h>

#include <memory>

#include "app/EdgeTrigger.h"
#include "gfx/CompositionTarget.h"
#include "gfx/GraphicsDevice.h"
#include "gfx/ShaderCache.h"
#include "glass/FrostChain.h"
#include "glass/WallpaperBackdrop.h"

namespace liquidock {

// Mirrors GlassConstants in shaders/Glass.hlsl. Both sides are float4-packed so
// the layouts cannot drift apart silently.
struct GlassConstants {
    float viewportCenter[4]; // xy = viewport size (px), zw = rect centre (px)
    float shape[4];          // xy = rect half size (px), z = corner radius (px), w = time (s)
    float light[4];          // x = angle (rad), y = intensity, z = refraction, w = depth
    float material[4];       // x = dispersion, y = frost, z = splay, w = tiled flag
    float tint[4];           // straight alpha
    float windowOrigin[4];   // xy = window origin (monitor px), zw = monitor size (px)
    float backdropUv[4];     // xy = uv scale, zw = uv offset
};
static_assert(sizeof(GlassConstants) == 112, "GlassConstants must match the HLSL cbuffer");
static_assert(sizeof(GlassConstants) % 16 == 0, "Constant buffers must be 16-byte aligned");

// The dock's own window: a click-through, always-on-top, per-pixel-alpha
// surface that owns nothing but the glass. Items, input and animation arrive in
// M2; for now this exists to prove the composition path works on real hardware.
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
    bool Create(GraphicsDevice& device, bool diagnostic = false, bool autoHide = true);
    void Destroy();

    HWND hwnd() const { return hwnd_; }

    // Marks the window dirty. The dock never renders on a timer - a frame is
    // presented only in response to something that actually changed.
    void RequestRedraw();

    // Slides the dock into view and restarts its dwell timer. Called when the
    // cursor reaches the screen edge, and safe to call repeatedly.
    void Reveal();

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    // Takes the HWND as a parameter rather than reading hwnd_: WM_DESTROY
    // clears the member, and WM_NCDESTROY arrives afterwards and still has to
    // reach DefWindowProc with a valid handle.
    LRESULT WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool CreateResources();
    void UpdatePlacement();
    void Render();

    // Advances the slide animation. Returns the eased 0..1 position, where 0 is
    // fully tucked below the screen edge and 1 is fully out.
    float AdvanceReveal(float deltaSeconds);
    void StartHideCountdown();
    void BeginHiding();

    GraphicsDevice* device_ = nullptr;
    std::unique_ptr<ShaderCache> shaders_;
    CompositionTarget target_;
    WallpaperBackdrop backdrop_;
    FrostChain frost_;

    HWND hwnd_ = nullptr;
    ComPtr<ID3D11Buffer> constantBuffer_;
    ComPtr<ID3D11SamplerState> sampler_;
    // Set whenever the cached frost is stale: the dock moved or resized, the
    // wallpaper changed, or a shader edit may have changed the frost amount.
    bool frostDirty_ = true;
    UINT dpi_ = 96;
    bool diagnostic_ = false;
    bool deviceLost_ = false;

    // Auto-hide. The dock is a window that is mostly not there: it sits tucked
    // below the screen edge until EdgeTrigger reports the cursor arriving, then
    // slides out, dwells, and slides back.
    enum class RevealState { Hidden, Revealing, Shown, Hiding };
    EdgeTrigger trigger_;
    RevealState revealState_ = RevealState::Shown;
    float revealProgress_ = 1.0f; // 0 = tucked away, 1 = fully out
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
