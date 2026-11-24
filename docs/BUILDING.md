# Building Convertible

Every component builds from a clean checkout, offline, with the platform's own toolchain. The
product has no third-party package dependencies: no package managers, no submodules, no
`FetchContent`. (The Android build alone downloads Gradle, the Android Gradle plugin and the
Jetpack libraries through Gradle, as every Android app does.)

Contents: [Core and CLI](#core-and-cli) · [Desktop](#desktop) · [Web](#web) ·
[Android](#android) · [iOS](#ios-and-ipados) · [Releases](#releases) · [CI](#continuous-integration)

## Core and CLI

Prerequisites: CMake 3.22 or newer and a C++23 compiler.

| Platform | Compiler | Notes |
| --- | --- | --- |
| Linux | GCC 14, or Clang 18 with libc++ 18 | Ubuntu 24.04: `sudo apt-get install g++-14 cmake`; select it with `CC=gcc-14 CXX=g++-14` |
| macOS 13.3+ | Apple Clang 21 (Xcode or Command Line Tools) | deployment target 13.3 is set automatically |
| Windows | MSVC 2022 17.10+ (Visual Studio generator) | run CMake from a Developer prompt or let the VS generator find it |
| Web | Emscripten (see [Web](#web)) | |

### Presets

`CMakePresets.json` defines three configure, build and test presets; each builds into
`build/<preset>`:

| Preset | Configuration | Contents |
| --- | --- | --- |
| `dev` | Debug | core, CLI, `convertible_tests` |
| `asan` | Debug, `CVT_SANITIZE=address;undefined` | as `dev`, with AddressSanitizer and UBSan (not MSVC) |
| `release` | Release | core and CLI, no tests |

```sh
cmake --preset dev && cmake --build --preset dev -j && ctest --preset dev
cmake --preset asan && cmake --build --preset asan -j && ctest --preset asan
cmake --preset release && cmake --build --preset release -j
```

The CLI is `build/<preset>/core/convertible` (`build/<preset>/core/<Config>/convertible.exe`
with the Visual Studio generator). `tools/ci/core.sh <preset>` runs the same three steps and
is what CI uses; it honours `CC`, `CXX`, `CXXFLAGS` and `LDFLAGS`:

```sh
CC=gcc-14 CXX=g++-14 tools/ci/core.sh dev
CC=clang-18 CXX=clang++-18 CXXFLAGS=-stdlib=libc++ LDFLAGS=-stdlib=libc++ tools/ci/core.sh asan
```

### Options

| Option | Default | Meaning |
| --- | --- | --- |
| `CVT_BUILD_TESTS` | ON at top level, OFF inside an app build | build `convertible_tests` |
| `CVT_BUILD_CLI` | ON at top level, OFF inside an app build | build the `convertible` tool |
| `CVT_SANITIZE` | empty | sanitizers, e.g. `address;undefined` (ON = both) |
| `CVT_WASM_EXCEPTIONS` | ON | Emscripten: native WebAssembly exception handling |
| `CVT_BUILD_DESKTOP` | OFF | also build `apps/desktop` |
| `CMAKE_CXX_STANDARD` | newest of 26/23 the toolchain supports | pass `23` to check the baseline |

Our own code builds warning-free with `-Wall -Wextra -Wpedantic` and MSVC `/W4`; imported
code is compiled with warnings off.

## Desktop

```sh
cmake -S . -B build/desktop -DCVT_BUILD_DESKTOP=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/desktop -j 6 --config Release
ctest --test-dir build/desktop -L '^desktop$' -C Release --output-on-failure     # unit tests
ctest --test-dir build/desktop -L desktop-e2e -C Release --output-on-failure   # end-to-end
cmake --build build/desktop --config Release --target package
```

Linux needs `libx11-dev libxext-dev` in addition to GCC 14 (`tools/ci/install-linux-deps.sh
desktop` installs everything on Ubuntu 24.04). Packages land in `build/desktop/packages/`:
a zipped unsigned `Convertible.app` on macOS, a `.zip` on Windows, `.tar.gz` and `.deb` on
Linux. `tools/ci/desktop.sh` runs all four steps (it skips the core's unit tests, which the core
jobs cover); see [`apps/desktop/README.md`](../apps/desktop/README.md) for the app itself.

## Web

```sh
apps/web/build.sh --docker      # inside the pinned emscripten/emsdk:7.0.0 image
apps/web/build.sh --local       # with a locally activated emsdk 7.0.0
node apps/web/tools/serve.mjs   # serve apps/web/dist at http://127.0.0.1:8080/
```

Docker (or emsdk) and Node.js 18+ are all that is needed. Output: a complete static site in
`apps/web/dist/`. Details, hosting checklist and environment knobs:
[`apps/web/README.md`](../apps/web/README.md).

## Android

Prerequisites: JDK 17+, `ANDROID_HOME` pointing at an SDK with `platforms;android-37.0`,
`build-tools;37.0.0`, `ndk;27.1.12297006` and `cmake;3.22.1`.

```sh
cd apps/android
./gradlew --max-workers=4 assembleDebug assembleRelease testDebugUnitTest lint
```

APKs (one per ABI plus a universal one) land in `app/build/outputs/apk/{debug,release}/`;
release APKs are unsigned. `-Pconvertible.abis=arm64-v8a` builds one ABI for faster local
iteration. See [`apps/android/README.md`](../apps/android/README.md).

## iOS and iPadOS

Prerequisites: macOS with Xcode and CMake on `PATH`.

```sh
xcodebuild build -project apps/ios/Convertible.xcodeproj -scheme Convertible \
  -destination 'platform=iOS Simulator,name=iPhone 18 Pro' CODE_SIGNING_ALLOWED=NO
```

A pre-build phase compiles the core into an XCFramework first. Device builds and archives
need your own signing team; the project contains none. There is deliberately no iOS job in CI
and no iOS release artefact, because anything installable needs the owner's certificate. See
[`apps/ios/README.md`](../apps/ios/README.md).

## Releases

The version lives in one place: `project(Convertible VERSION x.y.z)` in the root
`CMakeLists.txt` (the CLI, the desktop packages and the Android `versionName` read it).
Pushing a tag `v<version>` runs `.github/workflows/release.yml`, which refuses tags that do not
match, builds everything and publishes a GitHub Release:

| Artefact | Built by |
| --- | --- |
| `convertible-cli-<v>-linux-x64.tar.gz`, `convertible-desktop-<v>-linux-x64.tar.gz`, `convertible-desktop_<v>_amd64.deb` | `tools/ci/desktop.sh`, `tools/ci/package-cli.sh` on ubuntu-24.04 |
| `convertible-cli-<v>-macos-universal.tar.gz`, `Convertible-<v>-macos-arm64-x64.zip` | the same, with `CMAKE_OSX_ARCHITECTURES=arm64;x86_64` (unsigned, not notarised) |
| `convertible-cli-<v>-windows-x64.zip`, `Convertible-<v>-windows-x64.zip` | the same on windows-2025 (MSVC) |
| `convertible-android-<v>-<abi>-release-unsigned.apk`, `...-<abi>-debug.apk` | Gradle, `tools/ci/package-android.sh` |
| `convertible-web-<v>.zip` | `apps/web/build.sh --docker`, `tools/ci/package-web.sh` |
| `SHA256SUMS.txt` | `tools/ci/checksums.sh` |

Running the workflow by hand (`workflow_dispatch`) produces the same files as a workflow
artifact without publishing anything. The step-by-step procedure is in the `release` skill
(`.agents/skills/release/SKILL.md`).

**macOS Gatekeeper.** The `.app` is unsigned and not notarised, so macOS blocks the first
launch. Right-click the app and choose *Open*, or remove the quarantine attribute:
`xattr -dr com.apple.quarantine Convertible.app`. The same applies to the CLI binary.

## Continuous integration

`.github/workflows/`:

| Workflow | Trigger | Jobs |
| --- | --- | --- |
| `ci.yml` | push to `main`, pull requests, nightly, manual | core (GCC 14, Clang 18 + ASan/UBSan, Apple Clang, MSVC), web, android, android on an emulator (nightly or manual), desktop (Linux, macOS, Windows), hygiene |
| `release.yml` | tags `v*`, manual | release artefacts and the GitHub Release |
| `pages.yml` | manual | builds the web app and deploys it to GitHub Pages |

Only GitHub's own actions are used, each pinned to a full commit SHA; everything else is a
shell step that calls a script in `tools/ci/`, so every job can be reproduced locally (see
[TESTING.md](TESTING.md#rehearsing-ci-locally)). No secrets are needed. Runner images are
pinned (`ubuntu-24.04`, `macos-26`, `windows-2025`) so toolchains change only with the
workflow files.

The full core compiles every family and imported codec (libaom, libjxl, libvpx, libopus, ...)
in every job that builds it, so those jobs keep their build directory in the Actions cache,
keyed on a hash of `core/**`, `assets/fonts/**` and the CMake files (plus the app's own files
for the web and desktop jobs). There is no compiler cache: after a restore,
`tools/ci/build-cache.sh restore <dir>` back-dates exactly the sources whose content is
unchanged since the cache was saved (it records `git ls-files -s` in the build directory), so
make and MSBuild rebuild only what changed and what depends on it. Caches are saved after a
successful build, even when tests then fail. Release builds always start clean.

**GitHub Pages** must be enabled once before `pages.yml` can deploy: repository *Settings ->
Pages -> Build and deployment -> Source: GitHub Actions*. Until then the workflow stops at its
first step with that instruction. The site is served from a sub-path
(`https://<owner>.github.io/<repository>/`); the web app uses relative URLs only.
