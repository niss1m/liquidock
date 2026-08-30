#include "ui/SettingsWindow.h"

#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "core/Check.h"
#include "core/DesignTokens.h"
#include "core/Log.h"

namespace liquidock {
namespace {

constexpr wchar_t kWindowClass[] = L"LiquiDock.Settings";
// The file is only rewritten once a drag settles. Dragging a slider produces a
// change per mouse report, and writing the file sixty times a second would also
// wake the dock's own config watcher sixty times a second.
constexpr UINT_PTR kSaveTimer = 1;
constexpr UINT kSaveDelayMs = 400;
// Posted by the icon loader thread as the list's icons come in.
constexpr UINT kIconsMessage = WM_APP + 1;

namespace layout {
constexpr float kWidth = 900.0f;
constexpr float kPadding = 26.0f;
constexpr float kTitleHeight = 62.0f;
// The tab strip sits between the title and the first row.
constexpr float kTabHeight = 38.0f;
constexpr float kTabGap = 6.0f;
constexpr float kTabPadX = 18.0f;
constexpr float kTabsTop = 66.0f;
constexpr float kContentTop = kTabsTop + kTabHeight + 22.0f;
constexpr float kRowHeight = 54.0f;
constexpr float kSectionHeight = 46.0f;
constexpr float kColumnGap = 26.0f;
constexpr float kControlWidth = 120.0f;
constexpr float kValueWidth = 48.0f;
constexpr float kFooterHeight = 34.0f;
// One line per item. The path used to sit under the name on a second line,
// which is a lot of vertical space spent on something you only want when you
// are asking "which one is this?" - so it moved to the bar under the list and
// the row got its height back.
constexpr float kItemHeight = 34.0f;
constexpr float kItemIcon = 24.0f;
// Added to a row while its editor is open.
constexpr float kEditorRow = 34.0f;
constexpr float kEditorHeight = 7.0f * kEditorRow + 16.0f;
constexpr float kEditorLabel = 104.0f;
constexpr float kEditorButton = 92.0f;
// The line under the list that says what the hovered row actually is.
constexpr float kDetailBar = 30.0f;
constexpr float kItemButton = 28.0f;
constexpr int kMaxColumns = 2;
// The tallest the list is allowed to make the window. Forty pinned apps is
// nothing unusual and would otherwise produce a panel taller than the screen -
// the list scrolls, which is the whole reason it has a scroll offset.
constexpr float kListMaxHeight = 620.0f;
// The fixed row of add buttons above the list, and how wide each one is.
constexpr float kActionRow = 56.0f;
constexpr float kActionWidth = 124.0f;
constexpr float kCorner = design::kCornerRadius;
} // namespace layout

const wchar_t* const kTabNames[] = {L"Items", L"Glass", L"Dock", L"Behaviour"};

D2D1_COLOR_F Grey(float level, float alpha) {
    return D2D1::ColorF(level, level, level, alpha);
}

// The panel is opaque. The dock is glass because you are meant to look past it;
// preferences are meant to be read. Even three percent of translucency ghosts
// white text from the window behind straight through the labels - which is a
// 35% swing in brightness against a panel this dark, and looks like a bug.
const D2D1_COLOR_F kPanel = D2D1::ColorF(0.086f, 0.086f, 0.098f, 1.0f);
const D2D1_COLOR_F kPanelEdge = Grey(1.0f, 0.10f);
const D2D1_COLOR_F kTitle = Grey(1.0f, 0.95f);
const D2D1_COLOR_F kSection = Grey(1.0f, 0.42f);
const D2D1_COLOR_F kLabel = Grey(1.0f, 0.88f);
const D2D1_COLOR_F kHint = Grey(1.0f, 0.40f);
const D2D1_COLOR_F kValue = Grey(1.0f, 0.62f);
const D2D1_COLOR_F kTrack = Grey(1.0f, 0.14f);
const D2D1_COLOR_F kFill = Grey(1.0f, 0.70f);
const D2D1_COLOR_F kKnob = Grey(1.0f, 0.95f);
const D2D1_COLOR_F kRowHover = Grey(1.0f, 0.045f);
const D2D1_COLOR_F kOn = D2D1::ColorF(0.25f, 0.60f, 1.0f, 0.95f);

std::wstring FormatValue(float value, int decimals, const wchar_t* suffix) {
    wchar_t buffer[48];
    switch (decimals) {
        case 0: swprintf_s(buffer, L"%.0f", value); break;
        case 1: swprintf_s(buffer, L"%.1f", value); break;
        default: swprintf_s(buffer, L"%.2f", value); break;
    }
    std::wstring text(buffer);
    if (suffix) {
        text += suffix;
    }
    return text;
}

} // namespace

SettingsWindow::~SettingsWindow() {
    Destroy();
}

bool SettingsWindow::Create(GraphicsDevice& device, const Settings& settings,
                            ChangedCallback onChanged, ItemsCallback onItemsChanged) {
    device_ = &device;
    settings_ = settings;
    onChanged_ = std::move(onChanged);
    onItemsChanged_ = std::move(onItemsChanged);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &SettingsWindow::WndProcThunk;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    // An ordinary top-level window, not a panel belonging to the dock. It was
    // a TOOLWINDOW - out of the taskbar, out of Alt+Tab - and dismissed itself
    // the moment it lost focus, which together meant that glancing at anything
    // else closed it and there was no way back except the tray. A window you
    // cannot Alt+Tab to is a window you cannot leave.
    //
    // WS_POPUP with APPWINDOW rather than a real frame: the panel draws its own
    // rounded shape on a composition swapchain, and a system caption above that
    // would be a second, differently-coloured window edge. SYSMENU and
    // MINIMIZEBOX are what make Alt+Space and the taskbar's own restore work.
    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW, kWindowClass,
                            L"LiquiDock Preferences", WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, 0, 0,
                            1, 1, nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) {
        LogError("Preferences window creation failed: {}", GetLastError());
        return false;
    }

    BuildRows();
    return true;
}

