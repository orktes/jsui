# Profile-guided optimization of QuickJS alone (included by cmake/Deps.cmake
# after the jsui_quickjs target; scripts/pgo-quickjs.sh drives both stages).
#
#   -DJSUI_QUICKJS_PGO=generate  instrument QuickJS; programs linking it write
#                                raw profiles into JSUI_QUICKJS_PROFILE (a
#                                directory, default <build>/pgo/raw)
#   -DJSUI_QUICKJS_PGO=use       optimize QuickJS with JSUI_QUICKJS_PROFILE:
#                                Clang / AppleClang: an indexed .profdata file
#                                (llvm-profdata merge), GCC: the directory of
#                                .gcda files the generate build wrote
#   (empty / OFF)                a normal build (the default)
#
# Only QuickJS's compile flags change, so switching modes in one build
# directory recompiles four C files and relinks; everything else is reused.
#
# A profile that is missing, empty, unreadable (another compiler version, a
# newer llvm-profdata format) or stale never fails the build: configure warns
# and builds QuickJS without it, and out-of-date / unprofiled functions are
# not diagnosed (-Wno-profile-instr-*, -Wno-missing-profile).

set(JSUI_QUICKJS_PGO "" CACHE STRING "Profile-guided optimization of QuickJS: generate, use, or empty for none")
set_property(CACHE JSUI_QUICKJS_PGO PROPERTY STRINGS "" generate use)
set(JSUI_QUICKJS_PROFILE "" CACHE PATH
  "PGO profile of QuickJS: the raw profile directory (generate) or the .profdata file / .gcda directory (use)")

set(_pgo_mode "${JSUI_QUICKJS_PGO}")
string(TOLOWER "${_pgo_mode}" _pgo_mode)
if(_pgo_mode STREQUAL "off" OR _pgo_mode STREQUAL "none" OR _pgo_mode STREQUAL "0" OR _pgo_mode STREQUAL "false")
  set(_pgo_mode "")
endif()

# What scripts/pgo-quickjs.sh needs to know about the toolchain (which
# merge tool, where the raw profiles go).
set(_pgo_state ${CMAKE_BINARY_DIR}/pgo/toolchain.env)

if(_pgo_mode STREQUAL "")
  file(REMOVE ${_pgo_state})
  return()
endif()

set(_pgo_kind "")
if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND NOT CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  set(_pgo_kind clang)
elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  set(_pgo_kind gcc)
endif()
if(NOT _pgo_kind OR (EMSCRIPTEN AND _pgo_mode STREQUAL "generate"))
  message(WARNING "JSUI_QUICKJS_PGO=${JSUI_QUICKJS_PGO}: not supported with ${CMAKE_C_COMPILER_ID}"
    " (${CMAKE_C_COMPILER_FRONTEND_VARIANT}) here; building QuickJS without PGO")
  file(REMOVE ${_pgo_state})
  return()
endif()

include(CheckCCompilerFlag)

