# audio_lossy family, speech area: opencore-amr 0.1.6 (AMR-NB decoder and
# encoder, AMR-WB decoder; Apache-2.0, C++ sources) and vo-amrwbenc 0.1.3
# (AMR-WB encoder; Apache-2.0, C sources), without assembly. Every global
# symbol gets a private prefix through a force-included header.

set(_cvt_imp "${CVT_CORE_DIR}/src/imported")
set(_cvt_speech "${CVT_FAMILY_DIR}/speech")

file(STRINGS "${_cvt_imp}/opencore-amr/sources.txt" _cvt_oc_sources)
set(_cvt_oc_amr "${_cvt_imp}/opencore-amr/opencore/codecs_v2/audio/gsm_amr")
cvt_add_imported(opencore-amr
  SOURCES ${_cvt_oc_sources}
  INCLUDE_DIRS amrnb amrwb
  PRIVATE_DEFINITIONS NDEBUG
  CXX_STANDARD 14)
target_include_directories(cvt_imported_opencore_amr PRIVATE
  "${_cvt_imp}/opencore-amr/oscl"
  "${_cvt_oc_amr}/amr_nb/common/include" "${_cvt_oc_amr}/amr_nb/dec/include" "${_cvt_oc_amr}/amr_nb/dec/src"
  "${_cvt_oc_amr}/amr_nb/enc/include" "${_cvt_oc_amr}/amr_nb/enc/src" "${_cvt_oc_amr}/common/dec/include"
  "${_cvt_oc_amr}/amr_wb/dec/include" "${_cvt_oc_amr}/amr_wb/dec/src")
if(MSVC)
  target_compile_options(cvt_imported_opencore_amr PRIVATE "/FI${_cvt_speech}/opencore_amr_rename.h")
else()
  target_compile_options(cvt_imported_opencore_amr PRIVATE -include "${_cvt_speech}/opencore_amr_rename.h")
endif()
if(CVT_SANITIZE AND NOT MSVC)
  target_compile_options(cvt_imported_opencore_amr PRIVATE -fno-sanitize=shift,signed-integer-overflow,bounds,pointer-overflow)
endif()

file(STRINGS "${_cvt_imp}/vo-amrwbenc/sources.txt" _cvt_voamr_sources)
cvt_add_imported(vo-amrwbenc
  SOURCES ${_cvt_voamr_sources}
  INCLUDE_DIRS .
  PRIVATE_DEFINITIONS NDEBUG)
target_include_directories(cvt_imported_vo_amrwbenc PRIVATE
  "${_cvt_imp}/vo-amrwbenc/amrwbenc/inc" "${_cvt_imp}/vo-amrwbenc/common/include")
if(MSVC)
  target_compile_options(cvt_imported_vo_amrwbenc PRIVATE "/FI${_cvt_speech}/vo_amrwbenc_rename.h")
else()
  target_compile_options(cvt_imported_vo_amrwbenc PRIVATE -include "${_cvt_speech}/vo_amrwbenc_rename.h")
endif()
if(CVT_SANITIZE AND NOT MSVC)
  target_compile_options(cvt_imported_vo_amrwbenc PRIVATE -fno-sanitize=shift-base,signed-integer-overflow,bounds,pointer-overflow)
  # voAWB_Copy reads two elements past its source at the end of its unrolled
  # loop (values unused).
  set_source_files_properties("${_cvt_imp}/vo-amrwbenc/amrwbenc/src/util.c" PROPERTIES COMPILE_OPTIONS "-fno-sanitize=address")
endif()
target_compile_definitions(convertible_core PRIVATE CVT_HAVE_AMR=1)
