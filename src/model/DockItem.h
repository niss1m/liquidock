#pragma once

#include <string>

namespace liquidock {

// Which run of the bar an item belongs to. The design puts frequently launched
// applications on the left and the standing shortcuts - downloads, the recycle
// bin - on the right, with a hairline between them.
enum class ItemGroup { Main, Utility };

// One thing on the dock.
//
// `path` is whatever ShellExecute would accept: a file, a .lnk, a folder, a
// shell parsing name like `::{645FF040-...}` for the recycle bin, or a
// `shell:AppsFolder\...` moniker for a packaged app. Keeping it a single
// opaque string is what lets all four kinds live in the same list and in the
// same one-line-per-item config file.
struct DockItem {
    std::wstring path;
    std::wstring label; // shown in tooltips and the context menu
    ItemGroup group = ItemGroup::Main;

    // Index of this item's cell in the icon atlas, or -1 while its icon is
    // still being extracted on the loader thread.
    int atlasSlot = -1;
};

} // namespace liquidock
