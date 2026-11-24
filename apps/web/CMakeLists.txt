# Convertible web app: the core compiled to WebAssembly.
#
# Configure with Emscripten only (see build.sh, which runs this inside the
# pinned emscripten/emsdk container or with a locally activated emsdk):
#
#   emcmake cmake -S apps/web -B apps/web/build/wasm -DCMAKE_BUILD_TYPE=Release
#   cmake --build apps/web/build/wasm -j 6
#
# Output: convertible.mjs (ES module factory) + convertible.wasm.
cmake_minimum_required(VERSION 3.22)

project(ConvertibleWeb VERSION 0.1.0 LANGUAGES C CXX)

if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "apps/web must be configured with Emscripten (emcmake cmake ...). Use apps/web/build.sh.")
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../core/cmake/CvtCxxStandard.cmake")
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

set(CVT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CVT_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(CVT_WASM_EXCEPTIONS ON CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../core" core)

add_executable(convertible_wasm wasm/exports.c)
target_link_libraries(convertible_wasm PRIVATE convertible::core)
set_target_properties(convertible_wasm PROPERTIES OUTPUT_NAME convertible SUFFIX ".mjs")

# Exactly the public C ABI plus the allocator the binding needs for buffers.
set(CVT_WASM_EXPORTS
  _cvt_version _cvt_string_free _cvt_formats_json _cvt_detect _cvt_targets_json
  _cvt_options_schema_json _cvt_convert _cvt_convert_file _cvt_result_status
  _cvt_result_message _cvt_result_data _cvt_result_size _cvt_result_format
  _cvt_result_extension _cvt_result_mime _cvt_result_warnings_json _cvt_result_free
  _malloc _free)
list(JOIN CVT_WASM_EXPORTS "," CVT_WASM_EXPORTS_CSV)

target_compile_options(convertible_wasm PRIVATE -O3)
target_link_options(convertible_wasm PRIVATE
  -O3
  -g0
  --no-entry
  -fwasm-exceptions
  "-sEXPORTED_FUNCTIONS=${CVT_WASM_EXPORTS_CSV}"
  "-sEXPORTED_RUNTIME_METHODS=addFunction,removeFunction,UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAPU8"
  -sMODULARIZE=1
  -sEXPORT_ES6=1
  -sEXPORT_NAME=createConvertibleModule
  "-sENVIRONMENT=web,worker,node"
  -sALLOW_MEMORY_GROWTH=1
  -sINITIAL_MEMORY=33554432
  -sMAXIMUM_MEMORY=4294967296
  -sSTACK_SIZE=2097152
  -sALLOW_TABLE_GROWTH=1
  -sFILESYSTEM=0
  -sDYNAMIC_EXECUTION=0
  -sINCOMING_MODULE_JS_API=locateFile,wasmBinary,instantiateWasm,print,printErr
  -sTEXTDECODER=2)

# Belt and braces: also strip custom sections (names, producers) from the
# shipped binary.
target_link_options(convertible_wasm PRIVATE -Wl,--strip-all)
