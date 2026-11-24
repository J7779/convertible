# Generates a C++ source file that embeds files as byte arrays.
#
# Script mode:
#   cmake -DOUTPUT=<file.cpp> -DINPUTS_FILE=<list> -P EmbedResources.cmake
# where <list> holds one "<name>=<path>" entry per line.
#
# Each <name> becomes a key for cvd::resource(name). Runs at build time and
# writes only into the build directory.

cmake_minimum_required(VERSION 3.22)

if(NOT OUTPUT OR NOT INPUTS_FILE)
  message(FATAL_ERROR "EmbedResources.cmake: OUTPUT and INPUTS_FILE are required")
endif()
file(STRINGS "${INPUTS_FILE}" INPUTS)

set(_body "// Embedded resources. Built from the repository's assets at build time; do not edit.\n")
string(APPEND _body "#include <cstdint>\n#include <span>\n#include <string_view>\n\n#include \"base/resources.hpp\"\n\nnamespace {\n")
set(_table "")
set(_index 0)
foreach(_entry IN LISTS INPUTS)
  string(FIND "${_entry}" "=" _eq)
  string(SUBSTRING "${_entry}" 0 ${_eq} _name)
  math(EXPR _start "${_eq} + 1")
  string(SUBSTRING "${_entry}" ${_start} -1 _path)
  file(READ "${_path}" _hex HEX)
  string(LENGTH "${_hex}" _hexlen)
  math(EXPR _size "${_hexlen} / 2")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
  # Break the initialiser into lines of 32 bytes to keep compilers happy.
  string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" _bytes "${_bytes}")
  string(APPEND _body "alignas(16) const std::uint8_t kRes${_index}[${_size} + 1] = {\n${_bytes}0x00};\n")
  string(APPEND _table "    {\"${_name}\", kRes${_index}, ${_size}},\n")
  math(EXPR _index "${_index} + 1")
endforeach()

string(APPEND _body "\nstruct Entry {\n  std::string_view name;\n  const std::uint8_t* data;\n  std::size_t size;\n};\n\n")
string(APPEND _body "const Entry kEntries[] = {\n${_table}};\n\n}  // namespace\n\n")
string(APPEND _body "namespace cvd {\n\nstd::span<const std::uint8_t> resource(std::string_view name) {\n")
string(APPEND _body "  for (const Entry& e : kEntries) {\n    if (e.name == name) return {e.data, e.size};\n  }\n  return {};\n}\n\n}  // namespace cvd\n")

# Only touch the output when it changed, so dependants do not rebuild needlessly.
if(EXISTS "${OUTPUT}")
  file(READ "${OUTPUT}" _old)
  if(_old STREQUAL _body)
    return()
  endif()
endif()
file(WRITE "${OUTPUT}" "${_body}")
