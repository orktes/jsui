# jsui-raylib: the shared HUD over a raylib 3D scene (main.cpp). Built with
# -DJSUI_BUILD_RAYLIB=ON; raylib (and its bundled GLFW) is downloaded and
# built statically. JSUI_RAYLIB_DIR skips the download.
include(${CMAKE_SOURCE_DIR}/integrations/shared/fetch.cmake)

set(JSUI_RAYLIB_VERSION 6.2)
jsui_fetch_archive(_raylib_src NAME raylib
  URL https://github.com/raysan5/raylib/archive/refs/tags/${JSUI_RAYLIB_VERSION}.tar.gz
  SHA256 df7a13129b1bec9e73a6962efff170beba310ee6eab8f94cd000b7316b4db5ee
  DIR raylib-${JSUI_RAYLIB_VERSION})
if(NOT _raylib_src OR NOT EXISTS ${_raylib_src}/CMakeLists.txt)
  message(WARNING "raylib not available: skipping jsui-raylib (set JSUI_RAYLIB_DIR or -DJSUI_BUILD_RAYLIB=OFF)")
  return()
endif()

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(PLATFORM Desktop CACHE STRING "" FORCE)
set(OPENGL_VERSION 3.3 CACHE STRING "" FORCE)  # what Skia's GL backend wants (core profile on macOS)
# raylib's CMake treats warnings of its own sources as its business.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
add_subdirectory(${_raylib_src} ${CMAKE_BINARY_DIR}/_deps/raylib-build EXCLUDE_FROM_ALL)

add_executable(jsui-raylib main.cpp)
target_link_libraries(jsui-raylib PRIVATE raylib)
jsui_example_target(jsui-raylib)
