# WOFF / WOFF2 decoding for jsui_register_font and Fonts.load. Skia's font
# managers take plain sfnt data (TTF / OTF / TTC) on every platform we ship,
# so web fonts are unpacked to sfnt first (src/text/font_decode.cpp):
#
#   WOFF   - zlib, a private copy built with Z_PREFIX (Skia's zlib is
#            Cr_z_-prefixed, the system one may be missing on Windows/WASM)
#   WOFF2  - google/woff2's decoder on brotli. A Skia build that already
#            ships libbrotli (the wasm and source-built Skias, for FreeType)
#            provides the brotli symbols; otherwise the decoder is built here.
#
#   jsui_webfonts(<target>)  - links the decoders into <target>.

option(JSUI_WOFF2 "Decode WOFF2 fonts (google/woff2 + brotli)" ON)

if(TARGET jsui_zlib)
  # Deps.cmake already builds this zlib for Skia (no system zlib, e.g. Windows):
  # a second copy would clash on its unprefixed internals (z_errmsg).
  add_library(jsui_zinflate INTERFACE)
  target_link_libraries(jsui_zinflate INTERFACE jsui_zlib)
else()
FetchContent_Declare(zlib
  GIT_REPOSITORY https://github.com/madler/zlib.git
  GIT_TAG v1.3.1
  GIT_SHALLOW TRUE)
FetchContent_GetProperties(zlib)
if(NOT zlib_POPULATED)
  FetchContent_Populate(zlib)
endif()
set(_jsui_zinflate adler32 crc32 inffast inflate inftrees uncompr zutil)
list(TRANSFORM _jsui_zinflate PREPEND ${zlib_SOURCE_DIR}/)
list(TRANSFORM _jsui_zinflate APPEND .c)
add_library(jsui_zinflate STATIC ${_jsui_zinflate})
target_include_directories(jsui_zinflate PUBLIC ${zlib_SOURCE_DIR})
target_compile_definitions(jsui_zinflate PUBLIC Z_PREFIX)
if(MSVC)
  target_compile_definitions(jsui_zinflate PRIVATE _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(jsui_zinflate PRIVATE -w)
endif()
if(NOT WIN32)
  target_compile_definitions(jsui_zinflate PRIVATE HAVE_UNISTD_H)
endif()
endif()

if(JSUI_WOFF2)
  FetchContent_Declare(brotli
    GIT_REPOSITORY https://github.com/google/brotli.git
    GIT_TAG v1.1.0
    GIT_SHALLOW TRUE)
  FetchContent_GetProperties(brotli)
  if(NOT brotli_POPULATED)
    FetchContent_Populate(brotli)
  endif()
  FetchContent_Declare(woff2
    GIT_REPOSITORY https://github.com/google/woff2.git
    GIT_TAG v1.0.2
    GIT_SHALLOW TRUE)
  FetchContent_GetProperties(woff2)
  if(NOT woff2_POPULATED)
    FetchContent_Populate(woff2)
  endif()

  if(WIN32)
    set(_jsui_skia_brotli ${JSUI_SKIA_LIB_DIR}/brotli.lib)
  else()
    set(_jsui_skia_brotli ${JSUI_SKIA_LIB_DIR}/libbrotli.a)
  endif()
  set(_jsui_woff2_sources woff2_dec woff2_out woff2_common table_tags variable_length)
  list(TRANSFORM _jsui_woff2_sources PREPEND ${woff2_SOURCE_DIR}/src/)
  list(TRANSFORM _jsui_woff2_sources APPEND .cc)
  set(_jsui_brotli_sources "")
  if(NOT EXISTS ${_jsui_skia_brotli})
    file(GLOB _jsui_brotli_sources ${brotli_SOURCE_DIR}/c/common/*.c ${brotli_SOURCE_DIR}/c/dec/*.c)
  endif()
  add_library(jsui_woff2 STATIC ${_jsui_woff2_sources} ${_jsui_brotli_sources})
  target_include_directories(jsui_woff2 PUBLIC ${woff2_SOURCE_DIR}/include PRIVATE ${brotli_SOURCE_DIR}/c/include)
  if(EXISTS ${_jsui_skia_brotli})
    target_link_libraries(jsui_woff2 PUBLIC ${_jsui_skia_brotli})
  endif()
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(jsui_woff2 PRIVATE -w)
  endif()
endif()

function(jsui_webfonts target)
  target_link_libraries(${target} PRIVATE jsui_zinflate)
  target_compile_definitions(${target} PRIVATE JSUI_WOFF=1)
  if(JSUI_WOFF2)
    target_link_libraries(${target} PRIVATE jsui_woff2)
    target_compile_definitions(${target} PRIVATE JSUI_WOFF2=1)
  endif()
endfunction()
