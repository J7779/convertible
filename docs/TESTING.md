# Testing

Convertible is tested at three levels: the core's unit and real-file tests (the bulk), each
app's own unit tests, and end-to-end runs of the real apps. Everything runs headless and
offline, and everything CI runs can be run locally.

Contents: [Core](#core) · [Real-file policy](#the-real-file-policy) ·
[Sanitizers](#sanitizers) · [Apps](#apps) · [Rehearsing CI locally](#rehearsing-ci-locally) ·
[Repository checks](#repository-checks)

## Core

```sh
cmake --preset dev && cmake --build --preset dev -j && ctest --preset dev
```

CTest runs two entries: `convertible_tests` (one executable with every unit, format and
engine test, built on the in-house framework in `core/tests/support/test.hpp`) and
`convertible_cli_smoke` (the CLI end to end, `core/tests/cli_smoke.cmake`). Run a subset while
working:

```sh
build/dev/core/convertible_tests --test-case="image/png*"           # by name, wildcards, comma lists
build/dev/core/convertible_tests --source-file="*families/encoding*" # by file
build/dev/core/convertible_tests --list-test-cases
build/dev/core/convertible_tests --durations                        # slowest cases
```

Writing tests, helpers and metrics: [`core/tests/README.md`](../core/tests/README.md).

## The real-file policy

Every format is tested against **real files**, not only against our own encoder:

1. **Decode** fixtures written by independent, widely used software (ffmpeg, ImageMagick,
   LibreOffice, Python's standard library, `zip`, ...) and compare with known content:
   exact pixels, samples, cells or text for lossless formats, PSNR/SSIM/SNR thresholds for
   lossy ones.
2. **Round-trip** generated input through our writer and reader, with a metric threshold per
   quality level and a check that lower quality gives smaller output.
3. **Reject hostile input**: truncations and corruptions (`test::fuzz_reader`) and hand-made
   bombs against the limits must return an error, never crash, hang or allocate without bound.

Fixtures live in `core/tests/data/<family>/`, are tiny, synthetic and free of personal or
machine data, and each one is documented in that directory's `SOURCES.md` with the tool,
its version and the exact command. They are bytes: `.gitattributes` marks them `-text`, so no
platform ever rewrites their line endings. See [`core/tests/data/README.md`](../core/tests/data/README.md)
and the `make-fixtures` skill.

## Sanitizers

```sh
cmake --preset asan && cmake --build --preset asan -j && ctest --preset asan
```

AddressSanitizer and UndefinedBehaviorSanitizer cover the core, the tests and imported code
(`ASAN_OPTIONS` and `UBSAN_OPTIONS` come from the test preset). CI runs this preset with
Clang 18 and libc++ on Linux. Ubuntu's shared libc++ 18 allocates and frees the message of
`std::runtime_error` with mismatched functions, which ASan reports as an
alloc-dealloc-mismatch inside the C++ runtime; that job therefore adds
`alloc_dealloc_mismatch=0` through `CVT_ASAN_EXTRA_OPTIONS` (see `tools/ci/core.sh`). MSVC builds
have no sanitizer job.

## Apps

| App | Unit tests | End-to-end |
| --- | --- | --- |
| Desktop | `ctest --test-dir build/desktop -L desktop -C Release --output-on-failure`: `desktop_unit_tests` and `desktop_automation_smoke` (the real executable in headless automation mode) | label `desktop-e2e` when registered; `tools/ci/desktop.sh` runs it (under Xvfb on Linux) |
| Web | `apps/web/test.sh` (Node's built-in runner, no packages; real conversions through `convertible.wasm`) | `apps/web/e2e.sh` against a local Chrome, when present |
| Android | `./gradlew testDebugUnitTest lint` (JVM, headless) | `./gradlew connectedDebugAndroidTest` on a device or emulator, plus `apps/android/e2e.sh` when present |
| iOS | `xcodebuild test -project apps/ios/Convertible.xcodeproj -scheme Convertible -destination 'platform=iOS Simulator,name=iPhone 18 Pro' CODE_SIGNING_ALLOWED=NO` | XCUITest identifiers are listed in `apps/ios/README.md` |

Android instrumented tests need an emulator. `tools/ci/android-emulator.sh up` installs the
system image, creates an AVD and boots it headless with the SDK's own tools (Linux needs
`/dev/kvm`); `tools/ci/android-emulator.sh shutdown` stops it. CI runs these nightly and on
demand (`workflow_dispatch` with *android_device_tests*).

iOS is tested locally only: CI has no iOS job because shipping builds need the owner's
signing certificate.

## Rehearsing CI locally

`tools/ci/docker-linux.sh` runs the Linux jobs in throwaway `ubuntu:24.04` containers (the
repository is mounted read-only and copied in, so host builds are untouched). Packages are
installed by `tools/ci/install-linux-deps.sh`, the script the workflows use.

```sh
tools/ci/docker-linux.sh gcc       # core with GCC 14 (dev preset) + formats doc check
tools/ci/docker-linux.sh clang     # core with Clang 18 + libc++, ASan + UBSan
tools/ci/docker-linux.sh desktop   # desktop build, tests, packages
tools/ci/docker-linux.sh hygiene   # privacy audit, skills, notices, YAML parse
tools/ci/docker-linux.sh mingw     # MinGW-w64 cross build of core + CLI (debian:trixie, GCC 14)
apps/web/build.sh --docker && apps/web/test.sh   # the web job
```

Differences from GitHub's runners: a minimal image rather than the full runner image, the
distribution's CMake (3.28) rather than the runner's newer one, no compiler cache, and memory
capped with `--memory` (default 6 GB, `CVT_DOCKER_MEMORY`). Ubuntu 24.04's own MinGW is GCC 13,
which cannot compile the core, hence Debian trixie for the MinGW smoke check. macOS jobs are
rehearsed by running `tools/ci/core.sh dev` and `tools/ci/desktop.sh` on a Mac; Windows/MSVC
can only be exercised on Windows or in CI.

Reading a failed CI run: `gh run view <run-id> --log-failed`; see the `ci-triage` skill.

## Repository checks

Run these before every push (CI's hygiene job runs them too):

```sh
tools/privacy-audit --history       # personal data, machine details, secrets, commit identities
tools/sync-skills --check           # .claude/skills mirrors .agents/skills
tools/gen-notices --check           # NOTICES.md matches core/src/imported and assets/fonts
tools/gen-formats-doc --check       # docs/FORMATS.md and the README table match the built CLI
```
