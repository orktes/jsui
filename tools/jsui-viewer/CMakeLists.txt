# jsui-viewer: windowed host with dev mode, built on SDL2.
#
# SDL2 is downloaded (release tarball, SHA-256 checked) and built statically.
# Point JSUI_SDL_DIR at an SDL2 source tree to skip the download. When SDL
# cannot be fetched the viewer is skipped with a warning instead of failing
# the whole configure.
set(JSUI_SDL_VERSION 2.30.12)
set(JSUI_SDL_SHA256 ac356ea55e8b9dd0b2d1fa27da40ef7e238267ccf9324704850d5d47375b48ea)
set(JSUI_SDL_DIR "" CACHE PATH "SDL2 source tree to use instead of downloading release-${JSUI_SDL_VERSION}")

set(_sdl_src ${JSUI_SDL_DIR})
if(NOT _sdl_src)
  set(_sdl_root ${CMAKE_BINARY_DIR}/_deps/sdl2-src)
  set(_sdl_src ${_sdl_root}/SDL2-${JSUI_SDL_VERSION})
  if(NOT EXISTS ${_sdl_src}/CMakeLists.txt)
    set(_tgz ${CMAKE_BINARY_DIR}/_deps/SDL2-${JSUI_SDL_VERSION}.tar.gz)
    set(_url https://github.com/libsdl-org/SDL/releases/download/release-${JSUI_SDL_VERSION}/SDL2-${JSUI_SDL_VERSION}.tar.gz)
    message(STATUS "Downloading SDL2: ${_url}")
    file(DOWNLOAD ${_url} ${_tgz} STATUS _st)
    list(GET _st 0 _code)
    if(_code EQUAL 0)
      file(SHA256 ${_tgz} _hash)
      if(_hash STREQUAL JSUI_SDL_SHA256)
        file(MAKE_DIRECTORY ${_sdl_root})
        file(ARCHIVE_EXTRACT INPUT ${_tgz} DESTINATION ${_sdl_root})
      else()
        message(WARNING "SDL2 download has an unexpected SHA-256 (${_hash})")
      endif()
    else()
      message(WARNING "SDL2 download failed: ${_st}")
    endif()
  endif()
endif()

if(NOT EXISTS ${_sdl_src}/CMakeLists.txt)
  message(WARNING "SDL2 not available: skipping jsui-viewer (set JSUI_SDL_DIR or -DJSUI_BUILD_VIEWER=OFF)")
  return()
endif()

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(${_sdl_src} ${CMAKE_BINARY_DIR}/_deps/sdl2-build EXCLUDE_FROM_ALL)

add_executable(jsui-viewer main.cpp keymap.cpp)
target_compile_definitions(jsui-viewer PRIVATE SDL_MAIN_HANDLED)
target_link_libraries(jsui-viewer PRIVATE jsui SDL2::SDL2-static)
if(WIN32)
  set_target_properties(jsui-viewer PROPERTIES WIN32_EXECUTABLE OFF)
endif()
