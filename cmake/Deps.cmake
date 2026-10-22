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
# JSUI keeps layout fractional (point scale factor 0), but Yoga still walks
# the whole tree after every layout pass to "round" it. Compile a copy of
# PixelGrid.cpp that returns at once when there is nothing to round (the
# fetched sources stay untouched).
set(_yoga_pixel_grid ${yoga_SOURCE_DIR}/yoga/algorithm/PixelGrid.cpp)
file(READ ${_yoga_pixel_grid} _yoga_pg)
set(_yoga_pg_anchor "  const auto pointScaleFactor = node->getConfig()->getPointScaleFactor();\n")
string(FIND "${_yoga_pg}" "${_yoga_pg_anchor}" _yoga_pg_at)
if(_yoga_pg_at GREATER -1)
  string(REPLACE "${_yoga_pg_anchor}"
    "${_yoga_pg_anchor}  if (pointScaleFactor == 0.0f) return;  // jsui: fractional layout, nothing to round\n"
    _yoga_pg "${_yoga_pg}")
  set(_yoga_pg_out ${CMAKE_BINARY_DIR}/yoga-patched/PixelGrid.cpp)
  file(CONFIGURE OUTPUT ${_yoga_pg_out} CONTENT "${_yoga_pg}" @ONLY)
  list(REMOVE_ITEM JSUI_YOGA_SOURCES ${_yoga_pixel_grid})
  list(APPEND JSUI_YOGA_SOURCES ${_yoga_pg_out})
else()
  message(WARNING "Yoga PixelGrid.cpp changed: skipping the no-rounding fast path")
endif()
# justifyMainAxis asks isBaselineLayout(node) once per flex line, and that
# scans every child: O(lines x children) for a wrapping grid. Remember the
# answer for the node within one layout pass (styles cannot change during
# a pass; the generation count changes with every YGNodeCalculateLayout).
set(_yoga_calc ${yoga_SOURCE_DIR}/yoga/algorithm/CalculateLayout.cpp)
file(READ ${_yoga_calc} _yoga_cl)
set(_yoga_cl_anchor "  bool isNodeBaselineLayout = isBaselineLayout(node);\n")
string(FIND "${_yoga_cl}" "${_yoga_cl_anchor}" _yoga_cl_at)
if(_yoga_cl_at GREATER -1)
  string(REPLACE "${_yoga_cl_anchor}" "\
  // jsui: once per node and layout pass instead of once per flex line.\n\
  static thread_local const yoga::Node* jsuiBaselineNode = nullptr;\n\
  static thread_local uint32_t jsuiBaselineGen = 0;\n\
  static thread_local bool jsuiBaselineValue = false;\n\
  const uint32_t jsuiGen = gCurrentGenerationCount.load(std::memory_order_relaxed);\n\
  if (node != jsuiBaselineNode || jsuiGen != jsuiBaselineGen) {\n\
    jsuiBaselineNode = node;\n\
    jsuiBaselineGen = jsuiGen;\n\
    jsuiBaselineValue = isBaselineLayout(node);\n\
  }\n\
  bool isNodeBaselineLayout = jsuiBaselineValue;\n" _yoga_cl "${_yoga_cl}")
  set(_yoga_cl_out ${CMAKE_BINARY_DIR}/yoga-patched/CalculateLayout.cpp)
  file(CONFIGURE OUTPUT ${_yoga_cl_out} CONTENT "${_yoga_cl}" @ONLY)
  list(REMOVE_ITEM JSUI_YOGA_SOURCES ${_yoga_calc})
  list(APPEND JSUI_YOGA_SOURCES ${_yoga_cl_out})
else()
  message(WARNING "Yoga CalculateLayout.cpp changed: skipping the baseline-layout memo")
endif()
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
  elseif(IOS)
    # Built from source by scripts/build-skia-ios.sh (no prebuilt for iOS):
    # one library slice per SDK + architecture, picked from the toolchain.
    set(JSUI_SKIA_DIR ${CMAKE_SOURCE_DIR}/third_party/skia-ios)
    if(NOT EXISTS ${JSUI_SKIA_DIR}/include)
      message(FATAL_ERROR "iOS Skia not found at ${JSUI_SKIA_DIR}. Run scripts/build-skia-ios.sh first.")
    endif()
    if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
      set(_ios_sdk iphonesimulator)
    else()
      set(_ios_sdk iphoneos)
    endif()
    if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
      set(_ios_cpu x64)
    else()
      set(_ios_cpu arm64)
    endif()
    if(NOT JSUI_SKIA_LIB_DIR)
      set(JSUI_SKIA_LIB_DIR ${JSUI_SKIA_DIR}/out/${_ios_sdk}-${_ios_cpu})
    endif()
    if(NOT EXISTS ${JSUI_SKIA_LIB_DIR}/libskia.a)
      message(FATAL_ERROR "No iOS Skia slice at ${JSUI_SKIA_LIB_DIR}: SLICES=${_ios_sdk}-${_ios_cpu} scripts/build-skia-ios.sh")
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
# (iOS links every library of its Skia slice, below.)
if(IOS)
  set(_jsui_skia_named_libs "")
