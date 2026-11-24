# Vector family build fragment.
#
# Embeds the bundled fallback fonts used for text in vector drawings
# (assets/fonts: Inter and JetBrains Mono, SIL Open Font Licence 1.1) as
# byte arrays generated into the build directory at configure time. Nothing
# is downloaded; the fonts are part of the repository.

set(_cvt_vector_font_dir "${CVT_CORE_DIR}/../assets/fonts")
set(_cvt_vector_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/vector")
set(_cvt_vector_fonts_cpp "${_cvt_vector_gen_dir}/bundled_fonts.cpp")
file(MAKE_DIRECTORY "${_cvt_vector_gen_dir}")

set(_cvt_vector_source "// Generated at configure time from assets/fonts (SIL Open Font Licence 1.1). Do not edit.\n")
string(APPEND _cvt_vector_source "#include <cstddef>\n\nnamespace cvt::vector::bundled {\n")
foreach(_cvt_pair IN ITEMS "Inter-Regular.ttf=font_sans" "Inter-SemiBold.ttf=font_sans_bold"
                           "JetBrainsMonoNL-Regular.ttf=font_mono")
  string(REPLACE "=" ";" _cvt_pair "${_cvt_pair}")
  list(GET _cvt_pair 0 _cvt_file)
  list(GET _cvt_pair 1 _cvt_symbol)
  set(_cvt_path "${_cvt_vector_font_dir}/${_cvt_file}")
  if(NOT EXISTS "${_cvt_path}")
    message(FATAL_ERROR "vector family: bundled font ${_cvt_path} is missing")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_cvt_path}")
  file(READ "${_cvt_path}" _cvt_hex HEX)
  string(LENGTH "${_cvt_hex}" _cvt_hex_length)
  math(EXPR _cvt_size "${_cvt_hex_length} / 2")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _cvt_hex "${_cvt_hex}")
  string(REGEX REPLACE "((0x..,){40})" "\\1\n" _cvt_hex "${_cvt_hex}")
  string(APPEND _cvt_vector_source "extern const unsigned char ${_cvt_symbol}[${_cvt_size}];\n")
  string(APPEND _cvt_vector_source "const unsigned char ${_cvt_symbol}[${_cvt_size}] = {\n${_cvt_hex}};\n")
  string(APPEND _cvt_vector_source "extern const std::size_t ${_cvt_symbol}_size;\n")
  string(APPEND _cvt_vector_source "const std::size_t ${_cvt_symbol}_size = ${_cvt_size};\n\n")
endforeach()
string(APPEND _cvt_vector_source "}  // namespace cvt::vector::bundled\n")

# Rewrite only when the content changed so that a reconfigure does not force a rebuild.
set(_cvt_vector_tmp "${_cvt_vector_gen_dir}/bundled_fonts.cpp.tmp")
file(WRITE "${_cvt_vector_tmp}" "${_cvt_vector_source}")
configure_file("${_cvt_vector_tmp}" "${_cvt_vector_fonts_cpp}" COPYONLY)
target_sources(convertible_core PRIVATE "${_cvt_vector_fonts_cpp}")

unset(_cvt_vector_source)
unset(_cvt_hex)
