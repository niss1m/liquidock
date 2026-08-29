#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "gfx/CompositionTarget.h"
#include "gfx/GraphicsDevice.h"
#include "gfx/ShaderCache.h"
#include "gfx/TextLayer.h"
#include "glass/FrostChain.h"
#include "glass/SnapshotBackdrop.h"

namespace liquidock {

// The dock's context menu, made of the same glass as the dock.
//
// A Win32 popup menu would be one call, and it would look like a Win32 popup
// menu sitting under a sheet of glass - which is exactly the seam this project
// exists to avoid. So it is the same shader, the same corner radius and the
// same rim, with DirectWrite for the text on top.
//
// The backdrop is deliberately the wallpaper rather than the live screen even
// when the dock is capturing. The capture only covers the strip the dock sits
// in, and a menu opens somewhere else; giving the menu its own duplication for
// the second and a half it is on screen would cost far more than it is worth.
//
// Track() runs its own message loop and returns the chosen command, the way
// TrackPopupMenu does, so calling code reads the same as it did before.
class GlassMenu {
public:
    struct Item {
        UINT id = 0;
        std::wstring label;
        bool enabled = true;
        bool separator = false;
        // A heading: the name of the thing the menu is about. Not selectable.
        bool header = false;
    };

    GlassMenu() = default;
    GlassMenu(const GlassMenu&) = delete;
    GlassMenu& operator=(const GlassMenu&) = delete;
    ~GlassMenu();

    bool Initialize(GraphicsDevice& device, ShaderCache& shaders);
    void Destroy();

    // Shows the menu at `screen` and does not return until something is chosen
    // or it is dismissed. 0 means dismissed.
    UINT Track(std::vector<Item> items, POINT screen);

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    void Measure();
    void Place(POINT screen);
    void Render();
    int ItemAt(float x, float y) const;
    void Choose(int index);

    GraphicsDevice* device_ = nullptr;
    ShaderCache* shaders_ = nullptr;
    SnapshotBackdrop backdrop_;

    HWND hwnd_ = nullptr;
    CompositionTarget target_;
    FrostChain frost_;
    TextLayer text_;
    ComPtr<ID3D11Buffer> constantBuffer_;
    ComPtr<ID3D11SamplerState> sampler_;

    std::vector<Item> items_;
    std::vector<float> tops_; // each item's top edge, logical, within the panel

    UINT dpi_ = 96;
    float width_ = 0.0f;  // the glass panel, logical
    float height_ = 0.0f;
    int hover_ = -1;
    UINT chosen_ = 0;
    bool running_ = false;
};

} // namespace liquidock
