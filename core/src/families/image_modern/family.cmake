# image_modern: build fragment. Each part of the family that imports code keeps
# its own commented block below; append new blocks rather than editing others.

# ---------------------------------------------------------------------------
# libwebp
# Google libwebp (WebP decoder, encoder, demux, mux and animation encoder,
# libsharpyuv), used only by webp*.cpp. Plain C11, single-threaded (no
# WEBP_USE_THREAD), scalar only: HAVE_CONFIG_H selects src/webp/config.h,
# which enables no SIMD instruction set, so src/dsp/cpu.h picks the C
# implementations on every target; the SIMD source files are not imported.
# See core/src/imported/libwebp/CONVERTIBLE_CHANGES.md.
cvt_add_imported(libwebp
  C_STANDARD 11
  INCLUDE_DIRS .
  PRIVATE_DEFINITIONS HAVE_CONFIG_H NDEBUG
  SOURCES
    sharpyuv/sharpyuv.c
    sharpyuv/sharpyuv_cpu.c
    sharpyuv/sharpyuv_csp.c
    sharpyuv/sharpyuv_dsp.c
    sharpyuv/sharpyuv_gamma.c
    src/dec/alpha_dec.c
    src/dec/buffer_dec.c
    src/dec/frame_dec.c
    src/dec/idec_dec.c
    src/dec/io_dec.c
    src/dec/quant_dec.c
    src/dec/tree_dec.c
    src/dec/vp8_dec.c
    src/dec/vp8l_dec.c
    src/dec/webp_dec.c
    src/demux/anim_decode.c
    src/demux/demux.c
    src/dsp/alpha_processing.c
    src/dsp/cost.c
    src/dsp/cpu.c
    src/dsp/dec.c
    src/dsp/dec_clip_tables.c
    src/dsp/enc.c
    src/dsp/filters.c
    src/dsp/lossless.c
    src/dsp/lossless_enc.c
    src/dsp/rescaler.c
    src/dsp/ssim.c
    src/dsp/upsampling.c
    src/dsp/yuv.c
    src/enc/alpha_enc.c
    src/enc/analysis_enc.c
    src/enc/backward_references_cost_enc.c
    src/enc/backward_references_enc.c
    src/enc/config_enc.c
    src/enc/cost_enc.c
    src/enc/filter_enc.c
    src/enc/frame_enc.c
    src/enc/histogram_enc.c
    src/enc/iterator_enc.c
    src/enc/near_lossless_enc.c
    src/enc/picture_csp_enc.c
    src/enc/picture_enc.c
    src/enc/picture_psnr_enc.c
    src/enc/picture_rescale_enc.c
    src/enc/picture_tools_enc.c
    src/enc/predictor_enc.c
    src/enc/quant_enc.c
    src/enc/syntax_enc.c
    src/enc/token_enc.c
    src/enc/tree_enc.c
    src/enc/vp8l_enc.c
    src/enc/webp_enc.c
    src/mux/anim_encode.c
    src/mux/muxedit.c
    src/mux/muxinternal.c
    src/mux/muxread.c
    src/utils/bit_reader_utils.c
    src/utils/bit_writer_utils.c
    src/utils/color_cache_utils.c
    src/utils/filters_utils.c
    src/utils/huffman_encode_utils.c
    src/utils/huffman_utils.c
    src/utils/palette.c
    src/utils/quant_levels_dec_utils.c
    src/utils/quant_levels_utils.c
    src/utils/random_utils.c
    src/utils/rescaler_utils.c
    src/utils/thread_utils.c
    src/utils/utils.c
)
target_compile_definitions(convertible_core PRIVATE CVT_HAVE_LIBWEBP=1)

