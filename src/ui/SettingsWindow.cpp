#include "ui/SettingsWindow.h"

#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <utility>

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
// Drives the row animations. A timer rather than a message that reposts itself:
// posted messages outrank hardware input in GetMessage's order, so an animation
// pumped that way starves the clicks it exists to respond to. WM_TIMER is the
// lowest priority there is and cannot.
constexpr UINT_PTR kAnimTimer = 2;
constexpr UINT kAnimIntervalMs = 16;
constexpr UINT kSaveDelayMs = 400;
// Posted by the icon loader thread as the list's icons come in.
constexpr UINT kIconsMessage = WM_APP + 1;
// Posted when the installed-app list has been read, and as its icons arrive.
constexpr UINT kCatalogMessage = WM_APP + 2;
constexpr UINT kSuggestionIconsMessage = WM_APP + 3;

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
// A row is a card: the label, what it is for, and the control all live on one
// rounded track, and for a slider that track *is* the control. Measured off the
// reference: a 651x70 pill with a radius of 12 and a 12 px gap, which is a
// radius of 0.17 of the height and a gap of 0.17 again.
// The explanation moved to a tooltip, so a card holds one line instead of two
// and gets its height back. It was stacked under the label, where the slider's
// own fill boundary ran straight through it - a description cut in half by the
// control it describes reads as a rendering fault, and it was one of the few
// things on the page you could not fix by adjusting anything.
constexpr float kRowHeight = 54.0f;   // pitch, gap included
constexpr float kPillHeight = 44.0f;
constexpr float kPillRadius = 9.0f;
constexpr float kPillPadX = 16.0f;
// The knob: a bright bar half the track's height, three pixels wide, kept a
// radius clear of either end so it never rides the rounding.
constexpr float kKnobWidth = 3.0f;
constexpr float kKnobHeight = 26.0f;
constexpr float kKnobInset = kPillRadius + 5.0f;
// The track begins after the label, so zero is past the text rather than under
// it. The knob then cannot cross the label at any value - not at the minimum,
// and not on the way up either, which is the half of the problem that indenting
// the label alone would have left.
constexpr float kLabelIndent = 16.0f;
constexpr float kLabelGap = 14.0f;

// The window's own buttons, top right.
constexpr float kWindowButton = 30.0f;
constexpr float kWindowButtonTop = 18.0f;

// The switch. Measured off the reference and held as ratios of the circle,
// which is the only measurement that matters: ring 0.14 of the diameter, bar
// 0.16 thick and 1.2 long, a gap of 0.12 between them.
constexpr float kSwitchCircle = 20.0f;
constexpr float kSwitchRing = 2.8f;
constexpr float kSwitchBar = 3.2f;
constexpr float kSwitchBarLength = 24.0f;
constexpr float kSwitchGap = 2.5f;
constexpr float kSwitchWidth = kSwitchCircle + kSwitchGap + kSwitchBarLength;

// Long enough to be seen, short enough that it never delays the setting it is
// reporting. The value itself changes on the click; this is only the drawing
// catching up.
constexpr float kToggleSeconds = 0.17f;
constexpr float kHoverSeconds = 0.11f;
// Long enough that sweeping the list does not strobe, short enough that
// stopping to ask feels answered rather than waited for.
constexpr float kTooltipDelay = 0.28f;
constexpr float kTooltipFade = 0.12f;
constexpr float kTooltipPadX = 13.0f;
// Roomier than the text needs, on purpose: a tooltip that hugs its own
// ascender reads as cramped however well centred it is.
constexpr float kTooltipHeight = 34.0f;
// Clear of the pointer, below and to the right, the way every tooltip is.
constexpr float kTooltipOffsetX = 16.0f;
constexpr float kTooltipOffsetY = 20.0f;
constexpr float kSectionHeight = 46.0f;
constexpr float kColumnGap = 26.0f;
constexpr float kControlWidth = 120.0f;
constexpr float kChoicePadX = 13.0f;
constexpr float kChoiceHeight = 29.0f;
constexpr float kEmptyHeight = 30.0f;
// The value gets a column of its own on the right of the card, and the track
// stops before it. Sharing the width meant that at the top of a range the knob
// arrived on top of the number - "2.00x" with a caret through the x, and two
// pixels between the two of them and the card's edge where the label has
// sixteen. The knob can no longer reach it, and the fill still ends flush with
// the track it fills.
constexpr float kValueWidth = 52.0f;   // wide enough for "236 px"
constexpr float kValueGap = 10.0f;     // between the track's end and the number
constexpr float kValueGutter = kPillPadX + kValueWidth + kValueGap;
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

// The grids. A row of names one per line spent the whole window on twenty
// items and told you nothing a picture would not have; this is what the dock
// itself looks like, at a size you can pick a target out of, with the name and
// the path on the tooltip where they cost nothing until wanted.
constexpr float kTile = 58.0f;      // one cell, icon and its air
constexpr float kTileIcon = 40.0f;
constexpr float kTileGap = 6.0f;
constexpr float kGridGap = 18.0f;   // between the dock's grid and the divider
constexpr float kGridRuleGap = 16.0f;  // between the header line and the icons
constexpr float kRuleCaptionGap = 8.0f;
constexpr float kSearchWidth = 220.0f;
constexpr float kSearchHeight = 26.0f;
// The fixed row of add buttons above the list, and how wide each one is.
constexpr float kActionRow = 56.0f;
constexpr float kActionWidth = 124.0f;
constexpr float kCorner = design::kCornerRadius;
} // namespace layout

const wchar_t* const kTabNames[] = {L"Items", L"Glass", L"Dock", L"Behaviour"};

D2D1_COLOR_F Grey(float level, float alpha) {
    return D2D1::ColorF(level, level, level, alpha);
}

D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
                        a.a + (b.a - a.a) * t);
}

// Smoothstep. Zero slope at both ends, so nothing starts or stops abruptly.
float Smooth(float t) {
    return t * t * (3.0f - 2.0f * t);
}

// Black, and opaque. The dock is glass because you are meant to look past it;
// preferences are meant to be read, and every other surface in the window is
// defined as a percentage of white - so a black ground is the one that makes
// those percentages mean what they say. Even three percent of translucency
// ghosts white text from the window behind straight through the labels, which
// against a ground this dark is a 35% swing in brightness and looks like a bug.
const D2D1_COLOR_F kPanel = D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f);
const D2D1_COLOR_F kPanelEdge = Grey(1.0f, 0.14f);
const D2D1_COLOR_F kTitle = Grey(1.0f, 0.95f);
const D2D1_COLOR_F kSection = Grey(1.0f, 0.42f);
const D2D1_COLOR_F kLabel = Grey(1.0f, 0.88f);
const D2D1_COLOR_F kHint = Grey(1.0f, 0.40f);
const D2D1_COLOR_F kValue = Grey(1.0f, 0.62f);
// The track, and the part of it behind the value. The fill has to be clearly
// brighter than the track and clearly dimmer than the text, or it reads as a
// second control rather than as how far along this one is.
const D2D1_COLOR_F kTrack = Grey(1.0f, 0.07f);
const D2D1_COLOR_F kTrackHover = Grey(1.0f, 0.10f);
const D2D1_COLOR_F kFill = Grey(1.0f, 0.17f);
const D2D1_COLOR_F kFillHover = Grey(1.0f, 0.21f);
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
    suggestionLoader_.Stop();
    catalog_.Stop();
    itemIcons_.clear();
    suggestionIcons_.clear();
    if (hwnd_) {
        KillTimer(hwnd_, kSaveTimer);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    backBuffer_.Reset();
    target_.Reset();
}

