# Changes relative to upstream

No upstream source file is edited. The import differs from the upstream
trees in these ways:

- Only the files the decoder, encoder and CMS need are kept: `lib/jxl` (no
  tests, tools, extras, thread runners, benchmarks), `lib/include/jxl`, the
  highway headers plus `targets.cc`, `per_target.cc`, `abort.cc`, `print.cc`,
  `aligned_allocator.cc`, brotli `c/common`, `c/dec` and `c/include`, and the
  portable skcms files (`skcms.cc`, `src/skcms_TransformBaseline.cc` and their
  headers; the AVX2/AVX-512 transform files are left out).
- Added `lib/include/jxl/version.h`, `jxl_export.h`, `jxl_cms_export.h`: what
  libjxl's CMake generates for a static build of version 0.13.0.
- Added `cvt_jxl_prefix.h`, force-included into every source of the import:
  it renames every external brotli and skcms symbol to `cvt_jxl_<name>` and the
  C++ namespaces `hwy` and `skcms_private`, so this private copy cannot clash
  with another brotli/highway/skcms linked into the same binary.
- The brotli encoder (`c/enc`) is replaced by `cvt_brotli_encoder_stub.c`,
  whose `BrotliEncoderCreateInstance` fails. libjxl only uses the encoder to
  write Brotli-compressed boxes and JPEG reconstruction data, neither of which
  Convertible requests.
- Added `sources.cmake` (the compiled file list).

Build configuration (see `core/src/families/image_modern/family.cmake`):
`HWY_COMPILE_ONLY_EMU128` (highway's portable C++ target only, no intrinsics,
no runtime dispatch), `SKCMS_PORTABLE=1`, `SKCMS_HAS_MUSTTAIL=0`,
`FJXL_ENABLE_NEON/AVX2/AVX512=0` (scalar fast-lossless encoder), no threads,
`NDEBUG`, C11 / C++17.