void SettingsWindow::Destroy() {
    // Before the window goes: the loader posts to it, and a post to a destroyed
    // handle is at best wasted and at worst a message delivered to whatever is
    // handed that handle value next.
    iconLoader_.Stop();
    itemIcons_.clear();
    if (hwnd_) {
        KillTimer(hwnd_, kSaveTimer);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    backBuffer_.Reset();
    target_.Reset();
}

void SettingsWindow::BuildRows() {
    rows_.clear();
    backdropChoice_ = settings_.backdrop == BackdropSource::Screen ? 1 : 0;

    auto section = [this](Tab tab, const wchar_t* label, int column) {
        Row row;
        row.kind = Row::Kind::Section;
        row.tab = tab;
        row.label = label;
        row.column = column;
        rows_.push_back(std::move(row));
    };
    auto slider = [this](Tab tab, const wchar_t* label, const wchar_t* hint, float* value,
                         float low, float high, int decimals, int column,
                         const wchar_t* suffix = nullptr) {
        Row row;
        row.kind = Row::Kind::Slider;
        row.tab = tab;
        row.label = label;
        row.hint = hint;
        row.number = value;
        row.minimum = low;
        row.maximum = high;
        row.decimals = decimals;
        row.suffix = suffix;
        row.column = column;
        rows_.push_back(std::move(row));
    };
    auto toggle = [this](Tab tab, const wchar_t* label, const wchar_t* hint, bool* flag,
                         int column) {
        Row row;
        row.kind = Row::Kind::Toggle;
        row.tab = tab;
        row.label = label;
        row.hint = hint;
        row.flag = flag;
        row.column = column;
        rows_.push_back(std::move(row));
    };
    auto choice = [this](Tab tab, const wchar_t* label, const wchar_t* hint, int* value,
                         std::vector<std::wstring> options, int column) {
        Row row;
        row.kind = Row::Kind::Choice;
        row.tab = tab;
        row.label = label;
        row.hint = hint;
        row.choice = value;
        row.options = std::move(options);
        row.column = column;
        rows_.push_back(std::move(row));
    };

    // --- Items ------------------------------------------------------------
    // First, and the page the window opens on. Editing what is *on* the dock is
    // what people come here for; the glass is what they come here for once.
    const auto& items = items_.items();
    for (size_t i = 0; i < items.size(); ++i) {
        Row row;
        row.kind = Row::Kind::Item;
        row.tab = Tab::Items;
        row.itemIndex = static_cast<int>(i);
        row.column = 0;
        rows_.push_back(std::move(row));
    }
    Row add;
    add.kind = Row::Kind::AddItem;
    add.tab = Tab::Items;
    add.label = L"Add app…";
    add.column = 0;
    rows_.push_back(std::move(add));

    Row rule;
    rule.kind = Row::Kind::AddSeparator;
    rule.tab = Tab::Items;
    rule.label = L"Add divider";
    rule.column = 0;
    rows_.push_back(std::move(rule));

    // --- Glass ------------------------------------------------------------
    section(Tab::Glass, L"Material", 0);
    choice(Tab::Glass, L"Backdrop", L"Screen mode hides it from screenshots", &backdropChoice_,
           {L"Wallpaper", L"Screen"}, 0);
    slider(Tab::Glass, L"Frost", L"How softened the desktop behind is", &settings_.frost, 0.0f,
           1.0f, 2, 0);
    slider(Tab::Glass, L"Refraction", L"How far the rim bends what is behind", &settings_.refraction,
           0.0f, 1.0f, 2, 0);
    slider(Tab::Glass, L"Depth", L"How hard the edge bends it", &settings_.depth, 0.0f, 1.0f, 2, 0);
    slider(Tab::Glass, L"Splay", L"How far inward the bending reaches", &settings_.splay, 0.0f,
           1.0f, 2, 0);
    slider(Tab::Glass, L"Dispersion", L"Colour fringing at the rim", &settings_.dispersion, 0.0f,
           1.0f, 2, 0);

    section(Tab::Glass, L"Light", 1);
    slider(Tab::Glass, L"Angle", L"Where the highlight falls", &settings_.lightAngleDegrees,
           -180.0f, 180.0f, 0, 1, L"°");
    slider(Tab::Glass, L"Intensity", L"How bright the rim reflects", &settings_.lightIntensity,
           0.0f, 1.0f, 2, 1);
    slider(Tab::Glass, L"Tint", L"The white the glass is tinted with", &settings_.tintAlpha, 0.0f,
           0.4f, 2, 1);

    // --- Dock -------------------------------------------------------------
    section(Tab::Dock, L"Size", 0);
    slider(Tab::Dock, L"Icon size", L"Everything else scales with it", &settings_.iconSize,
           design::kMinIconSize, design::kMaxIconSize, 0, 0, L" px");

    section(Tab::Dock, L"Magnification", 0);
    toggle(Tab::Dock, L"Magnify under the cursor", L"The macOS swell", &settings_.magnification, 0);
    slider(Tab::Dock, L"Maximum size", L"How big the hovered icon gets", &settings_.maxScale, 1.0f,
           design::kMaxConfigurableScale, 2, 0, L"x");
    slider(Tab::Dock, L"Reach", L"How far the swell carries", &settings_.influencePx, 40.0f, 320.0f,
           0, 0, L" px");

    section(Tab::Dock, L"Movement", 1);
    toggle(Tab::Dock, L"Follow the cursor",
           L"The row slides so the hovered icon stays put", &settings_.followCursor, 1);
    toggle(Tab::Dock, L"Glass follows the icons", L"The bar swells around a raised icon",
           &settings_.iconBulge, 1);

    section(Tab::Dock, L"Placement", 1);
    toggle(Tab::Dock, L"Reserve screen space", L"Maximised windows stop above it",
           &settings_.reserveSpace, 1);

    // --- Behaviour --------------------------------------------------------
    section(Tab::Behaviour, L"Hiding", 0);
    toggle(Tab::Behaviour, L"Hide until needed", L"Comes back at the screen edge",
           &settings_.autoHide, 0);
    toggle(Tab::Behaviour, L"Only when covered",
           L"Stays out over an empty desktop", &settings_.hideWhenCovered, 0);
    slider(Tab::Behaviour, L"Stay out for", L"Before sliding away again", &settings_.dwellSeconds,
           0.5f, 15.0f, 1, 0, L" s");
    slider(Tab::Behaviour, L"Slide time", L"How long the dock takes to arrive",
           &settings_.slideSeconds, 0.0f, 0.8f, 2, 0, L" s");

    section(Tab::Behaviour, L"Hover label", 1);
    slider(Tab::Behaviour, L"Label size", L"The name shown above an icon",
           &settings_.labelFontSize, 9.0f, design::label::kMaxFontSize, 1, 1, L" px");
    toggle(Tab::Behaviour, L"Bold label", L"Heavier text on the pill", &settings_.labelBold, 1);
}

int SettingsWindow::ColumnsFor(Tab tab) const {
    // The list gets the whole width: a row carries an icon, a name, what it
    // points at and its buttons, and squeezing that into half a panel is what
    // made the old items column a list of truncated names.
    return (tab == Tab::Items) ? 1 : layout::kMaxColumns;
}

float SettingsWindow::MeasureTab(Tab tab) const {
    float y[layout::kMaxColumns]{};
    const int columns = ColumnsFor(tab);
    for (const Row& row : rows_) {
        if (row.tab != tab) {
            continue;
        }
        // The add buttons share one fixed row above the list rather than
        // sitting at the end of it, where forty pinned apps would put them a
        // scroll away from the person who came here to add a forty-first.
        if (row.kind == Row::Kind::AddItem || row.kind == Row::Kind::AddSeparator) {
            continue;
        }
        const int column = std::clamp(row.column, 0, columns - 1);
        if (row.kind == Row::Kind::Section) {
            y[column] += layout::kSectionHeight;
        } else if (row.kind == Row::Kind::Item) {
            y[column] += layout::kItemHeight;
            if (row.itemIndex == expandedItem_) {
                y[column] += layout::kEditorHeight;
            }
        } else {
            y[column] += layout::kRowHeight;
        }
    }
    float tallest = *std::max_element(y, y + columns);
    if (tab == Tab::Items) {
        tallest = std::min(tallest, layout::kListMaxHeight) + layout::kActionRow +
                  layout::kDetailBar;
    }
    return tallest;
}

void SettingsWindow::LayoutRows() {
    const int columns = ColumnsFor(activeTab_);
    const float columnWidth =
        (layout::kWidth - 2.0f * layout::kPadding - (columns - 1) * layout::kColumnGap) /
        static_cast<float>(columns);

    float y[layout::kMaxColumns];
    for (float& value : y) {
        value = layout::kContentTop;
    }

    const bool listTab = (activeTab_ == Tab::Items);
    const float listTop = layout::kContentTop + (listTab ? layout::kActionRow : 0.0f);
    for (float& value : y) {
        value = listTop;
    }
    float actionX = layout::kPadding;

    for (Row& row : rows_) {
        if (row.tab != activeTab_) {
            row.bounds = D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);
            continue;
        }
        if (row.kind == Row::Kind::AddItem || row.kind == Row::Kind::AddSeparator) {
            row.bounds = D2D1::RectF(actionX, layout::kContentTop, actionX + layout::kActionWidth,
                                     layout::kContentTop + layout::kItemHeight);
            row.control = D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);
            actionX += layout::kActionWidth + 10.0f;
            continue;
        }

        const int column = std::clamp(row.column, 0, columns - 1);
        const float left = layout::kPadding + column * (columnWidth + layout::kColumnGap);
        const bool listRow = (row.kind == Row::Kind::Item);
        float height = layout::kRowHeight;
        if (row.kind == Row::Kind::Section) {
            height = layout::kSectionHeight;
        } else if (listRow) {
            height = layout::kItemHeight;
            if (row.itemIndex == expandedItem_) {
                height += layout::kEditorHeight;
            }
        }

        // Only the list scrolls.
        const float scroll = listRow ? itemScroll_ : 0.0f;
        row.bounds =
            D2D1::RectF(left, y[column] - scroll, left + columnWidth, y[column] + height - scroll);

        if (listRow) {
            // Three equal buttons hugging the right edge of the row's *first*
            // line, so an open editor underneath does not carry them down with it.
            const float top = row.bounds.top + (layout::kItemHeight - layout::kItemButton) * 0.5f;
            row.control = D2D1::RectF(row.bounds.right - 3.0f * layout::kItemButton, top,
                                      row.bounds.right, top + layout::kItemButton);
        } else {
            // The control hugs the right-hand edge of its column, with the
            // numeric readout beyond it, so every control in a column shares one
            // baseline.
            const float controlRight = row.bounds.right - layout::kValueWidth;
            row.control = D2D1::RectF(controlRight - layout::kControlWidth, row.bounds.top + 12.0f,
                                      controlRight, row.bounds.top + 34.0f);
        }
        y[column] += height;
    }

    // Sized to the page being shown, and the window keeps its top-left corner
    // when it changes, so the tab strip does not move out from under the pointer
    // that just clicked it. Padding every page out to the tallest one instead
    // would leave the three short pages with a third of the panel empty.
    const float content = layout::kContentTop + MeasureTab(activeTab_);
    const float scale = static_cast<float>(dpi_) / 96.0f;
    height_ = static_cast<int>(std::lround((content + layout::kFooterHeight) * scale));
    width_ = static_cast<int>(std::lround(layout::kWidth * scale));

    const float listBottom = listTab ? (content - layout::kDetailBar) : content;
    itemsClip_ = D2D1::RectF(layout::kPadding, listTop - 4.0f, layout::kWidth - layout::kPadding,
                             listBottom);
    if (!listTab) {
        itemScrollMax_ = 0.0f;
        itemScroll_ = 0.0f;
        return;
    }
    const float listContent = y[0] + itemScroll_ - listTop;
    itemScrollMax_ = std::max(0.0f, listContent - (itemsClip_.bottom - itemsClip_.top));
    itemScroll_ = std::clamp(itemScroll_, 0.0f, itemScrollMax_);
}

