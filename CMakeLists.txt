cmake_minimum_required(VERSION 3.22)

# The oldest Apple systems whose C++ runtime has everything the core uses
# (std::format and floating-point std::to_chars need macOS 13.3 / iOS 16.4).
if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET
   AND NOT DEFINED ENV{MACOSX_DEPLOYMENT_TARGET})
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS version")
endif()

project(Convertible
  VERSION 0.1.0
  DESCRIPTION "Offline, privacy-first universal file converter"
  LANGUAGES C CXX)

# C++26 where the toolchain has it, otherwise C++23 (the baseline the code is
# written against). See core/cmake/CvtCxxStandard.cmake.
include("${CMAKE_CURRENT_SOURCE_DIR}/core/cmake/CvtCxxStandard.cmake")
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(CVT_IS_TOP_LEVEL ON)
  set_property(GLOBAL PROPERTY USE_FOLDERS ON)
  include(CTest)
else()
  set(CVT_IS_TOP_LEVEL OFF)
endif()

option(CVT_BUILD_DESKTOP "Build the desktop application (apps/desktop)" OFF)

add_subdirectory(core)

if(CVT_BUILD_DESKTOP AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/desktop/CMakeLists.txt")
  add_subdirectory(apps/desktop)
endif()