void SettingsWindow::BuildRows() {
    // Carried across the rebuild by label, so a rebuild - which happens for
    // reasons that have nothing to do with these rows, like an item being
    // added - does not make every switch on the page animate from scratch.
    std::vector<std::pair<const wchar_t*, float>> carried;
    for (const Row& row : rows_) {
        if (row.kind == Row::Kind::Toggle && row.label) {
            carried.emplace_back(row.label, row.anim);
        }
    }

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
    ApplyFilter();
    for (const int index : filtered_) {
        Row row;
        row.kind = Row::Kind::Suggestion;
        row.tab = Tab::Items;
        row.itemIndex = index;
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
    slider(Tab::Glass, L"Inner shadow", L"The dark shoulder just inside the rim",
           &settings_.innerShadow, 0.0f, 1.0f, 2, 1);
    slider(Tab::Glass, L"Border", L"How opaque the bright edge is", &settings_.rimOpacity, 0.0f,
           1.0f, 2, 1);

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

    slider(Tab::Dock, L"Icon spacing", L"The space between neighbouring icons",
           &settings_.iconGap, 0.0f, 60.0f, 0, 0, L" px");
    slider(Tab::Dock, L"Divider spacing", L"The air on each side of a divider",
           &settings_.dividerGap, 0.0f, 120.0f, 0, 0, L" px");

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

    for (Row& row : rows_) {
        if (row.kind != Row::Kind::Toggle || !row.label) {
            continue;
        }
        for (const auto& [label, value] : carried) {
            if (wcscmp(label, row.label) == 0) {
                row.anim = value;
                break;
            }
        }
    }
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
        if (tab == Tab::Items &&
            (row.kind == Row::Kind::Item || row.kind == Row::Kind::Suggestion)) {
            continue; // counted as grids, below
        }
        const int column = std::clamp(row.column, 0, columns - 1);
        if (row.kind == Row::Kind::Section) {
            y[column] += layout::kSectionHeight;
        } else {
            y[column] += layout::kRowHeight;
        }
    }
    if (tab == Tab::Items) {
        const int perLine = GridColumns();
        const float line = layout::kTile + layout::kTileGap;
        const auto lines = [perLine](size_t count) {
            return static_cast<float>((count + perLine - 1) / perLine);
        };
        y[0] += lines(items_.items().size()) * line;
        if (expandedItem_ >= 0) {
            y[0] += layout::kEditorHeight + 8.0f;
        }
        y[0] += layout::kGridGap + layout::kRuleCaptionGap + layout::kSearchHeight +
                layout::kGridRuleGap;
        // A filter that matches nothing still needs a line to say so in. Without
        // it the window closed up to exactly the height of no icons at all, and
        // the message landed on top of the footer.
        y[0] += filtered_.empty() ? layout::kEmptyHeight : lines(filtered_.size()) * line;
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
        if (row.kind == Row::Kind::Item || row.kind == Row::Kind::Suggestion) {
            continue; // both grids are placed together, below
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
            const D2D1_RECT_F pill = D2D1::RectF(
                row.bounds.left, row.bounds.top,
                row.bounds.right, row.bounds.top + layout::kPillHeight);
            if (row.kind == Row::Kind::Slider) {
                // The knob's travel: from the end of the label to the start of
                // the value's column. Measured per row rather than reserving one
                // column for the widest label on the page - "Reach" would then
                // start its track where "Magnify under the cursor" ends, and
                // most of its card would be empty for the sake of an alignment
                // nobody is looking for.
                const float labelEnd = pill.left + layout::kLabelIndent +
                                       MeasureText(labelFormat_.Get(), row.label ? row.label : L"") +
                                       layout::kLabelGap;
                row.control = D2D1::RectF(labelEnd + layout::kKnobWidth, pill.top,
                                          pill.right - layout::kValueGutter - layout::kKnobInset,
                                          pill.bottom);
            } else {
                // Toggles and choices keep a widget on the right of the card.
                const float centreY = (pill.top + pill.bottom) * 0.5f;
                row.control =
                    D2D1::RectF(pill.right - layout::kPillPadX - layout::kControlWidth,
                                centreY - 12.0f, pill.right - layout::kPillPadX, centreY + 12.0f);
                if (row.kind == Row::Kind::Choice) {
                    // Whatever its options happen to measure, so the cursor and
                    // the click agree with what was drawn.
                    const std::vector<D2D1_RECT_F> cells = ChoiceCells(row);
                    if (!cells.empty()) {
                        row.control = D2D1::RectF(cells.front().left, cells.front().top,
                                                  cells.back().right, cells.back().bottom);
                    }
                }
            }
        }
        y[column] += height;
    }

    if (listTab) {
        // Both grids in one pass: the second has to start where the first ends,
        // and neither knows how many lines the other took.
        const float width = layout::kWidth - 2.0f * layout::kPadding;
        y[0] = LayoutGrids(listTop, width);
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
    // y[] accumulates unscrolled - the scroll is subtracted into each row's
    // bounds, not into the running total - so adding it back here counted it
    // twice, and the ceiling grew by a notch for every notch scrolled. Which is
    // a scrollbar that never reaches the end.
    const float listContent = y[0] - listTop;
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

const wchar_t* SettingsWindow::CursorFor(float x, float y) const {
    if (WindowButtonAt(x, y) >= 0 || TabAt(x, y) >= 0) {
        return IDC_HAND;
    }
    if (activeTab_ == Tab::Items && !suggestions_.empty() && x >= searchRect_.left &&
        x <= searchRect_.right && y >= searchRect_.top - 4.0f && y <= searchRect_.bottom + 4.0f) {
        return IDC_IBEAM;
    }
    if (activeTab_ == Tab::Items && expandedItem_ >= 0 && x >= editorPanel_.left &&
        x <= editorPanel_.right && y >= editorPanel_.top && y <= editorPanel_.bottom) {
        return EditorCursorFor(x, y);
    }
    const int index = RowAt(x, y);
    if (index < 0) {
        return IDC_ARROW;
    }
    const Row& row = rows_[static_cast<size_t>(index)];
    const bool inControl = (x >= row.control.left && x <= row.control.right);

    switch (row.kind) {
        case Row::Kind::Toggle:
            return IDC_HAND; // the whole row flips it
        case Row::Kind::Slider:
            // The track, which is the card from the end of the label onward.
            return (y <= PillRect(row).bottom && x >= row.control.left - layout::kKnobWidth)
                       ? IDC_HAND
                       : IDC_ARROW;
        case Row::Kind::Choice:
            return inControl ? IDC_HAND : IDC_ARROW;
        case Row::Kind::AddItem:
        case Row::Kind::AddSeparator:
        case Row::Kind::Suggestion:
            return IDC_HAND;
        case Row::Kind::Item:
            return IDC_HAND; // opens the editor, or drags to reorder
        default:
            return IDC_ARROW;
    }
}

const wchar_t* SettingsWindow::EditorCursorFor(float x, float y) const {
    D2D1_RECT_F fields[static_cast<int>(Field::Count)];
    D2D1_RECT_F buttons[static_cast<int>(Field::Count)];
    EditorRects(editorPanel_, fields, buttons);
    for (int i = 0; i < static_cast<int>(Field::Count); ++i) {
        const D2D1_RECT_F& button = buttons[i];
        if (button.right > button.left && x >= button.left && x <= button.right &&
            y >= button.top && y <= button.bottom) {
            return IDC_HAND;
        }
        const D2D1_RECT_F& field = fields[i];
        if (x >= field.left && x <= field.right && y >= field.top && y <= field.bottom) {
            if (i == static_cast<int>(Field::Show) || i == static_cast<int>(Field::Admin)) {
                return IDC_HAND;
            }
            if (i == static_cast<int>(Field::Path) || i == static_cast<int>(Field::Icon)) {
                return IDC_ARROW; // read-only; the button beside it changes them
            }
            return IDC_IBEAM;
        }
    }
    return IDC_ARROW;
}

int SettingsWindow::GridColumns() const {
    const float width = layout::kWidth - 2.0f * layout::kPadding;
    return std::max(1, static_cast<int>((width + layout::kTileGap) /
                                        (layout::kTile + layout::kTileGap)));
}

float SettingsWindow::LayoutGrids(float top, float width) {
    const int columns = GridColumns();
    const float used = columns * layout::kTile + (columns - 1) * layout::kTileGap;
    const float left = layout::kPadding + (width - used) * 0.5f;

    auto place = [&](Row::Kind kind, float startY) {
        int index = 0;
        for (Row& row : rows_) {
            if (row.tab != Tab::Items || row.kind != kind) {
                continue;
            }
            const int column = index % columns;
            const int line = index / columns;
            const float x = left + column * (layout::kTile + layout::kTileGap);
            const float y = startY + line * (layout::kTile + layout::kTileGap) - itemScroll_;
            row.bounds = D2D1::RectF(x, y, x + layout::kTile, y + layout::kTile);
            row.control = D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);
            ++index;
        }
        const int lines = (index + columns - 1) / columns;
        return startY + lines * (layout::kTile + layout::kTileGap);
    };

    float y = place(Row::Kind::Item, top);

    editorPanel_ = D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);
    if (expandedItem_ >= 0) {
        // Under the grid rather than inside it: a cell is fifty-eight pixels
        // and the editor is five rows of fields.
        const float panelTop = y + 8.0f;
        editorPanel_ = D2D1::RectF(layout::kPadding, panelTop - itemScroll_,
                                   layout::kWidth - layout::kPadding,
                                   panelTop + layout::kEditorHeight - itemScroll_);
        y = panelTop + layout::kEditorHeight;
    }

    gridRuleY_ = y + layout::kGridGap - itemScroll_;

    // The header line under the rule carries the caption on the left and the
    // search box on the right, and the grid starts a clear gap below it - the
    // caption sitting directly on top of the first line of icons read as a
    // label for the icon under it rather than for the section.
    const float headerTop = gridRuleY_ + layout::kRuleCaptionGap;
    searchRect_ = D2D1::RectF(layout::kWidth - layout::kPadding - layout::kSearchWidth, headerTop,
                              layout::kWidth - layout::kPadding,
                              headerTop + layout::kSearchHeight);
    y = place(Row::Kind::Suggestion,
              y + layout::kGridGap + layout::kRuleCaptionGap + layout::kSearchHeight +
                  layout::kGridRuleGap);
    return y;
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

    // DirectWrite's vertical centring centres the *line box*, and a line box is
    // whatever line height the font recommends - which reserves more room above
    // the ascent than below the descent, because that is what stacked lines of
    // prose want. One line inside a pill wants the opposite: the ink centred.
    // Collapsing the line to exactly ascent + descent, with the baseline at the
    // ascent, makes the line box the ink box, and centring it then centres what
    // you can actually see. Which is the same fix the dock's hover label needed,
    // for the same reason.
    //
    // Nudging the rectangle down by a pixel or two is what this replaces. That
    // works at exactly one font size and is wrong at every other, so the panel
    // had four different nudges in it and none of them agreed.
    auto tighten = [this](IDWriteTextFormat* format) {
        if (!format) {
            return;
        }
        ComPtr<IDWriteFontCollection> collection;
        if (FAILED(format->GetFontCollection(&collection)) || !collection) {
            if (FAILED(dwrite_->GetSystemFontCollection(&collection, FALSE))) {
                return;
            }
        }
        wchar_t family[128]{};
        if (FAILED(format->GetFontFamilyName(family, static_cast<UINT32>(std::size(family))))) {
            return;
        }
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (FAILED(collection->FindFamilyName(family, &index, &exists)) || !exists) {
            return;
        }
        ComPtr<IDWriteFontFamily> group;
        ComPtr<IDWriteFont> font;
        if (FAILED(collection->GetFontFamily(index, &group)) ||
            FAILED(group->GetFirstMatchingFont(format->GetFontWeight(), format->GetFontStretch(),
                                               format->GetFontStyle(), &font))) {
            return;
        }
        DWRITE_FONT_METRICS metrics{};
        font->GetMetrics(&metrics);
        if (metrics.designUnitsPerEm == 0) {
            return;
        }
        const float em = format->GetFontSize() / static_cast<float>(metrics.designUnitsPerEm);
        const float ascent = metrics.ascent * em;
        const float descent = metrics.descent * em;
        format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, ascent + descent, ascent);
    };

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
    // A size up from the label it explains. It was set in the hint's 11.5,
    // which is a footnote size - fine under a label where the eye is already
    // there, too small for something the pointer has to be held still to read.
    LD_CHECK(format(14.5f, DWRITE_FONT_WEIGHT_NORMAL, &tipFormat_));
    tipFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    tipFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    tipFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    LD_CHECK(format(12.5f, DWRITE_FONT_WEIGHT_NORMAL, &valueFormat_));
    valueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    // The value is drawn into the whole height of the card now, not onto a
    // single baseline beside a control, so it has to find its own middle.
    valueFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // Everything that is ever drawn centred in a box, which on this panel is
    // everything.
    tighten(titleFormat_.Get());
    tighten(sectionFormat_.Get());
    tighten(labelFormat_.Get());
    tighten(hintFormat_.Get());
    tighten(tipFormat_.Get());
    tighten(valueFormat_.Get());
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
    // Again, now that the text formats exist: the track's start is measured
    // from the label, and the first pass ran before there was anything to
    // measure with.
    LayoutRows();

    // After the device resources, because turning the pixels into D2D bitmaps
    // needs the context that CreateDeviceResources builds.
    search_.clear();
    searchFocused_ = false;
    filtered_.clear();
    StartIconLoad();
    StartCatalogLoad();

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

