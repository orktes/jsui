cmake_minimum_required(VERSION 3.20)
project(jsui C CXX)
# Objective-C / C++ sources live in subdirectories (SDL2's .m files, Godot's
# Metal texture, the Unity plugin's Metal path). Once one subdirectory enables
# OBJCXX, .m files elsewhere may be classified as OBJCXX in directories that
# have no rules for it; enabling both languages here sets them up everywhere.
if(APPLE)
  enable_language(OBJC OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Size: one section per function / data item, and the linkers drop what
# nothing reaches (--gc-sections; -dead_strip on Apple, where sections split
# per symbol anyway; MSVC's release /OPT:REF) in every library and program:
# the shared library, the engine plugins (most of godot-cpp's bindings, the
# Skia an engine never calls) and the tools. Skia's own archives are compiled
# this way too.
if(MSVC)
  add_compile_options(/Gy /Gw)
elseif(APPLE)
  add_link_options(LINKER:-dead_strip)
elseif(NOT EMSCRIPTEN AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-ffunction-sections -fdata-sections)
  add_link_options(LINKER:--gc-sections)
endif()

option(JSUI_BUILD_TOOLS "Build jsui-render and the viewer" ON)
option(JSUI_BUILD_TESTS "Build native tests" ON)
option(JSUI_BUILD_VIEWER "Build tools/jsui-viewer (downloads and builds SDL2)" ON)
option(JSUI_BUILD_GODOT "Build the Godot 4 GDExtension in integrations/godot (fetches godot-cpp, needs python3)" OFF)
# The built-in audio backend (src/audio/audio_builtin.cpp) for hosts without
# an audio handler: desktop only. The browser host plays sounds with Web Audio
# (web/jsui-web.js) and iOS embedders bring their own handler.
set(_jsui_audio_default ON)
if(EMSCRIPTEN OR IOS OR ANDROID)
  set(_jsui_audio_default OFF)
endif()
option(JSUI_AUDIO "Build the built-in miniaudio audio backend" ${_jsui_audio_default})
# The JS debugger (src/js/inspector.cpp, Chrome DevTools / VS Code through
# `jsui dev`). Dormant unless a dev-mode runtime loads its bundle from a dev
# server that offers it; OFF compiles out the engine hooks and the agent. The
# browser build debugs with the browser's own DevTools instead.
set(_jsui_debugger_default ON)
if(EMSCRIPTEN)
  set(_jsui_debugger_default OFF)
endif()
option(JSUI_DEBUGGER "Build the JS debugger (Chrome DevTools Protocol through jsui dev)" ${_jsui_debugger_default})
option(JSUI_BUILD_RAYLIB "Build the raylib example host in integrations/raylib (downloads raylib)" OFF)
option(JSUI_BUILD_SDL3 "Build the SDL3 example host in integrations/sdl3 (downloads SDL3)" OFF)
option(JSUI_BUILD_SOKOL "Build the sokol example host in integrations/sokol (downloads the sokol headers)" OFF)
option(JSUI_BUILD_UNITY "Build the Unity native plugin into integrations/unity/package/Plugins" OFF)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(Deps)

file(GLOB_RECURSE JSUI_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(jsui STATIC ${JSUI_SOURCES})
target_include_directories(jsui PUBLIC include PRIVATE src)
target_link_libraries(jsui PUBLIC jsui_quickjs jsui_yoga jsui_skia)
target_compile_definitions(jsui PRIVATE JSUI_BUILDING)
# Precompiled bundles (src/js/bytecode.h) only load on the QuickJS that
# compiled them: the key hashes the patched sources (cmake/Deps.cmake's
# ${_qjs_c}), so any patch change makes old bytecode fall back to its source.
file(SHA256 ${_qjs_c} _qjs_c_hash)
file(SHA256 ${_qjs_src}/quickjs.h _qjs_h_hash)
file(SHA256 ${_qjs_src}/quickjs-opcode.h _qjs_op_hash)
string(SHA256 _qjs_key "${_qjs_c_hash}${_qjs_h_hash}${_qjs_op_hash}")
string(SUBSTRING ${_qjs_key} 0 16 _qjs_key)
set_property(SOURCE src/js/bytecode.cpp APPEND PROPERTY COMPILE_DEFINITIONS JSUI_BYTECODE_KEY="${_qjs_key}")
if(JSUI_LOTTIE)
  target_compile_definitions(jsui PRIVATE JSUI_LOTTIE=1)
endif()
if(JSUI_DEBUGGER)
  target_compile_definitions(jsui PRIVATE JSUI_DEBUGGER=1)
endif()
message(STATUS "JSUI_DEBUGGER: ${JSUI_DEBUGGER}")
if(JSUI_AUDIO)
  target_compile_definitions(jsui PUBLIC JSUI_AUDIO=1)
  target_link_libraries(jsui PRIVATE jsui_miniaudio)
endif()
message(STATUS "JSUI_AUDIO: ${JSUI_AUDIO}")
# Subclass Skia classes (a canvas, Skottie observers, the font manager, the pipeline
# compile executor): Skia is built without RTTI, so are these files.
set_source_files_properties(src/render/paint_counter.cpp src/render/lottie.cpp src/text/font_manager.cpp
  src/render/pipeline_cache.cpp PROPERTIES COMPILE_OPTIONS
  "$<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>")
include(EmbedFonts)
jsui_embed_fonts(jsui)
include(WebFonts)
jsui_webfonts(jsui)

# GPU backend (Skia Ganesh on OpenGL / GLES / WebGL, jsui_render_gl*). The
# native prebuilt Skia always has Ganesh GL; GL itself is resolved at run
# time (Linux: dlopen of libEGL / libGL, macOS / Windows: Skia's native
# interface), so nothing extra is linked on Linux. The WASM default Skia is
# CPU-only: the option follows its args.gn (WEBGL=1 scripts/build-skia-wasm.sh).
set(_jsui_gpu_default ON)
# A source-built Skia says what it has in its args.gn.
set(_jsui_skia_gn "")
if(EXISTS "${JSUI_SKIA_LIB_DIR}/args.gn")
  file(READ "${JSUI_SKIA_LIB_DIR}/args.gn" _jsui_skia_gn)
endif()
if(IOS)
  set(_jsui_gpu_default OFF)  # GLES is deprecated on iOS: Metal below
elseif(EMSCRIPTEN)
  set(_jsui_gpu_default OFF)
  if(_jsui_skia_gn MATCHES "skia_use_webgl *= *true" AND _jsui_skia_gn MATCHES "skia_enable_ganesh *= *true")
    set(_jsui_gpu_default ON)
  endif()
elseif(JSUI_SKIA_SOURCE_BUILT)
  # scripts/build-skia-desktop.sh: Ganesh GL unless its args turn it off.
  if(_jsui_skia_gn MATCHES "skia_use_gl *= *false" OR _jsui_skia_gn MATCHES "skia_enable_ganesh *= *false")
    set(_jsui_gpu_default OFF)
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

# Graphite on an embedder's Vulkan / Metal device (jsui_gpu_create_vulkan /
# _metal, jsui_render_gpu_target): on when the Skia has Graphite with that API
# (scripts/build-skia-desktop.sh; the m124 prebuilt has neither). Vulkan is
# loaded at run time (the embedder's loader), so nothing is linked for it.
set(_jsui_vulkan_default OFF)
set(_jsui_graphite_metal_default OFF)
if(NOT EMSCRIPTEN AND _jsui_skia_gn MATCHES "skia_enable_graphite *= *true")
  if(_jsui_skia_gn MATCHES "skia_use_vulkan *= *true")
    set(_jsui_vulkan_default ON)
  endif()
  if(APPLE AND _jsui_skia_gn MATCHES "skia_use_metal *= *true")
    set(_jsui_graphite_metal_default ON)
  endif()
endif()
option(JSUI_VULKAN "Build the Skia Graphite Vulkan backend (jsui_gpu_create_vulkan)" ${_jsui_vulkan_default})
option(JSUI_GRAPHITE_METAL "Build the Skia Graphite Metal backend (jsui_gpu_create_metal)" ${_jsui_graphite_metal_default})
if(JSUI_VULKAN)
  target_compile_definitions(jsui PUBLIC JSUI_VULKAN=1)
  # Skia's own Vulkan headers (include/third_party/vulkan), as Skia was built.
  target_compile_definitions(jsui PRIVATE SK_USE_INTERNAL_VULKAN_HEADERS)
endif()
if(JSUI_GRAPHITE_METAL)
  target_compile_definitions(jsui PUBLIC JSUI_GRAPHITE_METAL=1)
  target_link_libraries(jsui PUBLIC "-framework Metal" "-framework Foundation")
endif()
message(STATUS "JSUI_VULKAN: ${JSUI_VULKAN}, JSUI_GRAPHITE_METAL: ${JSUI_GRAPHITE_METAL}")

# Graphite on an embedder's native Dawn device (jsui_gpu_create_dawn_procs /
# jsui_gpu_create_dawn, jsui_render_gpu_target), e.g. the WebGPU rendering
# driver of the godot-aw3 Godot fork: on when the Skia has Graphite + Dawn
# (scripts/build-skia-desktop.sh always builds it, header-only). Skia and
# JSUI compile against Dawn's headers from that build (<skia>/dawn/include,
# of the Dawn revision in <skia>/dawn/REVISION), but no Dawn is linked:
# tools/dawn-shim generates the wgpu* entry points as trampolines into a
# table that is filled at run time from the host's wgpuGetProcAddress (or,
# for jsui_gpu_create_dawn, from the process's exported symbols). The library
# loads anywhere; the Dawn path works where the host hands over its Dawn.
set(_jsui_graphite_dawn_default OFF)
set(_jsui_dawn_include "")
set(_jsui_dawn_revision "")
if(NOT EMSCRIPTEN AND _jsui_skia_gn MATCHES "skia_use_dawn *= *true")
  set(_jsui_graphite_dawn_default ON)
  if(EXISTS "${JSUI_SKIA_DIR}/dawn/include/dawn/webgpu.h")
    set(_jsui_dawn_include "${JSUI_SKIA_DIR}/dawn/include")
  else()
    string(REGEX MATCH "skia_dawn_header_only_include_dir *= *\"([^\"]*)\"" _m "${_jsui_skia_gn}")
    set(_jsui_dawn_include "${CMAKE_MATCH_1}")
  endif()
  if(EXISTS "${JSUI_SKIA_DIR}/dawn/REVISION")
    file(STRINGS "${JSUI_SKIA_DIR}/dawn/REVISION" _jsui_dawn_revision LIMIT_COUNT 1)
  endif()
endif()
option(JSUI_GRAPHITE_DAWN "Build the Skia Graphite Dawn backend on the host's Dawn (jsui_gpu_create_dawn_procs)" ${_jsui_graphite_dawn_default})
set(JSUI_DAWN_INCLUDE_DIR "${_jsui_dawn_include}" CACHE PATH "Dawn include dir (dawn/webgpu.h, webgpu/webgpu_cpp.h) the Skia was built against")
set(JSUI_DAWN_REVISION "${_jsui_dawn_revision}" CACHE STRING "Dawn revision of JSUI_DAWN_INCLUDE_DIR (jsui_dawn_revision(); the host's Dawn must match)")
if(JSUI_GRAPHITE_DAWN)
  if(NOT EXISTS "${JSUI_DAWN_INCLUDE_DIR}/dawn/webgpu.h")
    message(FATAL_ERROR "JSUI_GRAPHITE_DAWN needs JSUI_DAWN_INCLUDE_DIR (no dawn/webgpu.h in '${JSUI_DAWN_INCLUDE_DIR}')")
  endif()
  if(NOT JSUI_DAWN_REVISION MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "JSUI_GRAPHITE_DAWN needs JSUI_DAWN_REVISION (the Dawn revision of its headers; got '${JSUI_DAWN_REVISION}')")
  endif()
  set(_jsui_dawn_shim ${CMAKE_BINARY_DIR}/generated/dawn_shim.c)
  add_custom_command(OUTPUT ${_jsui_dawn_shim}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/dawn-shim/gen_dawn_shim.py ${JSUI_DAWN_INCLUDE_DIR}/dawn/webgpu.h ${_jsui_dawn_shim} ${JSUI_DAWN_REVISION}
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/dawn-shim/gen_dawn_shim.py ${JSUI_DAWN_INCLUDE_DIR}/dawn/webgpu.h
    COMMENT "Generating the Dawn entry-point shim")
  target_sources(jsui PRIVATE ${_jsui_dawn_shim})
  target_compile_definitions(jsui PUBLIC JSUI_GRAPHITE_DAWN=1)
  target_include_directories(jsui PRIVATE ${JSUI_DAWN_INCLUDE_DIR})
  target_link_libraries(jsui PUBLIC ${CMAKE_DL_LIBS})
endif()
message(STATUS "JSUI_GRAPHITE_DAWN: ${JSUI_GRAPHITE_DAWN} (Dawn ${JSUI_DAWN_REVISION}, ${JSUI_DAWN_INCLUDE_DIR})")

# The Skia revision, when the Skia build recorded one in <skia>/REVISION:
# part of the GPU pipeline cache's build id (src/render/pipeline_cache.h), so
# caches of another build of the same Skia milestone are not reused.
set(_jsui_skia_revision "")
if(EXISTS "${JSUI_SKIA_DIR}/REVISION")
  file(STRINGS "${JSUI_SKIA_DIR}/REVISION" _jsui_skia_revision LIMIT_COUNT 1)
endif()
set(JSUI_SKIA_REVISION "${_jsui_skia_revision}" CACHE STRING "Skia revision for the GPU pipeline cache's build id (empty: the Skia milestone alone)")
if(JSUI_SKIA_REVISION MATCHES "^[0-9A-Za-z._-]+$")
  set_source_files_properties(src/render/pipeline_cache.cpp PROPERTIES
    COMPILE_DEFINITIONS "JSUI_SKIA_REVISION=\"${JSUI_SKIA_REVISION}\"")
endif()

# Metal backend (Skia Ganesh on Metal, jsui_render_metal). On by default on
# iOS, whose Skia is built from source with Metal (scripts/build-skia-ios.sh);
# elsewhere it needs a Skia built with skia_use_metal=true.
set(_jsui_metal_default OFF)
if(IOS)
  set(_jsui_metal_default ON)
elseif(APPLE AND _jsui_skia_gn MATCHES "skia_use_metal *= *true" AND _jsui_skia_gn MATCHES "skia_enable_ganesh *= *true")
  set(_jsui_metal_default ON)
endif()
option(JSUI_METAL "Build the Skia Ganesh Metal backend (jsui_render_metal)" ${_jsui_metal_default})
if(JSUI_METAL)
  target_compile_definitions(jsui PUBLIC JSUI_METAL=1)
  target_compile_definitions(jsui_skia INTERFACE SK_GANESH SK_METAL)
  target_link_libraries(jsui PUBLIC "-framework Metal" "-framework Foundation")
endif()
message(STATUS "JSUI_METAL: ${JSUI_METAL}")

# Direct3D backend (Skia Ganesh on Direct3D 12, jsui_gpu_create_d3d12 /
# _d3d11, jsui_render_gpu_target): on by default on Windows with a Skia built
# with skia_use_direct3d (scripts/build-skia-desktop.sh; not the m124
# prebuilt). Graphite has no Direct3D backend.
set(_jsui_d3d_default OFF)
if(WIN32 AND _jsui_skia_gn MATCHES "skia_use_direct3d *= *true")
  set(_jsui_d3d_default ON)
endif()
option(JSUI_D3D "Build the Skia Ganesh Direct3D 12 backend (jsui_gpu_create_d3d12 / _d3d11)" ${_jsui_d3d_default})
if(JSUI_D3D)
  target_compile_definitions(jsui PUBLIC JSUI_D3D=1)
  target_compile_definitions(jsui_skia INTERFACE SK_GANESH SK_DIRECT3D)
  target_link_libraries(jsui PUBLIC d3d12 d3d11 dxgi dxguid)
endif()
message(STATUS "JSUI_D3D: ${JSUI_D3D}")

# jsui_shared: the prebuilt release library (libjsui.so / libjsui.dylib /
# jsui.dll) for hosts that load JSUI as a plain C library (Unity, Unreal, a
# game built with another toolchain). The static library is linked into it
# whole and only the C API is exported: Skia, QuickJS, Yoga and their
# dependencies stay private, and on Linux so does libstdc++ (linked
# statically), so the host's C++ runtime never meets ours.
option(JSUI_BUILD_SHARED "Build the shared C API library jsui_shared (libjsui.so / .dylib / jsui.dll)" OFF)
if(JSUI_BUILD_SHARED AND NOT EMSCRIPTEN AND NOT IOS)
  if(CMAKE_VERSION VERSION_LESS 3.24)
    message(FATAL_ERROR "JSUI_BUILD_SHARED needs CMake 3.24 or newer (WHOLE_ARCHIVE linking)")
  endif()
  add_library(jsui_shared SHARED cmake/jsui_shared.c)
  target_link_libraries(jsui_shared PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,jsui>")
  target_include_directories(jsui_shared PUBLIC include)
  set_target_properties(jsui_shared PROPERTIES OUTPUT_NAME jsui LINKER_LANGUAGE CXX
    C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)
  if(WIN32)
    # jsui.dll's import library is jsui.lib: the static library takes
    # another name, or the two overwrite each other in the same directory
    # (and whatever links the static one after jsui_shared gets the import
    # library).
    set_target_properties(jsui PROPERTIES OUTPUT_NAME jsui_static)
    # The functions are compiled dllexport into the static library (which
    # the tools still link statically); consumers define JSUI_SHARED too.
    target_compile_definitions(jsui PRIVATE JSUI_SHARED)
    target_compile_definitions(jsui_shared INTERFACE JSUI_SHARED)
  elseif(APPLE)
    target_link_options(jsui_shared PRIVATE "LINKER:-exported_symbol,_jsui_*" "LINKER:-dead_strip")
    set_target_properties(jsui_shared PROPERTIES INSTALL_NAME_DIR "@rpath" BUILD_WITH_INSTALL_NAME_DIR ON)
  else()
    target_link_options(jsui_shared PRIVATE "LINKER:--version-script=${CMAKE_SOURCE_DIR}/cmake/jsui_shared.map"
      "LINKER:--no-undefined" "LINKER:-Bsymbolic" "LINKER:--gc-sections" -static-libstdc++ -static-libgcc)
    set_target_properties(jsui_shared PROPERTIES LINK_DEPENDS ${CMAKE_SOURCE_DIR}/cmake/jsui_shared.map)
  endif()
endif()

# Android: the library plus the example host app (integrations/android, a
# NativeActivity .so that scripts/build-android.sh packages into an APK).
if(ANDROID)
  set(JSUI_BUILD_TOOLS OFF)
  set(JSUI_BUILD_TESTS OFF)
  option(JSUI_BUILD_ANDROID_APP "Build the Android example app's native library (integrations/android)" ON)
  if(JSUI_BUILD_ANDROID_APP)
    add_subdirectory(integrations/android)
  endif()
endif()

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
  # Showcase navigation: frame times and GC pauses over page changes.
  add_executable(jsui-nav tools/jsui-bench/nav.cpp)
  target_link_libraries(jsui-nav PRIVATE jsui)
  if(APPLE)
    target_sources(jsui-nav PRIVATE tools/jsui-paint-bench/metal_helper.mm)
    target_link_libraries(jsui-nav PRIVATE "-framework Metal" "-framework Foundation")
  endif()
  jsui_copy_icu_data(jsui-nav)
  # Paint benchmark: CPU raster (incremental / full) and headless Ganesh GL.
  add_executable(jsui-paint-bench tools/jsui-paint-bench/main.cpp)
  target_link_libraries(jsui-paint-bench PRIVATE jsui)
  target_include_directories(jsui-paint-bench PRIVATE src)
  if(UNIX AND NOT APPLE)
    target_link_libraries(jsui-paint-bench PRIVATE ${CMAKE_DL_LIBS})
  endif()
  if(APPLE)
    target_sources(jsui-paint-bench PRIVATE tools/jsui-paint-bench/metal_helper.mm)
    set_source_files_properties(tools/jsui-paint-bench/metal_helper.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_link_libraries(jsui-paint-bench PRIVATE "-framework Metal" "-framework Foundation")
  endif()
  jsui_copy_icu_data(jsui-paint-bench)
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
  if(JSUI_GRAPHITE_DAWN)
    target_compile_definitions(jsui-tests PRIVATE JSUI_TEST_DAWN_REVISION="${JSUI_DAWN_REVISION}")
  endif()
  jsui_copy_icu_data(jsui-tests)
  add_test(NAME jsui-tests COMMAND jsui-tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

if(JSUI_BUILD_GODOT AND NOT EMSCRIPTEN)
  add_subdirectory(integrations/godot)
endif()

# Engine examples: the shared HUD (integrations/shared/ui) in a raylib, an
# SDL3 and a sokol game loop, each through the C API.
if(NOT EMSCRIPTEN AND NOT IOS AND NOT ANDROID)
  if(JSUI_BUILD_RAYLIB)
    add_subdirectory(integrations/raylib)
  endif()
  if(JSUI_BUILD_SDL3)
    add_subdirectory(integrations/sdl3)
  endif()
  if(JSUI_BUILD_SOKOL)
    add_subdirectory(integrations/sokol)
  endif()
  # The Unity package's native plugin (the example is a Unity project).
  if(JSUI_BUILD_UNITY)
    add_subdirectory(integrations/unity)
  endif()
endif()

if(EMSCRIPTEN)
  include(Emscripten)
endif()
