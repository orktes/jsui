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
foreach(_lib brotli skunicode_core skunicode_icu jpeg12 jpeg16)
  if(EXISTS "${JSUI_SKIA_LIB_DIR}/lib${_lib}.a")
    target_link_libraries(jsui_skia INTERFACE "${JSUI_SKIA_LIB_DIR}/lib${_lib}.a")
  endif()
endforeach()

# WebAssembly SIMD: follow the Skia build (scripts/build-skia-wasm.sh enables
# it by default, with -msse4.1 so Skia's raster pipeline uses its SSE4.1 stages
# on SIMD128 instead of the one-pixel-at-a-time scalar fallback). JSUI gets the
# same flags: SK_CPU_SSE_LEVEL (from __SSE4_1__) selects inline code in Skia
# headers, which must match the libraries.
set(_jsui_simd_default OFF)
set(_jsui_sse41 OFF)
if(EXISTS "${JSUI_SKIA_LIB_DIR}/args.gn")
  file(READ "${JSUI_SKIA_LIB_DIR}/args.gn" _jsui_skia_args)
  if(_jsui_skia_args MATCHES "msimd128")
    set(_jsui_simd_default ON)
  endif()
  if(_jsui_skia_args MATCHES "msse4\\.1")
    set(_jsui_sse41 ON)
  endif()
endif()
option(JSUI_WASM_SIMD "Compile JSUI with -msimd128 (needs a browser with WASM SIMD)" ${_jsui_simd_default})
if(JSUI_WASM_SIMD)
  foreach(_t jsui jsui_quickjs jsui_yoga)
    target_compile_options(${_t} PRIVATE -msimd128)
  endforeach()
  if(_jsui_sse41)
    target_compile_options(jsui PRIVATE -msse4.1)
  endif()
endif()

# QuickJS-ng: no threads / atomics in the browser build. (In a threaded build
# the engine still runs on one thread, the JS thread: this only leaves out
# QuickJS's own Atomics / worker support.)
target_compile_definitions(jsui_quickjs PRIVATE EMSCRIPTEN=1)

# Threaded builds (JSUI_WASM_THREADS) need a Skia compiled the same way
# (scripts/build-skia-wasm-mt.sh): wasm-ld refuses objects without atomics in
# a shared-memory program, and the two longjmp ABIs do not mix.
set(_jsui_skia_threads OFF)
if(DEFINED _jsui_skia_args AND _jsui_skia_args MATCHES "-pthread")
  set(_jsui_skia_threads ON)
endif()
if(JSUI_WASM_THREADS AND NOT _jsui_skia_threads)
  message(FATAL_ERROR "JSUI_WASM_THREADS needs a threaded Skia (scripts/build-skia-wasm-mt.sh); ${JSUI_SKIA_LIB_DIR} is not one")
elseif(_jsui_skia_threads AND NOT JSUI_WASM_THREADS)
  message(FATAL_ERROR "${JSUI_SKIA_LIB_DIR} is a threaded Skia: configure with -DJSUI_WASM_THREADS=ON")
endif()
message(STATUS "JSUI_WASM_THREADS: ${JSUI_WASM_THREADS}")
if(JSUI_WASM_THREADS)
  # No browser module (web/dist*) in this configuration: the library is
  # linked into another program (integrations/godot, jsui_godot_web).
  return()
endif()

# Exported C API: every JSUI_API function declared in jsui.h, plus the web
# glue and the allocator the JS host uses to pass strings and frame buffers.
file(READ "${CMAKE_SOURCE_DIR}/include/jsui/jsui.h" _jsui_h)
string(REGEX MATCHALL "JSUI_API[^;(]*[ *](jsui_[a-z0-9_]+)[ ]*\\(" _jsui_decls "${_jsui_h}")
set(_jsui_exports _malloc _free _jsui_web_create _jsui_web_call_ui _jsui_web_resolve_call _jsui_web_reject_call
    _jsui_web_audio_voice_ended _jsui_web_video_status
    _jsui_emscripten_fetch_begin _jsui_emscripten_fetch_end _jsui_emscripten_stream_head
    _jsui_emscripten_stream_alloc _jsui_emscripten_stream_push _jsui_emscripten_stream_end)
foreach(_d ${_jsui_decls})
  string(REGEX REPLACE ".*[ *](jsui_[a-z0-9_]+)[ ]*\\($" "_\\1" _name "${_d}")
  list(APPEND _jsui_exports ${_name})
