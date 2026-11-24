# Convertible for Android

Native Android app: Kotlin + Jetpack Compose UI on top of the shared C++ core (`../../core`), linked through a
small JNI bridge. Fully offline: the app has **no permissions at all** (not even `INTERNET`).

## Prerequisites

| Tool | Version |
| --- | --- |
| JDK | 17 or newer to run Gradle (the JBR bundled with Android Studio works) |
| Android SDK platform | `platforms;android-37.0` (compileSdk / targetSdk 37) |
| Build tools | `build-tools;37.0.0` (36.x also works) |
| NDK | `ndk;27.1.12297006` |
| CMake | `cmake;3.22.1` |
| Emulator (instrumented tests only) | `emulator`, `platform-tools`, and a system image, e.g. `system-images;android-37.0;google_apis;x86_64` on Linux/KVM |

Set `ANDROID_HOME` (and `JAVA_HOME` if `java` is not on the path). Nothing else is needed: the Gradle wrapper
(Gradle 9.9.2, checksum-pinned), AGP 9.5.2, Kotlin 2.4.21 and the AndroidX libraries are downloaded by Gradle.
`local.properties` is not used and must never be committed.

## Build

```sh
cd apps/android
./gradlew --max-workers=6 assembleDebug assembleRelease
```

Outputs (one APK per ABI plus a universal one):

```
app/build/outputs/apk/debug/app-{arm64-v8a,armeabi-v7a,x86_64,universal}-debug.apk
app/build/outputs/apk/release/app-{arm64-v8a,armeabi-v7a,x86_64,universal}-release-unsigned.apk
```

Release APKs are unsigned (R8-minified, resources shrunk); sign them in the release pipeline. Debug builds use the
machine's default debug key. For faster local iteration build one ABI: `-Pconvertible.abis=arm64-v8a`.
`-Pconvertible.nativeJobs=N` bounds parallel C++ compile jobs (default 6).

## Run

```sh
./gradlew installDebug          # with a device or emulator attached
adb shell am start -n app.convertible/.MainActivity
```

## Test

```sh
# JVM unit tests (headless, no device): JSON, catalogue/schema parsing, target ranking and search, option
# resolution, size formatting, error mapping, the queue store, and the real tokens.json / animation.json.
./gradlew testDebugUnitTest

# Lint (fails on errors)
./gradlew lint

# Instrumented tests on a device/emulator: real conversions through libconvertible_jni.so (base64/hex round trips,
# checksum vectors, text re-encoding, Unicode paths, error statuses, cancel, listener exceptions, a 64 MiB file
# through the path API, direct ByteBuffers, concurrency) plus Compose UI tests including an end-to-end conversion.
./gradlew connectedDebugAndroidTest
```

Headless emulator for CI, e.g.
`emulator -avd <name> -no-window -no-audio -no-boot-anim -gpu swiftshader_indirect -no-snapshot`.
Key controls carry `testTag`s (`choose_files`, `drop_zone`, `privacy_note`, `queue`, `card_<id>`,
`pick_target_<id>`, `suggest_<id>_<format>`, `convert_<id>`, `cancel_<id>`, `save_<id>`, `share_<id>`, `convert_all`,
`save_all`, `cancel_all`, `target_picker`, `picker_search`, `target_<format>`, `nav_formats`, `nav_settings`,
`formats_search`, `format_<id>`, `theme_<n>`, `app_version`, `core_version`) for end-to-end tests.

## Architecture

```
core (C++, ../../core) ── C ABI ── app/src/main/cpp/jni_bridge.cpp  (libconvertible_jni.so)
                                        │
          app.convertible.core   NativeBridge (raw externals) → NativeCore : CoreApi (results, errors, progress)
          app.convertible.model  catalogue / targets / options-schema parsing, ranking & search, formatting
          app.convertible.state  ConverterStore (queue logic, platform-free, unit-tested), settings
          app.convertible.files  SAF / share / open-with import, cache workspace, save, FileProvider sharing
          app.convertible.ui     ViewModel, Compose screens, custom theme built from tokens.json, mark animations
```

- **Binding.** One JNI function per C call, registered in `JNI_OnLoad` (the only exported symbol). Strings cross as
  UTF-16 and are converted to standard UTF-8 in `utf.cpp` (never the modified-UTF-8 `*StringUTF*` calls, which break
  emoji and other supplementary characters); strings containing U+0000 are rejected rather than truncated. Inputs are
  file paths (`cvt_convert_file` memory-maps them) or direct `ByteBuffer`s, so file contents never pass through the
  Java heap; results are exposed as a read-only direct buffer aliasing the `cvt_result` until it is freed. The
  progress callback reuses its stage string, is throttled to ~60 Hz and returns the Kotlin listener's cancel flag; a
  listener exception cancels the core and is rethrown, then mapped to `CoreError.Bridge`. Every status maps to a
  `CoreError` subtype; `ConversionOutcome` is a sealed result.
- **Files.** Incoming files (system picker with multi-select, drag and drop, Share → Convertible, Open with) are
  copied into `cacheDir/work/in/<id>/`, converted with `cvt_convert_file` into `cacheDir/work/out/<id>/`, saved with
  `ACTION_CREATE_DOCUMENT` / a picked folder, and shared through a `FileProvider` limited to `work/out/`. The whole
  workspace is deleted when the screen is finished and again at the next start. Nothing is persisted except three
  preferences (theme, default quality, strip metadata).
- **Lifecycle.** The queue lives in a `ViewModel` and conversions run on `Dispatchers.IO` (at most two at a time) in
  `viewModelScope`, so they survive rotation and other configuration changes. No foreground service is used (it would
  require extra permissions); a conversion left running in the background may be stopped by the system.
- **Design.** Colours, type scale, radii, spacing and motion come from the packaged copy of
  `assets/brand/tokens.json`; the intro, spinner and determinate progress are drawn on a Compose `Canvas` by a generic
  evaluator of `assets/brand/animation.json` (keyframes, cubic-bezier easings, trim paths, group transforms). The
  Android 12+ splash screen shows only the background colour and hands over to the intro, which draws the mark from
  nothing; tap or any key skips it, and "Remove animations" shows the static mark. Fonts and launcher-icon layers
  (adaptive + monochrome) are copied from `assets/` at build time by the `generateBrandResources` task.

## How the core is linked

`app/src/main/cpp/CMakeLists.txt` does `add_subdirectory(<repo>/core)` (the core selects C++26/C++23 itself) and
links the static `convertible::core` into `libconvertible_jni.so` with the static C++ runtime, `--gc-sections`,
a version script exporting only `JNI_OnLoad`, and 16 KB page alignment (`-z max-page-size=16384`; APKs store the
libraries uncompressed and aligned). ABIs: arm64-v8a, armeabi-v7a, x86_64; minSdk 26.
