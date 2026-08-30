# LiquiDock

A macOS-style dock for Windows, with a real liquid glass background.

Not Electron. Not a WebView. A single native C++/Direct3D 11 executable that idles at zero.

## What makes it different

Every other Windows dock paints its background from a pre-baked bitmap — a nine-slice PNG stretched
to fit. LiquiDock computes the background per pixel, in a shader, from whatever is actually behind
it: refraction bending the backdrop through the bevel, chromatic dispersion at the rim, frost, and a
specular edge driven by a light angle you control.

- **Real liquid glass** — refraction, depth, dispersion, frost, splay and light, all live and all
  tweakable, and calibrated against the design rather than guessed at: the shipped defaults are the
  numbers that reproduce the Figma frame's own measured blur, rim and edge. Save the settings file
  and the dock re-renders; it watches the file rather than polling it, so tuning costs nothing when
  you are not tuning.
- **The macOS magnification wave** — icons swell under the cursor on a critically damped spring,
  and the one under it names itself.
- **Survives a driver reset** — a TDR or a graphics driver updating itself mid-session rebuilds the
  device and every resource on it, rather than leaving a dock that has to be restarted.
- **Zero idle cost** — the dock is event-driven throughout. Nothing moving means no frames
  presented, no GPU work, no wakeups. Screen capture waits on the desktop's own change
  notifications and ignores changes that miss the dock; running indicators wait on a window event
  hook; the settings file is watched on a waitable handle, not polled.
- **Preferences that were designed**, not accumulated over fifteen years of checkboxes. Every
  setting says what it is *for* underneath its name, and every change applies to the running dock
  as you make it - there is no OK button, because the only way to judge a value like `frost` is to
  see it.

## Status

Early. See the milestones below for where things stand.

| Milestone | Scope | Status |
|---|---|---|
| M0 | Build system, D3D11 + DirectComposition transparent window | done |
| M1 | The liquid glass shader and backdrop pipeline | done |
| M2 | Item model, icons, layout, launching, magnification | done |
| M3 | Multi-monitor, appbar, running indicators, live-capture backdrop | done |
| M4 | Settings UI | done |
| M5 | Portable / Microsoft Store / Steam packaging | |

## Items

Right-click the dock for *Add app…*, *Preferences…*, and — on an icon — *Open* and *Remove from
Dock*. The preferences window has the whole list, with reordering and removal per row.

The list is stored in `%LOCALAPPDATA%\LiquiDock\items.txt`, one item per line, and editing it by
hand still works:

```
group | path | label
```

`group` is `main` or `utility` — utility items sit to the right of the hairline. `label` is optional.
`path` is anything the shell can open: a program, a shortcut, a folder, a `shell:AppsFolder\...`
moniker for a packaged app, or a `::{guid}` parsing name. Environment variables are expanded.

On a first run there is no file, so the list is seeded from whatever is pinned to your taskbar, plus
Downloads and the Recycle Bin.

## Settings

*Preferences…* in the tray menu opens a custom-drawn panel with every setting on it. Changes apply
to the running dock immediately and are written back to the settings file.

That file — `%LOCALAPPDATA%\LiquiDock\settings.txt`, `key = value`, with a comment on every
setting — stays a first-class way in, and *Edit settings file…* opens it. The dock watches it, so
saving takes effect without a restart, and the preferences panel rewrites only the value half of
each line, so comments you add survive being edited from the UI.

Glass: `backdrop`, `refraction`, `depth`, `dispersion`, `frost`, `splay`, `light-angle`,
`light-intensity`, `tint-alpha`. Magnification: `magnification`, `max-scale`, `influence`,
`icon-bulge`, `follow-cursor`. Appearance: `separator-image`. Placement: `monitor`, `reserve-space`. Auto-hide: `auto-hide`, `hide-when-covered`,
`dwell-seconds`, `slide-seconds`.

`hide-when-covered` is **on** by default and is what makes auto-hide behave the way people expect:
the dock tucks away under a window and comes straight back when the desktop is clear, so minimising
everything shows it rather than hiding it from nothing. Turn it off to have the dock hide on its
dwell whatever is behind it. Coverage is worked out from window events, not a timer — nothing is
polled to answer it.

`reserve-space` makes the dock an appbar, so maximised windows stop above it the way they do above
the taskbar. It is off by default and ignored while auto-hide is on, where a dock that is not on
screen has no business holding room.

`backdrop` decides what the glass refracts. `wallpaper` (the default) decodes your desktop
background once and costs nothing after that — it is exactly right whenever nothing is behind the
dock, and wrong-looking over a window. `screen` refracts what is actually there, by duplicating the
strip of desktop the dock covers and ignoring every change that misses it. The catch is not
performance: the dock has to be excluded from screen capture to stop it refracting its own last
frame forever, and that same exclusion makes the dock invisible in your own screenshots and screen
shares. That is why it is not the default.

`follow-cursor` is **off** by default. On, the row slides sideways as it swells so the icon under
the pointer stays exactly under it, which is what macOS does — and is also why the whole bar appears
to drift left and right as you move along it. Off, the bar holds its centre and grows evenly to both
sides; the hovered icon drifts by a few pixels and the dock sits still.

`separator-image` replaces the divider between the two groups with an image, stretched to the
divider's height and keeping its own aspect, so a 1×59 hairline stays a hairline. A Nexus theme's
`sep.png` drops straight in. Empty draws the built-in rule.

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
