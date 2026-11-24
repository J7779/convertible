---
name: run-tests
description: Run Convertible's test suites - core unit and real-file tests with the dev and asan presets, a subset of test cases, the desktop tests, the web Node tests, the Android JVM and instrumented tests, and the iOS simulator tests. Use before handing in any change or when asked whether something passes.
---

# Run the tests

All commands run from the repository root unless noted. Details: `docs/TESTING.md`.

## Core

```sh
cmake --preset dev && cmake --build --preset dev -j && ctest --preset dev
build/dev/core/convertible_tests --test-case="image/png*"           # one area while iterating
build/dev/core/convertible_tests --source-file="*families/table*"
build/dev/core/convertible_tests --list-test-cases
```

Sanitizers (required for anything that parses input):

```sh
cmake --preset asan && cmake --build --preset asan -j && ctest --preset asan
```

`ctest --preset dev` prints the failing test cases at the end of the `convertible_tests`
output ("failed test cases:"). Re-run one with `--test-case="<exact name>"` to see all its
assertions.

## Desktop

```sh
cmake -S . -B build/desktop -DCVT_BUILD_DESKTOP=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/desktop -j 6 --config Release
ctest --test-dir build/desktop -L desktop -C Release --output-on-failure
```

`tools/ci/desktop.sh` does the same and also runs the `desktop-e2e` label when it exists
(under `xvfb-run` on Linux) and packages the app.

## Web

```sh
apps/web/build.sh --docker
apps/web/test.sh
[ -f apps/web/e2e.sh ] && apps/web/e2e.sh     # end-to-end suite, when present
```

## Android

```sh
cd apps/android
./gradlew --max-workers=4 testDebugUnitTest lint
tools/ci/android-emulator.sh up                # from the repository root; needs ANDROID_HOME
./gradlew -Pconvertible.abis=x86_64 connectedDebugAndroidTest   # arm64-v8a on Apple silicon
```

## iOS (macOS with Xcode)

```sh
xcodebuild test -project apps/ios/Convertible.xcodeproj -scheme Convertible \
  -destination 'platform=iOS Simulator,name=iPhone 18 Pro' CODE_SIGNING_ALLOWED=NO
```

Use any installed simulator name (`xcrun simctl list devices available`).

## Linux and the CI matrix

On macOS or Windows, rehearse the Linux jobs in Docker with the `verify-linux` skill. Before
pushing, run the repository checks: `tools/privacy-audit --history`, `tools/sync-skills
--check`, `tools/gen-notices --check`, `tools/gen-formats-doc --check`.
