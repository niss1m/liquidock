#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "gfx/CompositionTarget.h"
#include "gfx/GraphicsDevice.h"
#include "gfx/ShaderCache.h"
#include "gfx/TextLayer.h"

namespace liquidock {

// The dock's context menu, drawn as the hover label's bigger sibling.
//
// A Win32 popup menu would be one call, and it would look like a Win32 popup
// menu hanging off a sheet of glass - which is exactly the seam this project
// exists to avoid.
//
// It was glass, and it should not have been. Two things hang off the dock - the
// name of the icon under the cursor, and this - and making them different
// materials reads as two different programs. The label got there first and got
// it right: flat black, no border, and a tail pointing back at what it came
// from. So this is the same shape, the same colour and the same tail, and it is
// also far cheaper than the glass version, which had to snapshot the screen
// behind it and run a blur chain before it could show anything.
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

    HWND hwnd_ = nullptr;
    CompositionTarget target_;
    TextLayer text_;

    std::vector<Item> items_;
    std::vector<float> tops_; // each item's top edge, logical, within the panel

    UINT dpi_ = 96;
    float width_ = 0.0f;  // the panel, logical
    // Where the tail meets the panel's bottom edge, in the panel's own space.
    float tailCenterX_ = 0.0f;
    float height_ = 0.0f;
    int hover_ = -1;
    UINT chosen_ = 0;
    bool running_ = false;
};

} // namespace liquidock