void SettingsWindow::ApplyWindowSize() {
    if (!hwnd_) {
        return;
    }
    RECT current{};
    GetWindowRect(hwnd_, &current);
    // Top-left anchored: the strip stays exactly where it was clicked.
    SetWindowPos(hwnd_, nullptr, current.left, current.top, width_, height_,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    if (target_.width()) {
        // The bitmap goes first. It wraps the swap chain's back buffer, and the
        // flip model refuses to resize while any reference to that buffer is
        // outstanding - so releasing it afterwards means ResizeBuffers fails,
        // nothing is ever presented, and the compositor keeps showing the last
        // good frame cropped to the new window size. Which looked exactly like
        // "every tab but the first shows the item list, cut off".
        backBuffer_.Reset();
        d2d_->SetTarget(nullptr);
        if (!target_.Resize(static_cast<UINT>(width_), static_cast<UINT>(height_))) {
            LogWarn("Preferences swap chain resize failed");
        }
    }
    // Redrawn here, not left to WM_PAINT. The swap chain has just been resized
    // and its buffer holds whatever was in it before, so until something draws,
    // the window shows the *previous* page's pixels cropped to the new size -
    // which is what switching to any tab but the first looked like: the item
    // list, cut off. WM_PAINT would have got there eventually, and eventually
    // is seconds while the dock's capture thread is feeding the queue.
    if (visible_) {
        Render();
    }
}

int SettingsWindow::TabAt(float x, float y) const {
    for (int i = 0; i < kTabCount; ++i) {
        const D2D1_RECT_F& box = tabBounds_[i];
        if (x >= box.left && x <= box.right && y >= box.top && y <= box.bottom) {
            return i;
        }
    }
    return -1;
}

bool SettingsWindow::CreateDeviceResources() {
    if (d2d_) {
        return true;
    }

    D2D1_FACTORY_OPTIONS options{};
#ifdef LIQUIDOCK_DEBUG
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    LD_CHECK(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                               reinterpret_cast<void**>(d2dFactory_.GetAddressOf())));
    LD_CHECK(d2dFactory_->CreateDevice(device_->dxgi(), &d2dDevice_));
    LD_CHECK(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_));
    LD_CHECK(d2d_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush_));

    LD_CHECK(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())));

    // Segoe UI Variable on Windows 11, Segoe UI everywhere else - DirectWrite
    // falls back on its own when the first is missing.
    auto format = [this](float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out) {
        return dwrite_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                         L"en-us", out);
    };
    LD_CHECK(format(19.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &titleFormat_));
    LD_CHECK(format(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &sectionFormat_));
    LD_CHECK(format(14.0f, DWRITE_FONT_WEIGHT_NORMAL, &labelFormat_));
    LD_CHECK(format(11.5f, DWRITE_FONT_WEIGHT_NORMAL, &hintFormat_));

    // Labels and hints are one line each. Left to wrap they spill into the row
    // below, which reads as a rendering bug rather than as a long sentence -
    // and a settings panel where the rows do not line up is worse than one
    // where a hint is a word shorter.
    ComPtr<IDWriteInlineObject> ellipsis;
    dwrite_->CreateEllipsisTrimmingSign(hintFormat_.Get(), &ellipsis);
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    labelFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    hintFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    labelFormat_->SetTrimming(&trimming, ellipsis.Get());
    hintFormat_->SetTrimming(&trimming, ellipsis.Get());
    LD_CHECK(format(12.5f, DWRITE_FONT_WEIGHT_NORMAL, &valueFormat_));
    valueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    return true;
}

void SettingsWindow::Show(HMONITOR nearMonitor) {
    if (!hwnd_) {
        return;
    }

    // Re-read from disk on the way in. The file is editable while the dock is
    // running, so opening the window over a stale copy would silently revert
    // whatever was typed there.
    settings_.Load();
    items_.Load();
    BuildRows();

    UINT dpiX = 96;
    UINT dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(nearMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        dpi_ = dpiX;
    }
    LayoutRows();

    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(nearMonitor, &info);
    const RECT& work = info.rcWork;
    const int x = work.left + ((work.right - work.left) - width_) / 2;
    const int y = work.top + ((work.bottom - work.top) - height_) / 2;

    // Centred the first time and never again: a window the user has dragged
    // somewhere should still be there when they open it a second time.
    if (placed_) {
        RECT current{};
        GetWindowRect(hwnd_, &current);
        SetWindowPos(hwnd_, nullptr, current.left, current.top, width_, height_,
                     SWP_NOACTIVATE | SWP_NOZORDER);
    } else {
        SetWindowPos(hwnd_, nullptr, x, y, width_, height_, SWP_NOACTIVATE | SWP_NOZORDER);
        placed_ = true;
    }

    if (!target_.width()) {
        if (!target_.Initialize(*device_, hwnd_, static_cast<UINT>(width_),
                                static_cast<UINT>(height_))) {
            return;
        }
    } else {
        backBuffer_.Reset();
        if (d2d_) {
            d2d_->SetTarget(nullptr);
        }
        target_.Resize(static_cast<UINT>(width_), static_cast<UINT>(height_));
    }
    if (!CreateDeviceResources()) {
        return;
    }

    // After the device resources, because turning the pixels into D2D bitmaps
    // needs the context that CreateDeviceResources builds.
    StartIconLoad();

    visible_ = true;
    // SW_SHOW on a minimised window leaves it minimised; RESTORE is what brings
    // it back from the taskbar as well as showing it the first time.
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
    // Drawn now rather than left to WM_PAINT, which is only synthesised when
    // the message queue is empty - and with the dock's capture thread feeding
    // it, on a busy desktop that can be seconds away. An empty window for a
    // second after a click reads as the program having hung.
    Render();
}

void SettingsWindow::Hide() {
    if (!hwnd_ || !visible_) {
        return;
    }
    CommitEdit();
    visible_ = false;
    dragRow_ = -1;
    hoverRow_ = -1;
    pressItem_ = -1;
    draggingItem_ = false;
    dropIndex_ = -1;
    ShowWindow(hwnd_, SW_HIDE);
    // Whatever was still pending is written now rather than on the next open.
    KillTimer(hwnd_, kSaveTimer);
    settings_.Save();
}

