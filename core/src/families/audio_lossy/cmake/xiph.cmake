# audio_lossy family, Xiph area: libopus 1.5.2 (C, floating point, no
# assembly, no intrinsics, no threads, no DNN extensions) and libvorbis 1.3.7
# with the two libogg 1.3.5 sources it needs, and Speex 1.2.1. Every global symbol of these
# copies is renamed with a `cvt_` prefix through a generated header that is
# force-included (see opus_rename.h / vorbis_rename.h next to the wrappers).

set(_cvt_imp "${CVT_CORE_DIR}/src/imported")
set(_cvt_xiph "${CVT_FAMILY_DIR}/xiph")

# alloca(): <malloc.h>/_alloca on Windows (the libraries know), <alloca.h>
# everywhere else (glibc, bionic, musl, Emscripten, Apple). Without
# HAVE_ALLOCA_H they would rely on <stdlib.h> declaring it, which strict C
# modes do not.
if(NOT WIN32)
  set(_cvt_alloca_h HAVE_ALLOCA_H)
else()
  set(_cvt_alloca_h "")
endif()

function(_cvt_force_include target header)
  if(MSVC)
    target_compile_options(${target} PRIVATE "/FI${header}")
  else()
    target_compile_options(${target} PRIVATE -include "${header}")
  endif()
endfunction()

# ---- libopus
file(STRINGS "${_cvt_imp}/libopus/sources.txt" _cvt_opus_sources)
cvt_add_imported(libopus
  SOURCES ${_cvt_opus_sources}
  INCLUDE_DIRS include
  PRIVATE_DEFINITIONS OPUS_BUILD USE_ALLOCA NDEBUG ${_cvt_alloca_h})
target_include_directories(cvt_imported_libopus PRIVATE
  "${_cvt_imp}/libopus/celt" "${_cvt_imp}/libopus/silk" "${_cvt_imp}/libopus/silk/float" "${_cvt_imp}/libopus/src")
_cvt_force_include(cvt_imported_libopus "${_cvt_xiph}/opus_rename.h")
target_compile_definitions(convertible_core PRIVATE CVT_HAVE_LIBOPUS=1)

# ---- libvorbis + libogg (one object library)
file(STRINGS "${_cvt_imp}/libvorbis/sources.txt" _cvt_vorbis_sources)
file(STRINGS "${_cvt_imp}/libogg/sources.txt" _cvt_ogg_sources)
list(TRANSFORM _cvt_ogg_sources PREPEND "${_cvt_imp}/libogg/")
cvt_add_imported(libvorbis
  SOURCES ${_cvt_vorbis_sources} ${_cvt_ogg_sources}
  INCLUDE_DIRS include "${_cvt_imp}/libogg/include"
  PRIVATE_DEFINITIONS NDEBUG ${_cvt_alloca_h})
target_include_directories(cvt_imported_libvorbis PRIVATE "${_cvt_imp}/libvorbis/lib")
_cvt_force_include(cvt_imported_libvorbis "${_cvt_xiph}/vorbis_rename.h")
target_compile_definitions(convertible_core PRIVATE CVT_HAVE_LIBVORBIS=1)
if(CVT_SANITIZE AND NOT MSVC)
  # libogg's bit packer left-shifts into the sign bit (two's complement);
  # libvorbis' psychoacoustic setup reads one element past a row of a
  # two-dimensional parameter array (inside its struct, weight zero).
  target_compile_options(cvt_imported_libvorbis PRIVATE -fno-sanitize=shift-base,bounds)
endif()

# ---- Speex (floating point)
file(STRINGS "${_cvt_imp}/speex/sources.txt" _cvt_speex_sources)
cvt_add_imported(speex
  SOURCES ${_cvt_speex_sources}
  INCLUDE_DIRS include
  PRIVATE_DEFINITIONS FLOATING_POINT USE_SMALLFT EXPORT= NDEBUG ${_cvt_alloca_h})
target_include_directories(cvt_imported_speex PRIVATE "${_cvt_imp}/speex/libspeex")
_cvt_force_include(cvt_imported_speex "${_cvt_xiph}/speex_rename.h")
target_compile_definitions(convertible_core PRIVATE CVT_HAVE_SPEEX=1)

# libogg's directory carries its own licence; cvt_add_imported only checks
# the libvorbis one, so make sure the libogg copy is complete too.
if(NOT EXISTS "${_cvt_imp}/libogg/COPYING" OR NOT EXISTS "${_cvt_imp}/libogg/VENDOR.json")
  message(FATAL_ERROR "core/src/imported/libogg is incomplete")
endif()
