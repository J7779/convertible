# Helper functions shared by core/CMakeLists.txt and the per-family
# `family.cmake` fragments.

# cvt_set_warnings(<target>)
#   Strict warnings for first-party code.
function(cvt_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /bigobj)
    target_compile_definitions(${target} PRIVATE
      NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _SCL_SECURE_NO_WARNINGS UNICODE _UNICODE)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# cvt_apply_sanitizers(<target>)
#   Adds -fsanitize=... when CVT_SANITIZE is set (ON means address+undefined).
function(cvt_apply_sanitizers target)
  if(NOT CVT_SANITIZE OR MSVC)
    return()
  endif()
  set(kinds "${CVT_SANITIZE}")
  if(kinds MATCHES "^(ON|TRUE|YES|1)$")
    set(kinds "address;undefined")
  endif()
  string(REPLACE ";" "," kinds "${kinds}")
  get_target_property(type ${target} TYPE)
  if(type STREQUAL "INTERFACE_LIBRARY")
    set(scope INTERFACE)
  else()
    set(scope PUBLIC)
  endif()
  target_compile_options(${target} ${scope} -fsanitize=${kinds} -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  target_link_options(${target} ${scope} -fsanitize=${kinds})
endfunction()

# cvt_add_imported(<name>
#     [SOURCES <file>...]            # relative to core/src/imported/<name>/ unless absolute
#     [INCLUDE_DIRS <dir>...]        # relative to core/src/imported/<name>/; default: the library root
#     [DEFINITIONS <def>...]         # compile definitions for the library AND for core code that includes it
#     [PRIVATE_DEFINITIONS <def>...] # compile definitions for the library sources only
#     [C_STANDARD <n>] [CXX_STANDARD <n>])   # CXX_STANDARD default 17, never the core's
#
#   The core is written in-house. This function exists for the rare component
#   that policy allows to be imported (see core/README.md, "Imported code"):
#   it compiles source kept in-tree under core/src/imported/<name>/ with
#   warnings disabled and folds the objects into `convertible_core`, so apps
#   still link exactly one static library. Header-only code omits SOURCES.
#   The include directories are added to the core as SYSTEM includes.
#
#   Nothing is ever downloaded, looked up on the system or built elsewhere:
#   no FetchContent, ExternalProject, find_package, pkg-config or submodules.
#
#   Call it from core/src/families/<family>/family.cmake.
function(cvt_add_imported name)
  cmake_parse_arguments(TP "" "C_STANDARD;CXX_STANDARD" "SOURCES;INCLUDE_DIRS;DEFINITIONS;PRIVATE_DEFINITIONS" ${ARGN})
  set(root "${CVT_CORE_DIR}/src/imported/${name}")
  if(NOT IS_DIRECTORY "${root}")
    message(FATAL_ERROR "cvt_add_imported(${name}): ${root} does not exist")
  endif()
  # Imported code documents itself: a VENDOR.json
  # ({"name","version","license","url","files_modified"}) and the upstream
  # licence text next to the sources.
  if(NOT EXISTS "${root}/VENDOR.json")
    message(FATAL_ERROR "cvt_add_imported(${name}): ${root}/VENDOR.json is missing")
  endif()
  file(GLOB _cvt_licences "${root}/LICENSE*" "${root}/LICENCE*" "${root}/COPYING*" "${root}/UNLICENSE*")
  if(NOT _cvt_licences)
    message(FATAL_ERROR "cvt_add_imported(${name}): no licence file found in ${root}")
  endif()

  set(include_dirs "")
  if(TP_INCLUDE_DIRS)
    foreach(dir IN LISTS TP_INCLUDE_DIRS)
      if(IS_ABSOLUTE "${dir}")
        list(APPEND include_dirs "${dir}")
      else()
        list(APPEND include_dirs "${root}/${dir}")
      endif()
    endforeach()
  else()
    set(include_dirs "${root}")
  endif()

  target_include_directories(convertible_core SYSTEM PRIVATE ${include_dirs})
  target_include_directories(convertible_core_internal SYSTEM INTERFACE ${include_dirs})
  if(TP_DEFINITIONS)
    target_compile_definitions(convertible_core PRIVATE ${TP_DEFINITIONS})
    target_compile_definitions(convertible_core_internal INTERFACE ${TP_DEFINITIONS})
  endif()

  if(NOT TP_SOURCES)
    return()
  endif()

  set(sources "")
  foreach(src IN LISTS TP_SOURCES)
    if(IS_ABSOLUTE "${src}")
      list(APPEND sources "${src}")
    else()
      list(APPEND sources "${root}/${src}")
    endif()
  endforeach()

  set(tp_target "cvt_imported_${name}")
  string(REGEX REPLACE "[^A-Za-z0-9_]" "_" tp_target "${tp_target}")
  add_library(${tp_target} OBJECT ${sources})
  target_include_directories(${tp_target} SYSTEM PRIVATE ${include_dirs})
  target_compile_definitions(${tp_target} PRIVATE ${TP_DEFINITIONS} ${TP_PRIVATE_DEFINITIONS})
  set_target_properties(${tp_target} PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "imported")
  if(TP_C_STANDARD)
    set_target_properties(${tp_target} PROPERTIES C_STANDARD ${TP_C_STANDARD})
  endif()
  # Imported C++ always compiles at the standard it was written for, never at
  # the core's C++26/23: newer standards reject constructs that old codec
  # sources use (C++26 mixed-enum arithmetic, C++17 `register`). The explicit
  # property wins over CMAKE_CXX_STANDARD and disables the manual flag of
  # CvtCxxStandard.cmake for this target. Default: C++17.
  if(NOT TP_CXX_STANDARD)
    set(TP_CXX_STANDARD 17)
  endif()
  set_target_properties(${tp_target} PROPERTIES CXX_STANDARD ${TP_CXX_STANDARD} CXX_STANDARD_REQUIRED ON
                                                CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(${tp_target} PRIVATE /w /utf-8)
    target_compile_definitions(${tp_target} PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE NOMINMAX)
  else()
    target_compile_options(${tp_target} PRIVATE -w)
  endif()
  if(CVT_SANITIZE AND NOT MSVC)
    cvt_apply_sanitizers(${tp_target})
  endif()
  if(EMSCRIPTEN AND CVT_WASM_EXCEPTIONS)
    target_compile_options(${tp_target} PRIVATE -fwasm-exceptions)
  endif()
  target_sources(convertible_core PRIVATE $<TARGET_OBJECTS:${tp_target}>)
endfunction()
