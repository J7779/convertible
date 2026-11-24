# Select the newest C++ standard that this CMake + compiler pair really supports.
#
# Preference order: C++26, then C++23 (the baseline the code is written against).
# A CMake release can be older than the compiler it drives (for example the
# CMake bundled with the Android SDK), in which case CMake does not know the
# flag for a standard the compiler does implement. Simply setting
# CMAKE_CXX_STANDARD would then silently fall back to the compiler default, so
# the flag is probed and added by hand in that case.
#
# Include after project(). Does nothing when the caller already set
# CMAKE_CXX_STANDARD.

if(DEFINED CMAKE_CXX_STANDARD OR DEFINED CVT_CXX_STANDARD_SELECTED)
  return()
endif()

include(CheckCXXCompilerFlag)

function(_cvt_try_manual_flags out_var)
  set(result "")
  foreach(candidate ${ARGN})
    string(MAKE_C_IDENTIFIER "CVT_HAS_FLAG_${candidate}" check_name)
    check_cxx_compiler_flag("${candidate}" ${check_name})
    if(${check_name})
      set(result "${candidate}")
      break()
    endif()
  endforeach()
  set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

if(MSVC)
  set(_cvt_flags_26 "/std:c++latest")
  set(_cvt_flags_23 "/std:c++23" "/std:c++23preview" "/std:c++latest")
else()
  set(_cvt_flags_26 "-std=c++26" "-std=c++2c")
  set(_cvt_flags_23 "-std=c++23" "-std=c++2b")
endif()

set(_cvt_selected "")
foreach(_cvt_std 26 23)
  if(CMAKE_CXX${_cvt_std}_STANDARD_COMPILE_OPTION)
    set(CMAKE_CXX_STANDARD ${_cvt_std})
    set(_cvt_selected "C++${_cvt_std}")
    break()
  endif()
  _cvt_try_manual_flags(_cvt_flag ${_cvt_flags_${_cvt_std}})
  if(_cvt_flag)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:${_cvt_flag}>")
    set(_cvt_selected "C++${_cvt_std} (${_cvt_flag})")
    break()
  endif()
endforeach()

if(NOT _cvt_selected)
  message(FATAL_ERROR
    "Convertible needs a compiler with C++23 support "
    "(${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} offers neither C++26 nor C++23).")
endif()

set(CVT_CXX_STANDARD_SELECTED "${_cvt_selected}")
message(STATUS "Convertible: building as ${_cvt_selected}")
