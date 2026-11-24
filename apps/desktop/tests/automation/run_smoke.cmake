# Runs the real desktop executable in headless automation mode.
#   cmake -DAPP=<exe> -DSCRIPT=<smoke.json> -DWORK_DIR=<dir> -DFIXTURES=<dir> -P run_smoke.cmake
cmake_minimum_required(VERSION 3.22)

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
file(READ "${SCRIPT}" _script)
string(REPLACE "FIXTURES" "${FIXTURES}" _script "${_script}")
file(WRITE "${WORK_DIR}/script.json" "${_script}")

execute_process(
  COMMAND "${APP}" --headless --skip-intro --automation "${WORK_DIR}/script.json" --out "${WORK_DIR}"
          --config-dir "${WORK_DIR}/config" --size 1120x760 --scale 1
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err
  TIMEOUT 120)
message("${_out}${_err}")
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "automation run failed (exit ${_rc})")
endif()
foreach(_f 01-empty.png 02-done.png state.json saved/hello.b64 saved/fox.hex)
  if(NOT EXISTS "${WORK_DIR}/${_f}")
    message(FATAL_ERROR "automation did not produce ${_f}")
  endif()
endforeach()
