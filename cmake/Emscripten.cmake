# WebAssembly build (included from CMakeLists.txt when EMSCRIPTEN is set).
#
#   scripts/setup-emsdk.sh && scripts/build-skia-wasm.sh
#   source third_party/emsdk/emsdk_env.sh
#   emcmake cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-wasm
#
# Produces web/dist/jsui.js (ES module factory `createJsuiModule`) and
# web/dist/jsui.wasm, consumed by web/jsui-web.js.

set(JSUI_WEB_DIST "${CMAKE_SOURCE_DIR}/web/dist" CACHE PATH "Output directory for jsui.js / jsui.wasm")
set(JSUI_WASM_INITIAL_MEMORY 67108864 CACHE STRING "Initial WASM heap in bytes (grows on demand)")
# QuickJS recurses on the C stack while parsing / running JS; the Emscripten
# default (64 KB) overflows on real React bundles.
set(JSUI_WASM_STACK_SIZE 4194304 CACHE STRING "WASM C stack size in bytes")

# Skia for WASM is built with defines that change public headers
# (SK_FORCE_8_BYTE_ALIGNMENT, SKNX_NO_SIMD, SK_DISABLE_TRACING, ...). The build
# script records them; compile everything that includes Skia with the same.
if(EXISTS "${JSUI_SKIA_LIB_DIR}/defines.txt")
  file(STRINGS "${JSUI_SKIA_LIB_DIR}/defines.txt" _jsui_skia_defs)
  target_compile_definitions(jsui_skia INTERFACE ${_jsui_skia_defs})
endif()
# Libraries the native list in Deps.cmake does not know about (wasm-ld resolves
# archives in any order, so appending is fine).
foreach(_lib brotli)
  if(EXISTS "${JSUI_SKIA_LIB_DIR}/lib${_lib}.a")
    target_link_libraries(jsui_skia INTERFACE "${JSUI_SKIA_LIB_DIR}/lib${_lib}.a")
  endif()
endforeach()

# WebAssembly SIMD: follow the Skia build (scripts/build-skia-wasm.sh enables
# it by default); mixing is legal, but SIMD everywhere is the point.
set(_jsui_simd_default OFF)
if(EXISTS "${JSUI_SKIA_LIB_DIR}/args.gn")
  file(READ "${JSUI_SKIA_LIB_DIR}/args.gn" _jsui_skia_args)
  if(_jsui_skia_args MATCHES "msimd128")
    set(_jsui_simd_default ON)
  endif()
endif()
option(JSUI_WASM_SIMD "Compile JSUI with -msimd128 (needs a browser with WASM SIMD)" ${_jsui_simd_default})
if(JSUI_WASM_SIMD)
  foreach(_t jsui jsui_quickjs jsui_yoga)
    target_compile_options(${_t} PRIVATE -msimd128)
  endforeach()
endif()

# QuickJS-ng: no threads / atomics in the browser build.
target_compile_definitions(jsui_quickjs PRIVATE EMSCRIPTEN=1)

# Exported C API: every JSUI_API function declared in jsui.h, plus the web
# glue and the allocator the JS host uses to pass strings and frame buffers.
file(READ "${CMAKE_SOURCE_DIR}/include/jsui/jsui.h" _jsui_h)
string(REGEX MATCHALL "JSUI_API[^;(]*[ *](jsui_[a-z0-9_]+)[ ]*\\(" _jsui_decls "${_jsui_h}")
set(_jsui_exports _malloc _free _jsui_web_create _jsui_web_call_ui _jsui_web_resolve_call _jsui_web_reject_call
    _jsui_emscripten_fetch_begin _jsui_emscripten_fetch_end)
foreach(_d ${_jsui_decls})
  string(REGEX REPLACE ".*[ *](jsui_[a-z0-9_]+)[ ]*\\($" "_\\1" _name "${_d}")
  list(APPEND _jsui_exports ${_name})
endforeach()
list(REMOVE_DUPLICATES _jsui_exports)
string(REPLACE ";" "," _jsui_exports_csv "${_jsui_exports}")

add_executable(jsui-wasm web/native/jsui_web.cpp)
target_link_libraries(jsui-wasm PRIVATE jsui)
set_target_properties(jsui-wasm PROPERTIES
  OUTPUT_NAME jsui
  SUFFIX ".js"
  RUNTIME_OUTPUT_DIRECTORY "${JSUI_WEB_DIST}")
target_link_options(jsui-wasm PRIVATE
  "-sMODULARIZE=1"
  "-sEXPORT_ES6=1"
  "-sEXPORT_NAME=createJsuiModule"
  "-sENVIRONMENT=web,worker,node"
  "-sALLOW_MEMORY_GROWTH=1"
  "-sINITIAL_MEMORY=${JSUI_WASM_INITIAL_MEMORY}"
  "-sSTACK_SIZE=${JSUI_WASM_STACK_SIZE}"
  "-sWASM_BIGINT=1"
  "-sFILESYSTEM=0"
  "-sEXPORTED_FUNCTIONS=${_jsui_exports_csv}"
  "$<$<CONFIG:Debug>:-sASSERTIONS=2>"
  "$<$<CONFIG:Debug>:-gsource-map>")
if(JSUI_GPU)
  # Skia Ganesh on WebGL (third_party/skia-wasm-webgl, WEBGL=1
  # scripts/build-skia-wasm.sh), like canvaskit's WebGL link flags. The JS host
  # creates the WebGL context itself and hands it to emscripten through the
  # exported GL object (GL.registerContext / makeContextCurrent).
  target_link_options(jsui-wasm PRIVATE
    "-lGL"
    "-sUSE_WEBGL2=1"
    "-sMAX_WEBGL_VERSION=2"
    "-sMIN_WEBGL_VERSION=1"
    "-sEXPORTED_RUNTIME_METHODS=UTF8ToString,stringToUTF8,lengthBytesUTF8,GL")
else()
  target_link_options(jsui-wasm PRIVATE
    "-sEXPORTED_RUNTIME_METHODS=UTF8ToString,stringToUTF8,lengthBytesUTF8")
endif()
message(STATUS "jsui-wasm: ${JSUI_WEB_DIST}/jsui.{js,wasm} (GPU: ${JSUI_GPU}), exports: ${_jsui_exports_csv}")