bool SettingsWindow::active() const {
    return hwnd_ && visible_ && GetForegroundWindow() == hwnd_;
}

void SettingsWindow::ReloadFromDisk() {
    if (!hwnd_ || !visible_ || interacting()) {
        return;
    }
    settings_.Load();
    items_.Load();
    BuildRows();
    LayoutRows();
    InvalidateRect(hwnd_, nullptr, FALSE);
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
        if (row.kind == Row::Kind::Item || row.kind == Row::Kind::Suggestion) {
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

bool SettingsWindow::StepSlider(int index, int steps) {
    if (index < 0 || index >= static_cast<int>(rows_.size()) || steps == 0) {
        return false;
    }
    Row& row = rows_[static_cast<size_t>(index)];
    if (row.kind != Row::Kind::Slider || !row.number) {
        return false;
    }
    // One unit of what the row displays: 1, 0.1 or 0.01.
    float unit = 1.0f;
    for (int i = 0; i < row.decimals; ++i) {
        unit *= 0.1f;
    }
    const float value =
        std::clamp(*row.number + unit * static_cast<float>(steps), row.minimum, row.maximum);
    if (std::fabs(value - *row.number) < unit * 0.01f) {
        return false;
    }
    // Snapped to the unit, or a value nudged from something a drag left at
    // 0.5037 would keep its stray digits forever and the readout would stop
    // agreeing with what the arrows do to it.
    *row.number = std::round(value / unit) * unit;
    return true;
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
            const std::vector<D2D1_RECT_F> cells = ChoiceCells(row);
            int picked = -1;
            for (size_t i = 0; i < cells.size(); ++i) {
                if (x >= cells[i].left && x <= cells[i].right) {
                    picked = static_cast<int>(i);
                    break;
                }
            }
            // Outside every option is not a choice. The old even split had no
            // outside - the whole right of the card belonged to one option or
            // another, so a click near the value column changed the setting.
            if (picked < 0 || picked == *row.choice) {
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

int SettingsWindow::DropIndexAt(float x, float y) const {
    // Where an insertion at the pointer would land. A grid reads in two axes:
    // the line comes from y, the position within it from x, and a pointer past
    // the end of a line lands after its last tile rather than snapping back to
    // the start of the next one. Measured against the tiles as drawn, so it
    // follows the scroll without having to know about it.
    int index = 0;
    for (const Row& row : rows_) {
        if (row.tab != Tab::Items || row.kind != Row::Kind::Item) {
            continue;
        }
        index = row.itemIndex + 1;
        if (y > row.bounds.bottom) {
            continue; // a line above the pointer
        }
        if (y < row.bounds.top || x < (row.bounds.left + row.bounds.right) * 0.5f) {
            return row.itemIndex;
        }
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

bool SettingsWindow::HandleEditorClick(const D2D1_RECT_F& panel, int itemIndex, float x,
                                      float y) {
    D2D1_RECT_F fields[static_cast<int>(Field::Count)];
    D2D1_RECT_F buttons[static_cast<int>(Field::Count)];
    EditorRects(panel, fields, buttons);

    const size_t index = static_cast<size_t>(itemIndex);
    if (itemIndex < 0 || index >= items_.items().size()) {
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
            BeginEdit(itemIndex, static_cast<Field>(i));
            return true;
        }
    }
    return false;
}

void SettingsWindow::StartCatalogLoad() {
    if (!hwnd_) {
        return;
    }
    catalog_.Start(hwnd_, kCatalogMessage);
}

void SettingsWindow::DrainSuggestionIcons() {
    std::vector<IconBitmap> loaded;
    suggestionLoader_.Collect(loaded);
    if (!d2d_) {
        return;
    }
    for (const IconBitmap& icon : loaded) {
        if (icon.slot < 0 || static_cast<size_t>(icon.slot) >= suggestionIcons_.size() ||
            icon.pixels.empty()) {
            continue;
        }
        const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap> bitmap;
        const D2D1_SIZE_U size =
            D2D1::SizeU(static_cast<UINT32>(icon.size), static_cast<UINT32>(icon.size));
        if (SUCCEEDED(d2d_->CreateBitmap(size, icon.pixels.data(),
                                         static_cast<UINT32>(icon.size) * 4, properties,
                                         &bitmap))) {
            suggestionIcons_[static_cast<size_t>(icon.slot)] = std::move(bitmap);
        }
    }
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
    // At the size the grid draws them, in device pixels. Loading them at the
    // old list's twenty-four and drawing them into a forty-pixel cell is an
    // upscale of two thirds, and looks it.
    const float scale = static_cast<float>(dpi_) / 96.0f;
    iconLoader_.Start(items_.items(), static_cast<int>(std::lround(layout::kTileIcon * scale)),
                      hwnd_, kIconsMessage);
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

bool SettingsWindow::AdvanceAnimation() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (frequency_.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency_);
    }
    float delta = 0.0f;
    if (lastFrame_.QuadPart != 0 && frequency_.QuadPart != 0) {
        delta = static_cast<float>(now.QuadPart - lastFrame_.QuadPart) /
                static_cast<float>(frequency_.QuadPart);
    }
    lastFrame_ = now;
    // A window that has been sitting idle must not have its animations jump the
    // whole way on the first frame after it wakes.
    delta = std::clamp(delta, 0.0f, 0.05f);

    bool moving = false;
    auto approach = [&](float& value, float target, float seconds) {
        if (value < 0.0f) {
            value = target; // first sight of this row: no journey to make
            return;
        }
        if (std::fabs(value - target) < 0.001f) {
            value = target;
            return;
        }
        const float step = (seconds > 0.0f) ? (delta / seconds) : 1.0f;
        value = (value < target) ? std::min(target, value + step) : std::max(target, value - step);
        moving = true;
    };

    for (size_t i = 0; i < rows_.size(); ++i) {
        Row& row = rows_[i];
        if (row.tab != activeTab_) {
            continue;
        }
        if (row.kind == Row::Kind::Toggle && row.flag) {
            approach(row.anim, *row.flag ? 1.0f : 0.0f, layout::kToggleSeconds);
        }
        if (row.kind == Row::Kind::Choice && row.choice) {
            // The same journey the switch makes, over as many stops as there
            // are options: the pill slides to the one you picked rather than
            // appearing there.
            approach(row.anim, static_cast<float>(*row.choice), layout::kToggleSeconds * 2.0f);
        }
        approach(row.hover, (static_cast<int>(i) == hoverRow_) ? 1.0f : 0.0f,
                 layout::kHoverSeconds);
    }

    // The tooltip waits out its dwell, then fades in and follows the pointer.
    // It is kept alive by the pointer moving as much as by it resting, so the
    // redraw has to stay armed while it is up.
    float dwelled = 0.0f;
    if (hoverSince_.QuadPart != 0 && frequency_.QuadPart != 0) {
        dwelled = static_cast<float>(now.QuadPart - hoverSince_.QuadPart) /
                  static_cast<float>(frequency_.QuadPart);
    }
    const bool speaks =
        hoverRow_ >= 0 && static_cast<size_t>(hoverRow_) < rows_.size() &&
        (rows_[static_cast<size_t>(hoverRow_)].hint != nullptr ||
         rows_[static_cast<size_t>(hoverRow_)].kind == Row::Kind::Item ||
         rows_[static_cast<size_t>(hoverRow_)].kind == Row::Kind::Suggestion);
    const bool wantsTooltip = speaks && dwelled >= layout::kTooltipDelay;
    const float before = tooltipAlpha_;
    approach(tooltipAlpha_, wantsTooltip ? 1.0f : 0.0f, layout::kTooltipFade);
    if (tooltipAlpha_ != before) {
        moving = true;
    }
    // Still counting down to it: keep the clock running or the dwell would only
    // ever complete on some other row's animation happening to redraw us.
    if (!wantsTooltip && hoverRow_ >= 0 && dwelled < layout::kTooltipDelay) {
        moving = true;
    }
    return moving;
}

D2D1_RECT_F SettingsWindow::PillRect(const Row& row) const {
    return D2D1::RectF(row.bounds.left, row.bounds.top, row.bounds.right,
                       row.bounds.top + layout::kPillHeight);
}

void SettingsWindow::DrawSlider(const Row& row, float hover) {
    const D2D1_RECT_F pill = PillRect(row);
    const D2D1_ROUNDED_RECT card =
        D2D1::RoundedRect(pill, layout::kPillRadius, layout::kPillRadius);

    brush_->SetColor(Mix(kTrack, kTrackHover, hover));
    d2d_->FillRoundedRectangle(card, brush_.Get());

    const float span = std::max(row.maximum - row.minimum, 1e-5f);
    const float t = std::clamp((*row.number - row.minimum) / span, 0.0f, 1.0f);
    const float knobX = row.control.left + t * (row.control.right - row.control.left);

    // The fill runs from the card's left edge, under the label, the way the
    // reference does - the label sits on filled track. Only the *thumb* is kept
    // out of the label, which is the part that was crossing text.
    d2d_->PushLayer(D2D1::LayerParameters(pill, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                          D2D1::IdentityMatrix(), 1.0f, nullptr,
                                          D2D1_LAYER_OPTIONS_NONE),
                    nullptr);
    brush_->SetColor(Mix(kFill, kFillHover, hover));
    d2d_->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(pill.left, pill.top, knobX + layout::kKnobInset, pill.bottom),
                          layout::kPillRadius, layout::kPillRadius),
        brush_.Get());
    d2d_->PopLayer();

    // The knob. A bar rather than a circle: a circle on a track this tall reads
    // as a bead threaded on a wire, and the reference's is a caret sitting in
    // the text - which is what says "this is the value" rather than "this is a
    // thing to grab".
    const float centreY = (pill.top + pill.bottom) * 0.5f;
    const D2D1_RECT_F knob =
        D2D1::RectF(knobX - layout::kKnobWidth * 0.5f, centreY - layout::kKnobHeight * 0.5f,
                    knobX + layout::kKnobWidth * 0.5f, centreY + layout::kKnobHeight * 0.5f);
    brush_->SetColor(kKnob);
    d2d_->FillRoundedRectangle(
        D2D1::RoundedRect(knob, layout::kKnobWidth * 0.5f, layout::kKnobWidth * 0.5f),
        brush_.Get());
}

void SettingsWindow::DrawToggle(const Row& row, float hover) {
    brush_->SetColor(Mix(kTrack, kTrackHover, hover));
    d2d_->FillRoundedRectangle(
        D2D1::RoundedRect(PillRect(row), layout::kPillRadius, layout::kPillRadius), brush_.Get());

    const float centreY = (row.control.top + row.control.bottom) * 0.5f;
    const float right = row.control.right;
    const float left = right - layout::kSwitchWidth;

    // Eased, so it leaves and arrives slowly and is quickest in the middle. A
    // linear slide over this distance reads as a jump with a delay in it.
    const float t = Smooth(std::clamp(row.anim, 0.0f, 1.0f));
    const float radius = layout::kSwitchCircle * 0.5f;
    const float cx = left + radius + t * (layout::kSwitchWidth - layout::kSwitchCircle);

    // The bar lives on whichever side the circle is not. Rather than flipping
    // it at the halfway point - which would be a visible pop in the middle of
    // the very transition this is meant to smooth - both sides are drawn and
    // crossfaded, so one hands over to the other under the moving circle.
    const float barHalf = layout::kSwitchBar * 0.5f;
    const float barRadius = barHalf;
    auto bar = [&](float x0, float x1, float alpha) {
        if (alpha <= 0.01f || x1 - x0 < layout::kSwitchBar) {
            return;
        }
        brush_->SetColor(Grey(1.0f, 0.30f * alpha));
        d2d_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(x0, centreY - barHalf, x1, centreY + barHalf), barRadius,
                              barRadius),
            brush_.Get());
    };
    bar(left, cx - radius - layout::kSwitchGap, t);              // behind it, on the way to on
    bar(cx + radius + layout::kSwitchGap, right, 1.0f - t);      // ahead of it, on the way to off

    // The circle is a ring that fills in. Stroking and then filling by the same
    // amount the knob has travelled turns hollow into solid without needing a
    // hole punched in a geometry, and the two happen together, which is what
    // makes the state read at a glance rather than only from the position.
    const D2D1_ELLIPSE circle = D2D1::Ellipse(D2D1::Point2F(cx, centreY), radius - layout::kSwitchRing * 0.5f,
                                              radius - layout::kSwitchRing * 0.5f);
    brush_->SetColor(Grey(1.0f, 0.30f * t));
    d2d_->FillEllipse(circle, brush_.Get());
    brush_->SetColor(Grey(1.0f, 0.62f + 0.38f * t));
    d2d_->DrawEllipse(circle, brush_.Get(), layout::kSwitchRing);
    // The last of the fill goes on top of the ring, so a switch that is fully
    // on is one disc rather than a disc with a seam around it.
    if (t > 0.0f) {
        brush_->SetColor(Grey(1.0f, t));
        d2d_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, centreY), radius, radius), brush_.Get());
    }
}

