# Convertible for iPhone and iPad

A native SwiftUI shell around the shared C++ core. Every conversion runs on the
device, inside the app process; the app has no network code, no accounts and no
analytics.

## Prerequisites

- macOS with Xcode (developed and tested with Xcode 27.2 and the iOS 27.2 simulators)
- CMake 3.22 or newer on `PATH` (or in `/opt/homebrew/bin`, `/usr/local/bin`)
- Optional: [XcodeGen](https://github.com/yonaskolb/XcodeGen), only to regenerate the
  committed project after editing `project.yml`

Deployment target: iOS/iPadOS 16.4 (the oldest system whose C++ runtime has
everything the core needs). No Swift packages, no CocoaPods.

## Build, run, test

All commands are run from the repository root. Building from a clean checkout is
one `xcodebuild` call: a pre-build phase compiles the core first.

```sh
# Simulator build
xcodebuild build -project apps/ios/Convertible.xcodeproj -scheme Convertible \
  -destination 'platform=iOS Simulator,name=iPhone 18 Pro' CODE_SIGNING_ALLOWED=NO

# Unit tests (real conversions through the real core, plus view-model tests)
xcodebuild test -project apps/ios/Convertible.xcodeproj -scheme Convertible \
  -destination 'platform=iOS Simulator,name=iPhone 18 Pro' CODE_SIGNING_ALLOWED=NO \
  -only-testing:ConvertibleTests

# End-to-end UI tests (see "End-to-end tests" below)
apps/ios/e2e.sh

# Device slice, unsigned (proves arm64 iphoneos compiles and links)
xcodebuild build -project apps/ios/Convertible.xcodeproj -scheme Convertible \
  -destination 'generic/platform=iOS' CODE_SIGNING_ALLOWED=NO
```

To run it, open `apps/ios/Convertible.xcodeproj`, pick a simulator and press Run.
For screenshots and UI tests the app understands a few launch arguments:
`-ConvertibleSkipIntro`, `-ConvertibleSampleFiles` (queues three small text
files), `-ConvertibleLargeSample` (a 96 MiB binary, to watch progress),
`-ConvertibleAutoConvert` and `-ConvertibleScreen formats|settings|picker`, e.g.

```sh
xcrun simctl launch booted app.convertible.ios -ConvertibleSkipIntro -ConvertibleSampleFiles
```

Key controls carry accessibility identifiers for XCUITest (`chooseFilesButton`,
`dropZone`, `queueCard.<file name>`, `targetPickerButton`, `target.<format id>`,
`presetControl`, `advancedOptionsToggle`, `option.<key>`, `convertAllButton`,
`cancelButton`, `cancelAllButton`, `resultSaveButton`, `resultShareButton`,
`resultPreviewButton`, `convertAgainButton`, `errorBanner`, `formatsButton`,
`settingsButton`, `sidebar.<section>`, `themePicker`, …).

## End-to-end tests

`ConvertibleUITests` is an XCUITest suite (XCTest only: no packages, no third-party
libraries) that drives the real app on a simulator and performs real conversions
through the real core. One command builds once and runs it:

```sh
apps/ios/e2e.sh                                    # iPhone 18 Pro, whole suite
apps/ios/e2e.sh --device "iPad Pro 11-inch (M5)"   # iPad split view
apps/ios/e2e.sh --repeat 5 --clone                 # five runs on a private copy of the device
apps/ios/e2e.sh --only ConversionTests             # one class, or Class/testMethod
```

It runs `xcodebuild test-without-building -only-testing:ConvertibleUITests
CODE_SIGNING_ALLOWED=NO` on the named simulator and writes one result bundle per run
to `apps/ios/build/e2e/` (git-ignored; open it in Xcode to see the screenshots that
are attached to every failure). `--clone` runs on a throw-away copy of the device so
that another run on the same simulator cannot collide; the script shuts down what it
booted and deletes what it cloned. **These tests are local only**: the repository has
no iOS job in CI, because running on a device or archiving needs the owner's signing
certificate. Simulators need an unlocked, logged-in Mac session: while the screen is
locked they stay on the boot logo, and the unit tests and this suite cannot run (the
builds still can).

**How results are verified.** Under the Debug-only launch argument
`-ConvertibleUITestHooks` (see `Convertible/App/UITestHooks.swift`; Release builds
compile it out) the app

- queues files given by the test in the environment variable `CONVERTIBLE_UITEST_FILES`
  (JSON `[{"name", "base64"}]`), so every input byte is known to the test;
- puts an invisible accessibility element on each queue card: `uitest.item` (JSON with
  the detected format, the offered targets with their categories, the chosen target,
  the option values and the phase) and, once finished, `uitest.output` with the
  result's file name, size and bytes (Base64, up to 256 KiB);
- clears the stored settings first with `-ConvertibleUITestResetSettings`, and can
  shorten the intro as with Reduce Motion (`-ConvertibleUITestReduceMotion`) or resize
  the large sample (`CONVERTIBLE_UITEST_LARGE_MIB`).

The test reads the result bytes back through the accessibility tree and checks them
with Foundation and CryptoKit: Base64 and hex decode to the input, checksum reports
contain the CryptoKit SHA-256/SHA-1/MD5, UTF-16LE re-encoding has the BOM and the
right text. Going through accessibility rather than the pasteboard or the simulator's
file system needs no paste-permission prompt and no host access, reads the exact file
the app produced, and works the same on iPhone, iPad and a device.

The suite covers: cold launch with the intro, skipping it, Reduce Motion, launch time
(`XCTApplicationLaunchMetric`); the queue with format badges, groups and "set all files
of this format"; the target picker (grouping, search, selection) and the formats
browser; options rendered from the schema changing the output; Compress; convert →
progress → result with before/after sizes; the Save and Share sheets; cancel, cancel
all and convert again on a large file; background/foreground during a conversion; a
friendly error and recovery; settings persisted across relaunch; iPhone and iPad
layouts, light and dark, an accessibility text size, rotation; Xcode's accessibility
audit on the main screens; and the system document picker with a file `e2e.sh` puts
into the simulator's Files storage (skipped when run without the script). Tests that
need image, audio or archive converters ask the app what it offers and skip with the
reason until the core has them. Everything is written against the catalogue the app
reports, never a fixed list of formats.

## Signing and archiving

The project deliberately contains no team, no provisioning profile and no
signing identity (`DEVELOPMENT_TEAM = ""`, automatic signing). Simulator builds
need nothing. To run on a device or archive, set your own team in Xcode
(Signing & Capabilities) or on the command line, for example:

```sh
xcodebuild archive -project apps/ios/Convertible.xcodeproj -scheme Convertible \
  -destination 'generic/platform=iOS' -archivePath build/Convertible.xcarchive \
  DEVELOPMENT_TEAM=<your team id>
xcodebuild -exportArchive -archivePath build/Convertible.xcarchive \
  -exportOptionsPlist <your ExportOptions.plist> -exportPath build/export
```

Bundle id `app.convertible.ios` (tests: `app.convertible.ios.tests`). The app
ships a privacy manifest (`PrivacyInfo.xcprivacy`: no tracking, no collected
data, UserDefaults reason CA92.1), declares `ITSAppUsesNonExemptEncryption =
NO`, and asks for one permission only — adding to the photo library, at the
moment the user taps "Save to Photos".

## How the core is linked

- `scripts/build-core.sh` configures `core/` with CMake (`CMAKE_SYSTEM_NAME=iOS`,
  Release, deployment target 16.4, tests and CLI off, at most 6 jobs) twice —
  `iphoneos` arm64 and `iphonesimulator` arm64 + x86_64 — and combines the two
  static libraries into `apps/ios/build/ConvertibleCore.xcframework` (git-ignored,
  with the header and a module map). It runs CMake in a clean environment so
  Xcode's build settings cannot leak into the toolchain.
- The script is the first build phase of the app target. It fingerprints the core
  sources (`core/CMakeLists.txt`, `core/cmake`, `core/include`, `core/src`) and
  returns at once when nothing changed; a change rebuilds incrementally in
  `apps/ios/build/cmake-*`. Run it by hand with `--force` to rebuild everything.
- Swift sees the C interface as the module `CConvertible`
  (`Convertible/Core/CConvertible/module.modulemap`, pointing at
  `core/include/convertible/convertible.h`). The static library is linked by path
  per SDK (`OTHER_LDFLAGS[sdk=…]`) together with `-lc++`. Linking by path rather
  than as an XCFramework dependency is what lets a clean checkout build in a single
  `xcodebuild` call (Xcode would otherwise look for the XCFramework before the
  pre-build phase has created it).

CI notes: cache `apps/ios/build/` (keyed on the hash of `core/**` and
`apps/ios/scripts/build-core.sh`) to skip the ~30 s core build. The app bundle is
at `<DerivedData>/Build/Products/<Config>-<sdk>/Convertible.app`.

## Architecture

```
Convertible/
  Core/        binding: ConvertibleCore (catalogue, detect, targets, options schema,
               convert / convertFile) and the Codable models
  Model/       view models and state: QueueModel (queue, groups, conversion lifecycle,
               cancellation, concurrency limit), TargetCatalog (ordering + search),
               OptionsForm (schema → controls), formatting, error mapping, FileStore
               (temporary files), AppSettings
  Brand/       the mark's geometry, animation.json storyboard evaluation (cubic-bezier
               easings, keyframes), intro, spinner and progress mark
  Design/      tokens (colours from the asset catalogue, type scale, radii, spacing,
               button styles, format badges)
  Platform/    haptics, Photos saving, file export
  Views/       SwiftUI: root (iPhone stack / iPad split view), home, file card,
               options form, target picker, formats browser, settings, privacy, licences
  Resources/   Info.plist, asset catalogue, privacy manifest
ConvertibleTests/  XCTest unit tests (run hosted in the app on the simulator)
ConvertibleUITests/  XCUITest end-to-end suite (drives the real app; see below)
e2e.sh         one command for the end-to-end suite
scripts/       build-core.sh, generate-assets.py
project.yml    XcodeGen spec of the committed Convertible.xcodeproj
```

- **Binding.** The C calls are synchronous; `convert`/`convertFile` are `async throws`
  functions that run the call on a background queue. Progress arrives through an
  `AsyncStream` continuation; cancelling the Swift task makes the C progress callback
  return non-zero, and the core stops at its next checkpoint (`CoreError.cancelled`).
  Every returned `char*` is released with `cvt_string_free`, every result with
  `cvt_result_free`. Files are converted with `cvt_convert_file` (the core memory-maps
  the input) under security-scoped access, straight from where the user picked them.
- **State.** `QueueModel` is an `ObservableObject` on the main actor and owns no UI
  types. It never decides what a conversion means — formats, targets and options come
  from the core at run time, so new format families appear without app changes.
- **A large catalogue.** The core knows ~1,500 formats and a source can have several
  hundred targets. The JSON models ignore unknown fields, default missing ones and
  skip an entry they cannot decode instead of dropping the whole list (`LossyArray`),
  and read additive fields such as a target's `role` (`compress`, `archive`,
  `extract`) when the core reports them. The picker and the formats browser are lazy
  `List`s whose sections are computed outside `body` (the queue publishes progress
  many times a second) and recomputed only when the search, the category filter or
  the targets change; search ranks exact id/extension, then prefixes, words and
  substrings, then popularity, and the formats browser shows the category of the
  best match first. Option ranges wider than 1,000 steps (sample rates, sizes) and
  unbounded options are typed into a clamped number field rather than a slider.
- **Files.** Imported copies (drag and drop, paste, Photos, system inbox) and results
  live under the app's temporary directory and are deleted when a file is removed and
  at launch. Nothing about files or conversions is persisted; only the three settings
  (theme, default quality, strip metadata) are stored in `UserDefaults`.

## Assets

`assets/` is used in place, not copied: the fonts, `animation.json` and the licence
texts are referenced from the project, and the app icon set links to
`assets/brand/icons/ios/AppIcon-1024.png` with a relative symlink. The colour sets in
`Resources/Assets.xcassets/Colors` are generated from `assets/brand/tokens.json`:

```sh
python3 -I apps/ios/scripts/generate-assets.py
```
