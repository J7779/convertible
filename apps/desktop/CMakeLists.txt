# Convertible desktop app (Windows, Linux, macOS).
#
# Built from the repository root:
#   cmake -S . -B build/desktop -DCVT_BUILD_DESKTOP=ON -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/desktop -j 6
#   ctest --test-dir build/desktop -L desktop --output-on-failure
#
# Only the OS SDKs are used: Cocoa (macOS), Win32/COM/Shell (Windows), Xlib +
# XShm (Linux). Fonts, design tokens, the motion storyboard and licence texts
# are embedded into the executable at build time from the repository's assets.

cmake_minimum_required(VERSION 3.22)

if(APPLE)
  enable_language(OBJCXX)
endif()

set(CVD_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
get_filename_component(CVD_REPO_ROOT "${CVD_DIR}/../.." ABSOLUTE)
set(CVD_ASSETS "${CVD_REPO_ROOT}/assets")
set(CVD_VERSION "${PROJECT_VERSION}")
if(NOT CVD_VERSION)
  set(CVD_VERSION "0.1.0")
endif()
set(CVD_BUNDLE_ID "app.convertible.desktop")

option(CVD_BUILD_TESTS "Build the desktop unit tests" ON)

# ---------------------------------------------------------------------------
# Embedded resources (generated into the build directory)
# ---------------------------------------------------------------------------
set(CVD_RESOURCES
  "fonts/Inter-Regular.ttf=${CVD_ASSETS}/fonts/Inter-Regular.ttf"
  "fonts/Inter-Medium.ttf=${CVD_ASSETS}/fonts/Inter-Medium.ttf"
  "fonts/Inter-SemiBold.ttf=${CVD_ASSETS}/fonts/Inter-SemiBold.ttf"
  "fonts/JetBrainsMonoNL-Regular.ttf=${CVD_ASSETS}/fonts/JetBrainsMonoNL-Regular.ttf"
  "fonts/JetBrainsMonoNL-Medium.ttf=${CVD_ASSETS}/fonts/JetBrainsMonoNL-Medium.ttf"
  "brand/tokens.json=${CVD_ASSETS}/brand/tokens.json"
  "brand/animation.json=${CVD_ASSETS}/brand/animation.json"
  "licenses/Inter-LICENSE.txt=${CVD_ASSETS}/fonts/Inter-LICENSE.txt"
  "licenses/JetBrainsMono-LICENSE.txt=${CVD_ASSETS}/fonts/JetBrainsMono-LICENSE.txt"
  "licenses/LICENSE=${CVD_REPO_ROOT}/LICENSE")
set(_cvd_res_files "")
foreach(_entry IN LISTS CVD_RESOURCES)
  string(FIND "${_entry}" "=" _eq)
  math(EXPR _start "${_eq} + 1")
  string(SUBSTRING "${_entry}" ${_start} -1 _path)
  list(APPEND _cvd_res_files "${_path}")
endforeach()
set(CVD_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${CVD_GENERATED}")
set(CVD_RES_LIST "${CVD_GENERATED}/resources.list")
string(REPLACE ";" "\n" _cvd_res_lines "${CVD_RESOURCES}")
file(WRITE "${CVD_RES_LIST}.in" "${_cvd_res_lines}\n")
configure_file("${CVD_RES_LIST}.in" "${CVD_RES_LIST}" COPYONLY)
add_custom_command(
  OUTPUT "${CVD_GENERATED}/resources.cpp"
  COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${CVD_GENERATED}/resources.cpp" "-DINPUTS_FILE=${CVD_RES_LIST}"
          -P "${CVD_DIR}/cmake/EmbedResources.cmake"
  DEPENDS ${_cvd_res_files} "${CVD_RES_LIST}" "${CVD_DIR}/cmake/EmbedResources.cmake"
  COMMENT "Embedding fonts, tokens and licences"
  VERBATIM)

# ---------------------------------------------------------------------------
# Toolkit: geometry, JSON, renderer, fonts/text, UI widgets (OS-independent)
# ---------------------------------------------------------------------------
add_library(cvd_toolkit STATIC
  src/base/json.cpp
  src/base/text_util.cpp
  src/base/bundled_fonts.cpp
  src/gfx/path.cpp
  src/gfx/canvas.cpp
  src/gfx/image_io.cpp
  src/text/font.cpp
  src/text/text.cpp
  src/ui/anim.cpp
  src/ui/theme.cpp
  src/ui/ui.cpp
  src/ui/widgets.cpp
  src/platform/fileio.cpp
  "${CVD_GENERATED}/resources.cpp")
target_include_directories(cvd_toolkit PUBLIC "${CVD_DIR}/src")
target_compile_definitions(cvd_toolkit PUBLIC CVD_VERSION_STRING="${CVD_VERSION}")
cvt_set_warnings(cvd_toolkit)
cvt_apply_sanitizers(cvd_toolkit)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_options(cvd_toolkit PUBLIC -pthread)
  target_link_options(cvd_toolkit PUBLIC -pthread)
endif()

# ---------------------------------------------------------------------------
# App: core binding, view-model, views, automation (OS-independent)
# ---------------------------------------------------------------------------
add_library(cvd_app STATIC
  src/app/binding.cpp
  src/app/state.cpp
  src/app/logo.cpp
  src/app/app.cpp
  src/app/views_home.cpp
  src/app/views_picker.cpp
  src/app/views_other.cpp
  src/app/file_browser.cpp
  src/app/automation.cpp
  src/platform/headless.cpp)
target_link_libraries(cvd_app PUBLIC cvd_toolkit convertible::core)
cvt_set_warnings(cvd_app)
cvt_apply_sanitizers(cvd_app)

# ---------------------------------------------------------------------------
# Executable with the OS layer
# ---------------------------------------------------------------------------
if(APPLE)
  set(CVD_PLATFORM_SOURCES src/platform/mac/platform_mac.mm)
elseif(WIN32)
  set(CVD_PLATFORM_SOURCES src/platform/win/platform_win.cpp)
else()
  set(CVD_PLATFORM_SOURCES src/platform/x11/platform_x11.cpp)
endif()

add_executable(convertible_desktop WIN32 MACOSX_BUNDLE src/main.cpp ${CVD_PLATFORM_SOURCES})
target_link_libraries(convertible_desktop PRIVATE cvd_app)
cvt_set_warnings(convertible_desktop)
cvt_apply_sanitizers(convertible_desktop)

if(APPLE)
  set_target_properties(convertible_desktop PROPERTIES
    OUTPUT_NAME "Convertible"
    MACOSX_BUNDLE_INFO_PLIST "${CVD_DIR}/packaging/macos/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "Convertible"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${CVD_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${CVD_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${CVD_VERSION}"
    MACOSX_BUNDLE_ICON_FILE "icon.icns"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "")
  set_source_files_properties(src/platform/mac/platform_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_sources(convertible_desktop PRIVATE "${CVD_ASSETS}/brand/icons/icon.icns")
  set_source_files_properties("${CVD_ASSETS}/brand/icons/icon.icns" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  target_link_libraries(convertible_desktop PRIVATE "-framework Cocoa" "-framework QuartzCore"
                                                    "-framework UniformTypeIdentifiers")
elseif(WIN32)
  set_target_properties(convertible_desktop PROPERTIES OUTPUT_NAME "Convertible")
  configure_file("${CVD_DIR}/packaging/windows/convertible.rc.in" "${CVD_GENERATED}/convertible.rc" @ONLY)
  configure_file("${CVD_DIR}/packaging/windows/convertible.manifest" "${CVD_GENERATED}/convertible.manifest" COPYONLY)
  file(COPY "${CVD_ASSETS}/brand/icons/icon.ico" DESTINATION "${CVD_GENERATED}")
  target_sources(convertible_desktop PRIVATE "${CVD_GENERATED}/convertible.rc")
  target_compile_definitions(convertible_desktop PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(convertible_desktop PRIVATE user32 gdi32 shell32 ole32 uuid comdlg32 dwmapi shcore uxtheme)
  if(MSVC)
    target_link_options(convertible_desktop PRIVATE /MANIFEST:NO)
  else()
    target_link_options(convertible_desktop PRIVATE -municode -static)
  endif()
else()
  set_target_properties(convertible_desktop PROPERTIES OUTPUT_NAME "convertible-desktop")
  find_package(X11 REQUIRED)
  target_include_directories(convertible_desktop PRIVATE ${X11_INCLUDE_DIR})
  target_link_libraries(convertible_desktop PRIVATE ${X11_LIBRARIES})
  if(X11_XShm_FOUND OR X11_Xext_LIB)
    target_link_libraries(convertible_desktop PRIVATE ${X11_Xext_LIB})
    target_compile_definitions(convertible_desktop PRIVATE CVD_HAVE_XSHM=1)
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests (headless; label "desktop")
# ---------------------------------------------------------------------------
if(CVD_BUILD_TESTS AND BUILD_TESTING)
  file(GLOB CVD_TEST_SOURCES CONFIGURE_DEPENDS "${CVD_DIR}/tests/*.cpp")
  add_executable(convertible_desktop_tests ${CVD_TEST_SOURCES})
  # The core's test framework header uses the core's internal error types.
  target_link_libraries(convertible_desktop_tests PRIVATE cvd_app convertible_core_internal)
  target_include_directories(convertible_desktop_tests PRIVATE "${CVD_REPO_ROOT}/core/tests" "${CVD_DIR}/tests")
  target_compile_definitions(convertible_desktop_tests PRIVATE
    CVT_TEST_PROGRAM_NAME="convertible_desktop_tests"
    CVD_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-tmp")
  cvt_set_warnings(convertible_desktop_tests)
  cvt_apply_sanitizers(convertible_desktop_tests)
  add_test(NAME desktop_unit_tests COMMAND convertible_desktop_tests)
  set_tests_properties(desktop_unit_tests PROPERTIES LABELS "desktop")

  # End-to-end: the real executable in headless automation mode.
  add_test(NAME desktop_automation_smoke
    COMMAND "${CMAKE_COMMAND}"
      "-DAPP=$<TARGET_FILE:convertible_desktop>"
      "-DSCRIPT=${CVD_DIR}/tests/automation/smoke.json"
      "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/automation-smoke"
      "-DFIXTURES=${CVD_DIR}/tests/fixtures"
      -P "${CVD_DIR}/tests/automation/run_smoke.cmake")
  set_tests_properties(desktop_automation_smoke PROPERTIES LABELS "desktop")
endif()

# ---------------------------------------------------------------------------
# Install and packaging (CPack, no external tools)
# ---------------------------------------------------------------------------
include("${CVD_DIR}/cmake/Packaging.cmake")
