# End-to-end smoke test of the `convertible` command-line tool, run by CTest:
#   cmake -DCLI=<path to convertible> -DWORK_DIR=<scratch dir> -P cli_smoke.cmake
# Converts a text file to Base64 and back and checks the basic commands.

if(NOT CLI OR NOT WORK_DIR)
  message(FATAL_ERROR "usage: cmake -DCLI=... -DWORK_DIR=... -P cli_smoke.cmake")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(original "Hello, Convertible!\nSecond line with unicode: é€\n")
file(WRITE "${WORK_DIR}/hello.txt" "${original}")

function(run expected_status)
  execute_process(COMMAND "${CLI}" ${ARGN}
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT status EQUAL expected_status)
    message(FATAL_ERROR "convertible ${ARGN}: exit status ${status}, expected ${expected_status}\n${output}\n${errors}")
  endif()
  set(last_output "${output}" PARENT_SCOPE)
endfunction()

run(0 --version)
if(NOT last_output MATCHES "^convertible [0-9]+\\.[0-9]+\\.[0-9]+")
  message(FATAL_ERROR "unexpected --version output: ${last_output}")
endif()

run(0 --help)
run(0 formats)
if(NOT last_output MATCHES "base64")
  message(FATAL_ERROR "formats does not list base64")
endif()
run(0 formats --json --category encoding)
if(NOT last_output MATCHES "\"id\": \"hex\"")
  message(FATAL_ERROR "formats --json --category encoding does not list hex")
endif()

run(0 detect hello.txt)
string(STRIP "${last_output}" detected)
if(NOT detected STREQUAL "txt")
  message(FATAL_ERROR "detect hello.txt printed '${detected}', expected 'txt'")
endif()

run(0 targets hello.txt)
if(NOT last_output MATCHES "base64")
  message(FATAL_ERROR "targets does not offer base64")
endif()

# Text -> Base64 (default output name) -> back to the original bytes.
run(0 convert hello.txt -t base64 --quiet)
if(NOT EXISTS "${WORK_DIR}/hello.b64")
  message(FATAL_ERROR "convert did not create hello.b64")
endif()
run(0 convert hello.b64 -t bin -o roundtrip.bin --quiet)
file(READ "${WORK_DIR}/roundtrip.bin" roundtrip)
if(NOT roundtrip STREQUAL original)
  message(FATAL_ERROR "Base64 round trip changed the content")
endif()

# Options, explicit source format and an in-place-style identity conversion.
run(0 convert hello.txt -t txt --opt encoding=utf-16le --opt bom=true -o utf16.txt --quiet)
run(0 convert utf16.txt --from txt -t txt -o utf8.txt --quiet)
file(READ "${WORK_DIR}/utf8.txt" utf8)
if(NOT utf8 STREQUAL original)
  message(FATAL_ERROR "UTF-16 round trip changed the content")
endif()
run(0 convert hello.txt -t base64 --list-options)

# --opt values are typed by the option schema: an enum whose choices look
# like numbers ("4.0") stays text instead of becoming the number 4.
file(WRITE "${WORK_DIR}/card.vcf" "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Sample Person\r\nN:Person;Sample;;;\r\nEND:VCARD\r\n")
run(0 convert card.vcf -t vcf --opt vcard_version=4.0 -o card4.vcf --quiet)
file(READ "${WORK_DIR}/card4.vcf" card4)
if(NOT card4 MATCHES "VERSION:4\\.0")
  message(FATAL_ERROR "--opt vcard_version=4.0 was not applied:\n${card4}")
endif()
run(0 convert hello.txt -t base64 --opt line_width=8 -o narrow.b64 --quiet)   # integers still parse
file(STRINGS "${WORK_DIR}/narrow.b64" narrow_lines)
list(GET narrow_lines 0 narrow_first)
string(LENGTH "${narrow_first}" narrow_width)
if(NOT narrow_width EQUAL 8)
  message(FATAL_ERROR "--opt line_width=8 gave lines of ${narrow_width} characters")
endif()

# Failures map to the documented exit codes.
run(6 convert hello.txt -t base64 --quiet)             # output exists, no --force
run(0 convert hello.txt -t base64 --quiet --force)
run(3 convert hello.txt -t png --quiet)                # unsupported conversion
run(2 convert hello.txt -t no-such-format --quiet)     # unknown format
run(6 convert missing.txt -t base64 --quiet)           # unreadable input
run(1 convert hello.txt --quiet)                       # no target
run(1 frobnicate)                                      # unknown command
run(1 convert hello.txt -t base64 --opt line_width=abc -o bad.b64 --quiet)  # bad option value

file(REMOVE_RECURSE "${WORK_DIR}")
