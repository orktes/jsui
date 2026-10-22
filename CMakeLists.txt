cmake_minimum_required(VERSION 3.20)
project(jsui C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(JSUI_BUILD_TOOLS "Build jsui-render and the viewer" ON)
option(JSUI_BUILD_TESTS "Build native tests" ON)
option(JSUI_BUILD_VIEWER "Build tools/jsui-viewer (downloads and builds SDL2)" ON)
option(JSUI_BUILD_GODOT "Build the Godot 4 GDExtension in integrations/godot (fetches godot-cpp, needs python3)" OFF)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(Deps)

file(GLOB_RECURSE JSUI_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(jsui STATIC ${JSUI_SOURCES})
target_include_directories(jsui PUBLIC include PRIVATE src)
target_link_libraries(jsui PUBLIC jsui_quickjs jsui_yoga jsui_skia)
target_compile_definitions(jsui PRIVATE JSUI_BUILDING)
include(EmbedFonts)
jsui_embed_fonts(jsui)

# GPU backend (Skia Ganesh on OpenGL / GLES / WebGL, jsui_render_gl*). The
# native prebuilt Skia always has Ganesh GL; GL itself is resolved at run
# time (Linux: dlopen of libEGL / libGL, macOS / Windows: Skia's native
# interface), so nothing extra is linked on Linux. The WASM default Skia is
# CPU-only: the option follows its args.gn (WEBGL=1 scripts/build-skia-wasm.sh).
set(_jsui_gpu_default ON)
if(IOS)
  set(_jsui_gpu_default OFF)  # GLES is deprecated on iOS: Metal below
elseif(EMSCRIPTEN)
  set(_jsui_gpu_default OFF)
  if(EXISTS "${JSUI_SKIA_LIB_DIR}/args.gn")
    file(READ "${JSUI_SKIA_LIB_DIR}/args.gn" _jsui_skia_gn)
    if(_jsui_skia_gn MATCHES "skia_use_webgl *= *true" AND _jsui_skia_gn MATCHES "skia_enable_ganesh *= *true")
      set(_jsui_gpu_default ON)
    endif()
  endif()
endif()
option(JSUI_GPU "Build the Skia Ganesh GL / WebGL backend (jsui_render_gl)" ${_jsui_gpu_default})
if(JSUI_GPU)
  target_compile_definitions(jsui PUBLIC JSUI_GPU=1)
  if(NOT EMSCRIPTEN)
    target_compile_definitions(jsui_skia INTERFACE SK_GANESH SK_GL)
  endif()
  if(APPLE)
    target_link_libraries(jsui PUBLIC "-framework OpenGL")
  endif()
endif()
message(STATUS "JSUI_GPU: ${JSUI_GPU}")

# WebGPU backend (Skia Graphite on Dawn / WebGPU, jsui_render_webgpu*). On
# WASM the option follows the Skia build (scripts/build-skia-wasm-graphite.sh,
# third_party/skia-wasm-gpu), whose webgpu.h comes from the Emdawnwebgpu port
# (scripts/setup-emdawnwebgpu.sh); JSUI compiles and links with the same port.
# Native builds need a Skia with Graphite + Dawn (not the m124 prebuilt yet).
set(_jsui_webgpu_default OFF)
if(EMSCRIPTEN AND EXISTS "${JSUI_SKIA_LIB_DIR}/args.gn")
  file(READ "${JSUI_SKIA_LIB_DIR}/args.gn" _jsui_skia_gn)
  if(_jsui_skia_gn MATCHES "skia_enable_graphite *= *true" AND _jsui_skia_gn MATCHES "skia_use_webgpu *= *true")
    set(_jsui_webgpu_default ON)
  endif()
endif()
option(JSUI_WEBGPU "Build the Skia Graphite WebGPU backend (jsui_render_webgpu)" ${_jsui_webgpu_default})
set(JSUI_EMDAWNWEBGPU_PORT "${CMAKE_SOURCE_DIR}/third_party/emdawnwebgpu/emdawnwebgpu_pkg/emdawnwebgpu.port.py"
    CACHE FILEPATH "Emdawnwebgpu port file (WASM + JSUI_WEBGPU)")
if(JSUI_WEBGPU)
  target_compile_definitions(jsui PUBLIC JSUI_WEBGPU=1)
  if(EMSCRIPTEN)
    if(NOT EXISTS "${JSUI_EMDAWNWEBGPU_PORT}")
      message(FATAL_ERROR "Emdawnwebgpu not found at ${JSUI_EMDAWNWEBGPU_PORT}: run scripts/setup-emdawnwebgpu.sh")
    endif()
    target_compile_options(jsui PUBLIC "--use-port=${JSUI_EMDAWNWEBGPU_PORT}")
    target_link_options(jsui PUBLIC "--use-port=${JSUI_EMDAWNWEBGPU_PORT}")
  endif()
endif()
message(STATUS "JSUI_WEBGPU: ${JSUI_WEBGPU}")

# Metal backend (Skia Ganesh on Metal, jsui_render_metal). On by default on
# iOS, whose Skia is built from source with Metal (scripts/build-skia-ios.sh);
# elsewhere it needs a Skia built with skia_use_metal=true.
set(_jsui_metal_default OFF)
if(IOS)
  set(_jsui_metal_default ON)
endif()
option(JSUI_METAL "Build the Skia Ganesh Metal backend (jsui_render_metal)" ${_jsui_metal_default})
if(JSUI_METAL)
  target_compile_definitions(jsui PUBLIC JSUI_METAL=1)
  target_compile_definitions(jsui_skia INTERFACE SK_GANESH SK_METAL)
  target_link_libraries(jsui PUBLIC "-framework Metal" "-framework Foundation")
endif()
message(STATUS "JSUI_METAL: ${JSUI_METAL}")

# iOS: the library plus the example host app (integrations/ios); the
# desktop tools and native tests are not built.
if(IOS)
  set(JSUI_BUILD_TOOLS OFF)
  set(JSUI_BUILD_TESTS OFF)
  option(JSUI_BUILD_IOS_APP "Build the iOS example app (integrations/ios)" ON)
  if(JSUI_BUILD_IOS_APP)
    add_subdirectory(integrations/ios)
  endif()
endif()

if(JSUI_BUILD_TOOLS AND NOT EMSCRIPTEN)
  add_executable(jsui-render tools/jsui-render/main.cpp)
  target_link_libraries(jsui-render PRIVATE jsui)
  jsui_copy_icu_data(jsui-render)
  # Headless recorder: scripted input timeline -> frames / H.264 via ffmpeg.
  add_executable(jsui-record tools/jsui-record/main.cpp)
  target_link_libraries(jsui-record PRIVATE jsui)
  target_include_directories(jsui-record PRIVATE src)
  jsui_copy_icu_data(jsui-record)
  # Frame-phase benchmark (tools/jsui-bench/run.sh builds the scenarios).
  add_executable(jsui-bench tools/jsui-bench/main.cpp)
  target_link_libraries(jsui-bench PRIVATE jsui)
  target_include_directories(jsui-bench PRIVATE src)
  jsui_copy_icu_data(jsui-bench)
  if(JSUI_BUILD_VIEWER)
    add_subdirectory(tools/jsui-viewer)
  endif()
endif()

if(JSUI_BUILD_TESTS AND NOT EMSCRIPTEN)
  enable_testing()
  file(GLOB JSUI_TEST_SOURCES CONFIGURE_DEPENDS tests/native/*.cpp)
  add_executable(jsui-tests ${JSUI_TEST_SOURCES})
  target_include_directories(jsui-tests PRIVATE src)
  target_link_libraries(jsui-tests PRIVATE jsui)
  jsui_copy_icu_data(jsui-tests)
  add_test(NAME jsui-tests COMMAND jsui-tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

if(JSUI_BUILD_GODOT AND NOT EMSCRIPTEN)
  add_subdirectory(integrations/godot)
endif()

if(EMSCRIPTEN)
  include(Emscripten)
endif()
