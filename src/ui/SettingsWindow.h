#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <functional>
#include <string>
#include <vector>

#include "core/Settings.h"
#include "model/IconLoader.h"
#include "model/ItemStore.h"
#include "gfx/CompositionTarget.h"
#include "gfx/GraphicsDevice.h"

namespace liquidock {

// The preferences window.
//
// Custom-drawn with Direct2D rather than assembled from common controls. That
// is more work, and it is the whole point: the promise this project makes is
// preferences that were designed rather than accumulated, and a Win32 dialog
// full of grey checkboxes is exactly the thing being replaced. It also means
// every control can say what it is *for* underneath its name, which a settings
// dialog almost never has room to do and which is most of why settings dialogs
// are intimidating.
//
// Every change applies to the running dock immediately. There is no OK button,
// because the only way to judge a value like `frost` is to see it, and a
// preview that needs confirming is not a preview.
//
// Organised into tabs rather than one wall of controls. Three columns of
// everything at once is a screenshot of a settings file, not a design: it makes
// the reader do the grouping, and it gives the dock's own contents - the thing
// people actually come here to change - a narrow column at the edge.
class SettingsWindow {
public:
    // Called on every change, with the new settings, so the dock can apply them
    // before the file has even been written.
    using ChangedCallback = std::function<void(const Settings&)>;
    // Called when the dock's contents change, so the dock can re-read them.
    using ItemsCallback = std::function<void()>;

    SettingsWindow() = default;
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;
    ~SettingsWindow();

    bool Create(GraphicsDevice& device, const Settings& settings, ChangedCallback onChanged,
                ItemsCallback onItemsChanged);
    void Destroy();

    // Brings the window up, centred on `nearMonitor`, loading the current
    // values first so an edit made in the text file is not overwritten.
    void Show(HMONITOR nearMonitor);
    void Hide();
    bool visible() const { return visible_; }

    HWND hwnd() const { return hwnd_; }

private:
    // One line of the panel. A section header, or a setting bound directly to
    // the field in Settings that it edits - binding by pointer keeps the whole
    // table declarative, which is what makes it cheap to add a setting.
    // Which page a row lives on. Items first: it is what the window is most
    // often opened for.
    enum class Tab { Items, Glass, Dock, Behaviour };
    static constexpr int kTabCount = 4;

    struct Row {
        enum class Kind { Section, Slider, Toggle, Choice, Item, AddItem, AddSeparator };

        Kind kind = Kind::Slider;
        Tab tab = Tab::Glass;
        const wchar_t* label = nullptr;
        const wchar_t* hint = nullptr;

        float* number = nullptr;
        bool* flag = nullptr;
        int* choice = nullptr;

        float minimum = 0.0f;
        float maximum = 1.0f;
        int decimals = 2;
        const wchar_t* suffix = nullptr;
        std::vector<std::wstring> options;

        int column = 0;
        int itemIndex = -1; // for Kind::Item
        D2D1_RECT_F bounds{};  // the whole row, for hit testing and hover
        D2D1_RECT_F control{}; // the interactive part on the right
    };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool CreateDeviceResources();
    void BuildRows();
    void LayoutRows();
    // The tallest column a tab produces, so the window can be sized to the
    // largest of them once and not resize as pages are switched.
    float MeasureTab(Tab tab) const;
    int ColumnsFor(Tab tab) const;
    // Resizes the window to the page being shown, keeping its top-left corner.
    void ApplyWindowSize();
    void Render();
    void DrawTabs();
    // -1 when the point is over no tab.
    int TabAt(float x, float y) const;

    // Starts extracting the list's icons, and turns finished ones into D2D
    // bitmaps. The list is the one place a name alone is not enough - half the
    // point of a dock is that you recognise things by their icon.
    void StartIconLoad();
    void DrainIcons();

    // -1 when the point is over no row.
    int RowAt(float x, float y) const;
    // Applies a click or drag at `x` to the row, and returns true if the value
    // actually moved - a redraw and a save are only worth it if it did.
    bool ApplyPointer(int row, float x, bool dragging);
    // Item rows rebuild the whole table, so they cannot be handled through
    // ApplyPointer without invalidating the Row& it is holding.
    bool HandleItemClick(int row, float x);
    void CommitChange();
    void CommitItems();

    void DrawSlider(const Row& row, bool hovered);
    void DrawToggle(const Row& row, bool hovered);
    void DrawChoice(const Row& row, float pointerX);
    void DrawItem(const Row& row, bool hovered, float pointerX);
    // Chevrons and crosses drawn from line segments rather than glyphs, so they
    // do not depend on a symbol font being present and are crisp at any size.
    void DrawChevron(const D2D1_RECT_F& box, bool up, const D2D1_COLOR_F& colour);
    void DrawCross(const D2D1_RECT_F& box, const D2D1_COLOR_F& colour);
    void DrawText(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect,
                  const D2D1_COLOR_F& colour);

    GraphicsDevice* device_ = nullptr;
    HWND hwnd_ = nullptr;
    CompositionTarget target_;

    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> d2d_;
    ComPtr<ID2D1Bitmap1> backBuffer_;
    ComPtr<ID2D1SolidColorBrush> brush_;

    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IDWriteTextFormat> titleFormat_;
    ComPtr<IDWriteTextFormat> sectionFormat_;
    ComPtr<IDWriteTextFormat> labelFormat_;
    ComPtr<IDWriteTextFormat> hintFormat_;
    ComPtr<IDWriteTextFormat> valueFormat_;

    Settings settings_;
    ItemStore items_;
    IconLoader iconLoader_;
    // One per item, by index, and null until its icon arrives or if it has none.
    std::vector<ComPtr<ID2D1Bitmap>> itemIcons_;
    Tab activeTab_ = Tab::Items;
    D2D1_RECT_F tabBounds_[kTabCount]{};
    ChangedCallback onChanged_;
    ItemsCallback onItemsChanged_;
    std::vector<Row> rows_;
    // Mirrors Settings::backdrop, which is an enum the declarative table cannot
    // bind to as an int without lying about its type.
    int backdropChoice_ = 0;

    UINT dpi_ = 96;
    int hoverRow_ = -1;
    // The items column scrolls on its own; the rest of the panel is fixed.
    float itemScroll_ = 0.0f;
    float itemScrollMax_ = 0.0f;
    D2D1_RECT_F itemsClip_{};
    int dragRow_ = -1;
    float pointerX_ = 0.0f;
    float pointerY_ = 0.0f;
    bool visible_ = false;
    bool mouseTracking_ = false;
    int width_ = 0;
    int height_ = 0;
};

} // namespace liquidock
