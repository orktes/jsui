# jsui_unity: the native plugin of the Unity package (package/), built with
# -DJSUI_BUILD_UNITY=ON. It is the static jsui library plus a few helpers
# (native/jsui_unity.cpp), linked into a loadable module that the package's
# C# code P/Invokes: jsui_unity.bundle on macOS, libjsui_unity.so on Linux.
# The build copies it into package/Plugins/<platform>/, where Unity picks it
# up for the Editor and standalone players.
add_library(jsui_unity MODULE native/jsui_unity.cpp)
target_link_libraries(jsui_unity PRIVATE jsui)
set_target_properties(jsui_unity PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

if(APPLE)
  set(_unity_platform macOS)
  # A loadable bundle (jsui_unity.bundle/Contents/MacOS/jsui_unity), which is
  # what Unity's macOS plugin importer expects.
  set_target_properties(jsui_unity PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION bundle
    MACOSX_BUNDLE_BUNDLE_NAME jsui_unity MACOSX_BUNDLE_GUI_IDENTIFIER com.orktes.jsui.unity)
  set(_unity_plugin $<TARGET_BUNDLE_DIR:jsui_unity>)
  set(_unity_plugin_name jsui_unity.bundle)
elseif(WIN32)
  message(WARNING "integrations/unity: Windows is not supported yet (jsui exports need JSUI_SHARED); skipping")
  return()
else()
  set(_unity_platform Linux/x86_64)
  set_target_properties(jsui_unity PROPERTIES PREFIX lib)
  set(_unity_plugin $<TARGET_FILE:jsui_unity>)
  set(_unity_plugin_name libjsui_unity.so)
endif()

set(JSUI_UNITY_PLUGIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/package/Plugins/${_unity_platform} CACHE PATH
    "Where the jsui_unity plugin is copied (the Unity package's Plugins folder)")
add_custom_command(TARGET jsui_unity POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${JSUI_UNITY_PLUGIN_DIR}
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${JSUI_UNITY_PLUGIN_DIR}/${_unity_plugin_name}
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${_unity_plugin} ${JSUI_UNITY_PLUGIN_DIR}/${_unity_plugin_name}
  VERBATIM)
if(NOT APPLE)
  # A single file, not a directory.
  add_custom_command(TARGET jsui_unity POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${JSUI_UNITY_PLUGIN_DIR}/${_unity_plugin_name}
    COMMAND ${CMAKE_COMMAND} -E copy ${_unity_plugin} ${JSUI_UNITY_PLUGIN_DIR}/${_unity_plugin_name}
    VERBATIM)
endif()
