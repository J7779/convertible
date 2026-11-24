# Changes made to libavc for Convertible

Upstream: https://github.com/ittiam-systems/libavc, tag `v1.9.0`
(commit `40a5577c961b687aba7a6201728827500503cab4`), the H.264 decoder and
encoder of the Android Open Source Project, developed by Ittiam Systems.
Licence: Apache-2.0 (`LICENSE`, `NOTICE`).

## What was copied

- `common/*.c`, `common/*.h`, `decoder/*.c`, `decoder/*.h`, `encoder/*.c`,
  `encoder/*.h` from the upstream tree, unchanged except for the three files
  listed under "Patched files" below.
- Not copied: the assembly and SIMD directories (`arm/`, `armv8/`, `x86/`,
  `riscv/` except the files below), the SVC and MVC codecs (`*/svc`, `*/mvc`),
  build files, examples, fuzzers and tests.

## Portable C platform layer (`*/generic/`)

libavc selects its leaf functions per architecture. Convertible builds the
plain C implementations on every target, so the generic glue comes from the
upstream `riscv/` directories, which already use only C functions:

- `decoder/generic/ih264d_function_selector.c` = upstream
  `decoder/riscv/ih264d_function_selector.c`, unchanged.
- `encoder/generic/ih264e_function_selector.c`, `ih264e_platform_macros.h`,
  `ime_platform_macros.h` = upstream `encoder/riscv/*`, unchanged.
- `common/generic/ih264_platform_macros.h` = upstream
  `common/riscv/ih264_platform_macros.h`, modified so that it compiles with
  every C compiler (MSVC included): `CLZ`/`CTZ` and `ITT_BIG_ENDIAN` are
  written in plain C instead of `__builtin_clz`/`__builtin_ctz`/
  `__builtin_bswap32`; `NOP()` and `DATA_SYNC()` (spin-wait padding and
  memory barrier, only meaningful with several threads) are empty;
  `MEM_ALIGN*` use `__declspec(align)` under MSVC; `INLINE` is `__inline`.

## Patched files

- `decoder/ih264d_dpb_mgr.c`: the Android-only logging block. Upstream defines
  `inline int android_errorWriteLog(...)` (C99 `inline` without `static`: no
  external definition is emitted, so Android Debug builds failed to link) and
  logs through `__android_log_print` (needs liblog). It is now a
  `static inline` no-op and `ALOGE` expands to nothing; `<android/log.h>` is
  no longer included. Decoding is unchanged.
- No source edit is needed for glibc: `family.cmake` defines `_DEFAULT_SOURCE`
  so that `strnlen` and friends are declared under `-std=c99`/`c11`.

- `common/ithread.c`: replaced, see below.
- `encoder/ih264e_defs.h`: `MAX_WD` / `MAX_HT` raised from 2560 to 4096 so
  that UHD (3840x2160) and 4096-wide video can be encoded (the decoder already
  accepts 4096). Tested with a 3840x2160 encode/decode round trip.
- `encoder/ih264e_cabac.c` (`ih264e_cabac_flush`): the final-byte expression
  shifted `1` by -1 when seven bits were left (undefined behaviour, found by
  UBSan). It now uses 0 for that term, which is what x86 and ARM computed for
  the masked byte anyway, so the output is unchanged.

## Single-threaded `common/ithread.c`

The upstream file wraps pthreads. Convertible always configures the codec
with one core and without a persistent thread pool, so no thread is ever
needed; the replacement implements the same API with no-op locks and an
`ithread_create` that fails (and an empty `ithread_set_name`). This removes the pthread dependency (Windows,
WebAssembly without threads). The functions are renamed at compile time to
`cvt_avc_ithread_*` (see `core/src/families/codec_h264/family.cmake`) so
that they cannot clash with other Ittiam codecs.

## Build notes

- Compiled with `NDEBUG` so that upstream `assert()`s never abort on damaged
  input; the code checks and reports those conditions itself.
- In sanitizer builds the UBSan checks `pointer-overflow`, `shift-base` and
  `alignment` are disabled for this library only (null "unused plane" pointer
  arithmetic, left shifts of negative coefficients, 4-byte aligned encoder
  bookkeeping structs); ASan and all other UBSan checks stay on.
