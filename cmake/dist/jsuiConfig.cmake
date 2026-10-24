# CMake package of a JSUI release (lib/cmake/jsui/jsuiConfig.cmake): the
# prebuilt shared library as the imported target jsui::jsui.
#
#   list(APPEND CMAKE_PREFIX_PATH /path/to/jsui-<version>-<platform>)
#   find_package(jsui CONFIG REQUIRED)
#   target_link_libraries(game PRIVATE jsui::jsui)
#
# On Windows, ship bin/jsui.dll with bin/icudtl.dat (JSUI_ICU_DATA, the ICU
# data text shaping needs, read from jsui.dll's directory) next to the game.
get_filename_component(_jsui_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(NOT TARGET jsui::jsui)
  add_library(jsui::jsui SHARED IMPORTED)
  set_target_properties(jsui::jsui PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_jsui_root}/include")
  if(WIN32)
    set_target_properties(jsui::jsui PROPERTIES
      IMPORTED_LOCATION "${_jsui_root}/bin/jsui.dll"
      IMPORTED_IMPLIB "${_jsui_root}/lib/jsui.lib"
      INTERFACE_COMPILE_DEFINITIONS JSUI_SHARED)
  elseif(APPLE)
    set_target_properties(jsui::jsui PROPERTIES
      IMPORTED_LOCATION "${_jsui_root}/lib/libjsui.dylib"
      IMPORTED_SONAME "@rpath/libjsui.dylib")
  else()
    set_target_properties(jsui::jsui PROPERTIES
      IMPORTED_LOCATION "${_jsui_root}/lib/libjsui.so"
      IMPORTED_SONAME "libjsui.so")
  endif()
endif()
set(JSUI_ICU_DATA "")
if(WIN32)
  set(JSUI_ICU_DATA "${_jsui_root}/bin/icudtl.dat")
endif()
set(jsui_FOUND TRUE)
