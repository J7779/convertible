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
  -destination 'platform=iOS Simulator,name=iPhone 18 Pro' CODE_SIGNING_ALLOWED=NO

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
