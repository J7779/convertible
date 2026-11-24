# Kvazaar in Convertible

Upstream: https://github.com/ultravideo/kvazaar, tag `v2.3.2`
(commit `6040962bed5cc68c5ad01234c38c08b8b2822068`), BSD 3-Clause licence
(`LICENSE`; contributors in `CREDITS`). `src/extras/libmd5.c` is public
domain (see its header).

Built by `core/src/families/codec_hevc/family.cmake` as an 8-bit encoder
(`KVZ_BIT_DEPTH=8`) with `KVZ_C_ONLY`, `KVZ_STATIC_LIB` and
`KVZ_EXTERNAL_STRATEGY_INIT`.

## What was copied

`src/*.c`, `src/*.h`, `src/strategies/*.[ch]`, `src/strategies/generic/*`,
the headers (only) of `src/strategies/{avx2,sse2,sse41,altivec}`,
`src/extras/libmd5.[ch]` and `src/extras/crypto.h`.

Not copied: the command-line tool (`encmain.c`, `cli.[ch]`, `yuv_io.[ch]`),
the SIMD strategy sources, `src/threadwrapper` (C++ std::thread wrapper),
`src/extras/crypto.cpp`, `getopt`, tests, tools and build files.

## Changes

1. Threads: every `pthread_` identifier in the library sources was renamed
   to `kvz_pthread_` (mechanically) and `#include <pthread.h>` removed
   (also the quoted `#include "pthread.h"` of `rate_control.c`, which only
   compiled where a system pthread.h exists, never with MSVC);
   `src/threads.h` is rewritten to define those names, `kvz_sem_*` and the
   atomics as single-threaded no-ops, and the clock macros as constants.
   The wrapper always configures `threads = 0` and `owf = 0`, where Kvazaar
   runs every job in the calling thread (`kvz_threadqueue_submit`). Files
   touched by the rename: `encoder_state-ctors_dtors.c`, `encoderstate.[ch]`,
   `rate_control.[ch]`, `rdo.c`, `threadqueue.[ch]`, `threads.h`;
   `threadqueue.h` and `rate_control.h` include `threads.h`.
2. `src/global.h`: `KVZ_C_ONLY` turns off `COMPILE_INTEL`, `COMPILE_POWERPC`
   and `COMPILE_ARM`, so no intrinsics or CPU probing are compiled.
3. `src/kvazaar.c`: with `KVZ_EXTERNAL_STRATEGY_INIT`, `kvazaar_open` no longer
   (re)initialises the process-wide strategy function pointers; the
   Convertible wrapper calls `kvz_strategyselector_init` exactly once
   (`std::call_once`) before the first encoder is opened, so concurrent
   encoders never write shared state.
4. `src/version.h` is pre-generated from `version.h.in` with an empty compiler
   string and build date.
5. New `portable/simd_strategy_stubs.c`: the registration functions of the
   SIMD strategies, which in a C-only build would compile to these empty
   bodies anyway.
6. New `portable/cvt_kvazaar.c` and `api/cvt_kvazaar.h`: a narrow C interface
   (open with parameters, parameter sets, encode/drain one picture,
   close). It disables logging to stderr, the encoder-information SEI and the
   picture hash SEI.

No algorithm was changed.
