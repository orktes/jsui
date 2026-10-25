# jsui_godot: a Godot 4 GDExtension that hosts a JSUI runtime in a Control
# node (JsuiView). Enabled with -DJSUI_BUILD_GODOT=ON from the top-level
# CMakeLists.txt.
#
# godot-cpp is fetched at a pinned tag (not vendored). Its binding generator
# needs python3 at configure/build time. Point JSUI_GODOT_CPP_DIR at an
# existing godot-cpp checkout to skip the download.
#
# Since 10.x godot-cpp is versioned independently of Godot and builds against
# any Godot API from 4.3 on (it bundles every extension_api JSON). The API
# chosen with JSUI_GODOT_API_VERSION is the oldest Godot the library loads in
# (extensions are forward- but not backward-compatible), so it must match
# compatibility_minimum in demo/jsui.gdextension. JsuiView needs nothing newer
# than 4.3, so that stays the default; the library loads in 4.3 ... 4.7.
include(FetchContent)

set(JSUI_GODOT_CPP_TAG 10.0.0-stable CACHE STRING "godot-cpp git tag")
set(JSUI_GODOT_API_VERSION 4.3 CACHE STRING
    "Godot API to build against = the oldest Godot that loads the extension (keep in step with compatibility_minimum in demo/jsui.gdextension)")
set_property(CACHE JSUI_GODOT_API_VERSION PROPERTY STRINGS 4.3 4.4 4.5 4.6 4.7)
set(JSUI_GODOT_CPP_DIR "" CACHE PATH "godot-cpp source tree to use instead of fetching JSUI_GODOT_CPP_TAG")
set(JSUI_GODOT_OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/demo/bin CACHE PATH
    "Where the GDExtension library is written (demo/jsui.gdextension points at demo/bin)")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(JSUI_GODOT_CPP_DIR)
  set(_godot_cpp_src ${JSUI_GODOT_CPP_DIR})
else()
  FetchContent_Declare(godot-cpp
    GIT_REPOSITORY https://github.com/godotengine/godot-cpp.git
    GIT_TAG ${JSUI_GODOT_CPP_TAG}
    GIT_SHALLOW TRUE)
  FetchContent_GetProperties(godot-cpp)
  if(NOT godot-cpp_POPULATED)
    FetchContent_Populate(godot-cpp)
  endif()
  set(_godot_cpp_src ${godot-cpp_SOURCE_DIR})
endif()

# godot-cpp's own CMake build (target godot::cpp). Its headers are treated as
# system headers so their warnings do not leak into ours.
set(GODOTCPP_SYSTEM_HEADERS ON CACHE BOOL "" FORCE)
set(GODOTCPP_API_VERSION ${JSUI_GODOT_API_VERSION} CACHE STRING "" FORCE)
add_subdirectory(${_godot_cpp_src} ${CMAKE_BINARY_DIR}/_deps/godot-cpp-build EXCLUDE_FROM_ALL)

