# JNI bridge between the Kotlin app and the Convertible core.
#
# Builds libconvertible_jni.so: the core (a static library, pulled in from the
# repository's core/ directory) plus a thin binding to its C ABI. Nothing else
# is linked; the C++ runtime is static (ANDROID_STL=c++_static).
cmake_minimum_required(VERSION 3.22)
project(ConvertibleAndroid LANGUAGES C CXX)

# apps/android/app/src/main/cpp -> repository root.
get_filename_component(CVT_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)

# Bound the number of parallel compile jobs (Ninja job pool).
set(CVT_ANDROID_JOBS "6" CACHE STRING "Maximum parallel compile jobs for the native build")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS cvt_compile=${CVT_ANDROID_JOBS})
set(CMAKE_JOB_POOL_COMPILE cvt_compile)

# Smaller binaries: let the linker drop unused code of the static core.
add_compile_options(-ffunction-sections -fdata-sections)

# The core selects its own C++ standard (C++26 where the compiler has it, else C++23).
add_subdirectory("${CVT_REPO_ROOT}/core" "${CMAKE_CURRENT_BINARY_DIR}/core")

add_library(convertible_jni SHARED
  android_compat.cpp
  jni_bridge.cpp
  utf.cpp
  utf.hpp)
set_target_properties(convertible_jni PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
target_compile_options(convertible_jni PRIVATE -Wall -Wextra -Wpedantic -Werror=return-type)
target_link_libraries(convertible_jni PRIVATE convertible::core android log)
target_link_options(convertible_jni PRIVATE
  # 16 KB page-size compatible ELF segments (required on 16 KB devices, harmless on 4 KB ones).
  "-Wl,-z,max-page-size=16384"
  "-Wl,-z,common-page-size=16384"
  "-Wl,--gc-sections"
  # Only JNI_OnLoad is exported; natives are registered explicitly.
  "-Wl,--exclude-libs,ALL"
  "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map"
  "-Wl,-z,relro,-z,now")
set_property(TARGET convertible_jni APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/exports.map")
