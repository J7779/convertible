# libhevc in Convertible

Upstream: https://github.com/ittiam-systems/libhevc, tag `v1.8.7`
(commit `1a27996586d45083e5cd6712f83cdf17d78831c8`), Apache License 2.0
(`LICENSE`, `NOTICE`). The same code is AOSP's `external/libhevc`.

Only the decoder is used. Built by `core/src/families/codec_hevc/family.cmake`
with `ENABLE_MAIN_REXT_PROFILE` (4:0:0, 4:2:2, 4:4:4) and 10-bit decoding
enabled at run time.

## What was copied

- `common/*.h` and the portable C sources of `common/` listed in upstream
  `common/common.cmake` (8-bit and high-bit-depth paths);
- `decoder/*.h` and the C sources listed in `decoder/libhevcdec.cmake`.

Not copied: every `arm/`, `arm64/`, `x86/` and `riscv64/` directory (assembly
and SIMD intrinsics), the encoder, examples, fuzzers, tests and build files,
`common/ithread.c` (pthreads) and the encoder-only `common/ihevc_quant_iquant_ssd.c`,
`common/ihevc_resi_trans.c` and `common/ihevc_function_selector_generic.c`.

## Changes

1. New `portable/ihevc_platform_macros.h`, derived from
   `common/riscv64/ihevc_platform_macros.h`: compiler builtins
   (`__builtin_clz`, `__builtin_ctz`, `__builtin_popcount`,
   `__sync_synchronize`, `asm("nop")`, `__attribute__((aligned))`) replaced by
   portable C with identical results, plus memcpy-based unaligned load/store
   helpers (`ihevc_load_u32`, `ihevc_load_u16`, `ihevc_store_u32`,
   `ihevc_store_u16`). The stray `;` after some upstream `CLIP_*` macros was
   dropped.
2. New `portable/ithread_single.c` replaces `common/ithread.c`: the decoder
   always runs with one core, so mutexes, semaphores and condition variables
   are no-ops and thread creation reports failure (never reached). All
   `ithread_*` symbols are renamed to `cvt_ihevc_ithread_*` through
   preprocessor definitions in `family.cmake`, so another import with its
   own `ithread.c` can be linked alongside.
3. New `portable/ihevcd_function_selector.c` (modelled on
   `decoder/riscv64/ihevcd_function_selector.c`): always selects the generic
   C functions.
4. New `portable/cvt_libhevc.c` and `api/cvt_libhevc.h`: a narrow C interface
   (create with an output layout, decode, flush, reset, destroy) so that no
   libhevc header is visible outside the library.
5. Type-punned unaligned accesses replaced by the helpers from (1):
   `decoder/ihevcd_boundary_strength.c` (2 sites),
   `decoder/ihevcd_iquant_itrans_recon_ctb.c` (2), `decoder/ihevcd_process_slice.c` (1),
   `decoder/ihevcd_sao.c` (4), `decoder/ihevcd_parse_slice.c` (2). Other
   word-sized accesses at 2-byte offsets remain in the deblocking code (see
   `family.cmake`, where UBSan's alignment check is disabled for this library).

6. `decoder/ihevcd_version.c`: the version string no longer embeds
   `__DATE__`/`__TIME__` (reproducible builds), and `strnlen` (POSIX) is
   replaced by a bounded loop so the file builds as strict C99.

No algorithm was changed; decoding stays bit-exact (verified against
ffmpeg on the fixtures in `core/tests/data/codec_hevc/`).