int SettingsWindow::RowAt(float x, float y) const {
    for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        if (row.tab != activeTab_ || row.kind == Row::Kind::Section) {
            continue;
        }
        if (row.kind == Row::Kind::Item) {
            // Scrolled out of sight is not clickable, however much the row's
            // rectangle still says it is there.
            if (y < itemsClip_.top || y > itemsClip_.bottom) {
                continue;
            }
        }
        if (x >= row.bounds.left && x <= row.bounds.right && y >= row.bounds.top &&
            y <= row.bounds.bottom) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool SettingsWindow::ApplyPointer(int index, float x, bool dragging) {
    if (index < 0 || index >= static_cast<int>(rows_.size())) {
        return false;
    }
    Row& row = rows_[static_cast<size_t>(index)];

    switch (row.kind) {
        case Row::Kind::Slider: {
            const float span = std::max(row.control.right - row.control.left, 1.0f);
            const float t = std::clamp((x - row.control.left) / span, 0.0f, 1.0f);
            const float value = row.minimum + t * (row.maximum - row.minimum);
            if (std::fabs(value - *row.number) < 1e-4f) {
                return false;
            }
            *row.number = value;
            return true;
        }
        case Row::Kind::Toggle:
            if (dragging) {
                return false; // a toggle flips once, not once per mouse report
            }
            *row.flag = !*row.flag;
            return true;
        case Row::Kind::Choice: {
            if (dragging) {
                return false;
            }
            const int count = static_cast<int>(row.options.size());
            const float span = (row.control.right - row.control.left) / std::max(count, 1);
            const int picked =
                std::clamp(static_cast<int>((x - row.control.left) / std::max(span, 1.0f)), 0,
                           count - 1);
            if (picked == *row.choice) {
                return false;
            }
            *row.choice = picked;
            return true;
        }
        default:
            return false;
    }
}

bool SettingsWindow::HandleItemClick(int index, float x) {
    if (index < 0 || index >= static_cast<int>(rows_.size())) {
        return false;
    }
    // Copied, not referenced: everything below rebuilds rows_ and would leave a
    // reference into a vector that has been reallocated.
    const Row row = rows_[static_cast<size_t>(index)];

    if (row.kind == Row::Kind::AddItem) {
        DockItem item;
        if (!ItemStore::PickProgram(hwnd_, &item) || !items_.Add(std::move(item))) {
            return false;
        }
        CommitItems();
        return true;
    }
    if (row.kind == Row::Kind::AddSeparator) {
        DockItem divider;
        divider.kind = ItemKind::Separator;
        divider.label = L"Divider";
        if (!items_.Add(std::move(divider))) {
            return false;
        }
        CommitItems();
        return true;
    }
    if (row.kind != Row::Kind::Item) {
        return false;
    }

    const float third = (row.control.right - row.control.left) / 3.0f;
    const int button = static_cast<int>((x - row.control.left) / std::max(third, 1.0f));
    if (x < row.control.left || button < 0 || button > 2) {
        return false;
    }

    const size_t item = static_cast<size_t>(row.itemIndex);
    if (button == 2) {
        if (!items_.Remove(item)) {
            return false;
        }
    } else if (items_.Move(item, button == 0 ? -1 : 1) < 0) {
        return false;
    }
    CommitItems();
    return true;
}

void SettingsWindow::CommitItems() {
    // ItemStore has already written the file; the dock re-reads it, which keeps
    // the file the single source of truth rather than having two live copies of
    // the list drift apart.
    if (onItemsChanged_) {
        onItemsChanged_();
    }
    BuildRows();
    LayoutRows();
    ApplyWindowSize();
    StartIconLoad();
    hoverRow_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int SettingsWindow::DropIndexAt(float y) const {
    // Where an insertion at `y` would land: the first row whose midpoint is
    // below the pointer. Measured against the rows as drawn, so it follows the
    // scroll without having to know about it.
    int index = 0;
    for (const Row& row : rows_) {
        if (row.tab != Tab::Items || row.kind != Row::Kind::Item) {
            continue;
        }
        const float mid = row.bounds.top + layout::kItemHeight * 0.5f;
        if (y < mid) {
            return row.itemIndex;
        }
        index = row.itemIndex + 1;
    }
    return index;
}

void SettingsWindow::BeginEdit(int itemIndex, Field field) {
    CommitEdit();
    const auto& items = items_.items();
    if (itemIndex < 0 || static_cast<size_t>(itemIndex) >= items.size()) {
        return;
    }
    const DockItem& item = items[static_cast<size_t>(itemIndex)];
    switch (field) {
        case Field::Name: editText_ = item.label; break;
        case Field::Arguments: editText_ = item.arguments; break;
        case Field::WorkingDir: editText_ = item.workingDirectory; break;
        default: return; // path and icon are chosen, not typed
    }
    editItem_ = itemIndex;
    editField_ = field;
    caret_ = editText_.size();
}

void SettingsWindow::CommitEdit() {
    if (editItem_ < 0 || editField_ == Field::Count) {
        return;
    }
    const size_t index = static_cast<size_t>(editItem_);
    if (index < items_.items().size()) {
        DockItem item = items_.items()[index];
        switch (editField_) {
            case Field::Name: item.label = editText_; break;
            case Field::Arguments: item.arguments = editText_; break;
            case Field::WorkingDir: item.workingDirectory = editText_; break;
            default: break;
        }
        items_.Replace(index, std::move(item));
        if (onItemsChanged_) {
            onItemsChanged_();
        }
    }
    editItem_ = -1;
    editField_ = Field::Count;
    editText_.clear();
    caret_ = 0;
}

bool SettingsWindow::HandleEditorClick(const Row& row, float x, float y) {
    D2D1_RECT_F fields[static_cast<int>(Field::Count)];
    D2D1_RECT_F buttons[static_cast<int>(Field::Count)];
    EditorRects(row, fields, buttons);

    const size_t index = static_cast<size_t>(row.itemIndex);
    if (index >= items_.items().size()) {
        return false;
    }

    for (int i = 0; i < static_cast<int>(Field::Count); ++i) {
        const D2D1_RECT_F& button = buttons[i];
        if (x >= button.left && x <= button.right && y >= button.top && y <= button.bottom &&
            button.right > button.left) {
            DockItem item = items_.items()[index];
            if (i == static_cast<int>(Field::Path)) {
                DockItem picked;
                if (!ItemStore::PickProgram(hwnd_, &picked)) {
                    return false;
                }
                item.path = picked.path;
                // The name follows the target unless it was set by hand, which
                // is the behaviour that makes repointing an entry one action.
                if (item.label.empty()) {
                    item.label = picked.label;
                }
            } else if (i == static_cast<int>(Field::Icon)) {
                std::wstring chosen;
                if (!ItemStore::PickImage(hwnd_, &chosen)) {
                    return false;
                }
                item.iconPath = chosen;
            } else {
                std::wstring chosen;
                if (!ItemStore::PickFolder(hwnd_, &chosen)) {
                    return false;
                }
                item.workingDirectory = chosen;
            }
            items_.Replace(index, std::move(item));
            CommitItems();
            return true;
        }

        const D2D1_RECT_F& field = fields[i];
        if (x >= field.left && x <= field.right && y >= field.top && y <= field.bottom) {
            if (i == static_cast<int>(Field::Show)) {
                // Cycled rather than given a dropdown: three values, and a
                // dropdown is a second window for a choice you can make by
                // clicking the answer.
                DockItem item = items_.items()[index];
                item.runState = (item.runState == RunState::Normal)      ? RunState::Minimized
                                : (item.runState == RunState::Minimized) ? RunState::Maximized
                                                                         : RunState::Normal;
                items_.Replace(index, std::move(item));
                CommitItems();
                return true;
            }
            if (i == static_cast<int>(Field::Admin)) {
                DockItem item = items_.items()[index];
                item.runAsAdmin = !item.runAsAdmin;
                items_.Replace(index, std::move(item));
                CommitItems();
                return true;
            }
            if (i == static_cast<int>(Field::Path) || i == static_cast<int>(Field::Icon)) {
                return false; // read-only: use the button beside it
            }
            BeginEdit(row.itemIndex, static_cast<Field>(i));
            return true;
        }
    }
    return false;
}

void SettingsWindow::StartIconLoad() {
    itemIcons_.assign(items_.items().size(), nullptr);
    if (!hwnd_ || items_.items().empty()) {
        return;
    }
    // The same extractor the dock uses, at the size this list draws. Shell icon
    // calls can block for tens of milliseconds each on a cold cache, and the
    // preferences window is opened by a click - stalling it for a second while
    // forty icons are fetched would be the most visible slowness in the program.
    iconLoader_.Start(items_.items(), static_cast<int>(layout::kItemIcon), hwnd_, kIconsMessage);
}

void SettingsWindow::DrainIcons() {
    std::vector<IconBitmap> loaded;
    iconLoader_.Collect(loaded);
    if (!d2d_) {
        return;
    }
    for (const IconBitmap& icon : loaded) {
        if (icon.slot < 0 || static_cast<size_t>(icon.slot) >= itemIcons_.size() ||
            icon.pixels.empty()) {
            continue;
        }
        const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap> bitmap;
        const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(icon.size),
                                             static_cast<UINT32>(icon.size));
        if (SUCCEEDED(d2d_->CreateBitmap(size, icon.pixels.data(),
                                         static_cast<UINT32>(icon.size) * 4, properties,
                                         &bitmap))) {
            itemIcons_[static_cast<size_t>(icon.slot)] = std::move(bitmap);
        }
    }
}

void SettingsWindow::CommitChange() {
    settings_.backdrop =
        backdropChoice_ == 1 ? BackdropSource::Screen : BackdropSource::Wallpaper;
    if (onChanged_) {
        onChanged_(settings_);
    }
    // Debounced: dragging a slider produces a change per mouse report, and the
    // dock is watching this file.
    SetTimer(hwnd_, kSaveTimer, kSaveDelayMs, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::DrawText(const std::wstring& text, IDWriteTextFormat* format,
                              const D2D1_RECT_F& rect, const D2D1_COLOR_F& colour) {
    if (text.empty() || !format) {
        return;
    }
    brush_->SetColor(colour);
    d2d_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush_.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void SettingsWindow::DrawSlider(const Row& row, bool hovered) {
    const float centreY = (row.control.top + row.control.bottom) * 0.5f;
    const float radius = 2.0f;
    const D2D1_ROUNDED_RECT track = D2D1::RoundedRect(
        D2D1::RectF(row.control.left, centreY - radius, row.control.right, centreY + radius),
        radius, radius);
    brush_->SetColor(kTrack);
    d2d_->FillRoundedRectangle(track, brush_.Get());

    const float span = std::max(row.maximum - row.minimum, 1e-5f);
    const float t = std::clamp((*row.number - row.minimum) / span, 0.0f, 1.0f);
    const float knobX = row.control.left + t * (row.control.right - row.control.left);

    if (t > 0.0f) {
        brush_->SetColor(kFill);
        d2d_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(row.control.left, centreY - radius, knobX,
                                          centreY + radius),
                              radius, radius),
            brush_.Get());
    }

    brush_->SetColor(kKnob);
    const float knobRadius = hovered ? 7.0f : 6.0f;
    d2d_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, centreY), knobRadius, knobRadius),
                      brush_.Get());
}

