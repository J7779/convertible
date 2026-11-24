# libvpx in Convertible

Upstream: <https://chromium.googlesource.com/webm/libvpx>, tag `v1.16.0`.
Licence: BSD-3-Clause (`LICENSE`) plus the WebM project's patent grant
(`PATENTS`); contributors are listed in `AUTHORS`.

## What is here

Only what a generic, C-only build of the VP8 and VP9 decoders and encoders
compiles: the `.c` files listed in
`core/src/families/codec_vpx/family.cmake` and the headers they include.
Not imported: assembly, SIMD (x86, Arm, MIPS, PowerPC, LoongArch), the
post-processor, the VP9 real-time rate control library (C++), examples,
tools, tests, documentation and the build system. `vpx_util/vpx_write_yuv_frame.c`
(a debugging aid that writes to a `FILE*`) is not built and was removed;
its header remains because other files include it.

No upstream source file was modified. `AUTHORS` keeps every contributor's
name but not their e-mail address (the repository holds no e-mail
addresses); the complete file is in the upstream tag.

When the library is built with sanitizers, `family.cmake` turns off UBSan's
signed-integer-overflow check for these sources only: on corrupt streams the
C inverse transforms (for example `iadst4_c`) multiply out-of-range
coefficients in `int`, which wraps on every supported target and is then
rejected or concealed by the decoder. AddressSanitizer and the other UBSan
checks stay on.

## Generated configuration (`config/`)

`vpx_config.h`, `vpx_version.h`, `vp8_rtcd.h`, `vp9_rtcd.h`, `vpx_dsp_rtcd.h`
and `vpx_scale_rtcd.h` were produced by the upstream build in a throwaway
Linux container:

```sh
configure --target=generic-gnu --disable-multithread --enable-vp9-highbitdepth \
  --disable-examples --disable-tools --disable-docs --disable-unit-tests \
  --disable-webm-io --disable-libyuv --enable-static --disable-shared \
  --disable-postproc --disable-vp9-postproc --disable-runtime-cpu-detect \
  --disable-install-docs --disable-install-bins --disable-install-libs \
  --disable-install-srcs --enable-vp8 --enable-vp9 --disable-debug \
  --size-limit=16384x16384
make
```

The `*_rtcd.h` headers and `vpx_version.h` are used unchanged (every DSP
function maps to its `_c` implementation). `vpx_config.h` was then made
host-neutral so the same file serves Clang, GCC, MSVC, the Android NDK and
Emscripten, on 32- and 64-bit little-endian targets:

- `HAVE_PTHREAD_H 0`, `HAVE_UNISTD_H 0` (threads are disabled anyway;
  nothing includes `pthread.h` or `unistd.h`);
- `CONFIG_OS_SUPPORT 0` (no `sys/time.h` / `windows.h` timers; encoder
  timing statistics read as zero, which also keeps output deterministic);
- `CONFIG_GCC 0`, `CONFIG_PIC 0`, `CONFIG_DEPENDENCY_TRACKING 0` (build-system
  facts that no source reads);
- an `#error` guard for big-endian targets, which this configuration does not
  describe.

`config/vpx_config.c` holds `vpx_codec_build_config()` with the configure
arguments above (no host paths).