std::vector<D2D1_RECT_F> SettingsWindow::ChoiceCells(const Row& row) const {
    const int count = static_cast<int>(row.options.size());
    std::vector<D2D1_RECT_F> cells;
    if (count <= 0) {
        return cells;
    }
    cells.reserve(static_cast<size_t>(count));

    const D2D1_RECT_F pill = PillRect(row);
    const float centreY = (pill.top + pill.bottom) * 0.5f;
    const float height = layout::kChoiceHeight;

    float total = 0.0f;
    for (int i = 0; i < count; ++i) {
        total += MeasureText(valueFormat_.Get(), row.options[static_cast<size_t>(i)]) +
                 2.0f * layout::kChoicePadX;
    }
    float x = pill.right - layout::kPillPadX - total;
    for (int i = 0; i < count; ++i) {
        const float width = MeasureText(valueFormat_.Get(), row.options[static_cast<size_t>(i)]) +
                            2.0f * layout::kChoicePadX;
        cells.push_back(D2D1::RectF(x, centreY - height * 0.5f, x + width, centreY + height * 0.5f));
        x += width;
    }
    return cells;
}

void SettingsWindow::DrawChoice(const Row& row, float pointerX) {
    brush_->SetColor(Mix(kTrack, kTrackHover, row.hover));
    d2d_->FillRoundedRectangle(
        D2D1::RoundedRect(PillRect(row), layout::kPillRadius, layout::kPillRadius), brush_.Get());

    const int count = static_cast<int>(row.options.size());
    if (count <= 0) {
        return;
    }
    // No inner frame, and no equal thirds. A box drawn on a card that is
    // already a surface is one border too many, and splitting the width evenly
    // gave "Screen" the same room as "Wallpaper" - which is how a two-letter
    // option ends up wearing a pill wide enough for a sentence. Each option
    // gets the width of its own text, and the row is packed against the card's
    // right edge like every other value on this panel.
    const std::vector<D2D1_RECT_F> cells = ChoiceCells(row);
    if (cells.empty()) {
        return;
    }
    const float height = layout::kChoiceHeight;

    // The travelling pill, interpolated between the cell it left and the one it
    // is heading for, so it changes width on the way as well as position.
    const float where = std::clamp(row.anim < 0.0f ? static_cast<float>(*row.choice) : row.anim,
                                   0.0f, static_cast<float>(count - 1));
    const int from = static_cast<int>(where);
    const int to = std::min(from + 1, count - 1);
    const float t = Smooth(where - static_cast<float>(from));
    const D2D1_RECT_F& a = cells[static_cast<size_t>(from)];
    const D2D1_RECT_F& b = cells[static_cast<size_t>(to)];
    const D2D1_RECT_F marker = D2D1::RectF(a.left + (b.left - a.left) * t, a.top,
                                           a.right + (b.right - a.right) * t, a.bottom);
    brush_->SetColor(Grey(1.0f, 0.16f));
    d2d_->FillRoundedRectangle(D2D1::RoundedRect(marker, height * 0.5f, height * 0.5f),
                               brush_.Get());

    for (int i = 0; i < count; ++i) {
        const D2D1_RECT_F& cell = cells[static_cast<size_t>(i)];
        const bool under = pointerX >= cell.left && pointerX <= cell.right;
        if (under && i != *row.choice) {
            brush_->SetColor(Grey(1.0f, 0.07f));
            d2d_->FillRoundedRectangle(D2D1::RoundedRect(cell, height * 0.5f, height * 0.5f),
                                       brush_.Get());
        }
        // Brightness carries the selection, not a colour: the panel has exactly
        // one accent and it is spent on the caret and the add badge.
        const float lit = 1.0f - std::min(1.0f, std::fabs(where - static_cast<float>(i)));
        const D2D1_COLOR_F colour = Mix(kValue, Grey(1.0f, 1.0f), lit);
        valueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(row.options[static_cast<size_t>(i)], valueFormat_.Get(),
                 D2D1::RectF(cell.left, cell.top, cell.right, cell.bottom), colour);
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

float SettingsWindow::MeasureText(IDWriteTextFormat* format, const std::wstring& text) const {
    ComPtr<IDWriteTextLayout> layout;
    if (!dwrite_ || text.empty() ||
        FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                         4000.0f, 100.0f, &layout))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) {
        return 0.0f;
    }
    return metrics.widthIncludingTrailingWhitespace;
}

