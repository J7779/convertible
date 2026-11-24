# Convertible for Windows, Linux and macOS

The desktop app is one C++ code base with an in-house UI stack: a small
platform layer per OS, a software renderer, a TrueType text engine and an
immediate-mode UI toolkit, all written for this project. It uses nothing but
the operating systems' own SDKs (Cocoa, Win32/COM/Shell, Xlib + XShm) and
links the Convertible core statically, so each OS gets a single
self-contained executable. Fonts (Inter, JetBrains Mono NL), the design tokens,
the motion storyboard and the licence texts are embedded at build time.

Contents: [Prerequisites](#prerequisites) · [Build and run](#build-and-run) ·
[Tests](#tests) · [End-to-end tests](#end-to-end-tests) · [Packaging](#packaging) ·
[Automation](#automation-mode) · [Architecture](#architecture) ·
[How the core is linked](#how-the-core-is-linked)

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
`--skip-intro`, `--config-dir <dir>`, `--workers N`, `--size WxH`, `--scale S`
and `--version` are available; see [Automation](#automation-mode) for the rest.
The `CONVERTIBLE_CONFIG_DIR` environment variable names the settings folder
when `--config-dir` is not given (tests, portable setups).

Settings are stored as JSON in the per-user config location:
`~/Library/Application Support/Convertible` (macOS),
`%APPDATA%\Convertible` (Windows), `$XDG_CONFIG_HOME/convertible` or
`~/.config/convertible` (Linux). Nothing else is persisted: converted files
live in a private temporary folder until saved and are deleted on exit.

## Tests

```sh
cmake -S . -B build/desktop -DCVT_BUILD_DESKTOP=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/desktop -j 6 --config Release
ctest --test-dir build/desktop -L '^desktop$' -C Release --output-on-failure
```

Two CTest entries carry the `desktop` label, and both run headless (no
display, no window). (`-L` is a regular expression: `-L desktop` also picks
up the [end-to-end groups](#end-to-end-tests), labelled `desktop-e2e`.)

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

## End-to-end tests

```sh
cmake -S . -B build/desktop -DCVT_BUILD_DESKTOP=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/desktop -j 6 --config Release
ctest --test-dir build/desktop -L desktop-e2e -C Release --output-on-failure
```

or the wrappers, which configure, build and run in one go:
`apps/desktop/e2e.sh [--windowed] [--repeat N]` (macOS, Linux) and
`powershell -File apps\desktop\e2e.ps1 [-Windowed] [-Repeat N]` (Windows).
Linux without a display: `xvfb-run -a` around the windowed run (the
wrapper does it when `xvfb-run` is installed).

The suite drives the real executable through [automation](#automation-mode)
with real conversions in the core, then checks what the app left behind. It
is in-house like the rest: a C++ runner (`convertible_desktop_e2e`, the
core's test framework, the toolkit's JSON type, the core's internal hashes),
scripts and nothing else. One CTest entry per group, all labelled
`desktop-e2e`:

| Group | What it proves |
| --- | --- |
| `intro` | sampled intro frames differ over time; the last one equals the static mark that reduced motion shows at once; a key or a click skips it |
| `add-files` | command line, a drop anywhere on the window (the overlay appears), the in-house file browser, the native-dialog seam; folders and missing files are refused with a message |
| `queue`, `picker` | detected-format chips, the "All N files to…" control, remove and clear; the picker's grouping, search, keyboard navigation, Escape and outside clicks |
| `options` | a slider, a switch, a menu and a text field found in the core's schemas change the saved output; presets and Compress where the catalogue offers them |
| `convert`, `cancel`, `errors` | monotonic per-file progress to 100 %; before/after sizes and percentages; Save, Save all (numbered on collisions, never overwriting), Convert again, Reveal and Open through the shell seam; saved bytes decoded or digested independently (Base64, hex, SHA-256/SHA-1/MD5/CRC-32, UTF-16LE with BOM); cancel one, cancel all, convert again; a friendly error with the queue intact |
| `formats`, `settings` | the formats browser against the catalogue the app reports; every preference changed through the UI, persisted in `CONVERTIBLE_CONFIG_DIR` and restored after a restart |
| `keyboard` | the main flow with the keyboard only, focus kept inside the file browser, a focus ring in the screenshot |
| `layout`, `themes`, `text`, `a11y` | every screen from the minimum window to 1920×1200, at 1×/1.5×/2× and 130 % text: no clipped or partially overlapping controls (layout rectangles from the state dump); token colours at known pixels in both themes; no missing glyphs; every control named and given a role |
| `catalog` | only when the core has the formats (skipped with a message otherwise): PNG → JPEG (JPEG markers, smaller at lower quality), any file → ZIP (central directory parsed here), WAV → another audio container |
| `window` | the real Cocoa/Win32/X11 window; opt-in with `CVD_E2E_WINDOWED=1` |

Determinism: headless runs use a virtual 60 Hz clock (frame *n* is at
*n*/60 s; scripts advance it with `at`/`wait`/`frames`, nothing sleeps);
conversions are stepped through a progress gate that parks the workers in
the core's progress callback, so progress, cancellation and "Cancel all" are
observed at exact points. Clicks and key presses arrive over separate frames
as real input does.

Isolation: every test gets fresh `in/`, `out/`, `config/`, `tmp/` and `home/`
folders under `<build>/apps/desktop/e2e/runs/<test>/`. The runner points
`CONVERTIBLE_CONFIG_DIR`, `TMPDIR`/`TMP`/`TEMP` and `HOME`/`XDG_CONFIG_HOME`
(`APPDATA`/`LOCALAPPDATA` on Windows) there, and after each run checks that
the app removed its session folder and wrote nothing to the user folders.
File dialogs, "Show in folder" and "Open" go through test seams under
automation, so no OS dialog or other app is ever opened. On a failure the
app writes `failures/failure-N.png` next to its outputs and the runner
copies every screenshot and dump of the failed test to
`<build>/apps/desktop/e2e/failures/<test>/` (inside the build tree, not in git).

Run one group or test: `ctest --test-dir build/desktop -R e2e_convert`, or
`build/desktop/apps/desktop/convertible_desktop_e2e --app=<exe> --work=<dir>
--e2e=apps/desktop/e2e --assets=assets --test-case="e2e/picker/*"` (the exact
command line is printed by `ctest -V`).

Linux with GCC 14 (`ubuntu:24.04`): configure with
`-DCMAKE_C_FLAGS=-D_DEFAULT_SOURCE` for now; the core's imported H.264
decoder calls `strnlen`, which strict C11 headers do not declare otherwise.

Windows: the runner starts the app with `CreateProcessW` and needs no POSIX
shell; the suite runs with `ctest -L desktop-e2e -C Release` on a GitHub
Windows runner (MSVC) and cross-compiles with MinGW-w64 GCC 14.

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
window, deterministic timing: frame *n* is at exactly *n*/60 s). Without it
the same script drives the real window. The exit code is 1 when any step
fails; failures and progress also go to `automation.log` in `--out`, with a
`failures/failure-N.png` screenshot of the first failures. Under automation
"Show in folder" and "Open" are recorded instead of calling the OS. A script
is a JSON array of steps; relative input paths resolve against the script's
folder, output paths against `--out`:

| Step | Effect |
| --- | --- |
| `{"op":"open","files":[…]}` | add files (as a drop; `"drop":false` adds directly) |
| `{"op":"setTarget","index":0,"target":"base64"}` / `{"op":"setTarget","source":"txt","target":"hex"}` | target for one file / every file of a format |
| `{"op":"setOption","index":0,"key":"line_width","value":0}`, `{"op":"setPreset","index":0,"preset":"small"}`, `{"op":"advanced","index":0,"open":true}` | options |
| `{"op":"openPicker","index":0}`, `{"op":"search","text":"hex"}`, `{"op":"closePicker"}` | target picker |
| `{"op":"convert","index":0}`, `{"op":"convertAll"}`, `{"op":"cancel","index":0}`, `{"op":"cancelAll"}`, `{"op":"waitIdle"}` | conversion |
| `{"op":"save","index":0,"path":"x.b64"}`, `{"op":"saveAll","dir":"saved","expect":3}` | save without dialogs |
| `{"op":"screenshot","path":"a.png"}` (`.png` or `.bmp`), `{"op":"dump","path":"state.json"}` | capture the next frame / the state |
| `{"op":"click","label":"Convert all"}` (`"exact"`, `"role"`, `"nth"`, `"last"`, `"dx"`/`"dy"` inside the element, `"shift"`, `"primary"`, `"clicks":2`) or `{"op":"click","x":10,"y":20}`, `{"op":"hover",…}`, `{"op":"scroll","dy":120}`, `{"op":"key","key":"Tab","shift":true,"primary":true,"repeat":3}`, `{"op":"type","text":"…"}`, `{"op":"drag","phase":"enter","x":40,"y":700}` (`enter`/`over`/`leave`) | input: a click is pressed in one frame and released in the next, one key press per frame |
| `{"op":"tabTo","label":"Open","exact":true}`, `{"op":"focusCycle","count":12,"path":"focus.json"}` | keyboard navigation: Tab until a control has focus / record the focus after each Tab |
| `{"op":"dialogs","mode":"seam"}` + `{"op":"dialogReply","paths":[…]}` (`[]` = cancelled), `{"op":"shellResult","value":false}` | answer file dialogs without the OS dialog or the in-house browser; make Reveal/Open report failure |
| `{"op":"gate","mode":"hold"}`, `{"op":"gateWait","parked":2}`, `{"op":"stepProgress","index":0,"steps":1,"stride":1}`, `{"op":"gate","mode":"open"}` | progress gate: park conversions at their progress reports, wait until N workers are parked, release one report per frame while recording the item's progress (`automation.samples`) |
| `{"op":"navigate","screen":"formats"}`, `{"op":"theme","value":"light"}`, `{"op":"settings","values":{…}}`, `{"op":"resize","width":800,"height":600,"scale":2}` | app state |
| `{"op":"at","ms":1800}`, `{"op":"wait","ms":500}`, `{"op":"frames","count":3}`, `{"op":"waitFor","path":"items.0.status","equals":"done","timeoutMs":20000}`, `{"op":"skipIntro"}` | timing: run the clock to a time after the first frame / by an amount / by frames; render until a condition holds |
| `{"op":"expect","path":"items.0.status","equals":"done"}` (also `notEquals`, `exists`, `min`, `max`, `length`, `contains`), `{"op":"expect","file":"saved/a.b64","content":"…"}` | assertions on the state dump / output files |

The state dump lists the screen, theme, window size, scale and zoom,
settings, the intro phase and its clock, every queued file (format, chip,
targets, option values and schemas, preset and compress availability, status,
progress, output name, size and temporary path, errors, warnings), the
picker (query, highlight, grouped targets), the option menu, the file
browser, the formats browser's search, toasts, glyph statistics (missing
glyphs in the last frame), the drop overlay, focus visibility, focusable
controls without an accessibility node, what the automation seams recorded,
and the accessibility tree (role, label, value, rectangle, unclipped
rectangle, scroll and layer, focus) of the last frame. `"catalog":true` adds
the whole format catalogue.

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
e2e/            end-to-end suite: runner (C++), tests, automation scripts
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
