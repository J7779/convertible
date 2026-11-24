---
name: import-codec
description: Bring permissively licensed third-party codec source code into the Convertible core under core/src/imported (licence check, in-tree copy, VENDOR.json, portable C-only build through cvt_add_imported in a family.cmake, wrapper, NOTICES.md). Use only for large codecs where an in-house implementation is unrealistic, and never for anything else.
---

# Import a codec

Importing is the exception to the in-house rule (`core/README.md`, "Imported code"). Existing
examples: `core/src/imported/libavc`, `libhevc`, `kvazaar`, `libvpx`, `libaom` with
`core/src/families/codec_*/family.cmake`.

## Steps

1. **Justify it**: a large codec (H.264, HEVC, VP9, AV1, AAC, Opus, WebP, JPEG XL class) where a
   from-scratch implementation would be unrealistic or worse. Get the maintainers' agreement.
2. **Check the licence**: MIT, BSD, zlib, Apache-2.0 or public domain only. Never GPL or LGPL,
   and nothing with field-of-use or non-commercial clauses. Read every licence file in the
   source tree, including `third_party/` subdirectories, and the PATENTS file if any.
3. **Copy the source in-tree** to `core/src/imported/<name>/` from an official release tag.
   Keep the licence files and original file headers intact. Leave out build systems, tests,
   tools, assembly and anything not compiled. Do not use submodules or download at build time.
4. **Write `VENDOR.json`** next to it:
   ```json
   {"name": "<name>", "version": "<tag> (<commit>)", "license": "<SPDX>", "url": "<upstream>", "files_modified": false}
   ```
   If you patch anything, set `"files_modified": true` and list every change in
   `core/src/imported/<name>/CONVERTIBLE_CHANGES.md`.
5. **Portable build**: C-only paths (disable SIMD/assembly with the library's own switches),
   no threads unless every target has them (the web build has none), generated config headers
   written for all targets. Compile it from the owning family's `family.cmake`:
   ```cmake
   cvt_add_imported(<name>
     SOURCES src/a.c src/b.c
     INCLUDE_DIRS include
     PRIVATE_DEFINITIONS SOME_NO_ASM=1 _DEFAULT_SOURCE)
   ```
   `_DEFAULT_SOURCE` (or `_POSIX_C_SOURCE=200809L`) is needed on glibc when the code calls POSIX
   functions such as `strnlen`, because the build uses strict C11 and GCC 14 rejects implicit
   declarations. In C, an `inline` function without `static` emits no definition at `-O0`:
   make such helpers `static inline` (and record it in `CONVERTIBLE_CHANGES.md`).
6. **Wrap it**: only the family's own code includes the library; imported types never appear
   in `src/ir`, `src/engine`, `src/util` or public headers. Register decoders/encoders with
   `registry.media()` using ids from `codec/ids.hpp`.
7. **Regenerate the notices** and verify:
   ```sh
   tools/gen-notices
   tools/privacy-audit         # e-mail addresses are allowed under core/src/imported only
   cmake --preset dev && cmake --build --preset dev -j && ctest --preset dev
   tools/ci/docker-linux.sh gcc && tools/ci/docker-linux.sh clang
   ```
   Check the Android (NDK, armeabi-v7a), web (Emscripten, no threads) and MSVC builds too: CI
   runs them all. Commit `NOTICES.md` with the import.