void SettingsWindow::DrawTooltip() {
    if (tooltipAlpha_ <= 0.01f || hoverRow_ < 0 ||
        static_cast<size_t>(hoverRow_) >= rows_.size()) {
        return;
    }
    const Row& hovered = rows_[static_cast<size_t>(hoverRow_)];
    std::wstring text;
    if (hovered.kind == Row::Kind::Item) {
        const auto& items = items_.items();
        const size_t index = static_cast<size_t>(hovered.itemIndex);
        if (index >= items.size()) {
            return;
        }
        text = (items[index].kind == ItemKind::Separator)
                   ? std::wstring(L"Divider — drag it where you want the break")
                   : items[index].label + L"   ·   " + items[index].path;
    } else if (hovered.kind == Row::Kind::Suggestion) {
        text = SuggestionLabel(hovered.itemIndex) + L"   ·   click to add";
    } else if (hovered.hint) {
        text = hovered.hint;
    }
    if (text.empty()) {
        return;
    }
    const float width = MeasureText(tipFormat_.Get(), text) + 2.0f * layout::kTooltipPadX;
    const float panelWidth = layout::kWidth;
    const float panelHeight = static_cast<float>(height_) / (static_cast<float>(dpi_) / 96.0f);

    float left = pointerX_ + layout::kTooltipOffsetX;
    float top = pointerY_ + layout::kTooltipOffsetY;
    // Flipped rather than clamped at the edges: a tooltip pinned to the frame
    // stops pointing at anything, and the cursor ends up sitting on top of it.
    if (left + width > panelWidth - 8.0f) {
        left = pointerX_ - layout::kTooltipOffsetX - width;
    }
    if (top + layout::kTooltipHeight > panelHeight - 8.0f) {
        top = pointerY_ - layout::kTooltipOffsetY - layout::kTooltipHeight;
    }
    left = std::clamp(left, 8.0f, std::max(8.0f, panelWidth - width - 8.0f));

    const D2D1_RECT_F pill = D2D1::RectF(left, top, left + width, top + layout::kTooltipHeight);
    const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(pill, 7.0f, 7.0f);

    // Lifted off the ground rather than sunk into it. Everything else on this
    // page is a percentage of white over black, so a *darker* tooltip would be
    // invisible; this one is simply brighter than the cards it floats over. No
    // outline: it is already the lightest thing on the page, and a hairline on
    // top of that reads as a second edge rather than as a definition of the
    // first.
    brush_->SetColor(Grey(0.17f, 0.98f * tooltipAlpha_));
    d2d_->FillRoundedRectangle(rounded, brush_.Get());

    DrawText(text, tipFormat_.Get(),
             D2D1::RectF(pill.left + layout::kTooltipPadX, pill.top,
                         pill.right - layout::kTooltipPadX, pill.bottom),
             Grey(1.0f, 0.95f * tooltipAlpha_));
}