if(EMSCRIPTEN)
  # Threaded web build: a static library linked into a Godot web export
  # template (README, "Threaded web build"), not a side module. godot-cpp's
  # web setup compiles for one (-sSIDE_MODULE=1, i.e. -fPIC + dynamic
  # linking glue); take that out.
  foreach(_prop COMPILE_OPTIONS INTERFACE_COMPILE_OPTIONS)
    get_target_property(_opts godot-cpp ${_prop})
    if(_opts)
      list(REMOVE_ITEM _opts -sSIDE_MODULE=1)
      set_target_properties(godot-cpp PROPERTIES ${_prop} "${_opts}")
    endif()
  endforeach()
  add_library(jsui_godot STATIC
    src/register_types.cpp
    src/jsui_view.cpp
    src/jsui_gpu_render.cpp
    src/keymap.cpp
    src/jsui_audio.cpp)
  target_link_libraries(jsui_godot PRIVATE jsui godot::cpp)

  # jsui_godot_web: everything (the integration, godot-cpp, JSUI, QuickJS,
  # Yoga, Skia and its HarfBuzz / FreeType / ICU / libpng / zlib / ...)
  # linked into ONE relocatable object whose symbols are all local except the
  # entry point jsui_godot_init (tools/wasm-localize), in an archive. The
  # engine links its own copies of several of those libraries at other
  # versions; with local symbols the two sets neither clash nor bind to each
  # other. Undefined references (libc, libc++, Emscripten's runtime, wgpu* of
  # the engine's Emdawnwebgpu) resolve against the engine.
  set(_web_name libjsui_godot.web.threads.wasm32.a)
  set(_web_dir ${CMAKE_CURRENT_BINARY_DIR}/web)
  set(_web_lib ${_web_dir}/${_web_name})
  # The archives a program using jsui_godot links, in link order, as CMake
  # computes them (Skia's, WebFonts' WOFF / WOFF2 decoders, ...): a stand-in
  # program whose "linker" only records its inputs.
  set(_web_rsp ${_web_dir}/inputs.txt)
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/web_inputs_main.cpp "int main() { return 0; }\n")
  add_executable(jsui_godot_web_inputs ${CMAKE_CURRENT_BINARY_DIR}/web_inputs_main.cpp)
  target_link_libraries(jsui_godot_web_inputs PRIVATE jsui_godot)
  set_target_properties(jsui_godot_web_inputs PROPERTIES CXX_LINKER_LAUNCHER
    "python3;${CMAKE_SOURCE_DIR}/tools/wasm-localize/capture_link.py;${_web_rsp};libjsui_godot.a")
  find_program(JSUI_WASM_LD wasm-ld HINTS "$ENV{EMSDK}/upstream/bin" "${EMSCRIPTEN_ROOT_PATH}/../bin" REQUIRED)
  find_program(JSUI_EMAR emar HINTS "${EMSCRIPTEN_ROOT_PATH}" REQUIRED)
  add_custom_command(OUTPUT ${_web_lib}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_web_dir}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/wasm-localize/link_relocatable.py
            --wasm-ld ${JSUI_WASM_LD} --inputs ${_web_rsp} --entry jsui_godot_init
            # The browser loader's fetch() glue calls these (the engine's link
            # must export them: the fork's modules/jsui does).
            --keep jsui_emscripten_fetch_begin --keep jsui_emscripten_fetch_end
            --output ${_web_dir}/jsui_godot.o
    COMMAND ${CMAKE_COMMAND} -E rm -f ${_web_lib}
    COMMAND ${JSUI_EMAR} rcs ${_web_lib} ${_web_dir}/jsui_godot.o
    COMMAND ${CMAKE_COMMAND} -E make_directory ${JSUI_GODOT_OUTPUT_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy ${_web_lib} ${JSUI_GODOT_OUTPUT_DIR}/${_web_name}
    DEPENDS jsui_godot_web_inputs
            ${CMAKE_SOURCE_DIR}/tools/wasm-localize/wasm_localize.py
            ${CMAKE_SOURCE_DIR}/tools/wasm-localize/link_relocatable.py
    COMMENT "Linking ${_web_name} (one object, local symbols)"
    VERBATIM)
  add_custom_target(jsui_godot_web ALL DEPENDS ${_web_lib})
  message(STATUS "jsui_godot_web: ${JSUI_GODOT_OUTPUT_DIR}/${_web_name}")
  return()
endif()

add_library(jsui_godot SHARED
  src/register_types.cpp
  src/jsui_view.cpp
  src/jsui_gpu_render.cpp
  src/keymap.cpp
  src/jsui_audio.cpp)
target_link_libraries(jsui_godot PRIVATE jsui godot::cpp)
if(APPLE)
  # Tracked Metal texture for JsuiView's src (src/metal_texture.mm).
  enable_language(OBJCXX)
  target_sources(jsui_godot PRIVATE src/metal_texture.mm)
  set_source_files_properties(src/metal_texture.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(jsui_godot PRIVATE "-framework Metal" "-framework Foundation")
endif()

# File name follows the godot-cpp convention used by demo/jsui.gdextension:
#   libjsui_godot.<platform>.<arch>.<so|dll|dylib>
if(WIN32)
  set(_godot_platform windows)
elseif(APPLE)
  set(_godot_platform macos)
else()
  set(_godot_platform linux)
endif()
if(APPLE)
  set(_godot_arch universal)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
  set(_godot_arch arm64)
else()
  set(_godot_arch x86_64)
endif()
set_target_properties(jsui_godot PROPERTIES
  OUTPUT_NAME jsui_godot.${_godot_platform}.${_godot_arch}
  PREFIX lib
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
foreach(_cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
  set_target_properties(jsui_godot PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY${_cfg} ${JSUI_GODOT_OUTPUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY${_cfg} ${JSUI_GODOT_OUTPUT_DIR})
endforeach()

if(UNIX AND NOT APPLE)
  # Keep Skia/HarfBuzz/FreeType/ICU symbols private (Godot ships its own
  # copies of several of them), bind our own references locally and fail at
  # link time rather than at load time on missing symbols.
  target_link_options(jsui_godot PRIVATE -Wl,--exclude-libs,ALL -Wl,-Bsymbolic -Wl,--no-undefined)
endif()
jsui_copy_icu_data(jsui_godot)