void SettingsWindow::DrawToggle(const Row& row, bool hovered) {
    const float centreY = (row.control.top + row.control.bottom) * 0.5f;
    const float height = 22.0f;
    const float width = 40.0f;
    // Right-aligned in the control slot so toggles line up with slider knobs.
    const float right = row.control.right;
    const D2D1_RECT_F pill =
        D2D1::RectF(right - width, centreY - height * 0.5f, right, centreY + height * 0.5f);

    brush_->SetColor(*row.flag ? kOn : kTrack);
    d2d_->FillRoundedRectangle(D2D1::RoundedRect(pill, height * 0.5f, height * 0.5f), brush_.Get());

    const float knobRadius = height * 0.5f - 3.0f;
    const float knobX = *row.flag ? (pill.right - knobRadius - 3.0f) : (pill.left + knobRadius + 3.0f);
    brush_->SetColor(Grey(1.0f, hovered ? 1.0f : 0.92f));
    d2d_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, centreY), knobRadius, knobRadius),
                      brush_.Get());
}

void SettingsWindow::DrawChoice(const Row& row, float pointerX) {
    const int count = static_cast<int>(row.options.size());
    if (count <= 0) {
        return;
    }
    const float height = 24.0f;
    const float centreY = (row.control.top + row.control.bottom) * 0.5f;
    const D2D1_RECT_F frame = D2D1::RectF(row.control.left, centreY - height * 0.5f,
                                          row.control.right, centreY + height * 0.5f);

    brush_->SetColor(kTrack);
    d2d_->FillRoundedRectangle(D2D1::RoundedRect(frame, 6.0f, 6.0f), brush_.Get());

    const float segment = (frame.right - frame.left) / count;
    for (int i = 0; i < count; ++i) {
        const D2D1_RECT_F cell = D2D1::RectF(frame.left + i * segment + 2.0f, frame.top + 2.0f,
                                             frame.left + (i + 1) * segment - 2.0f,
                                             frame.bottom - 2.0f);
        const bool selected = (i == *row.choice);
        const bool under = pointerX >= cell.left && pointerX <= cell.right;
        if (selected) {
            brush_->SetColor(kOn);
            d2d_->FillRoundedRectangle(D2D1::RoundedRect(cell, 5.0f, 5.0f), brush_.Get());
        } else if (under) {
            brush_->SetColor(Grey(1.0f, 0.08f));
            d2d_->FillRoundedRectangle(D2D1::RoundedRect(cell, 5.0f, 5.0f), brush_.Get());
        }
        auto centred = D2D1::RectF(cell.left, cell.top + 3.0f, cell.right, cell.bottom);
        valueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(row.options[static_cast<size_t>(i)], valueFormat_.Get(), centred,
                 selected ? Grey(1.0f, 1.0f) : kValue);
        valueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

void SettingsWindow::DrawChevron(const D2D1_RECT_F& box, bool up, const D2D1_COLOR_F& colour) {
    const float cx = (box.left + box.right) * 0.5f;
    const float cy = (box.top + box.bottom) * 0.5f;
    const float w = 4.5f;
    const float h = 2.8f;
    brush_->SetColor(colour);
    const float tipY = up ? cy - h : cy + h;
    const float armY = up ? cy + h : cy - h;
    d2d_->DrawLine(D2D1::Point2F(cx - w, armY), D2D1::Point2F(cx, tipY), brush_.Get(), 1.6f);
    d2d_->DrawLine(D2D1::Point2F(cx, tipY), D2D1::Point2F(cx + w, armY), brush_.Get(), 1.6f);
}

void SettingsWindow::DrawCross(const D2D1_RECT_F& box, const D2D1_COLOR_F& colour) {
    const float cx = (box.left + box.right) * 0.5f;
    const float cy = (box.top + box.bottom) * 0.5f;
    const float r = 4.0f;
    brush_->SetColor(colour);
    d2d_->DrawLine(D2D1::Point2F(cx - r, cy - r), D2D1::Point2F(cx + r, cy + r), brush_.Get(), 1.6f);
    d2d_->DrawLine(D2D1::Point2F(cx + r, cy - r), D2D1::Point2F(cx - r, cy + r), brush_.Get(), 1.6f);
}

void SettingsWindow::DrawTabs() {
    float x = layout::kPadding;
    for (int i = 0; i < kTabCount; ++i) {
        const std::wstring name = kTabNames[i];
        float textWidth = 60.0f;
        ComPtr<IDWriteTextLayout> measured;
        if (dwrite_ &&
            SUCCEEDED(dwrite_->CreateTextLayout(name.c_str(), static_cast<UINT32>(name.size()),
                                                labelFormat_.Get(), 400.0f, 40.0f, &measured))) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(measured->GetMetrics(&metrics))) {
                textWidth = metrics.width;
            }
        }
        const float width = textWidth + 2.0f * layout::kTabPadX;
        tabBounds_[i] =
            D2D1::RectF(x, layout::kTabsTop, x + width, layout::kTabsTop + layout::kTabHeight);
        x += width + layout::kTabGap;

        const bool active = (static_cast<Tab>(i) == activeTab_);
        const bool under = pointerX_ >= tabBounds_[i].left && pointerX_ <= tabBounds_[i].right &&
                           pointerY_ >= tabBounds_[i].top && pointerY_ <= tabBounds_[i].bottom;
        if (active || under) {
            brush_->SetColor(active ? Grey(1.0f, 0.12f) : Grey(1.0f, 0.06f));
            d2d_->FillRoundedRectangle(D2D1::RoundedRect(tabBounds_[i], 8.0f, 8.0f), brush_.Get());
        }
        // The label alone carries the state as well as a box does, so the active
        // tab is simply the one you can read.
        labelFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(name, labelFormat_.Get(),
                 D2D1::RectF(tabBounds_[i].left, tabBounds_[i].top + 8.0f, tabBounds_[i].right,
                             tabBounds_[i].bottom),
                 active ? kTitle : (under ? kLabel : kSection));
        labelFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
}

void SettingsWindow::DrawItem(const Row& row, bool hovered, float pointerX) {
    const auto& items = items_.items();

    if (row.kind == Row::Kind::AddItem || row.kind == Row::Kind::AddSeparator) {
        const D2D1_RECT_F pill = D2D1::RectF(row.bounds.left, row.bounds.top + 7.0f,
                                             row.bounds.right, row.bounds.bottom - 7.0f);
        brush_->SetColor(hovered ? Grey(1.0f, 0.16f) : Grey(1.0f, 0.10f));
        d2d_->FillRoundedRectangle(D2D1::RoundedRect(pill, 6.0f, 6.0f), brush_.Get());
        valueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(row.label, valueFormat_.Get(),
                 D2D1::RectF(pill.left, pill.top + 3.0f, pill.right, pill.bottom), kLabel);
        valueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        return;
    }

    const size_t index = static_cast<size_t>(row.itemIndex);
    if (index >= items.size()) {
        return;
    }
    const DockItem& item = items[index];
    const bool expanded = (row.itemIndex == expandedItem_);
    const bool dragged = draggingItem_ && row.itemIndex == pressItem_;

    // The row's first line. The editor, if any, is drawn under it.
    const D2D1_RECT_F line =
        D2D1::RectF(row.bounds.left, row.bounds.top, row.bounds.right,
                    row.bounds.top + layout::kItemHeight);
    const float mid = (line.top + line.bottom) * 0.5f;

    if (dragged) {
        // Lifted: dimmed in place, with the insertion line showing where it goes.
        brush_->SetColor(Grey(1.0f, 0.05f));
        d2d_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(line.left - 6.0f, line.top + 1.0f, line.right + 6.0f,
                                          line.bottom - 1.0f),
                              6.0f, 6.0f),
            brush_.Get());
    } else if (expanded || hovered) {
        brush_->SetColor(expanded ? Grey(1.0f, 0.09f) : kRowHover);
        d2d_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(line.left - 6.0f, line.top + 1.0f, line.right + 6.0f,
                                          expanded ? row.bounds.bottom - 2.0f : line.bottom - 1.0f),
                              6.0f, 6.0f),
            brush_.Get());
    }

    const float textLeft = row.bounds.left + layout::kItemIcon + 12.0f;
    const float textRight = row.control.left - 90.0f;

    if (item.kind == ItemKind::Separator) {
        brush_->SetColor(Grey(1.0f, 0.30f));
        const float left = row.bounds.left + layout::kItemIcon * 0.5f;
        d2d_->FillRectangle(D2D1::RectF(left, mid - 8.0f, left + 1.0f, mid + 8.0f), brush_.Get());
        DrawText(L"Divider", labelFormat_.Get(),
                 D2D1::RectF(textLeft, mid - 11.0f, textRight, line.bottom), kValue);
    } else {
        ID2D1Bitmap* icon = (index < itemIcons_.size()) ? itemIcons_[index].Get() : nullptr;
        const D2D1_RECT_F box =
            D2D1::RectF(row.bounds.left, mid - layout::kItemIcon * 0.5f,
                        row.bounds.left + layout::kItemIcon, mid + layout::kItemIcon * 0.5f);
        if (icon) {
            d2d_->DrawBitmap(icon, box, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            brush_->SetColor(Grey(1.0f, 0.08f));
            d2d_->FillRoundedRectangle(D2D1::RoundedRect(box, 6.0f, 6.0f), brush_.Get());
        }
        DrawText(item.label, labelFormat_.Get(),
                 D2D1::RectF(textLeft, mid - 11.0f, textRight, line.bottom), kLabel);
    }

    if (item.group == ItemGroup::Utility) {
        const D2D1_RECT_F pill = D2D1::RectF(row.control.left - 78.0f, mid - 9.0f,
                                             row.control.left - 18.0f, mid + 9.0f);
        brush_->SetColor(Grey(1.0f, 0.10f));
        d2d_->FillRoundedRectangle(D2D1::RoundedRect(pill, 9.0f, 9.0f), brush_.Get());
        valueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(L"utility", valueFormat_.Get(),
                 D2D1::RectF(pill.left, pill.top + 1.0f, pill.right, pill.bottom), kValue);
        valueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    if (expanded) {
        DrawEditor(row);
    }

    // Only shown on hover: three sets of buttons against every row is noise, and
    // the row you are pointing at is the only one you can act on anyway.
    if (!hovered || draggingItem_) {
        return;
    }
    const float third = (row.control.right - row.control.left) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        const D2D1_RECT_F box = D2D1::RectF(row.control.left + i * third, row.control.top,
                                            row.control.left + (i + 1) * third, row.control.bottom);
        const bool under = pointerX >= box.left && pointerX <= box.right;
        if (under) {
            brush_->SetColor(Grey(1.0f, 0.14f));
            d2d_->FillRoundedRectangle(D2D1::RoundedRect(box, 5.0f, 5.0f), brush_.Get());
        }
        const D2D1_COLOR_F ink = under ? Grey(1.0f, 0.95f) : Grey(1.0f, 0.55f);
        if (i == 0) {
            DrawChevron(box, true, ink);
        } else if (i == 1) {
            DrawChevron(box, false, ink);
        } else {
            DrawCross(box, ink);
        }
    }
}

