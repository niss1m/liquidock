#pragma once

#include <string>

namespace liquidock {

// Which run of the bar an item belongs to. The design puts frequently launched
// applications on the left and the standing shortcuts - downloads, the recycle
// bin - on the right, with a hairline between them.
enum class ItemGroup { Main, Utility };

// What an entry is. A separator is a divider the user placed themselves - it
// launches nothing, carries no icon, and exists purely to break a long row into
// runs you can find things in. Distinct from the hairline between the two
// groups, which is structural and comes from the design.
enum class ItemKind { App, Separator };

// How the target's window should come up. Nexus's Dock Entry Properties offers
// exactly these three and they are the ones that earn their place: a terminal
// you always want maximised, a sync client you always want out of the way.
enum class RunState { Normal, Minimized, Maximized };

// One thing on the dock.
//
// `path` is whatever ShellExecute would accept: a file, a .lnk, a folder, a
// shell parsing name like `::{645FF040-...}` for the recycle bin, or a
// `shell:AppsFolder\...` moniker for a packaged app. Keeping it a single
// opaque string is what lets all four kinds live in the same list and in the
// same one-line-per-item config file.
struct DockItem {
    ItemKind kind = ItemKind::App;

    std::wstring path;
    std::wstring label; // shown in the hover label and the context menu

    // Passed to the target when it is launched. A dock entry is a *command*,
    // not just a file: "Chrome with this profile" and "this script with that
    // argument" are the entries people actually want and cannot express by
    // pointing at an executable alone.
    std::wstring arguments;
    // The directory the target starts in. Plenty of programs - Blender and the
    // Adobe suite among them - fail in odd ways when started from elsewhere.
    std::wstring workingDirectory;

    // An image file to use instead of the icon the shell would give. Any format
    // WIC can decode, which in practice is png, jpeg, bmp, tiff, gif and ico.
    // Empty means "ask the shell", which is right for most things and wrong for
    // exactly the case people care about most: a themed icon set.
    std::wstring iconPath;

    ItemGroup group = ItemGroup::Main;

    RunState runState = RunState::Normal;
    // Launch elevated. The shell's "runas" verb, which is the same thing the
    // Explorer context menu does, so it prompts through UAC rather than needing
    // the dock itself to be elevated - a dock running as administrator would
    // hand every program it launches the same token, which is not a thing
    // anybody should ship.
    bool runAsAdmin = false;

    // Index of this item's cell in the icon atlas, or -1 while its icon is
    // still being extracted on the loader thread.
    int atlasSlot = -1;

    // The executable a running indicator watches for, resolved from `path` on
    // the loader thread - a .lnk has to be opened to find out what it points
    // at. Empty for anything that cannot be "running": a folder, the recycle
    // bin, a document.
    std::wstring executable;
};

} // namespace liquidock