void SettingsWindow::DrawWindowButtons() {
    const float right = layout::kWidth - layout::kPadding;
    for (int i = 0; i < 2; ++i) {
        // Close on the outside, where the corner is, and minimise inboard of
        // it - the order every window on the system uses, so the muscle memory
        // that reaches for the corner finds the right one.
        const float left = right - (2 - i) * layout::kWindowButton;
        buttonBounds_[i] =
            D2D1::RectF(left, layout::kWindowButtonTop, left + layout::kWindowButton,
                        layout::kWindowButtonTop + layout::kWindowButton);

        const D2D1_RECT_F& box = buttonBounds_[i];
        const bool under = pointerX_ >= box.left && pointerX_ <= box.right &&
                           pointerY_ >= box.top && pointerY_ <= box.bottom;
        if (under) {
            // Close goes red on the way in, the way it does everywhere. It is
            // the one button here that loses something if it is hit by mistake.
            brush_->SetColor(i == 1 ? D2D1::ColorF(0.90f, 0.24f, 0.26f, 0.90f) : Grey(1.0f, 0.13f));
            d2d_->FillRoundedRectangle(D2D1::RoundedRect(box, 7.0f, 7.0f), brush_.Get());
        }

        const float cx = (box.left + box.right) * 0.5f;
        const float cy = (box.top + box.bottom) * 0.5f;
        brush_->SetColor(Grey(1.0f, under ? 1.0f : 0.55f));
        if (i == 0) {
            d2d_->DrawLine(D2D1::Point2F(cx - 5.0f, cy), D2D1::Point2F(cx + 5.0f, cy),
                           brush_.Get(), 1.4f);
        } else {
            d2d_->DrawLine(D2D1::Point2F(cx - 4.5f, cy - 4.5f), D2D1::Point2F(cx + 4.5f, cy + 4.5f),
                           brush_.Get(), 1.4f);
            d2d_->DrawLine(D2D1::Point2F(cx + 4.5f, cy - 4.5f), D2D1::Point2F(cx - 4.5f, cy + 4.5f),
                           brush_.Get(), 1.4f);
        }
    }
}

int SettingsWindow::WindowButtonAt(float x, float y) const {
    for (int i = 0; i < 2; ++i) {
        const D2D1_RECT_F& box = buttonBounds_[i];
        if (box.right > box.left && x >= box.left && x <= box.right && y >= box.top &&
            y <= box.bottom) {
            return i;
        }
    }
    return -1;
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

void SettingsWindow::ApplyFilter() {
    filtered_.clear();
    filtered_.reserve(suggestions_.size());

    // Case-folded substring, over the name and the path both: people look for
    // "code" as readily as they look for "Visual Studio", and the thing they
    // typed is as likely to be in the executable's name as in its label.
    std::wstring needle = search_;
    for (wchar_t& ch : needle) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    for (size_t i = 0; i < suggestions_.size(); ++i) {
        if (needle.empty()) {
            filtered_.push_back(static_cast<int>(i));
            continue;
        }
        std::wstring hay = suggestions_[i].label + L" " + suggestions_[i].path;
        for (wchar_t& ch : hay) {
            ch = static_cast<wchar_t>(towlower(ch));
        }
        if (hay.find(needle) != std::wstring::npos) {
            filtered_.push_back(static_cast<int>(i));
        }
    }
}

const std::wstring& SettingsWindow::SuggestionLabel(int index) const {
    static const std::wstring empty;
    if (index < 0 || static_cast<size_t>(index) >= suggestions_.size()) {
        return empty;
    }
    return suggestions_[static_cast<size_t>(index)].label;
}

void SettingsWindow::DrawTile(const Row& row, bool hovered) {
    const bool suggestion = (row.kind == Row::Kind::Suggestion);
    const size_t index = static_cast<size_t>(row.itemIndex);
    const auto& items = items_.items();
    if (!suggestion && index >= items.size()) {
        return;
    }

    const bool dragged = draggingItem_ && !suggestion && row.itemIndex == pressItem_;
    const bool open = !suggestion && row.itemIndex == expandedItem_;
    if (hovered || dragged || open) {
        brush_->SetColor(Grey(1.0f, dragged ? 0.05f : (hovered ? 0.12f : 0.08f)));
        d2d_->FillRoundedRectangle(D2D1::RoundedRect(row.bounds, 10.0f, 10.0f), brush_.Get());
    }
    if (open) {
        // The one the panel below is talking about. Outlined rather than filled,
        // so a hover passing over it still reads as the hover.
        brush_->SetColor(kOn);
        d2d_->DrawRoundedRectangle(D2D1::RoundedRect(row.bounds, 10.0f, 10.0f), brush_.Get(),
                                   1.4f);
    }

    const float cx = (row.bounds.left + row.bounds.right) * 0.5f;
    const float cy = (row.bounds.top + row.bounds.bottom) * 0.5f;
    const D2D1_RECT_F box =
        D2D1::RectF(cx - layout::kTileIcon * 0.5f, cy - layout::kTileIcon * 0.5f,
                    cx + layout::kTileIcon * 0.5f, cy + layout::kTileIcon * 0.5f);

    if (!suggestion && items[index].kind == ItemKind::Separator) {
        // A divider has no icon, so it is drawn as what it is: the rule itself,
        // standing in the middle of the cell it occupies in the row.
        brush_->SetColor(Grey(1.0f, 0.45f));
        d2d_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(cx - 1.0f, box.top + 4.0f, cx + 1.0f, box.bottom - 4.0f),
                              1.0f, 1.0f),
            brush_.Get());
        DrawTileBadge(row, hovered, false);
        return;
    }

    const auto& icons = suggestion ? suggestionIcons_ : itemIcons_;
    ID2D1Bitmap* icon = (index < icons.size()) ? icons[index].Get() : nullptr;
    if (icon) {
        // Suggestions are dimmed until pointed at: they are not on the dock, and
        // a grid where everything looks equally present does not say which half
        // is which.
        const float opacity = suggestion && !hovered ? 0.55f : 1.0f;
        d2d_->DrawBitmap(icon, box, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        brush_->SetColor(Grey(1.0f, 0.07f));
        d2d_->FillRoundedRectangle(D2D1::RoundedRect(box, 8.0f, 8.0f), brush_.Get());
    }

    DrawTileBadge(row, hovered, suggestion);
}

void SettingsWindow::DrawTileBadge(const Row& row, bool hovered, bool suggestion) {
    if (hovered) {
        // A plus on a suggestion, a cross on something already docked: the same
        // corner, and the same click, reading as the opposite operations they
        // are. Only on hover, because a grid of forty icons each wearing a
        // permanent badge is a grid you cannot read.
        const float r = 6.0f;
        const float px = row.bounds.right - 11.0f;
        const float py = row.bounds.top + 11.0f;
        brush_->SetColor(suggestion ? kOn : D2D1::ColorF(0.85f, 0.24f, 0.24f, 0.95f));
        d2d_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), 8.5f, 8.5f), brush_.Get());
        brush_->SetColor(Grey(1.0f, 1.0f));
        if (suggestion) {
            d2d_->DrawLine(D2D1::Point2F(px - r + 2.0f, py), D2D1::Point2F(px + r - 2.0f, py),
                           brush_.Get(), 1.6f);
            d2d_->DrawLine(D2D1::Point2F(px, py - r + 2.0f), D2D1::Point2F(px, py + r - 2.0f),
                           brush_.Get(), 1.6f);
        } else {
            const float d = r - 2.5f;
            d2d_->DrawLine(D2D1::Point2F(px - d, py - d), D2D1::Point2F(px + d, py + d),
                           brush_.Get(), 1.6f);
            d2d_->DrawLine(D2D1::Point2F(px + d, py - d), D2D1::Point2F(px - d, py + d),
                           brush_.Get(), 1.6f);
        }
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
                 D2D1::RectF(pill.left, pill.top, pill.right, pill.bottom), kLabel);
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

void SettingsWindow::EditorRects(const D2D1_RECT_F& panel, D2D1_RECT_F* fields,
                                 D2D1_RECT_F* buttons) const {
    const float left = panel.left + 14.0f;
    const float right = panel.right - 14.0f;
    float y = panel.top + 8.0f;
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

void SettingsWindow::DrawEditor(const D2D1_RECT_F& panel, int itemIndex) {
    const auto& items = items_.items();
    const size_t index = static_cast<size_t>(itemIndex);
    if (itemIndex < 0 || index >= items.size()) {
        return;
    }
    brush_->SetColor(Grey(1.0f, 0.05f));
    d2d_->FillRoundedRectangle(D2D1::RoundedRect(panel, 10.0f, 10.0f), brush_.Get());
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
    EditorRects(panel, fields, buttons);

    const float labelLeft = panel.left + 14.0f;
    for (int i = 0; i < static_cast<int>(Field::Count); ++i) {
        const D2D1_RECT_F& field = fields[i];
        DrawText(kNames[i], hintFormat_.Get(),
                 D2D1::RectF(labelLeft, field.top, labelLeft + layout::kEditorLabel,
                             field.bottom),
                 kHint);

        const bool editing = (editItem_ == itemIndex && static_cast<int>(editField_) == i);
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
                 D2D1::RectF(field.left + 8.0f, field.top, field.right - 8.0f, field.bottom),
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
                     D2D1::RectF(button.left, button.top, button.right, button.bottom),
                     kLabel);
            valueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
    }
}

