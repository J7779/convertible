# audio_lossy family, AAC area: the Android "PacketVideo" AAC decoder
# (AAC-LC, HE-AAC v1/v2; Apache-2.0) and the VisualOn AAC-LC encoder
# (vo-aacenc 0.1.3; Apache-2.0), both portable C/C++ without assembly.
# Every global symbol gets a private prefix through a force-included header.

set(_cvt_imp "${CVT_CORE_DIR}/src/imported")
set(_cvt_aac "${CVT_FAMILY_DIR}/aac")

# ---- PacketVideo AAC decoder (C++ sources)
file(STRINGS "${_cvt_imp}/pv-aacdec/sources.txt" _cvt_pvaac_sources)
cvt_add_imported(pv-aacdec
  SOURCES ${_cvt_pvaac_sources}
  INCLUDE_DIRS .
  PRIVATE_DEFINITIONS AAC_PLUS HQ_SBR NDEBUG
  CXX_STANDARD 17)
if(MSVC)
  target_compile_options(cvt_imported_pv_aacdec PRIVATE "/FI${_cvt_aac}/pv_aacdec_rename.h")
else()
  # The tables initialise 32-bit integers with unsigned hexadecimal constants.
  target_compile_options(cvt_imported_pv_aacdec PRIVATE -include "${_cvt_aac}/pv_aacdec_rename.h"
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-c++11-narrowing> $<$<CXX_COMPILER_ID:GNU>:-Wno-narrowing>)
endif()
if(CVT_SANITIZE AND NOT MSVC)
  # Fixed-point code that shifts negative values and, in the SBR envelope
  # code, by more than 31 places (the result is discarded). The inverse
  # transform reads one element past a constant window table at the end of
  # its software-pipelined loops; the value is never used. These checks are
  # off for this library only.
  target_compile_options(cvt_imported_pv_aacdec PRIVATE -fno-sanitize=shift-base,shift-exponent)
  set_source_files_properties("${_cvt_imp}/pv-aacdec/trans4m_freq_2_time_fxp.cpp"
    PROPERTIES COMPILE_OPTIONS "-fno-sanitize=address")
endif()
target_compile_definitions(convertible_core PRIVATE CVT_HAVE_PV_AACDEC=1)

# ---- VisualOn AAC encoder (C sources)
file(STRINGS "${_cvt_imp}/vo-aacenc/sources.txt" _cvt_voaac_sources)
cvt_add_imported(vo-aacenc
  SOURCES ${_cvt_voaac_sources}
  INCLUDE_DIRS common/include
  PRIVATE_DEFINITIONS NDEBUG)
target_include_directories(cvt_imported_vo_aacenc PRIVATE "${_cvt_imp}/vo-aacenc/aacenc/inc" "${_cvt_imp}/vo-aacenc/aacenc/basic_op")
if(MSVC)
  target_compile_options(cvt_imported_vo_aacenc PRIVATE "/FI${_cvt_aac}/vo_aacenc_rename.h")
else()
  target_compile_options(cvt_imported_vo_aacenc PRIVATE -include "${_cvt_aac}/vo_aacenc_rename.h")
endif()
if(CVT_SANITIZE AND NOT MSVC)
  # Fixed-point basic operations shift negative values (two's complement).
  target_compile_options(cvt_imported_vo_aacenc PRIVATE -fno-sanitize=shift-base,signed-integer-overflow)
endif()
target_compile_definitions(convertible_core PRIVATE CVT_HAVE_VO_AACENC=1)
