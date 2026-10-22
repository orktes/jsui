# Third-party dependencies.
#
#   QuickJS-ng  - JS engine (C, builds everywhere incl. Emscripten)
#   Yoga        - flexbox layout (C++)
#   Skia        - 2D rendering. Native builds use the prebuilt static
#                 libraries published by the Aseprite project (m124). The WASM
#                 build uses a Skia build made with scripts/build-skia-wasm.sh.
#
# Override any of these with -DJSUI_SKIA_DIR=/path/to/skia etc.

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# ---------------------------------------------------------------- QuickJS-ng
FetchContent_Declare(quickjs
  GIT_REPOSITORY https://github.com/quickjs-ng/quickjs.git
  GIT_TAG v0.17.0
  GIT_SHALLOW TRUE)
FetchContent_GetProperties(quickjs)
if(NOT quickjs_POPULATED)
  FetchContent_Populate(quickjs)
endif()
add_library(jsui_quickjs STATIC
  ${quickjs_SOURCE_DIR}/dtoa.c
  ${quickjs_SOURCE_DIR}/libregexp.c
  ${quickjs_SOURCE_DIR}/libunicode.c
  ${quickjs_SOURCE_DIR}/quickjs.c)
target_include_directories(jsui_quickjs PUBLIC ${quickjs_SOURCE_DIR})
target_compile_definitions(jsui_quickjs PRIVATE _GNU_SOURCE)
if(UNIX)
  target_link_libraries(jsui_quickjs PUBLIC m)
endif()
set_target_properties(jsui_quickjs PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(jsui_quickjs PRIVATE -w)
endif()

# ---------------------------------------------------------------------- Yoga
FetchContent_Declare(yoga
  GIT_REPOSITORY https://github.com/facebook/yoga.git
  GIT_TAG v3.2.1
  GIT_SHALLOW TRUE)
FetchContent_GetProperties(yoga)
if(NOT yoga_POPULATED)
  FetchContent_Populate(yoga)
endif()
file(GLOB_RECURSE JSUI_YOGA_SOURCES CONFIGURE_DEPENDS ${yoga_SOURCE_DIR}/yoga/*.cpp)
add_library(jsui_yoga STATIC ${JSUI_YOGA_SOURCES})
target_include_directories(jsui_yoga PUBLIC ${yoga_SOURCE_DIR})
set_target_properties(jsui_yoga PROPERTIES CXX_STANDARD 20 POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------- Skia
set(JSUI_SKIA_DIR "" CACHE PATH "Directory with a Skia build (include/, modules/, out/<lib dir>)")
set(JSUI_SKIA_LIB_DIR "" CACHE PATH "Directory with Skia static libraries")

if(NOT JSUI_SKIA_DIR)
  if(EMSCRIPTEN)
    set(JSUI_SKIA_DIR ${CMAKE_SOURCE_DIR}/third_party/skia-wasm)
    if(NOT EXISTS ${JSUI_SKIA_DIR}/include)
      message(FATAL_ERROR "WASM Skia not found at ${JSUI_SKIA_DIR}. Run scripts/build-skia-wasm.sh first.")
    endif()
  else()
    if(APPLE)
      if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_skia_pkg Skia-macOS-Release-arm64)
        set(_skia_out Release-arm64)
      else()
        set(_skia_pkg Skia-macOS-Release-x64)
        set(_skia_out Release-x64)
      endif()
    elseif(WIN32)
      set(_skia_pkg Skia-Windows-Release-x64)
      set(_skia_out Release-x64)
    else()
      set(_skia_pkg Skia-Linux-Release-x64)
      set(_skia_out Release-x64)
    endif()
    set(JSUI_SKIA_DIR ${CMAKE_SOURCE_DIR}/third_party/skia-prebuilt/${_skia_pkg})
    if(NOT EXISTS ${JSUI_SKIA_DIR}/include)
      set(_url https://github.com/aseprite/skia/releases/download/m124-08a5439a6b/${_skia_pkg}.zip)
      message(STATUS "Downloading prebuilt Skia: ${_url}")
      file(DOWNLOAD ${_url} ${CMAKE_BINARY_DIR}/${_skia_pkg}.zip SHOW_PROGRESS STATUS _st)
      list(GET _st 0 _code)
      if(NOT _code EQUAL 0)
        message(FATAL_ERROR "Skia download failed: ${_st}")
      endif()
      file(MAKE_DIRECTORY ${JSUI_SKIA_DIR})
      file(ARCHIVE_EXTRACT INPUT ${CMAKE_BINARY_DIR}/${_skia_pkg}.zip DESTINATION ${JSUI_SKIA_DIR})
    endif()
    if(NOT JSUI_SKIA_LIB_DIR)
      set(JSUI_SKIA_LIB_DIR ${JSUI_SKIA_DIR}/out/${_skia_out})
    endif()
  endif()
endif()
if(NOT JSUI_SKIA_LIB_DIR)
  set(JSUI_SKIA_LIB_DIR ${JSUI_SKIA_DIR}/out)
endif()

add_library(jsui_skia INTERFACE)
target_include_directories(jsui_skia INTERFACE ${JSUI_SKIA_DIR})
if(WIN32)
  set(_p "")
  set(_s ".lib")
else()
  set(_p "lib")
  set(_s ".a")
endif()
# Order matters for single-pass linkers: dependents before dependencies.
foreach(_lib skparagraph skshaper skunicode skresources skia harfbuzz icu freetype2 png jpeg webp webp_sse41 wuffs zlib expat skcms)
  set(_f ${JSUI_SKIA_LIB_DIR}/${_p}${_lib}${_s})
  if(EXISTS ${_f})
    target_link_libraries(jsui_skia INTERFACE ${_f})
  endif()
endforeach()
target_compile_definitions(jsui_skia INTERFACE SK_RELEASE)
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
  find_package(Fontconfig QUIET)
  find_package(Threads REQUIRED)
  find_package(ZLIB REQUIRED)
  target_link_libraries(jsui_skia INTERFACE Threads::Threads ZLIB::ZLIB ${CMAKE_DL_LIBS})
  if(Fontconfig_FOUND)
    target_link_libraries(jsui_skia INTERFACE Fontconfig::Fontconfig)
  endif()
elseif(APPLE)
  target_link_libraries(jsui_skia INTERFACE "-framework CoreFoundation" "-framework CoreGraphics" "-framework CoreText" "-framework CoreServices")
elseif(WIN32)
  target_link_libraries(jsui_skia INTERFACE user32 gdi32 ole32 opengl32 usp10)
endif()