void SettingsWindow::DrawSearch() {
    // No box and no border: a rule under the text is enough to say "type here",
    // and a bordered field on this panel would be the only outlined thing on it.
    const float baseline = searchRect_.bottom;
    brush_->SetColor(Grey(1.0f, searchFocused_ ? 0.28f : 0.12f));
    d2d_->FillRectangle(
        D2D1::RectF(searchRect_.left, baseline, searchRect_.right, baseline + 1.0f), brush_.Get());

    const D2D1_RECT_F text = D2D1::RectF(searchRect_.left + 2.0f, searchRect_.top,
                                         searchRect_.right, searchRect_.bottom);
    if (search_.empty()) {
        DrawText(L"Search installed apps", hintFormat_.Get(), text,
                 Grey(1.0f, searchFocused_ ? 0.35f : 0.28f));
    } else {
        valueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        DrawText(search_, valueFormat_.Get(), text, kValue);
        valueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    if (searchFocused_) {
        // The caret sits after the text rather than inside it: this field has no
        // cursor keys, because there is nothing here worth editing in the middle
        // of - you type a few letters and the grid answers.
        float width = 0.0f;
        if (!search_.empty()) {
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwrite_->CreateTextLayout(search_.c_str(),
                                                    static_cast<UINT32>(search_.size()),
                                                    valueFormat_.Get(), 400.0f, 40.0f, &layout))) {
                DWRITE_TEXT_METRICS metrics{};
                layout->GetMetrics(&metrics);
                width = metrics.widthIncludingTrailingWhitespace;
            }
        }
        const float x = searchRect_.left + 2.0f + width + 1.0f;
        brush_->SetColor(kOn);
        d2d_->FillRectangle(
            D2D1::RectF(x, searchRect_.top + 4.0f, x + 1.4f, searchRect_.bottom - 2.0f),
            brush_.Get());
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
        } else if (row.kind == Row::Kind::Suggestion) {
            detail = SuggestionLabel(row.itemIndex);
            if (!detail.empty()) {
                detail += L"   ·   " + suggestions_[static_cast<size_t>(row.itemIndex)].path;
            }
        }
    }
    if (detail.empty()) {
        detail = L"Drag an icon to reorder it  ·  click it to edit  ·  click one below the line to add it";
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

    const bool moving = AdvanceAnimation();
    if (moving) {
        SetTimer(hwnd_, kAnimTimer, kAnimIntervalMs, nullptr);
    } else {
        KillTimer(hwnd_, kAnimTimer);
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
    DrawWindowButtons();

    bool clipped = false;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        if (row.tab != activeTab_) {
            continue;
        }
        const bool hovered = (static_cast<int>(i) == hoverRow_);

        // The items list scrolls, so it is drawn inside a clip that stops it
        // spilling out of the bottom of the panel.
        const bool wantsClip =
            (row.kind == Row::Kind::Item || row.kind == Row::Kind::Suggestion);
        if (wantsClip != clipped) {
            if (wantsClip) {
                d2d_->PushAxisAlignedClip(itemsClip_, D2D1_ANTIALIAS_MODE_ALIASED);
            } else {
                d2d_->PopAxisAlignedClip();
            }
            clipped = wantsClip;
        }

        if (row.kind == Row::Kind::Item || row.kind == Row::Kind::Suggestion) {
            DrawTile(row, hovered);
            continue;
        }
        if (row.kind == Row::Kind::Section && row.tab == Tab::Items) {
            continue;
        }
        if (row.kind == Row::Kind::AddItem || row.kind == Row::Kind::AddSeparator) {
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

        // The card first: it is the background every row's text sits on, and for
        // a slider it is the track, so it has to be under the label rather than
        // beside it.
        switch (row.kind) {
            case Row::Kind::Slider: DrawSlider(row, row.hover); break;
            case Row::Kind::Toggle: DrawToggle(row, row.hover); break;
            case Row::Kind::Choice: DrawChoice(row, hovered ? pointerX_ : -1.0f); break;
            default: break;
        }

        const D2D1_RECT_F pill = PillRect(row);
        // The value is right-aligned inside the card; the label and its
        // explanation stack against the left, stopping short of the value.
        const float textLeft = pill.left + layout::kLabelIndent;
        const float textRight = (row.kind == Row::Kind::Slider)
                                    ? (pill.right - layout::kValueGutter)
                                    : (row.control.left - 14.0f);

        // One line, centred. What the setting is *for* is a tooltip now.
        labelFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DrawText(row.label, labelFormat_.Get(),
                 D2D1::RectF(textLeft, pill.top, textRight, pill.bottom), kLabel);
        labelFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

        if (row.kind == Row::Kind::Slider) {
            DrawText(FormatValue(*row.number, row.decimals, row.suffix), valueFormat_.Get(),
                     D2D1::RectF(pill.right - layout::kPillPadX - layout::kValueWidth, pill.top,
                                 pill.right - layout::kPillPadX, pill.bottom),
                     kLabel);

        }
    }

    if (clipped) {
        d2d_->PopAxisAlignedClip();
    }

    if (activeTab_ == Tab::Items) {
        // The rule between what is on the dock and what could be.
        if (!suggestions_.empty() && gridRuleY_ > itemsClip_.top - layout::kSearchHeight &&
            gridRuleY_ < itemsClip_.bottom) {
            brush_->SetColor(Grey(1.0f, 0.10f));
            d2d_->FillRectangle(D2D1::RectF(layout::kPadding, gridRuleY_,
                                            layout::kWidth - layout::kPadding, gridRuleY_ + 1.0f),
                                brush_.Get());
            DrawText(L"Installed, not on the dock — click to add", hintFormat_.Get(),
                     D2D1::RectF(layout::kPadding, searchRect_.top, searchRect_.left - 12.0f,
                                 searchRect_.bottom),
                     kHint);
            DrawSearch();
            if (filtered_.empty()) {
                const float top = searchRect_.bottom + layout::kGridRuleGap;
                DrawText(L"Nothing installed matches “" + search_ + L"”.", hintFormat_.Get(),
                         D2D1::RectF(layout::kPadding, top, layout::kWidth - layout::kPadding,
                                     top + layout::kEmptyHeight),
                         kHint);
            }
        }
        if (editorPanel_.bottom > editorPanel_.top) {
            DrawEditor(editorPanel_, expandedItem_);
        }
        if (draggingItem_ && dropIndex_ >= 0) {
            // Where it would land. Drawn after the rows so it is not clipped
            // away by the one it happens to be sitting on.
            D2D1_RECT_F marker{};
            bool found = false;
            for (const Row& row : rows_) {
                if (row.tab != Tab::Items || row.kind != Row::Kind::Item) {
                    continue;
                }
                if (row.itemIndex == dropIndex_) {
                    marker = D2D1::RectF(row.bounds.left - 4.0f, row.bounds.top,
                                         row.bounds.left - 2.0f, row.bounds.bottom);
                    found = true;
                    break;
                }
                // Dropping past the end puts the bar after the last tile.
                marker = D2D1::RectF(row.bounds.right + 2.0f, row.bounds.top,
                                     row.bounds.right + 4.0f, row.bounds.bottom);
                found = true;
            }
            if (found && marker.bottom > itemsClip_.top && marker.top < itemsClip_.bottom) {
                brush_->SetColor(kOn);
                d2d_->FillRoundedRectangle(D2D1::RoundedRect(marker, 1.0f, 1.0f), brush_.Get());
            }
        }
        DrawDetailBar();
    }

    DrawTooltip();

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
            // The panel has no title bar, so the header area is the grab handle -
            // except over the buttons, which would otherwise be a drag handle
            // that happens to look like a close box.
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &point);
            const float x = static_cast<float>(point.x) / scale;
            const float y = static_cast<float>(point.y) / scale;
            if (WindowButtonAt(x, y) >= 0) {
                return HTCLIENT;
            }
            if (y < layout::kTitleHeight) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                POINT cursor{};
                const wchar_t* shape = IDC_ARROW;
                if (GetCursorPos(&cursor) && ScreenToClient(hwnd, &cursor)) {
                    shape = CursorFor(static_cast<float>(cursor.x) / scale,
                                      static_cast<float>(cursor.y) / scale);
                }
                SetCursor(LoadCursorW(nullptr, shape));
                return TRUE;
            }
            break;

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
                if (!draggingItem_ && (std::fabs(pointerY_ - pressY_) > 4.0f ||
                                       std::fabs(pointerX_ - pressX_) > 4.0f)) {
                    draggingItem_ = true;
                }
                if (draggingItem_) {
                    dropIndex_ = DropIndexAt(pointerX_, pointerY_);
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
                QueryPerformanceCounter(&hoverSince_);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                // A tooltip that is up is pinned to the pointer, so every move
                // is a redraw; below that it costs one while the dwell runs.
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            mouseTracking_ = false;
            hoverRow_ = -1;
            hoverSince_.QuadPart = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN: {
            const float x = static_cast<float>(GET_X_LPARAM(lParam)) / scale;
            const float y = static_cast<float>(GET_Y_LPARAM(lParam)) / scale;

            const int button = WindowButtonAt(x, y);
            if (button >= 0) {
                CommitEdit();
                if (button == 0) {
                    ShowWindow(hwnd, SW_MINIMIZE);
                } else {
                    Hide();
                }
                return 0;
            }

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

            if (activeTab_ == Tab::Items && !suggestions_.empty()) {
                // Before RowAt, not after: the field is not a row, so a click on
                // it finds nothing, and a test that lives inside "found a row"
                // never runs. Generous on the vertical, because the field is a
                // line of text over a hairline and aiming at a hairline is not a
                // thing to ask of anyone.
                const bool inSearch = x >= searchRect_.left && x <= searchRect_.right &&
                                      y >= searchRect_.top - 6.0f &&
                                      y <= searchRect_.bottom + 6.0f;
                if (inSearch != searchFocused_) {
                    searchFocused_ = inSearch;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                if (inSearch) {
                    CommitEdit();
                    return 0;
                }
            }

            const int row = RowAt(x, y);
            if (row >= 0) {
                const Row::Kind kind = rows_[static_cast<size_t>(row)].kind;
                if (kind == Row::Kind::AddItem || kind == Row::Kind::AddSeparator) {
                    HandleItemClick(row, x);
                    return 0;
                }
                if (expandedItem_ >= 0 && x >= editorPanel_.left && x <= editorPanel_.right &&
                    y >= editorPanel_.top && y <= editorPanel_.bottom) {
                    if (HandleEditorClick(editorPanel_, expandedItem_, x, y)) {
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }
                if (kind == Row::Kind::Suggestion) {
                    const Row tile = rows_[static_cast<size_t>(row)];
                    if (tile.itemIndex >= 0 &&
                        static_cast<size_t>(tile.itemIndex) < suggestions_.size()) {
                        const CatalogEntry& entry =
                            suggestions_[static_cast<size_t>(tile.itemIndex)];
                        DockItem item;
                        item.path = entry.path;
                        item.label = entry.label;
                        if (items_.Add(std::move(item))) {
                            suggestions_.erase(suggestions_.begin() + tile.itemIndex);
                            suggestionIcons_.erase(suggestionIcons_.begin() + tile.itemIndex);
                            CommitItems();
                        }
                    }
                    return 0;
                }
                if (kind == Row::Kind::Item) {
                    const Row& item = rows_[static_cast<size_t>(row)];
                    // The corner cross removes it. Everything else on the tile
                    // is either a click that opens the editor or the start of a
                    // drag; which one is decided on the way up.
                    if (x >= item.bounds.right - 22.0f && y <= item.bounds.top + 22.0f) {
                        CommitEdit();
                        if (items_.Remove(static_cast<size_t>(item.itemIndex))) {
                            expandedItem_ = -1;
                            CommitItems();
                        }
                        return 0;
                    }
                    CommitEdit();
                    pressItem_ = item.itemIndex;
                    pressY_ = y;
                    pressX_ = x;
                    draggingItem_ = false;
                    dropIndex_ = -1;
                    SetCapture(hwnd);
                    return 0;
                }
                // A slider's card *is* its track, so a click anywhere on it
                // sets the value - the label is written on the track, not
                // beside it. A choice still needs its segments: clicking its
                // label would otherwise pick whichever option was nearest.
                const Row& control = rows_[static_cast<size_t>(row)];
                if (control.kind == Row::Kind::Choice &&
                    (x < control.control.left || x > control.control.right)) {
                    return 0;
                }
                if (control.kind != Row::Kind::Slider && control.kind != Row::Kind::Toggle &&
                    control.kind != Row::Kind::Choice) {
                    return 0;
                }
                if (control.kind == Row::Kind::Slider &&
                    (y > PillRect(control).bottom ||
                     x < control.control.left - layout::kKnobWidth)) {
                    // The gap under the card is not the track, and neither is
                    // the label: clicking a name should not set its value to
                    // the minimum.
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
            const float y = static_cast<float>(point.y) / scale;

            // A slider under the pointer takes the wheel; the list only gets it
            // when the pointer is not on something the wheel can adjust.
            const int over = RowAt(x, y);
            if (over >= 0 && rows_[static_cast<size_t>(over)].kind == Row::Kind::Slider) {
                const int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
                if (StepSlider(over, notches)) {
                    CommitChange();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (itemScrollMax_ > 0.0f && x >= itemsClip_.left && x <= itemsClip_.right) {
                const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
                // A notch moves a line of the grid, which is what the eye is
                // tracking - the old list's row height leaves every icon a third
                // of a cell out of step with where it started.
                const float step = layout::kTile + layout::kTileGap;
                itemScroll_ = std::clamp(itemScroll_ - delta * step, 0.0f, itemScrollMax_);
                LayoutRows();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_CHAR:
            if (searchFocused_) {
                const wchar_t ch = static_cast<wchar_t>(wParam);
                if (ch == VK_BACK) {
                    if (!search_.empty()) {
                        search_.pop_back();
                    }
                } else if (ch >= 0x20) {
                    search_.push_back(ch);
                } else {
                    return 0;
                }
                // The grid is rebuilt rather than hidden row by row: the tiles
                // have to close up after a filter, and their positions are the
                // only thing that says which entry a click meant.
                itemScroll_ = 0.0f;
                hoverRow_ = -1;
                BuildRows();
                LayoutRows();
                ApplyWindowSize();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
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
            // Arrows nudge whatever the pointer is resting on. No focus ring
            // and no tabbing to a control first: the pointer is already saying
            // which setting is meant, and a settings window where the keyboard
            // and the pointer disagree about that is a window with two
            // selections in it.
            if (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP ||
                wParam == VK_DOWN) {
                const int steps = (wParam == VK_RIGHT || wParam == VK_UP) ? 1 : -1;
                if (StepSlider(hoverRow_, steps)) {
                    CommitChange();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            if (wParam == VK_ESCAPE && searchFocused_) {
                // Clears the search first and leaves the field second. Escape
                // closing the window out from under a half-typed filter is the
                // sort of thing that loses you the window you were working in.
                if (!search_.empty()) {
                    search_.clear();
                    itemScroll_ = 0.0f;
                    hoverRow_ = -1;
                    BuildRows();
                    LayoutRows();
                    ApplyWindowSize();
                } else {
                    searchFocused_ = false;
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

        case kCatalogMessage: {
            // Everything installed, less whatever is already docked. Matched on
            // the launch string rather than the name, because two entries can
            // share a name and only one of them is the thing on the dock.
            const std::vector<CatalogEntry> all = catalog_.Take();
            suggestions_.clear();
            for (const CatalogEntry& entry : all) {
                bool docked = false;
                for (const DockItem& item : items_.items()) {
                    if (_wcsicmp(item.path.c_str(), entry.path.c_str()) == 0) {
                        docked = true;
                        break;
                    }
                }
                if (!docked) {
                    suggestions_.push_back(entry);
                }
            }
            suggestionIcons_.assign(suggestions_.size(), nullptr);

            std::vector<DockItem> wanted;
            wanted.reserve(suggestions_.size());
            for (const CatalogEntry& entry : suggestions_) {
                DockItem item;
                item.path = entry.path;
                item.label = entry.label;
                wanted.push_back(std::move(item));
            }
            const float pixels = layout::kTileIcon * static_cast<float>(dpi_) / 96.0f;
            suggestionLoader_.Start(std::move(wanted), static_cast<int>(std::lround(pixels)),
                                    hwnd_, kSuggestionIconsMessage);
            BuildRows();
            LayoutRows();
            ApplyWindowSize();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case kSuggestionIconsMessage:
            DrainSuggestionIcons();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_TIMER:
            if (wParam == kAnimTimer) {
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
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