if(_pgo_mode STREQUAL "generate")
  set(_pgo_dir "${JSUI_QUICKJS_PROFILE}")
  if(NOT _pgo_dir)
    set(_pgo_dir ${CMAKE_BINARY_DIR}/pgo/raw)
  endif()
  get_filename_component(_pgo_dir "${_pgo_dir}" ABSOLUTE BASE_DIR ${CMAKE_BINARY_DIR})
  file(MAKE_DIRECTORY ${_pgo_dir})
  # The profile runtime is a separate package on some distributions
  # (Ubuntu: libclang-rt-<N>-dev); without it nothing would link.
  if(_pgo_kind STREQUAL "clang")
    set(_pgo_check_flag -fprofile-instr-generate)
  else()
    set(_pgo_check_flag -fprofile-generate)
  endif()
  include(CheckCSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS ${_pgo_check_flag})
  set(CMAKE_REQUIRED_LINK_OPTIONS ${_pgo_check_flag})
  set(CMAKE_REQUIRED_QUIET ON)
  unset(JSUI_QUICKJS_PGO_LINKS CACHE)
  check_c_source_compiles("int main(void) { return 0; }" JSUI_QUICKJS_PGO_LINKS)
  unset(CMAKE_REQUIRED_FLAGS)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
  unset(CMAKE_REQUIRED_QUIET)
  if(NOT JSUI_QUICKJS_PGO_LINKS)
    message(WARNING "JSUI_QUICKJS_PGO=generate: ${CMAKE_C_COMPILER} cannot link a program with"
      " ${_pgo_check_flag} (no profile runtime? Ubuntu's clang needs libclang-rt-dev); building QuickJS"
      " without instrumentation")
    file(REMOVE ${_pgo_state})
    return()
  endif()
  if(_pgo_kind STREQUAL "clang")
    # %p: one file per process (the training runs several programs).
    set(_pgo_flag "-fprofile-instr-generate=${_pgo_dir}/qjs-%p.profraw")
    target_compile_options(jsui_quickjs PRIVATE ${_pgo_flag})
    # The profile runtime goes into whatever links QuickJS.
    target_link_options(jsui_quickjs INTERFACE ${_pgo_flag})
  else()
    set(_pgo_gcc_flags -fprofile-generate=${_pgo_dir})
    # The .gcda names are the object paths with '/' turned into '#'; relative
    # to the build directory they match in any build directory.
    check_c_compiler_flag(-fprofile-prefix-path=${CMAKE_BINARY_DIR} JSUI_HAVE_PROFILE_PREFIX_PATH)
    if(JSUI_HAVE_PROFILE_PREFIX_PATH)
      list(APPEND _pgo_gcc_flags -fprofile-prefix-path=${CMAKE_BINARY_DIR})
    endif()
    target_compile_options(jsui_quickjs PRIVATE ${_pgo_gcc_flags})
    target_link_options(jsui_quickjs INTERFACE -fprofile-generate)
  endif()
  message(STATUS "QuickJS PGO: instrumented (${_pgo_kind}), raw profiles in ${_pgo_dir}")
  file(WRITE ${_pgo_state}
    "PGO_KIND=${_pgo_kind}\nPGO_RAW_DIR='${_pgo_dir}'\nPGO_CC='${CMAKE_C_COMPILER}'\n"
    "PGO_CC_ID=${CMAKE_C_COMPILER_ID}\nPGO_CC_VERSION=${CMAKE_C_COMPILER_VERSION}\n")
  return()
endif()

if(NOT _pgo_mode STREQUAL "use")
  message(WARNING "JSUI_QUICKJS_PGO=${JSUI_QUICKJS_PGO}: expected generate or use; building QuickJS without PGO")
  file(REMOVE ${_pgo_state})
  return()
endif()
file(REMOVE ${_pgo_state})

set(_pgo_profile "${JSUI_QUICKJS_PROFILE}")
if(_pgo_profile)
  get_filename_component(_pgo_profile "${_pgo_profile}" ABSOLUTE BASE_DIR ${CMAKE_BINARY_DIR})
endif()

