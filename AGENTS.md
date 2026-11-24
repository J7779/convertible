# Working on Convertible

Guidance for anyone - a new contributor or an automated coding agent - who changes this
repository. Read it once fully; it is short on purpose and points to the detailed guides.
Step-by-step procedures for common tasks are in [`.agents/skills/`](.agents/skills/).

## Project map

```
core/                    the engine: one C++23 static library + CLI + tests (core/README.md)
  include/convertible/   convertible.h - the C ABI, the only header apps use
  src/engine|ir|codec|util/   shared, frozen interfaces (do not edit from a family branch)
  src/families/<family>/ one directory per format family (readers, writers, bridges)
  src/imported/<name>/   imported permissive codecs with VENDOR.json and licence
  cli/main.cpp           the `convertible` tool
  tests/                 in-house test framework, families/<family>/, data/<family>/ fixtures
apps/web/                WebAssembly + offline PWA, no npm packages (apps/web/README.md)
apps/ios/                SwiftUI app, Xcode project, core built by scripts/build-core.sh
apps/android/            Kotlin/Compose app, JNI bridge, Gradle
apps/desktop/            Windows/Linux/macOS app with an in-house UI toolkit
assets/brand, assets/fonts   logo, icons, tokens, motion, Inter + JetBrains Mono NL
docs/                    ARCHITECTURE, BUILDING, TESTING, PRIVACY, FORMATS (generated)
tools/                   gen-formats-doc, gen-notices, privacy-audit, sync-skills, ci/*
.github/                 workflows (ci, release, pages) and the setup-android action
.agents/skills/          task skills; .claude/skills/ is a generated mirror
```

## Golden rules

1. **In-house first.** Formats, parsers, codecs, UI toolkits and tools are written here,
   against specifications. No libraries, package managers, submodules, `FetchContent`,
   `ExternalProject`, `find_package`, npm/pip packages or CDN code in the product. The only
   exception is imported source for large codecs, under the conditions in
   `core/README.md` ("Imported code"): permissive licence (MIT, BSD, zlib, Apache-2.0, public
   domain; never GPL/LGPL), in-tree under `core/src/imported/<name>/` with `VENDOR.json` and
   the licence, compiled only through `cvt_add_imported()`, used only behind a family wrapper,
   listed in `NOTICES.md` (`tools/gen-notices`). CI uses only GitHub's own actions, pinned to
   commit SHAs.
2. **Stay inside your ownership boundary.** A family edits only its own
   `src/families/<family>/`, `tests/families/<family>/`, `tests/data/<family>/`, its IR header
   additively, and appends single lines to `codec/ids.hpp` / `engine/known_formats.cpp`.
   App work stays in `apps/<app>/`. Shared interfaces (`include/`, `src/engine`, `src/ir/kind.hpp`,
   `src/ir/any.hpp`, `src/codec/*.hpp`, `src/util`, `tests/support`, CMake files) change only
   by agreement with the maintainers. Repository tooling lives in `tools/`, `docs/`, `.github/`.
3. **Offline and private.** No sockets, telemetry, analytics, accounts, child processes or
   reading of files other than those handed in. No dependence on locale, time zone, clock or
   random seeds: output is byte-identical everywhere. `strip_metadata` defaults to true and
   writers never invent identifying metadata.
4. **No personal or machine data, anywhere.** No absolute paths, user names, real names
   (beyond `LICENSE`), host names, e-mail addresses, keys, `local.properties` or signing
   material in code, comments, fixtures, docs or commit messages. Commits use the
   repository's configured identity (a GitHub noreply address), plain messages, and no
   co-author, "generated with" or tool-attribution trailers. Do not mention the tools used
   to write a change. Run `tools/privacy-audit --history` before every push.
5. **Real-file tests.** Every format is tested by decoding fixtures written by independent
   software, by round trips with quality thresholds, and by hostile input under ASan.
   Fixtures are tiny, synthetic, scrubbed and documented in `SOURCES.md` (tool, version,
   exact command). Never use our own encoder's output as a decode fixture.
