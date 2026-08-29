# LiquiDock

A macOS-style dock for Windows, with a real liquid glass background.

Not Electron. Not a WebView. A single native C++/Direct3D 11 executable that idles at zero.

## What makes it different

Every other Windows dock paints its background from a pre-baked bitmap — a nine-slice PNG stretched
to fit. LiquiDock computes the background per pixel, in a shader, from whatever is actually behind
it: refraction bending the backdrop through the bevel, chromatic dispersion at the rim, frost, and a
specular edge driven by a light angle you control.

- **Real liquid glass** — refraction, depth, dispersion, frost, splay and light, all live and all
  tweakable. Save the settings file and the dock re-renders; it watches the file rather than
  polling it, so tuning costs nothing when you are not tuning.
- **The macOS magnification wave** — icons swell under the cursor on a critically damped spring, and
  the glass body bulges with them.
- **Zero idle cost** — the dock is event-driven. Nothing moving means no frames presented, no GPU
  work, no wakeups.
- **Preferences that were designed**, not accumulated over fifteen years of checkboxes.

## Status

Early. See the milestones below for where things stand.

| Milestone | Scope | Status |
|---|---|---|
| M0 | Build system, D3D11 + DirectComposition transparent window | done |
| M1 | The liquid glass shader and backdrop pipeline | done |
| M2 | Item model, icons, layout, launching, magnification | done |
| M3 | Multi-monitor, appbar, running indicators, live-capture backdrop | in progress |
| M4 | Settings UI | |
| M5 | Portable / Microsoft Store / Steam packaging | |

## The items file

The dock reads its contents from `%LOCALAPPDATA%\LiquiDock\items.txt`, one item per line:

```
group | path | label
```

`group` is `main` or `utility` — utility items sit to the right of the hairline. `label` is optional.
`path` is anything the shell can open: a program, a shortcut, a folder, a `shell:AppsFolder\...`
moniker for a packaged app, or a `::{guid}` parsing name. Environment variables are expanded.

On a first run there is no file, so the list is seeded from whatever is pinned to your taskbar, plus
Downloads and the Recycle Bin, and then written out for you to edit. Right-clicking an icon offers
Open, Remove from Dock, and a shortcut to the file itself. Reordering is a drag in Notepad until the
preferences UI lands in M4.

## Settings

`%LOCALAPPDATA%\LiquiDock\settings.txt`, `key = value`, written with a comment on every setting the
first time the dock runs. Reachable from *Preferences…* in the tray menu. The dock watches the file,
so saving it takes effect immediately — which is the point, because half of these are judgement
calls about how something looks and the only way to settle one is to see it.

Glass: `backdrop`, `refraction`, `depth`, `dispersion`, `frost`, `splay`, `light-angle`,
`light-intensity`, `tint-alpha`. Magnification: `magnification`, `max-scale`, `influence`,
`icon-bulge`. Auto-hide: `auto-hide`, `dwell-seconds`, `slide-seconds`.

`backdrop` decides what the glass refracts. `wallpaper` (the default) decodes your desktop
background once and costs nothing after that — it is exactly right whenever nothing is behind the
dock, and wrong-looking over a window. `screen` refracts what is actually there, by duplicating the
strip of desktop the dock covers and ignoring every change that misses it. The catch is not
performance: the dock has to be excluded from screen capture to stop it refracting its own last
frame forever, and that same exclusion makes the dock invisible in your own screenshots and screen
shares. That is why it is not the default.

`icon-bulge` is **off** by default. With it on, the glass swells around a raised icon — the bar's
outline fuses to the icons and reads as liquid clinging to them, which is a much stronger effect
than this dock is going for. It is there because it is a good effect, not because it is the right
default.

## Building

Requires **Windows 10 version 2004** or newer, and Visual Studio 2022 with the *Desktop development
with C++* workload (MSVC v143, Windows 11 SDK, CMake, Ninja).

```
.\scripts\build.ps1                        # debug
.\scripts\build.ps1 -Preset release -Run   # release, then launch it
```

The script finds Visual Studio and enters its developer environment for you. If
you would rather drive CMake directly, do it from a *Developer PowerShell for VS
2022* — the presets pin `CMAKE_CXX_COMPILER` to a bare `cl.exe`, which only
resolves inside that environment:

```
cmake --preset debug
cmake --build --preset debug
```

The executable lands in `build/debug/liquidock.exe`. Or double-click `run.cmd`.

Because the dock's real fill is 5% white and easy to mistake for a failed
render, `run-diagnostic.cmd` draws it in flat opaque magenta instead.

## License

Source-available under the [Business Source License 1.1](LICENSE).

You may read, modify, build and use LiquiDock freely, including at work. You may not redistribute or
sell it. On 2030-08-29 this version converts to Apache 2.0.

Official signed builds are sold on the Microsoft Store and Steam — they add auto-updates, cloud
settings sync, and support the development.