if(_pgo_kind STREQUAL "clang")
  if(NOT _pgo_profile OR IS_DIRECTORY "${_pgo_profile}" OR NOT EXISTS "${_pgo_profile}")
    message(WARNING "QuickJS PGO: no profile at '${JSUI_QUICKJS_PROFILE}' (JSUI_QUICKJS_PROFILE, a .profdata"
      " file from scripts/pgo-quickjs.sh); building QuickJS without PGO")
    return()
  endif()
  # A profile this compiler cannot read (another LLVM's format, corrupt)
  # is a hard compiler error, so try it first.
  set(_pgo_flags -fprofile-instr-use=${_pgo_profile}
    -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled -Wno-profile-instr-missing)
  file(SHA256 "${_pgo_profile}" _pgo_hash)
  if(NOT JSUI_QUICKJS_PGO_CHECKED STREQUAL "${_pgo_hash};${CMAKE_C_COMPILER};${CMAKE_C_COMPILER_VERSION}")
    set(_pgo_test ${CMAKE_BINARY_DIR}/pgo/check.c)
    file(WRITE ${_pgo_test} "int jsui_pgo_check(int x) { return x * 3; }\n")
    set(_pgo_saved_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    try_compile(_pgo_ok ${CMAKE_BINARY_DIR}/pgo/check
      SOURCES ${_pgo_test}
      COMPILE_DEFINITIONS -fprofile-instr-use=${_pgo_profile}
      OUTPUT_VARIABLE _pgo_out)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_pgo_saved_type}")
    if(NOT _pgo_ok)
      string(REGEX MATCH "[^\n]*(error|rror:)[^\n]*" _pgo_err "${_pgo_out}")
      message(WARNING "QuickJS PGO: ${CMAKE_C_COMPILER} cannot use ${_pgo_profile} (${_pgo_err});"
        " building QuickJS without PGO. Regenerate it with scripts/pgo-quickjs.sh.")
      unset(JSUI_QUICKJS_PGO_CHECKED CACHE)
      return()
    endif()
    set(JSUI_QUICKJS_PGO_CHECKED "${_pgo_hash};${CMAKE_C_COMPILER};${CMAKE_C_COMPILER_VERSION}"
      CACHE INTERNAL "QuickJS PGO profile known to work with this compiler")
  endif()
  set(_pgo_depends ${_pgo_profile})
  set(_pgo_id "${_pgo_hash}")
else()
  if(_pgo_profile AND IS_DIRECTORY "${_pgo_profile}")
    file(GLOB_RECURSE _pgo_depends "${_pgo_profile}/*.gcda")
  endif()
  set(_pgo_id "")
  foreach(_f IN LISTS _pgo_depends)
    file(TIMESTAMP "${_f}" _t "%s" UTC)
    string(APPEND _pgo_id "${_f}${_t};")
  endforeach()
  string(SHA256 _pgo_id "${_pgo_id}")
  if(NOT _pgo_depends)
    message(WARNING "QuickJS PGO: no .gcda files in '${JSUI_QUICKJS_PROFILE}' (JSUI_QUICKJS_PROFILE, the"
      " directory scripts/pgo-quickjs.sh trained); building QuickJS without PGO")
    return()
  endif()
  # -fprofile-partial-training: code the training did not reach is optimized
  # normally, not for size. GCC skips data of another GCC version or of a
  # changed function with a warning (not an error, which -Wcoverage-mismatch
  # would otherwise be).
  set(_pgo_flags -fprofile-use=${_pgo_profile} -fprofile-partial-training
    -Wno-missing-profile -Wno-error=coverage-mismatch)
  check_c_compiler_flag(-fprofile-prefix-path=${CMAKE_BINARY_DIR} JSUI_HAVE_PROFILE_PREFIX_PATH)
  if(JSUI_HAVE_PROFILE_PREFIX_PATH)
    list(APPEND _pgo_flags -fprofile-prefix-path=${CMAKE_BINARY_DIR})
  endif()
  check_c_compiler_flag(-fprofile-partial-training JSUI_HAVE_PROFILE_PARTIAL_TRAINING)
  if(NOT JSUI_HAVE_PROFILE_PARTIAL_TRAINING)
    list(REMOVE_ITEM _pgo_flags -fprofile-partial-training)
  endif()
endif()

target_compile_options(jsui_quickjs PRIVATE ${_pgo_flags})
# A new profile under the same path must recompile QuickJS, and a removed one
# must not break the build: the profile is a configure dependency (changed or
# deleted, CMake reruns and checks it again), and its hash is part of
# QuickJS's command line (a changed command line recompiles).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_pgo_depends})
string(SUBSTRING "${_pgo_id}" 0 16 _pgo_id)
target_compile_definitions(jsui_quickjs PRIVATE JSUI_QUICKJS_PGO_PROFILE_ID=${_pgo_id})
message(STATUS "QuickJS PGO: optimizing with ${_pgo_profile}")
