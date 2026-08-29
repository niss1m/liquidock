# LiquiDock

A macOS-style dock for Windows, with a real liquid glass background.

Not Electron. Not a WebView. A single native C++/Direct3D 11 executable that idles at zero.

## What makes it different

Every other Windows dock paints its background from a pre-baked bitmap — a nine-slice PNG stretched
to fit. LiquiDock computes the background per pixel, in a shader, from whatever is actually behind
it: refraction bending the backdrop through the bevel, chromatic dispersion at the rim, frost, and a
specular edge driven by a light angle you control.

- **Real liquid glass** — refraction, depth, dispersion, frost, splay and light, all live and all
  tweakable.
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
| M2 | Item model, icons, layout, launching, magnification | |
| M3 | Multi-monitor, auto-hide, appbar, running indicators | |
| M4 | Settings UI | |
| M5 | Portable / Microsoft Store / Steam packaging | |

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
