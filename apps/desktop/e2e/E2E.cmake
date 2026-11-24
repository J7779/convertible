# End-to-end tests of the desktop app (CTest label "desktop-e2e").
#
# convertible_desktop_e2e drives the real executable through its automation
# mode (headless, virtual clock) with real conversions, then checks the saved
# files, the JSON state dumps and the screenshots. Included from
# apps/desktop/CMakeLists.txt when tests are enabled.
#
#   ctest --test-dir build/desktop -L desktop-e2e -C Release --output-on-failure
#
# Scratch folders and failure screenshots go to <build>/apps/desktop/e2e/
# (inside the build tree, never into the sources).

file(GLOB CVD_E2E_SOURCES CONFIGURE_DEPENDS
  "${CVD_DIR}/e2e/runner/*.cpp"
  "${CVD_DIR}/e2e/tests/*.cpp")
add_executable(convertible_desktop_e2e ${CVD_E2E_SOURCES})
# The toolkit's JSON and file helpers, the core's test framework and its
# internal hash functions (as in the unit tests). No other dependencies.
target_link_libraries(convertible_desktop_e2e PRIVATE cvd_toolkit convertible_core_internal)
target_include_directories(convertible_desktop_e2e PRIVATE "${CVD_REPO_ROOT}/core/tests" "${CVD_DIR}/e2e/runner")
target_compile_definitions(convertible_desktop_e2e PRIVATE CVT_TEST_PROGRAM_NAME="convertible_desktop_e2e")
cvt_set_warnings(convertible_desktop_e2e)
cvt_apply_sanitizers(convertible_desktop_e2e)
add_dependencies(convertible_desktop_e2e convertible_desktop)

# One CTest entry per group so failures are easy to place and groups can run in parallel.
set(CVD_E2E_GROUPS
  intro add-files queue picker options convert cancel errors formats settings
  keyboard layout themes text a11y catalog media window)
set(CVD_E2E_WORK "${CMAKE_CURRENT_BINARY_DIR}/e2e")
foreach(_group IN LISTS CVD_E2E_GROUPS)
  add_test(NAME desktop_e2e_${_group}
    COMMAND convertible_desktop_e2e
      "--app=$<TARGET_FILE:convertible_desktop>"
      "--work=${CVD_E2E_WORK}"
      "--e2e=${CVD_DIR}/e2e"
      "--assets=${CVD_ASSETS}"
      "--test-case=e2e/${_group}/*"
      --durations)
  set_tests_properties(desktop_e2e_${_group} PROPERTIES LABELS "desktop-e2e" TIMEOUT 900)
endforeach()
