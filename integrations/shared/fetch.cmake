# jsui_fetch_archive(<var> NAME <name> URL <url> SHA256 <hash> [DIR <subdir>])
#
# Downloads a release tarball once into build/_deps (SHA-256 checked),
# extracts it and sets <var> to the source tree (<subdir> inside the
# archive). A cache variable JSUI_<NAME>_DIR pointing at an existing source
# tree skips the download. <var> is empty when the download fails, so the
# caller can skip its example with a warning instead of failing the configure.
function(jsui_fetch_archive out_var)
  cmake_parse_arguments(A "" "NAME;URL;SHA256;DIR" "" ${ARGN})
  string(TOUPPER "${A_NAME}" _upper)
  set(JSUI_${_upper}_DIR "" CACHE PATH "${A_NAME} source tree to use instead of downloading ${A_URL}")
  if(JSUI_${_upper}_DIR)
    set(${out_var} "${JSUI_${_upper}_DIR}" PARENT_SCOPE)
    return()
  endif()
  set(_root ${CMAKE_BINARY_DIR}/_deps/${A_NAME}-src)
  set(_src ${_root}/${A_DIR})
  string(SUBSTRING "${A_SHA256}" 0 12 _short)
  set(_stamp ${_root}/.jsui-${_short})
  if(NOT EXISTS ${_stamp})
    set(_tgz ${CMAKE_BINARY_DIR}/_deps/${A_NAME}-${_short}.tar.gz)
    message(STATUS "Downloading ${A_NAME}: ${A_URL}")
    file(DOWNLOAD ${A_URL} ${_tgz} STATUS _st)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
      message(WARNING "${A_NAME} download failed: ${_st}")
      set(${out_var} "" PARENT_SCOPE)
      return()
    endif()
    file(SHA256 ${_tgz} _hash)
    if(NOT _hash STREQUAL A_SHA256)
      message(WARNING "${A_NAME} download has an unexpected SHA-256 (${_hash}, expected ${A_SHA256})")
      set(${out_var} "" PARENT_SCOPE)
      return()
    endif()
    file(REMOVE_RECURSE ${_root})
    file(MAKE_DIRECTORY ${_root})
    file(ARCHIVE_EXTRACT INPUT ${_tgz} DESTINATION ${_root})
    file(TOUCH ${_stamp})
  endif()
  set(${out_var} "${_src}" PARENT_SCOPE)
endfunction()

# The shared UI bundle of the examples (integrations/shared/ui/app.jsx),
# built by `npx jsui build` into integrations/shared/ui/dist/ by default.
set(JSUI_EXAMPLE_BUNDLE "${CMAKE_SOURCE_DIR}/integrations/shared/ui/dist/bundle.js" CACHE FILEPATH
    "Default UI bundle of the engine examples (npx jsui build integrations/shared/ui/app.jsx -o <this>)")

# Common settings of an example executable.
function(jsui_example_target target)
  target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/integrations/shared)
  target_compile_definitions(${target} PRIVATE JSUI_EXAMPLE_BUNDLE="${JSUI_EXAMPLE_BUNDLE}")
  target_link_libraries(${target} PRIVATE jsui)
  jsui_copy_icu_data(${target})
  # Next to the other tools: build/jsui-raylib, build/jsui-sdl3, ...
  set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endfunction()