endforeach()
if(JSUI_WEBGPU)
  list(APPEND _jsui_exports _jsui_web_wgpu_create_instance _jsui_web_wgpu_release_instance _jsui_web_wgpu_release_texture _jsui_web_wgpu_release_device)
endif()
list(REMOVE_DUPLICATES _jsui_exports)
string(REPLACE ";" "," _jsui_exports_csv "${_jsui_exports}")

add_executable(jsui-wasm web/native/jsui_web.cpp)
target_link_libraries(jsui-wasm PRIVATE jsui)
target_include_directories(jsui-wasm PRIVATE src)

# The host JS engine (src/js/engine_hostjs.cpp + web/native/jsui_hostjs.js):
# the same module can run the bundle on the JS engine that loaded it (V8,
# JavaScriptCore, SpiderMonkey) instead of QuickJS, picked per runtime
# (jsui_web_create's `js`, createJsui({ js: 'host' })). QuickJS stays in the
# module either way.
option(JSUI_HOST_JS "WASM: let runtimes run their bundle on the host's JS engine" ON)
if(JSUI_HOST_JS)
  target_compile_definitions(jsui PRIVATE JSUI_HOST_JS=1)
  target_compile_definitions(jsui-wasm PRIVATE JSUI_HOST_JS=1)
  set(_jsui_hostjs "${CMAKE_SOURCE_DIR}/web/native/jsui_hostjs.js")
  target_link_options(jsui-wasm PRIVATE "--pre-js=${_jsui_hostjs}")
  set_property(TARGET jsui-wasm APPEND PROPERTY LINK_DEPENDS "${_jsui_hostjs}")
endif()
message(STATUS "JSUI_HOST_JS: ${JSUI_HOST_JS}")
if(JSUI_WASM_SIMD)
  target_compile_options(jsui-wasm PRIVATE -msimd128 $<$<BOOL:${_jsui_sse41}>:-msse4.1>)
  target_link_options(jsui-wasm PRIVATE -msimd128)
endif()
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
# The heap views are not exported by default since emscripten 4.
# stackSave / stackRestore: the host puts the stack pointer back after a
# throw that unwound through WASM frames (web/jsui-web.js, guardGL).
set(_jsui_runtime_methods UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAPU8,HEAP32,HEAPU32,HEAPF64,stackSave,stackRestore)
if(JSUI_WEBGPU)
  # Skia Graphite on WebGPU: the JS host imports its GPUDevice / the canvas
  # texture through Emdawnwebgpu's WebGPU object (web/jsui-webgpu.js).
  string(APPEND _jsui_runtime_methods ",WebGPU")
  target_compile_definitions(jsui-wasm PRIVATE JSUI_WEBGPU=1)
endif()
if(JSUI_GPU)
  # Skia Ganesh on WebGL (third_party/skia-wasm-webgl, WEBGL=1
  # scripts/build-skia-wasm.sh), like canvaskit's WebGL link flags. The JS host
  # creates the WebGL context itself and hands it to emscripten through the
  # exported GL object (GL.registerContext / makeContextCurrent).
  # web/native/jsui_webgl_lost.js: GL glue that survives a context lost in
  # the middle of a frame (WebKit).
  set(_jsui_gl_lib "${CMAKE_SOURCE_DIR}/web/native/jsui_webgl_lost.js")
  target_link_options(jsui-wasm PRIVATE
    "-lGL"
    "-sUSE_WEBGL2=1"
    "-sMAX_WEBGL_VERSION=2"
    "-sMIN_WEBGL_VERSION=1"
    "--js-library=${_jsui_gl_lib}"
    "-sEXPORTED_RUNTIME_METHODS=${_jsui_runtime_methods},GL")
  set_property(TARGET jsui-wasm APPEND PROPERTY LINK_DEPENDS "${_jsui_gl_lib}")
else()
  target_link_options(jsui-wasm PRIVATE
    "-sEXPORTED_RUNTIME_METHODS=${_jsui_runtime_methods}")
endif()
message(STATUS "jsui-wasm: ${JSUI_WEB_DIST}/jsui.{js,wasm} (GPU: ${JSUI_GPU}, WebGPU: ${JSUI_WEBGPU}), exports: ${_jsui_exports_csv}")