void SettingsWindow::EditorRects(const Row& row, D2D1_RECT_F* fields,
                                 D2D1_RECT_F* buttons) const {
    const float left = row.bounds.left + layout::kItemIcon + 12.0f;
    const float right = row.bounds.right - 8.0f;
    float y = row.bounds.top + layout::kItemHeight + 8.0f;
    for (int i = 0; i < static_cast<int>(Field::Count); ++i) {
        const bool hasButton = (i == static_cast<int>(Field::Path) ||
                                i == static_cast<int>(Field::Icon) ||
                                i == static_cast<int>(Field::WorkingDir));
        const float fieldRight = hasButton ? (right - layout::kEditorButton - 8.0f) : right;
        fields[i] = D2D1::RectF(left + layout::kEditorLabel, y + 4.0f, fieldRight,
                                y + layout::kEditorRow - 4.0f);
        buttons[i] = hasButton ? D2D1::RectF(right - layout::kEditorButton, y + 4.0f, right,
                                             y + layout::kEditorRow - 4.0f)
                               : D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);
        y += layout::kEditorRow;
    }
}

void SettingsWindow::DrawEditor(const Row& row) {
    const auto& items = items_.items();
    const size_t index = static_cast<size_t>(row.itemIndex);
    if (index >= items.size()) {
        return;
    }
    const DockItem& item = items[index];

    static const wchar_t* const kNames[] = {L"Name",  L"Opens", L"Arguments", L"Start in",
                                            L"Icon",  L"Opens as", L"Elevated"};
    const std::wstring values[] = {
        item.label, item.path, item.arguments, item.workingDirectory, item.iconPath,
        item.runState == RunState::Minimized   ? L"Minimized"
        : item.runState == RunState::Maximized ? L"Maximized"
                                               : L"Normal window",
        item.runAsAdmin ? L"Run as administrator" : L"No"};
    static const wchar_t* const kButtons[] = {nullptr,    L"Choose…", nullptr, L"Browse…",
                                              L"Choose…", nullptr,    nullptr};

    D2D1_RECT_F fields[static_cast<int>(Field::Count)];
    D2D1_RECT_F buttons[static_cast<int>(Field::Count)];
    EditorRects(row, fields, buttons);

    const float labelLeft = row.bounds.left + layout::kItemIcon + 12.0f;
    for (int i = 0; i < static_cast<int>(Field::Count); ++i) {
        const D2D1_RECT_F& field = fields[i];
        DrawText(kNames[i], hintFormat_.Get(),
                 D2D1::RectF(labelLeft, field.top + 3.0f, labelLeft + layout::kEditorLabel,
                             field.bottom),
                 kHint);

        const bool editing = (editItem_ == row.itemIndex && static_cast<int>(editField_) == i);
        const bool typable = (i == static_cast<int>(Field::Name) ||
                              i == static_cast<int>(Field::Arguments) ||
                              i == static_cast<int>(Field::WorkingDir));
        if (typable) {
            brush_->SetColor(editing ? Grey(1.0f, 0.14f) : Grey(1.0f, 0.06f));
            d2d_->FillRoundedRectangle(D2D1::RoundedRect(field, 5.0f, 5.0f), brush_.Get());
        } else if (i == static_cast<int>(Field::Show) || i == static_cast<int>(Field::Admin)) {
            // Clickable, so it has to look it - a value you can change and a
            // value you can only read must not draw the same.
            const bool under = pointerX_ >= field.left && pointerX_ <= field.right &&
                               pointerY_ >= field.top && pointerY_ <= field.bottom;
            const D2D1_RECT_F pill =
                D2D1::RectF(field.left, field.top, field.left + 168.0f, field.bottom);
            brush_->SetColor(under ? Grey(1.0f, 0.16f) : Grey(1.0f, 0.09f));
            d2d_->FillRoundedRectangle(D2D1::RoundedRect(pill, 5.0f, 5.0f), brush_.Get());
        }

        const std::wstring text = editing ? editText_ : values[i];
        const bool empty = text.empty();
        DrawText(empty ? L"—" : text, labelFormat_.Get(),
                 D2D1::RectF(field.left + 8.0f, field.top + 2.0f, field.right - 8.0f, field.bottom),
                 empty ? kHint : kLabel);

        if (editing) {
            // The caret, measured rather than guessed: a fixed advance per
            // character would sit visibly wrong in a proportional face.
            float caretX = field.left + 8.0f;
            ComPtr<IDWriteTextLayout> layout;
            const std::wstring head = editText_.substr(0, caret_);
            if (!head.empty() && dwrite_ &&
                SUCCEEDED(dwrite_->CreateTextLayout(head.c_str(),
                                                    static_cast<UINT32>(head.size()),
                                                    labelFormat_.Get(), 4000.0f, 40.0f, &layout))) {
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                    caretX += metrics.widthIncludingTrailingWhitespace;
                }
            }
            brush_->SetColor(Grey(1.0f, 0.85f));
            d2d_->FillRectangle(
                D2D1::RectF(caretX, field.top + 4.0f, caretX + 1.0f, field.bottom - 4.0f),
                brush_.Get());
        }

        if (kButtons[i]) {
            const D2D1_RECT_F& button = buttons[i];
            const bool under = pointerX_ >= button.left && pointerX_ <= button.right &&
                               pointerY_ >= button.top && pointerY_ <= button.bottom;
            brush_->SetColor(under ? Grey(1.0f, 0.18f) : Grey(1.0f, 0.10f));
            d2d_->FillRoundedRectangle(D2D1::RoundedRect(button, 5.0f, 5.0f), brush_.Get());
            valueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            DrawText(kButtons[i], valueFormat_.Get(),
                     D2D1::RectF(button.left, button.top + 3.0f, button.right, button.bottom),
                     kLabel);
            valueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
    }
}

