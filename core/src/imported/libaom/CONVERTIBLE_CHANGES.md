# libaom in Convertible

Upstream: https://aomedia.googlesource.com/aom, tag `v3.15.3`
(commit b141d9c45db236335351c8a4b2a14b3ade27c937). Licences: `LICENSE`
(BSD 2-Clause), `PATENTS` (Alliance for Open Media Patent License 1.0),
`third_party/fastfeat/LICENSE` (BSD 3-Clause), `third_party/vector/LICENSE`
(MIT).

## What is here

Only the library sources compiled into `libaom` for the generic (C-only)
target, and the headers they include. Tests, tools, examples, apps,
documentation, the build system, and all architecture-specific code
(x86/Arm/PPC/RISC-V intrinsics and assembly) were left out. The list of
compiled sources is `core/src/families/codec_av1/libaom_sources.cmake`.

No upstream source file was edited.

## Generated files (`config/`)

Upstream generates these at configure time with CMake and Perl. They were
produced once by running the upstream CMake configure in a scratch directory:

    cmake <aom> -DAOM_TARGET_CPU=generic -DCONFIG_MULTITHREAD=0 \
      -DCONFIG_RUNTIME_CPU_DETECT=0 -DCONFIG_LIBYUV=0 -DCONFIG_WEBM_IO=0 \
      -DCONFIG_DENOISE=0 -DCONFIG_PIC=1 -DENABLE_DOCS=0 -DENABLE_EXAMPLES=0 \
      -DENABLE_TESTS=0 -DENABLE_TOOLS=0 -DENABLE_APPS=0

- `aom_dsp_rtcd.h`, `av1_rtcd.h`, `aom_scale_rtcd.h`, `aom_version.h`: as generated.
  With the generic target every run-time dispatched function maps to its `_c`
  version, so the headers are identical for every CPU, word size and compiler.
- `aom_config.h`: as generated, then made host-independent: `CONFIG_GCC`,
  `HAVE_PTHREAD_H`, `HAVE_PTHREAD_SETNAME_NP`, `HAVE_UNISTD_H` set to 0 (the
  library code does not use them when multithreading is off), and
  `CONFIG_OS_SUPPORT` set to 0 (only the internal statistics timers use it;
  this avoids `<windows.h>`/`<sys/time.h>` and keeps output deterministic).
- `aom_config.c`: written by hand in the generated form, with a fixed
  configuration string instead of the configure command line.

## Thread safety

With `CONFIG_MULTITHREAD=0`, upstream's `aom_once()` does not synchronise.
The Convertible wrapper (`families/codec_av1/libaom_glue.cpp`) runs every
one-time initialiser once under a C++ function-local static before any codec
instance is created, so concurrent conversions never race on them.
