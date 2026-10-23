# jsui-sdl3: the shared HUD over an SDL3 SDL_Renderer arena (main.cpp). Built
# with -DJSUI_BUILD_SDL3=ON; SDL3 is downloaded (release tarball, SHA-256
# checked) and built statically. JSUI_SDL3_DIR skips the download.
include(${CMAKE_SOURCE_DIR}/integrations/shared/fetch.cmake)

set(JSUI_SDL3_VERSION 3.4.18)
jsui_fetch_archive(_sdl3_src NAME sdl3
  URL https://github.com/libsdl-org/SDL/releases/download/release-${JSUI_SDL3_VERSION}/SDL3-${JSUI_SDL3_VERSION}.tar.gz
  SHA256 9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
  DIR SDL3-${JSUI_SDL3_VERSION})
if(NOT _sdl3_src OR NOT EXISTS ${_sdl3_src}/CMakeLists.txt)
  message(WARNING "SDL3 not available: skipping jsui-sdl3 (set JSUI_SDL3_DIR or -DJSUI_BUILD_SDL3=OFF)")
  return()
endif()

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(${_sdl3_src} ${CMAKE_BINARY_DIR}/_deps/sdl3-build EXCLUDE_FROM_ALL)

add_executable(jsui-sdl3 main.cpp)
target_link_libraries(jsui-sdl3 PRIVATE SDL3::SDL3-static)
jsui_example_target(jsui-sdl3)
if(WIN32)
  set_target_properties(jsui-sdl3 PROPERTIES WIN32_EXECUTABLE OFF)
endif()