6. **Portability.** Code must build warning-free (`-Wall -Wextra -Wpedantic`, MSVC `/W4`) on
   the whole matrix: GCC 14, Clang 18 + libc++ 18, Apple Clang 21, MSVC 2022 17.10,
   Emscripten (32-bit `size_t`, no threads), Android NDK r27 (also 32-bit armeabi-v7a), iOS.
   Pitfalls seen in practice:
   - C++ library features missing somewhere: `std::print`, `std::generator`, `std::flat_map`,
     `std::views::zip`/`enumerate`, `std::ranges::fold_left`, `std::move_only_function`,
     floating-point `std::from_chars` (use `util::parse_double`), modules.
   - Size arithmetic in `std::uint64_t`, checked before narrowing (WASM, armeabi-v7a).
   - Imported C is compiled as strict C11: POSIX functions such as `strnlen` need
     `_DEFAULT_SOURCE`/`_POSIX_C_SOURCE` on glibc, and GCC 14 rejects implicit declarations.
   - Windows: UTF-8 paths through `util::fs`, `wmain`, CRLF checkouts (fixtures are `-text`),
     long paths, `NOMINMAX`; MinGW needs GCC 14 (Debian trixie), not Ubuntu 24.04's GCC 13.
   - No SIMD intrinsics without a portable fallback; no unaligned pointer casts.
7. **Generated files are generated.** `docs/FORMATS.md`, the README format table,
   `NOTICES.md` and `.claude/skills/` are written by tools; edit the source and regenerate.

## Commands

Core (from the repository root):

```sh
cmake --preset dev && cmake --build --preset dev -j && ctest --preset dev
cmake --preset asan && cmake --build --preset asan -j && ctest --preset asan
build/dev/core/convertible_tests --test-case="image/png*"
build/dev/core/convertible formats --json
```

Desktop:

```sh
cmake -S . -B build/desktop -DCVT_BUILD_DESKTOP=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/desktop -j 6 --config Release
ctest --test-dir build/desktop -L desktop -C Release --output-on-failure
cmake --build build/desktop --config Release --target package
```

Web, Android, iOS:

```sh
apps/web/build.sh --docker && apps/web/test.sh
(cd apps/android && ./gradlew --max-workers=4 assembleDebug assembleRelease testDebugUnitTest lint)
xcodebuild test -project apps/ios/Convertible.xcodeproj -scheme Convertible \
  -destination 'platform=iOS Simulator,name=iPhone 18 Pro' CODE_SIGNING_ALLOWED=NO
```

Linux CI jobs in Docker (nothing installed on the host):

```sh
tools/ci/docker-linux.sh gcc | clang | desktop | hygiene | mingw
```

Repository checks:

```sh
tools/privacy-audit --history
tools/sync-skills --check
tools/gen-notices --check
tools/gen-formats-doc --check        # after building the CLI
```

## Common tasks

- **Add a format to an existing family**: reader/writer/options in
  `core/src/families/<family>/`, registration in `<family>_family.cpp`, tests in
  `core/tests/families/<family>/`, fixtures + `SOURCES.md` in `core/tests/data/<family>/`, then
  `tools/gen-formats-doc`. Worked example: `core/README.md` ("Adding a format"). Skill:
  `add-format`.
- **Add a family**: a new directory with `register_<family>_family`, hooked into
  `families.hpp`/`register_all.cpp` by the maintainers. Skill: `add-format-family`.
- **Import a codec**: licence check, in-tree copy, `VENDOR.json`, C-only portable build via
  `family.cmake`, `tools/gen-notices`. Skill: `import-codec`.
- **Fixtures**: generated with independent tools in a container, scrubbed, documented.
  Skill: `make-fixtures`.
- **Release**: bump `VERSION` in the root `CMakeLists.txt`, tag `v<version>`. Skill: `release`.

## Definition of done

A change is done when:

- it builds warning-free and its tests pass with `dev` and `asan` on your platform, and the
  Linux jobs pass in Docker (`tools/ci/docker-linux.sh gcc` and `clang`) when you touched the
  core;
- new formats have real-file, round-trip, hostile-input, option and integration tests, with
  documented fixtures;
- app changes pass that app's unit tests (and end-to-end suite where one exists);
- generated files are regenerated (`tools/gen-formats-doc`, `tools/gen-notices`,
  `tools/sync-skills`) and `tools/privacy-audit --history` is clean;
- docs that describe the changed behaviour are updated;
- the commit is focused, with a plain descriptive message and no trailers.

## Skills

Task procedures, one directory each, under `.agents/skills/<name>/SKILL.md` (mirrored to
`.claude/skills/` by `tools/sync-skills`; edit only `.agents/skills/`):

| Skill | Use it to |
| --- | --- |
| `add-format` | add a reader/writer to an existing family |
| `add-format-family` | start a new format family |
| `import-codec` | bring in permissively licensed codec source |
| `run-tests` | run core, sanitizer and app test suites |
| `verify-linux` | rehearse GCC/Clang/desktop/MinGW builds in Docker |
| `make-fixtures` | produce and document test fixtures |
| `privacy-audit` | check tree and history before pushing |
| `release` | cut a release and verify its artefacts |
| `update-brand-assets` | change logos, icons, tokens or motion |
| `ci-triage` | diagnose a failing workflow and reproduce it locally |
