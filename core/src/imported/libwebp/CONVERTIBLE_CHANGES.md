# libwebp in Convertible

Upstream: <https://chromium.googlesource.com/webm/libwebp>, tag `v1.6.0`
(commit `4fa21912338357f89e4fd51cf2368325b59e9bd9`). Licence: BSD-3-Clause
(`COPYING`) plus Google's additional patent grant (`PATENTS`); contributors
are listed in `AUTHORS`.

Used by `core/src/families/image_modern/webp*.cpp` (the `webp` Image reader
and writer) and built by the `libwebp` block of
`core/src/families/image_modern/family.cmake`.

## What is here

The portable C sources and headers of `src/dec`, `src/enc`, `src/dsp`,
`src/utils`, `src/demux`, `src/mux`, `src/webp` (public headers) and
`sharpyuv`. Not imported: every SIMD source and header (`*_sse2.c`,
`*_sse41.c`, `*_avx2.c`, `*_neon.c`, `*_mips32.c`, `*_mips_dsp_r2.c`,
`*_msa.c`, `common_sse2.h`, `common_sse41.h`, `neon.h`, `msa_macro.h`,
`mips_macro.h`), the command line tools (`examples`, `imageio`, `extras`),
tests, documentation, Android/Gradle/Makefile/CMake build files, `swig` and
`webp_js`. `AUTHORS` keeps every contributor's name but not their e-mail
address; the complete file is in the upstream tag.

## Build configuration

- `HAVE_CONFIG_H` is defined for the library sources, which makes them
  include `src/webp/config.h`. That file is **not upstream**: it was written
  for Convertible and serves every target unchanged. It defines the
  `HAVE_BUILTIN_BSWAP*` macros where the compiler has the builtins (the
  headers have portable fallbacks) and `WORDS_BIGENDIAN` on big-endian hosts,
  and deliberately defines none of `WEBP_HAVE_SSE2`, `WEBP_HAVE_SSE41`,
  `WEBP_HAVE_AVX2` and `WEBP_HAVE_NEON`. With `HAVE_CONFIG_H` set,
  `src/dsp/cpu.h` enables a SIMD path only when the matching `WEBP_HAVE_*`
  macro exists, so every DSP initialiser installs the plain C functions on
  every target (x86/x86-64 including MSVC, arm64, armeabi-v7a, wasm32), and
  `WEBP_NEON_OMIT_C_CODE` stays 0.
- `WEBP_USE_THREAD` is not defined: the decoder and encoder are
  single-threaded and `src/utils/thread_utils.c` runs work synchronously.
- `NDEBUG` is defined for the library (its `assert`s check internal
  invariants and are compiled out, as in upstream release builds).
- Plain C11; optimised (`-O2`) in Debug builds too, like libaom and libvpx.

## Source changes

1. `src/dsp/cpu.h`: the MSVC Arm/Arm64 block that defines `WEBP_USE_NEON`
   unconditionally now also requires `!defined(HAVE_CONFIG_H) ||
   defined(WEBP_HAVE_NEON)`, the same condition the GCC/Clang Arm block
   already has. Without it an MSVC Arm64 build would select NEON code that
   is not imported and omit the C fallbacks.
2. `sharpyuv/sharpyuv.c` (`SharpYuvInit`): the global `SharpYuvGetCPUInfo`
   is assigned only when its value actually changes. libwebp calls this
   function on every sharp-YUV encode with the same pointer; skipping the
   redundant store removes the only write to shared state after
   initialisation, so concurrent conversions never write the same global.
3. `src/webp/config.h` added (see above).

## Thread safety

Built without threads, libwebp guards its lazily filled DSP function tables
and lookup tables (`WEBP_DSP_INIT`, the gamma tables of
`src/enc/picture_csp_enc.c`, libsharpyuv's tables, `InitGetCoeffs` in
`src/dec/vp8_dec.c`) with unsynchronised flags. The wrapper
(`webp::ensure_initialised()` in `core/src/families/image_modern/webp.cpp`)
runs every initialiser once, under a C++ function-local static, before any
libwebp call, so later calls only read the flags and tables. The remaining
file-scope variables (`src/utils/utils.c` allocation counters,
`src/utils/bit_reader_utils.c` trace state) exist only with the debugging
macros `PRINT_MEM_INFO` / `BITTRACE`, which are not defined.

## Symbols

Exported symbols are prefixed `WebP`, `VP8`, `VP8L` or `SharpYuv`, except
these internal helpers: the mux `Chunk*`/`Mux*` functions and `kChunks`,
`GetColorPalette`, `PaletteSort`, `PrepareMapToPalette`, `SearchColorNoIdx`,
`QuantizeLevels`, `IsValidColorspace`, `ReadHuffmanCodesHelper`,
`kLog2Table`, `kSLog2Table`, `kPrefixEncodeCode`,
`kPrefixEncodeExtraBitsValue`, `kVP8Log2Range`, `kVP8NewRange`. None of them
is defined anywhere else in the core at the time of import (libvpx's VP8
symbols are lower-case `vp8_*`); if another import ever collides, rename
them with a prefix-define header passed to this library only.
