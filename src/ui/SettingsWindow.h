#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <functional>
#include <string>
#include <vector>

#include "core/Settings.h"
#include "model/AppCatalog.h"
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

    // Re-reads the settings file into the window's own copy.
    //
    // The window holds a whole Settings and writes all of it on every change,
    // so anything edited in the file while the window is open was reverted by
    // the next slider it touched - an edit silently undone by a program that
    // had no idea it had been made. Ignored mid-drag, where the file is being
    // written by this window anyway and re-reading would fight the pointer.
    void ReloadFromDisk();
    bool interacting() const { return dragRow_ >= 0 || pressItem_ >= 0 || editItem_ >= 0; }

    // Brings the window up, centred on `nearMonitor`, loading the current
    // values first so an edit made in the text file is not overwritten.
    void Show(HMONITOR nearMonitor);
    void Hide();
    bool visible() const { return visible_; }
    // Visible *and* the window you are using. The dock holds still while its
    // own preferences are being adjusted - you cannot judge `frost` against a
    // dock that has slid away - but "being adjusted" has to mean focused, not
    // merely open, or a preferences window left up pins the dock on screen for
    // the rest of the session.
    bool active() const;

    HWND hwnd() const { return hwnd_; }

private:
    // One line of the panel. A section header, or a setting bound directly to
    // the field in Settings that it edits - binding by pointer keeps the whole
    // table declarative, which is what makes it cheap to add a setting.
    // Which page a row lives on. Items first: it is what the window is most
    // often opened for.
    enum class Tab { Items, Glass, Dock, Appearance, Behaviour };
    static constexpr int kTabCount = 5;

    struct Row {
        enum class Kind {
            Section, Slider, Toggle, Choice,
            Item,          // one icon in the dock's own grid
            Suggestion,    // one installed app not on the dock yet
            AddItem, AddSeparator
        };

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
        // Where this row's animations have got to. `anim` is the toggle's knob,
        // 0 at the left and 1 at the right; `hover` fades the card. Both start
        // at -1 meaning "not yet drawn", so the first frame snaps to the truth
        // instead of sliding in from nowhere when the window opens.
        float anim = -1.0f;
        float hover = 0.0f;
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
    // The explanation for whatever the pointer is on, drawn last so nothing
    // clips it, and following the cursor rather than living under the label.
    void DrawTooltip();
    // Close and minimise, drawn from line segments like the item buttons - no
    // symbol font to be missing, and crisp at any DPI.
    void DrawWindowButtons();
    // 0 = minimise, 1 = close, -1 = neither.
    int WindowButtonAt(float x, float y) const;
    float MeasureText(IDWriteTextFormat* format, const std::wstring& text) const;
    // Advances the row animations. Returns true while anything is still
    // moving, which is what keeps the redraw timer armed.
    bool AdvanceAnimation();
    void DrawTabs();
    // -1 when the point is over no tab.
    int TabAt(float x, float y) const;

    // Starts extracting the list's icons, and turns finished ones into D2D
    // bitmaps. The list is the one place a name alone is not enough - half the
    // point of a dock is that you recognise things by their icon.
    void StartIconLoad();
    void DrainIcons();
    // The installed-app list, and the icons for whichever of them are shown.
    void StartCatalogLoad();
    void DrainSuggestionIcons();
    // Lays the two grids out. Returns the y the content ends at.
    float LayoutGrids(float top, float width);
    // How many tiles fit across. Layout and measurement have to agree on this.
    int GridColumns() const;
    const wchar_t* EditorCursorFor(float x, float y) const;
    void DrawTile(const Row& row, bool hovered);
    void DrawTileBadge(const Row& row, bool hovered, bool suggestion);
    // What a tile is showing, so drawing and hit testing agree.
    const std::wstring& SuggestionLabel(int index) const;
    // Which suggestions the search box currently lets through, as indices into
    // suggestions_. Everything downstream - rows, layout, measurement - counts
    // this rather than the catalog.
    void ApplyFilter();
    void DrawSearch();

    // -1 when the point is over no row.
    int RowAt(float x, float y) const;
    // Which system cursor belongs over this point: a hand where a click does
    // something, a caret where typing does, an arrow everywhere else.
    const wchar_t* CursorFor(float x, float y) const;
    // Nudges a slider by whole units of whatever it displays: a setting shown
    // to two decimals steps by 0.01, one shown to none steps by 1. Taking the
    // step from the format means the keyboard and the wheel move a value by
    // exactly the amount the reader can see change, with nothing to configure.
    bool StepSlider(int index, int steps);

    // Applies a click or drag at `x` to the row, and returns true if the value
    // actually moved - a redraw and a save are only worth it if it did.
    bool ApplyPointer(int row, float x, bool dragging);
    // Item rows rebuild the whole table, so they cannot be handled through
    // ApplyPointer without invalidating the Row& it is holding.
    bool HandleItemClick(int row, float x);
    void CommitChange();
    void CommitItems();

    // The rounded card a setting sits on. For a slider it is also the track,
    // which is why the whole row responds to a click.
    D2D1_RECT_F PillRect(const Row& row) const;
    void DrawSlider(const Row& row, float hover);
    void DrawToggle(const Row& row, float hover);
    void DrawChoice(const Row& row, float pointerX);
    // Where a choice's options sit, sized to their own text and packed against
    // the card's right edge. Drawing, hit testing and the cursor all read this,
    // so a click lands where the eye says it should.
    std::vector<D2D1_RECT_F> ChoiceCells(const Row& row) const;
    void DrawItem(const Row& row, bool hovered, float pointerX);
    // The open editor under an item row. Returns the field rectangles in the
    // order the enum below gives them, so drawing and hit testing cannot drift.
    enum class Field { Name, Path, Arguments, WorkingDir, Icon, Show, Admin, Count };
    void EditorRects(const D2D1_RECT_F& panel, D2D1_RECT_F* fields,
                     D2D1_RECT_F* buttons) const;
    void DrawEditor(const D2D1_RECT_F& panel, int itemIndex);
    // Handles a click inside an open editor. True if anything changed.
    bool HandleEditorClick(const D2D1_RECT_F& panel, int itemIndex, float x, float y);
    // Commits whatever is being typed back into the item.
    void CommitEdit();
    void BeginEdit(int itemIndex, Field field);
    // The item index a drop at `y` would land on.
    int DropIndexAt(float x, float y) const;
    void DrawDetailBar();
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
    ComPtr<IDWriteTextFormat> tipFormat_;

    Settings settings_;
    ItemStore items_;
    IconLoader iconLoader_;
    // One per item, by index, and null until its icon arrives or if it has none.
    std::vector<ComPtr<ID2D1Bitmap>> itemIcons_;
    // Everything installed, and the icons for the ones not already docked. The
    // catalog is read once per window opening; the icons are fetched only for
    // the entries that survive the filter, because there are usually a few
    // hundred apps and nobody scrolls past the first dozen.
    AppCatalog catalog_;
    std::vector<CatalogEntry> suggestions_;
    IconLoader suggestionLoader_;
    std::vector<ComPtr<ID2D1Bitmap>> suggestionIcons_;
    std::vector<int> filtered_;
    std::wstring search_;
    bool searchFocused_ = false;
    D2D1_RECT_F searchRect_{};
    Tab activeTab_ = Tab::Items;
    D2D1_RECT_F tabBounds_[kTabCount]{};
    // Which of the offered faces is selected. The setting itself is a name.
    int fontChoice_ = 0;
    D2D1_RECT_F buttonBounds_[2]{};
    // Where the rule between the two grids sits, in the panel's own space.
    float gridRuleY_ = 0.0f;
    // Where the open item's editor sits, between the two grids. Empty when
    // nothing is selected.
    D2D1_RECT_F editorPanel_{};
    ChangedCallback onChanged_;
    ItemsCallback onItemsChanged_;
    std::vector<Row> rows_;
    // Mirrors Settings::backdrop, which is an enum the declarative table cannot
    // bind to as an int without lying about its type.
    int backdropChoice_ = 0;

    // How long the pointer has been on the row it is on. A tooltip that
     // appears the instant the pointer touches a row flashes all the way down
     // the list as you sweep past; a short dwell is what stops that.
    LARGE_INTEGER hoverSince_{};
    float tooltipAlpha_ = 0.0f;
    LARGE_INTEGER lastFrame_{};
    LARGE_INTEGER frequency_{};
    UINT dpi_ = 96;
    int hoverRow_ = -1;
    // The items column scrolls on its own; the rest of the panel is fixed.
    float itemScroll_ = 0.0f;
    float itemScrollMax_ = 0.0f;
    D2D1_RECT_F itemsClip_{};
    int dragRow_ = -1;
    // The item whose editor is open, or -1. Opening one is a click on the row;
    // it is the same gesture as selecting, because there is nothing else
    // selecting an item would be for.
    int expandedItem_ = -1;
    // Press-and-move on a row is a drag; press-and-release is a click. Both
    // start the same way, so the press is remembered until it turns into one.
    int pressItem_ = -1;
    float pressY_ = 0.0f;
    float pressX_ = 0.0f;
    bool draggingItem_ = false;
    int dropIndex_ = -1;
    // The field being typed into, its buffer and the caret's position in it.
    // Deliberately a plain buffer with a caret and no selection: this edits a
    // path and a couple of short strings, and a full text control - selection,
    // IME composition, undo - is a project in itself.
    Field editField_ = Field::Count;
    int editItem_ = -1;
    std::wstring editText_;
    size_t caret_ = 0;
    float pointerX_ = 0.0f;
    float pointerY_ = 0.0f;
    bool visible_ = false;
    // Whether the window has been positioned once. It is centred on first use
    // and then left wherever the user put it.
    bool placed_ = false;
    bool mouseTracking_ = false;
    int width_ = 0;
    int height_ = 0;
};

} // namespace liquidock