void SettingsWindow::DrawDetailBar() {
    const float scale = static_cast<float>(dpi_) / 96.0f;
    const float top = static_cast<float>(height_) / scale - layout::kFooterHeight -
                      layout::kDetailBar;
    brush_->SetColor(Grey(1.0f, 0.05f));
    d2d_->FillRectangle(
        D2D1::RectF(layout::kPadding, top, layout::kWidth - layout::kPadding, top + 1.0f),
        brush_.Get());

    std::wstring detail;
    if (hoverRow_ >= 0 && static_cast<size_t>(hoverRow_) < rows_.size()) {
        const Row& row = rows_[static_cast<size_t>(hoverRow_)];
        if (row.kind == Row::Kind::Item && row.itemIndex >= 0 &&
            static_cast<size_t>(row.itemIndex) < items_.items().size()) {
            const DockItem& item = items_.items()[static_cast<size_t>(row.itemIndex)];
            if (item.kind == ItemKind::Separator) {
                detail = L"A divider. It launches nothing; drag it where you want the break.";
            } else {
                detail = item.path;
                if (!item.arguments.empty()) {
                    detail += L"   " + item.arguments;
                }
                if (!item.workingDirectory.empty()) {
                    detail += L"   · in " + item.workingDirectory;
                }
            }
        }
    }
    if (detail.empty()) {
        detail = L"Drag a row to reorder it · click it to edit";
    }
    DrawText(detail, hintFormat_.Get(),
             D2D1::RectF(layout::kPadding, top + 7.0f, layout::kWidth - layout::kPadding,
                         top + layout::kDetailBar),
             kHint);
}

void SettingsWindow::Render() {
    if (!d2d_ || !target_.width()) {
        return;
    }

    if (!backBuffer_) {
        ComPtr<IDXGISurface> surface;
        if (FAILED(target_.swap_chain()->GetBuffer(0, IID_PPV_ARGS(&surface)))) {
            return;
        }
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f,
            96.0f);
        if (FAILED(d2d_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &backBuffer_))) {
            return;
        }
    }

    if (!target_.BeginFrame()) {
        return;
    }

    const float scale = static_cast<float>(dpi_) / 96.0f;
    d2d_->SetTarget(backBuffer_.Get());
    d2d_->BeginDraw();
    // One transform for the whole panel: everything below is written in the
    // same logical units as the design tokens, at any DPI.
    d2d_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    d2d_->Clear(D2D1::ColorF(0, 0.0f));

    const D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(
        D2D1::RectF(0.0f, 0.0f, layout::kWidth, static_cast<float>(height_) / scale),
        layout::kCorner, layout::kCorner);
    brush_->SetColor(kPanel);
    d2d_->FillRoundedRectangle(panel, brush_.Get());
    brush_->SetColor(kPanelEdge);
    d2d_->DrawRoundedRectangle(panel, brush_.Get(), 1.0f);

    DrawText(L"LiquiDock", titleFormat_.Get(),
             D2D1::RectF(layout::kPadding, 18.0f, 400.0f, 52.0f), kTitle);
    DrawText(L"Every change applies straight away", hintFormat_.Get(),
             D2D1::RectF(layout::kPadding, 40.0f, 460.0f, 60.0f), kHint);

    DrawTabs();

    bool clipped = false;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        if (row.tab != activeTab_) {
            continue;
        }
        const bool hovered = (static_cast<int>(i) == hoverRow_);

        // The items list scrolls, so it is drawn inside a clip that stops it
        // spilling out of the bottom of the panel.
        const bool wantsClip = (row.kind == Row::Kind::Item);
        if (wantsClip != clipped) {
            if (wantsClip) {
                d2d_->PushAxisAlignedClip(itemsClip_, D2D1_ANTIALIAS_MODE_ALIASED);
            } else {
                d2d_->PopAxisAlignedClip();
            }
            clipped = wantsClip;
        }

        if (row.kind == Row::Kind::Item || row.kind == Row::Kind::AddItem ||
            row.kind == Row::Kind::AddSeparator) {
            DrawItem(row, hovered, pointerX_);
            continue;
        }

        if (row.kind == Row::Kind::Section) {
            DrawText(row.label, sectionFormat_.Get(),
                     D2D1::RectF(row.bounds.left, row.bounds.top + 18.0f, row.bounds.right,
                                 row.bounds.bottom),
                     kSection);
            continue;
        }

        if (hovered) {
            brush_->SetColor(kRowHover);
            d2d_->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(row.bounds.left - 8.0f, row.bounds.top + 2.0f,
                                              row.bounds.right + 8.0f, row.bounds.bottom - 2.0f),
                                  8.0f, 8.0f),
                brush_.Get());
        }

        const float textRight = row.control.left - 14.0f;
        DrawText(row.label, labelFormat_.Get(),
                 D2D1::RectF(row.bounds.left, row.bounds.top + 8.0f, textRight, row.bounds.top + 30.0f),
                 kLabel);
        if (row.hint) {
            DrawText(row.hint, hintFormat_.Get(),
                     D2D1::RectF(row.bounds.left, row.bounds.top + 27.0f, textRight,
                                 row.bounds.bottom),
                     kHint);
        }

        switch (row.kind) {
            case Row::Kind::Slider:
                DrawSlider(row, hovered);
                DrawText(FormatValue(*row.number, row.decimals, row.suffix), valueFormat_.Get(),
                         D2D1::RectF(row.control.right + 8.0f, row.control.top,
                                     row.bounds.right, row.control.bottom + 4.0f),
                         kValue);
                break;
            case Row::Kind::Toggle:
                DrawToggle(row, hovered);
                break;
            case Row::Kind::Choice:
                DrawChoice(row, hovered ? pointerX_ : -1.0f);
                break;
            default:
                break;
        }
    }

    if (clipped) {
        d2d_->PopAxisAlignedClip();
    }

    if (activeTab_ == Tab::Items) {
        if (draggingItem_ && dropIndex_ >= 0) {
            // Where it would land. Drawn after the rows so it is not clipped
            // away by the one it happens to be sitting on.
            float y = itemsClip_.bottom;
            for (const Row& row : rows_) {
                if (row.tab == Tab::Items && row.kind == Row::Kind::Item &&
                    row.itemIndex == dropIndex_) {
                    y = row.bounds.top;
                    break;
                }
            }
            y = std::clamp(y, itemsClip_.top, itemsClip_.bottom);
            brush_->SetColor(kOn);
            d2d_->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(layout::kPadding, y - 1.0f,
                                              layout::kWidth - layout::kPadding, y + 1.0f),
                                  1.0f, 1.0f),
                brush_.Get());
        }
        DrawDetailBar();
    }

    DrawText(activeTab_ == Tab::Items
                 ? L"Esc to close  ·  Ctrl+Z undoes the last change to the list"
                 : L"Esc to close  ·  these are the same values as settings.txt",
             hintFormat_.Get(),
             D2D1::RectF(layout::kPadding, static_cast<float>(height_) / scale - 28.0f,
                         layout::kWidth - layout::kPadding,
                         static_cast<float>(height_) / scale - 8.0f),
             Grey(1.0f, 0.28f));

    d2d_->SetTransform(D2D1::Matrix3x2F::Identity());
    const HRESULT hr = d2d_->EndDraw();
    d2d_->SetTarget(nullptr);
    if (FAILED(hr)) {
        // The device went away underneath us. The dock rebuilds it; this window
        // simply closes, because a preferences panel is not worth recovering.
        LogWarn("Preferences drawing failed: {}", FormatHResult(hr));
        backBuffer_.Reset();
        Hide();
        return;
    }
    target_.EndFrame();
}

