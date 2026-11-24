# codec_av1: AV1 decoding and encoding through the imported libaom (generic C build).
include("${CVT_FAMILY_DIR}/libaom_sources.cmake")
cvt_add_imported(libaom
  SOURCES ${CVT_LIBAOM_SOURCES}
  INCLUDE_DIRS .
  PRIVATE_DEFINITIONS NDEBUG
  C_STANDARD 11)
target_compile_definitions(convertible_core PRIVATE CVT_HAVE_LIBAOM=1)

# libaom is a large, compute-bound codec: compile it optimised even in Debug
# builds so that the test suite stays fast. Release flags are left alone.
if(NOT MSVC)
  target_compile_options(cvt_imported_libaom PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()
