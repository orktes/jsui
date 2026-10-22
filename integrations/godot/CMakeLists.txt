# jsui_godot: a Godot 4 GDExtension that hosts a JSUI runtime in a Control
# node (JsuiView). Enabled with -DJSUI_BUILD_GODOT=ON from the top-level
# CMakeLists.txt.
#
# godot-cpp is fetched at a pinned tag (not vendored). Its binding generator
# needs python3 at configure/build time. Point JSUI_GODOT_CPP_DIR at an
# existing godot-cpp checkout to skip the download.
include(FetchContent)

set(JSUI_GODOT_CPP_TAG godot-4.3-stable CACHE STRING
    "godot-cpp git tag (keep in step with compatibility_minimum in demo/jsui.gdextension)")
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
set(GODOT_CPP_SYSTEM_HEADERS ON CACHE BOOL "" FORCE)
add_subdirectory(${_godot_cpp_src} ${CMAKE_BINARY_DIR}/_deps/godot-cpp-build EXCLUDE_FROM_ALL)

add_library(jsui_godot SHARED
  src/register_types.cpp
  src/jsui_view.cpp
  src/keymap.cpp)
target_link_libraries(jsui_godot PRIVATE jsui godot::cpp)

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