LRESULT CALLBACK SettingsWindow::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                              LPARAM lParam) {
    SettingsWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<SettingsWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->WndProc(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT SettingsWindow::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    const float scale = static_cast<float>(dpi_) / 96.0f;

    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            Render();
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_NCHITTEST: {
            // The panel has no title bar, so the header area is the grab handle.
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &point);
            if (static_cast<float>(point.y) / scale < layout::kTitleHeight) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_MOUSEMOVE: {
            if (!mouseTracking_) {
                TRACKMOUSEEVENT track{sizeof(track)};
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                mouseTracking_ = TrackMouseEvent(&track) != 0;
            }
            pointerX_ = static_cast<float>(GET_X_LPARAM(lParam)) / scale;
            pointerY_ = static_cast<float>(GET_Y_LPARAM(lParam)) / scale;

            if (dragRow_ >= 0) {
                if (ApplyPointer(dragRow_, pointerX_, true)) {
                    CommitChange();
                }
                return 0;
            }

            if (pressItem_ >= 0) {
                // Four pixels of slop, so a click with a shaky hand is still a
                // click. Past that it is a drag and stays one.
                if (!draggingItem_ && std::fabs(pointerY_ - pressY_) > 4.0f) {
                    draggingItem_ = true;
                }
                if (draggingItem_) {
                    dropIndex_ = DropIndexAt(pointerY_);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            const int row = RowAt(pointerX_, pointerY_);
            if (TabAt(pointerX_, pointerY_) >= 0) {
                InvalidateRect(hwnd, nullptr, FALSE); // the strip highlights under the pointer
            }
            if (row != hoverRow_) {
                hoverRow_ = row;
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (row >= 0 && rows_[static_cast<size_t>(row)].kind == Row::Kind::Choice) {
                InvalidateRect(hwnd, nullptr, FALSE); // segment hover follows the pointer
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            mouseTracking_ = false;
            hoverRow_ = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN: {
            const float x = static_cast<float>(GET_X_LPARAM(lParam)) / scale;
            const float y = static_cast<float>(GET_Y_LPARAM(lParam)) / scale;

            const int tab = TabAt(x, y);
            if (tab >= 0) {
                if (static_cast<Tab>(tab) != activeTab_) {
                    CommitEdit();
                    activeTab_ = static_cast<Tab>(tab);
                    itemScroll_ = 0.0f;
                    hoverRow_ = -1;
                    expandedItem_ = -1;
                    LayoutRows();
                    ApplyWindowSize();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }

            const int row = RowAt(x, y);
            if (row >= 0) {
                const Row::Kind kind = rows_[static_cast<size_t>(row)].kind;
                if (kind == Row::Kind::AddItem || kind == Row::Kind::AddSeparator) {
                    HandleItemClick(row, x);
                    return 0;
                }
                if (kind == Row::Kind::Item) {
                    const Row& item = rows_[static_cast<size_t>(row)];
                    // The buttons first: they sit on the row and are not a drag
                    // handle.
                    if (x >= item.control.left && x <= item.control.right &&
                        y >= item.control.top && y <= item.control.bottom) {
                        HandleItemClick(row, x);
                        return 0;
                    }
                    if (item.itemIndex == expandedItem_ &&
                        y > item.bounds.top + layout::kItemHeight) {
                        if (HandleEditorClick(item, x, y)) {
                            InvalidateRect(hwnd, nullptr, FALSE);
                        }
                        return 0;
                    }
                    // Otherwise this is either a click that opens the editor or
                    // the start of a drag; which one is decided on the way up.
                    CommitEdit();
                    pressItem_ = item.itemIndex;
                    pressY_ = y;
                    draggingItem_ = false;
                    dropIndex_ = -1;
                    SetCapture(hwnd);
                    return 0;
                }
                if (ApplyPointer(row, x, false)) {
                    CommitChange();
                }
                if (rows_[static_cast<size_t>(row)].kind == Row::Kind::Slider) {
                    dragRow_ = row;
                    SetCapture(hwnd);
                }
            }
            return 0;
        }

        case WM_LBUTTONUP:
            if (dragRow_ >= 0) {
                dragRow_ = -1;
                ReleaseCapture();
                return 0;
            }
            if (pressItem_ >= 0) {
                const int pressed = pressItem_;
                const bool dragged = draggingItem_;
                const int target = dropIndex_;
                pressItem_ = -1;
                draggingItem_ = false;
                dropIndex_ = -1;
                ReleaseCapture();
                if (dragged && target >= 0) {
                    items_.MoveTo(static_cast<size_t>(pressed), static_cast<size_t>(target));
                    expandedItem_ = -1;
                    CommitItems();
                } else if (!dragged) {
                    // A click on a row opens it. Clicking the open one closes
                    // it again, because there is nowhere else for the gesture
                    // to go and a row that cannot be closed is a trap.
                    expandedItem_ = (expandedItem_ == pressed) ? -1 : pressed;
                    LayoutRows();
                    ApplyWindowSize();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;

        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &point);
            const float x = static_cast<float>(point.x) / scale;
            if (itemScrollMax_ > 0.0f && x >= itemsClip_.left && x <= itemsClip_.right) {
                const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
                itemScroll_ = std::clamp(itemScroll_ - delta * layout::kItemHeight, 0.0f,
                                         itemScrollMax_);
                LayoutRows();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_CHAR:
            if (editItem_ >= 0 && editField_ != Field::Count) {
                const wchar_t ch = static_cast<wchar_t>(wParam);
                if (ch == VK_BACK) {
                    if (caret_ > 0) {
                        editText_.erase(caret_ - 1, 1);
                        --caret_;
                    }
                } else if (ch == VK_RETURN || ch == VK_ESCAPE) {
                    // Handled in WM_KEYDOWN; swallowed here so Enter does not
                    // insert a control character into the field.
                } else if (ch >= 0x20) {
                    editText_.insert(caret_, 1, ch);
                    ++caret_;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            return 0;

        case WM_KEYDOWN:
            if (wParam == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                CommitEdit();
                if (items_.Undo()) {
                    expandedItem_ = -1;
                    CommitItems();
                } else {
                    // Saying nothing at all would read as a dead key.
                    MessageBeep(MB_OK);
                }
                return 0;
            }
            if (editItem_ >= 0 && editField_ != Field::Count) {
                switch (wParam) {
                    case VK_LEFT:
                        if (caret_ > 0) {
                            --caret_;
                        }
                        break;
                    case VK_RIGHT:
                        if (caret_ < editText_.size()) {
                            ++caret_;
                        }
                        break;
                    case VK_HOME: caret_ = 0; break;
                    case VK_END: caret_ = editText_.size(); break;
                    case VK_DELETE:
                        if (caret_ < editText_.size()) {
                            editText_.erase(caret_, 1);
                        }
                        break;
                    case VK_RETURN:
                        CommitEdit();
                        CommitItems();
                        break;
                    case VK_ESCAPE:
                        // Escape leaves the field rather than the window: the
                        // nearer thing wins, which is what Escape means.
                        editItem_ = -1;
                        editField_ = Field::Count;
                        editText_.clear();
                        break;
                    default: break;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                Hide();
            }
            return 0;

        case WM_ACTIVATE:
            // Deliberately nothing. This used to hide on losing focus, which is
            // reasonable for a popover and wrong for a window: it made the
            // window impossible to Alt+Tab back to, because switching away was
            // what destroyed it.
            return 0;

        case kIconsMessage:
            DrainIcons();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_TIMER:
            if (wParam == kSaveTimer) {
                KillTimer(hwnd, kSaveTimer);
                settings_.Save();
            }
            return 0;

        case WM_CLOSE:
            Hide();
            return 0;

        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace liquidock