# libwebp is a compute-bound codec: compile it optimised even in Debug builds
# so that the test suite stays fast (sanitizer instrumentation still applies).
# Release flags are left alone; MSVC Debug keeps /RTC1, which excludes /O2.
if(NOT MSVC)
  target_compile_options(cvt_imported_libwebp PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()
# end libwebp
# ---------------------------------------------------------------------------

# openjpeg
# JPEG 2000 (jp2, j2k, jpx) through the imported OpenJPEG library: plain C11,
# no threads, no file I/O used (memory streams only). Core code includes it as
# "openjp2/openjpeg.h".
cvt_add_imported(openjpeg
  SOURCES
    openjp2/bio.c openjp2/cio.c openjp2/dwt.c openjp2/event.c openjp2/function_list.c
    openjp2/ht_dec.c openjp2/image.c openjp2/invert.c openjp2/j2k.c openjp2/jp2.c
    openjp2/mct.c openjp2/mqc.c openjp2/openjpeg.c openjp2/opj_clock.c openjp2/opj_malloc.c
    openjp2/pi.c openjp2/sparse_array.c openjp2/t1.c openjp2/t2.c openjp2/tcd.c
    openjp2/tgt.c openjp2/thread.c
  INCLUDE_DIRS .
  DEFINITIONS OPJ_STATIC
  PRIVATE_DEFINITIONS _DEFAULT_SOURCE
  C_STANDARD 11)
target_compile_definitions(convertible_core PRIVATE CVT_HAVE_OPENJPEG=1)
# The wavelet and entropy coders are compute-bound: compile them optimised even
# in Debug builds so the test suite stays fast. Release flags are left alone.
if(NOT MSVC)
  target_compile_options(cvt_imported_openjpeg PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()
# end openjpeg


# libjxl
# JPEG XL decoder + encoder (imported libjxl with private highway, brotli and
# skcms copies). Highway is forced to its portable EMU128 target (no SIMD
# intrinsics, no runtime CPU dispatch), skcms to its portable path, no threads.
# The brotli and skcms symbols and the highway namespace are renamed through a
# force-included header so they cannot clash with any other copy in the binary.
include("${CVT_CORE_DIR}/src/imported/libjxl/sources.cmake")
cvt_add_imported(libjxl
  SOURCES ${CVT_LIBJXL_SOURCES}
  INCLUDE_DIRS lib/include
  DEFINITIONS JXL_STATIC_DEFINE JXL_CMS_STATIC_DEFINE
  PRIVATE_DEFINITIONS
    NDEBUG
    JXL_INTERNAL_LIBRARY_BUILD
    JPEGXL_ENABLE_SKCMS=1
    JPEGXL_ENABLE_BOXES=1
    JPEGXL_ENABLE_TRANSCODE_JPEG=1
    HWY_COMPILE_ONLY_EMU128
    HWY_STATIC_DEFINE
    SKCMS_PORTABLE=1
    SKCMS_HAS_MUSTTAIL=0
    SKCMS_DISABLE_HSW=1
    SKCMS_DISABLE_SKX=1
    FJXL_ENABLE_NEON=0
    FJXL_ENABLE_AVX2=0
    FJXL_ENABLE_AVX512=0
  C_STANDARD 11
  CXX_STANDARD 17)
set(_cvt_libjxl_root "${CVT_CORE_DIR}/src/imported/libjxl")
# The bundled dependencies stay private to the import: their headers are not
# visible to the rest of the core.
target_include_directories(cvt_imported_libjxl SYSTEM PRIVATE
  "${_cvt_libjxl_root}"
  "${_cvt_libjxl_root}/third_party/highway"
  "${_cvt_libjxl_root}/third_party/brotli/c/include"
  "${_cvt_libjxl_root}/third_party/skcms")
if(MSVC)
  target_compile_options(cvt_imported_libjxl PRIVATE "/FI${_cvt_libjxl_root}/cvt_jxl_prefix.h")
else()
  target_compile_options(cvt_imported_libjxl PRIVATE -include "${_cvt_libjxl_root}/cvt_jxl_prefix.h")
  # Compute-bound codec: optimise it even in Debug builds so the tests stay fast.
  target_compile_options(cvt_imported_libjxl PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()
target_compile_definitions(convertible_core PRIVATE CVT_HAVE_LIBJXL=1)
unset(_cvt_libjxl_root)
# end libjxl
