# core/src/families/codec_legacy/family.cmake
#
# Theora is imported (Xiph libtheora 1.1.1, BSD-3-Clause) together with the
# bit packer of libogg 1.3.5 (BSD-3-Clause) that its encoder uses. Plain C,
# no assembly (OC_X86_ASM stays undefined), no threads, no configure step.
cvt_add_imported(libogg
  SOURCES src/bitwise.c
  INCLUDE_DIRS include)

cvt_add_imported(libtheora
  SOURCES
    lib/analyze.c lib/bitpack.c lib/decinfo.c lib/decode.c lib/dequant.c lib/encfrag.c lib/encinfo.c
    lib/encode.c lib/enquant.c lib/fdct.c lib/fragment.c lib/huffdec.c lib/huffenc.c lib/idct.c lib/info.c
    lib/internal.c lib/mathops.c lib/mcenc.c lib/quant.c lib/rate.c lib/state.c lib/tokenize.c
  INCLUDE_DIRS include ${CVT_CORE_DIR}/src/imported/libogg/include)

# libtheora shifts negative values left (strides of its bottom-up frames,
# motion vectors): two's complement arithmetic on every supported compiler
# and well defined since C23/C++20, but flagged by UBSan's "shift" check.
# AddressSanitizer and the other UBSan checks stay on for it.
if(CVT_SANITIZE AND NOT MSVC)
  target_compile_options(cvt_imported_libtheora PRIVATE -fno-sanitize=shift)
endif()