else()
  set(_jsui_skia_named_libs skparagraph skshaper skunicode skresources skia harfbuzz icu freetype2 png jpeg webp webp_sse41 wuffs zlib expat skcms)
endif()
foreach(_lib ${_jsui_skia_named_libs})
  set(_f ${JSUI_SKIA_LIB_DIR}/${_p}${_lib}${_s})
  if(EXISTS ${_f})
    target_link_libraries(jsui_skia INTERFACE ${_f})
  endif()
endforeach()
target_compile_definitions(jsui_skia INTERFACE SK_RELEASE)
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
  find_package(Fontconfig QUIET)
  find_package(Threads REQUIRED)
  target_link_libraries(jsui_skia INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
  if(Fontconfig_FOUND)
    target_link_libraries(jsui_skia INTERFACE Fontconfig::Fontconfig)
  endif()
elseif(IOS)
  # The source-built Skia records the defines its public headers need
  # (SK_GANESH, SK_METAL, SK_SHAPER_*...); compile with exactly those.
  if(EXISTS "${JSUI_SKIA_LIB_DIR}/defines.txt")
    file(STRINGS "${JSUI_SKIA_LIB_DIR}/defines.txt" _jsui_skia_defs)
    target_compile_definitions(jsui_skia INTERFACE ${_jsui_skia_defs})
  endif()
  # Every library of the slice (m157 adds skunicode_core / _icu, jpeg12 /
  # 16, ...); ld64 resolves archives regardless of order.
  file(GLOB _jsui_skia_libs "${JSUI_SKIA_LIB_DIR}/lib*.a")
  target_link_libraries(jsui_skia INTERFACE ${_jsui_skia_libs})
  target_link_libraries(jsui_skia INTERFACE "-framework CoreFoundation" "-framework CoreGraphics" "-framework CoreText"
    "-framework Foundation" "-framework UIKit" "-framework Metal")
elseif(APPLE)
  target_link_libraries(jsui_skia INTERFACE "-framework CoreFoundation" "-framework CoreGraphics" "-framework CoreText" "-framework CoreServices")
elseif(WIN32)
  target_link_libraries(jsui_skia INTERFACE user32 gdi32 ole32 opengl32 usp10)
endif()

# On Windows the prebuilt Skia's ICU loads its data from icudtl.dat next to
# the executable (SkLoadICU); without it SkUnicode is unavailable and text
# shaping fails. Embedders ship the same file beside their game binary.
set(JSUI_ICU_DATA "")
if(WIN32 AND EXISTS ${JSUI_SKIA_DIR}/third_party/externals/icu/flutter/icudtl.dat)
  set(JSUI_ICU_DATA ${JSUI_SKIA_DIR}/third_party/externals/icu/flutter/icudtl.dat)
endif()
function(jsui_copy_icu_data target)
  if(JSUI_ICU_DATA)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${JSUI_ICU_DATA} $<TARGET_FILE_DIR:${target}>/icudtl.dat)
  endif()
endfunction()

# The prebuilt Skia's FreeType (ftgzip) needs zlib's unprefixed inflate*
# symbols. Use the system zlib where there is one (Linux, macOS SDK) and
# build it from source elsewhere (Windows).
if(NOT EMSCRIPTEN)
  find_package(ZLIB QUIET)
  if(ZLIB_FOUND)
    target_link_libraries(jsui_skia INTERFACE ZLIB::ZLIB)
  else()
    FetchContent_Declare(zlib
      GIT_REPOSITORY https://github.com/madler/zlib.git
      GIT_TAG v1.3.1
      GIT_SHALLOW TRUE)
    FetchContent_GetProperties(zlib)
    if(NOT zlib_POPULATED)
      FetchContent_Populate(zlib)
    endif()
    # Only the codec is needed; the gz* file API wants platform headers.
    file(GLOB JSUI_ZLIB_SOURCES ${zlib_SOURCE_DIR}/*.c)
    list(FILTER JSUI_ZLIB_SOURCES EXCLUDE REGEX "/gz[a-z]+\\.c$")
    add_library(jsui_zlib STATIC ${JSUI_ZLIB_SOURCES})
    target_include_directories(jsui_zlib PUBLIC ${zlib_SOURCE_DIR})
    if(MSVC)
      target_compile_definitions(jsui_zlib PRIVATE _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
    endif()
    target_link_libraries(jsui_skia INTERFACE jsui_zlib)
  endif()
endif()
