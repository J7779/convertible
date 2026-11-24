# Convertible for Windows, Linux and macOS

The desktop app is one C++ code base with an in-house UI stack: a small
platform layer per OS, a software renderer, a TrueType text engine and an
immediate-mode UI toolkit, all written for this project. It uses nothing but
the operating systems' own SDKs (Cocoa, Win32/COM/Shell, Xlib + XShm) and
links the Convertible core statically, so each OS gets a single
self-contained executable. Fonts (Inter, JetBrains Mono NL), the design tokens,
the motion storyboard and the licence texts are embedded at build time.

Contents: [Prerequisites](#prerequisites) · [Build and run](#build-and-run) ·
[Tests](#tests) · [Packaging](#packaging) · [Automation](#automation-mode) ·
[Architecture](#architecture) · [How the core is linked](#how-the-core-is-linked)

## Prerequisites

| OS | Toolchain | Extra packages |
| --- | --- | --- |
| macOS 13.3+ | Xcode / Command Line Tools (Apple Clang 21), CMake 3.22+ | none |
| Windows 10 1703+ | Visual Studio 2022 17.10+ (MSVC, `/std:c++latest`), CMake 3.22+ | none (Windows SDK) |
| Linux | GCC 14 or Clang 18 (libc++ 18), CMake 3.22+, make or Ninja | `libx11-dev libxext-dev` |

Linux, Debian/Ubuntu: `sudo apt-get install g++-14 cmake libx11-dev libxext-dev`.
Running needs `libx11-6` and `libxext6` (and works on Wayland through
XWayland). "Show in Files" and "Open" call `xdg-open` when it is installed
(`xdg-utils`); everything else works without it. Headless tests under X need
`xvfb` only for the optional windowed check.

## Build and run

From the repository root (the root `CMakeLists.txt` adds this directory when
`CVT_BUILD_DESKTOP=ON`):

```sh
cmake -S . -B build/desktop -DCVT_BUILD_DESKTOP=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/desktop -j 6 --config Release
```

| OS | Executable |
| --- | --- |
| macOS | `build/desktop/apps/desktop/Convertible.app` (`open` it, or run `Contents/MacOS/Convertible`) |
| Windows | `build/desktop/apps/desktop/Release/Convertible.exe` (multi-config generators) |
| Linux | `build/desktop/apps/desktop/convertible-desktop` |

Command line: `Convertible [files…]` opens files straight into the queue.
`--skip-intro`, `--config-dir <dir>`, `--size WxH`, `--scale S` and `--version`
are available; see [Automation](#automation-mode) for the rest.

Settings are stored as JSON in the per-user config location:
`~/Library/Application Support/Convertible` (macOS),
`%APPDATA%\Convertible` (Windows), `$XDG_CONFIG_HOME/convertible` or
`~/.config/convertible` (Linux). Nothing else is persisted: converted files
live in a private temporary folder until saved and are deleted on exit.

## Tests

```sh
cmake -S . -B build/desktop -DCVT_BUILD_DESKTOP=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/desktop -j 6 --config Release
ctest --test-dir build/desktop -L desktop -C Release --output-on-failure
```

Two CTest entries carry the `desktop` label, and both run headless (no
display, no window):

- `desktop_unit_tests` (`convertible_desktop_tests`, the core's in-house test
  framework): JSON/UTF-8/formatting, the renderer (coverage, AA, strokes,
  gradients, shadows, PNG output checked with the core's inflater), TrueType
  parsing, GPOS kerning and `tnum`, layout/truncation/wrapping/hit-testing,
  easing and springs, tokens and contrast, UI hit-testing, focus order and
  keyboard activation, text editing with the clipboard, sliders, segmented
  controls and scroll views; binding tests with real conversions through the
  core (Base64/hex round trips, checksums against known vectors, UTF-16 with a
  BOM, file conversion, cancellation, error mapping, every schema in the
  catalogue); view-model tests (queue, shared-format control, option coercion
  and presets, jobs, saving with unique names, settings persistence); whole-app
  tests on the headless platform (intro, every screen in both themes, picker
  search and keyboard selection, a conversion through the UI), the logo
  storyboard and the in-house file browser.
- `desktop_automation_smoke`: the real executable in `--headless --automation`
  mode opens three files, picks targets and options, clicks "Convert all",
  saves the results and checks their bytes, then writes screenshots and a state
  dump to `build/desktop/apps/desktop/automation-smoke/`.

Run a subset: `build/desktop/apps/desktop/convertible_desktop_tests --test-case="desktop/ui*"`.

## Packaging

CPack, no external tools:

```sh
cmake --build build/desktop --config Release --target package   # or: (cd build/desktop && cpack -C Release)
```

| OS | Artefacts in `build/desktop/packages/` |
| --- | --- |
| macOS | `Convertible-<version>-macos-<arch>.zip` with `Convertible.app` (unsigned; Info.plist with document types so files can be opened with it, `icon.icns`) |
| Windows | `Convertible-<version>-windows-<arch>.zip` with `Convertible.exe` (embedded icon, manifest with per-monitor DPI v2, version resource) and the licences |
| Linux | `convertible-desktop-<version>-linux-<arch>.tar.gz` and `convertible-desktop_<version>_<arch>.deb` (binary, `app.convertible.desktop.desktop`, hicolor icons, licences) |

## Automation mode

```sh
Convertible --headless --automation script.json --out out-dir [--size 1120x760] [--scale 2]
```

`--headless` renders into an off-screen bitmap on a virtual 60 Hz clock (no
window, deterministic timing). Without it the same script drives the real
window. The exit code is 1 when any `expect` step fails. A script is a JSON
array of steps; relative input paths resolve against the script's folder,
output paths against `--out`:

| Step | Effect |
| --- | --- |
| `{"op":"open","files":[…]}` | add files (as a drop; `"drop":false` adds directly) |
| `{"op":"setTarget","index":0,"target":"base64"}` / `{"op":"setTarget","source":"txt","target":"hex"}` | target for one file / every file of a format |
| `{"op":"setOption","index":0,"key":"line_width","value":0}`, `{"op":"setPreset","index":0,"preset":"small"}`, `{"op":"advanced","index":0,"open":true}` | options |
| `{"op":"openPicker","index":0}`, `{"op":"search","text":"hex"}`, `{"op":"closePicker"}` | target picker |
| `{"op":"convert","index":0}`, `{"op":"convertAll"}`, `{"op":"cancel","index":0}`, `{"op":"cancelAll"}`, `{"op":"waitIdle"}` | conversion |
| `{"op":"save","index":0,"path":"x.b64"}`, `{"op":"saveAll","dir":"saved","expect":3}` | save without dialogs |
| `{"op":"screenshot","path":"a.png"}` (`.png` or `.bmp`), `{"op":"dump","path":"state.json"}` | capture the next frame / the state |
| `{"op":"click","label":"Convert all"}` or `{"op":"click","x":10,"y":20}`, `{"op":"hover",…}`, `{"op":"key","key":"Tab","shift":true,"primary":true}`, `{"op":"type","text":"…"}`, `{"op":"drag","phase":"enter"}` | input |
| `{"op":"navigate","screen":"formats"}`, `{"op":"theme","value":"light"}`, `{"op":"settings","values":{…}}`, `{"op":"resize","width":800,"height":600,"scale":2}` | app state |
| `{"op":"wait","ms":500}`, `{"op":"frames","count":3}`, `{"op":"skipIntro"}` | timing |
| `{"op":"expect","path":"items.0.status","equals":"done"}`, `{"op":"expect","file":"saved/a.b64","content":"…"}` | assertions on the state dump / output files |

The state dump lists the screen, theme, window size and zoom, settings, every
queued file (format, target, options, status, progress, output size, errors,
warnings), the picker, toasts, and the accessibility tree (role, label, value,
rectangle, focus) of the last frame.

## Architecture

```
src/base/       geometry, JSON, UTF-8 and formatting helpers, embedded resources
src/gfx/        software renderer: paths (SVG data, flattening, trimming, stroking),
                area-coverage AA rasteriser (non-zero), analytic rounded rects and
                CSS-style shadows, linear/radial gradients, clipping, opacity,
                images, blur; PNG/BMP writers
src/text/       TrueType (glyf, cmap 4/12, hmtx, composite glyphs), GPOS pair
                kerning (formats 1 and 2, extension lookups) and the legacy kern
                table, GSUB single substitution (tnum), shaping, truncation,
                wrapping, hit-testing, glyph cache with sub-pixel positioning
src/ui/         immediate-mode toolkit: layers, clipping, hit-testing, focus order
                and keyboard navigation, focus rings, tooltips, scroll views with
                spring physics, buttons, segmented control, switch, slider, text
                field (caret, selection, clipboard), disclosure, chips, panels;
                cubic-bezier tweens, springs, keyframe tracks; design tokens
src/app/        binding (C API -> typed catalogue, targets, schemas, conversions),
                view-model (queue, options, jobs on worker threads, settings),
                logo/intro/spinner from animation.json, views, in-house file
                browser, automation
src/platform/   platform.hpp (the one OS interface), headless.cpp, fileio,
                mac/ (Cocoa, Objective-C++), win/ (Win32), x11/ (Xlib)
tests/          unit tests, fixtures, automation smoke script
```

Layering: `binding` → `state` (view-model, no UI) → views (`app.cpp`,
`views_*.cpp`) → `ui` toolkit → `gfx`/`text`. Only `src/platform/*/` touches
an OS API; everything else is plain C++23 and is tested headlessly.

Each frame the views describe the whole interface; widgets register their
rectangles for hit-testing, focus order and the accessibility tree, and record
draw commands per layer (base, overlay, modal, tooltip). The platform layer
presents the resulting BGRA bitmap (CoreAnimation layer contents on macOS,
`SetDIBitsToDevice` on Windows, XShm or `XPutImage` on X11) and only asks for
frames while something animates or a timer (caret blink, tooltip) is due.
HiDPI: the UI is laid out in points; the canvas scales by the display scale
times the text-size setting (100/115/130 %, also Ctrl/Cmd +/−/0).

Conversions run through `cvt_convert_file` (the input is memory-mapped by the
core) on a pool of `std::jthread` workers; progress and cancellation go
through the progress callback. Results are written to a private session
folder and copied wherever the user saves them.

Accessibility: full keyboard operation with visible focus, reduced motion
(system setting or in-app), text size, AA contrast from the tokens. On macOS
the toolkit's accessibility tree is exposed to VoiceOver through
`NSAccessibilityElement`s; on Windows (UI Automation) and Linux (AT-SPI) the
tree exists but is not bridged to the OS screen reader yet.

## How the core is linked

`apps/desktop/CMakeLists.txt` links `convertible::core` (the static library
from `core/`, added by the root `CMakeLists.txt`) into `cvd_app`. The app
uses only `convertible/convertible.h`; the unit tests additionally use the
core's test framework header (`core/tests/support/test.hpp`) and, to check
PNG output, its internal inflater. The format catalogue, targets and option
schemas are read at run time, so new core formats appear without app changes.
