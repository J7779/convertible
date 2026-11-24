# pdf family: embed the bundled SIL OFL fonts (Inter, JetBrains Mono NL) used
# for PDF text output and for substituting non-embedded fonts when rendering.
# The TTF files stay in assets/fonts/; a byte-array source is generated in the
# build directory at configure time (and regenerated when a font changes).
set(_cvt_pdf_font_dir "${CVT_CORE_DIR}/../assets/fonts")
set(_cvt_pdf_font_out "${CMAKE_CURRENT_BINARY_DIR}/cvt_pdf_embedded_fonts.cpp")
set(_cvt_pdf_fonts
  "inter_regular|Inter-Regular.ttf"
  "inter_semibold|Inter-SemiBold.ttf"
  "mono_regular|JetBrainsMonoNL-Regular.ttf")

set(_cvt_pdf_need_regen FALSE)
if(NOT EXISTS "${_cvt_pdf_font_out}")
  set(_cvt_pdf_need_regen TRUE)
endif()
foreach(entry IN LISTS _cvt_pdf_fonts)
  string(REPLACE "|" ";" parts "${entry}")
  list(GET parts 1 file)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_cvt_pdf_font_dir}/${file}")
  if(EXISTS "${_cvt_pdf_font_out}" AND "${_cvt_pdf_font_dir}/${file}" IS_NEWER_THAN "${_cvt_pdf_font_out}")
    set(_cvt_pdf_need_regen TRUE)
  endif()
endforeach()

if(_cvt_pdf_need_regen)
  set(_src "// Generated at build time from assets/fonts (SIL Open Font License 1.1). Do not edit.\n")
  string(APPEND _src "#include <cstddef>\n#include <cstdint>\n#include \"families/pdf/embedded_fonts.hpp\"\n")
  string(APPEND _src "namespace cvt::pdf::fonts {\nnamespace {\n")
  set(_funcs "")
  foreach(entry IN LISTS _cvt_pdf_fonts)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 symbol)
    list(GET parts 1 file)
    set(path "${_cvt_pdf_font_dir}/${file}")
    if(EXISTS "${path}")
      file(READ "${path}" hex HEX)
      string(LENGTH "${hex}" hex_length)
      math(EXPR byte_count "${hex_length} / 2")
      string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," body "${hex}")
      string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" body "${body}")
      string(APPEND _src "constexpr std::uint8_t k_${symbol}[${byte_count}] = {\n${body}};\n")
      string(APPEND _funcs "ByteView ${symbol}() { return {reinterpret_cast<const std::byte*>(k_${symbol}), sizeof k_${symbol}}; }\n")
    else()
      string(APPEND _funcs "ByteView ${symbol}() { return {}; }\n")
    endif()
  endforeach()
  string(APPEND _src "}  // namespace\n${_funcs}}  // namespace cvt::pdf::fonts\n")
  file(WRITE "${_cvt_pdf_font_out}" "${_src}")
endif()

target_sources(convertible_core PRIVATE "${_cvt_pdf_font_out}")
