# jsui_unity: the native plugin of the Unity package (package/), built with
# -DJSUI_BUILD_UNITY=ON. It is the static jsui library plus a few helpers
# (native/jsui_unity.cpp), linked into a loadable module that the package's
# C# code P/Invokes: jsui_unity.bundle on macOS, libjsui_unity.so on Linux.
# The build copies it into package/Plugins/<platform>/, where Unity picks it
# up for the Editor and standalone players.
add_library(jsui_unity MODULE native/jsui_unity.cpp)
target_link_libraries(jsui_unity PRIVATE jsui)
set_target_properties(jsui_unity PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

# GPU path (native/jsui_unity_metal.mm): the plugin is also a Unity native
# rendering plugin and Skia Graphite draws into a RenderTexture on Unity's
# own Metal device. It needs Unity's plugin API headers (not vendored: they
# come with every editor, in Unity.app/Contents/PluginAPI) and a jsui built
# with JSUI_GRAPHITE_METAL. Without either the plugin is CPU-only.
set(_unity_api_default "")
if(APPLE)
  file(GLOB _unity_api_dirs /Applications/Unity/Hub/Editor/*/Unity.app/Contents/PluginAPI)
  if(_unity_api_dirs)
    list(SORT _unity_api_dirs COMPARE NATURAL ORDER DESCENDING)
    list(GET _unity_api_dirs 0 _unity_api_default)
  endif()
endif()
set(JSUI_UNITY_PLUGIN_API_DIR "${_unity_api_default}" CACHE PATH
    "Unity's native plugin API headers (Unity.app/Contents/PluginAPI), for the GPU path")
if(APPLE AND JSUI_GRAPHITE_METAL AND EXISTS "${JSUI_UNITY_PLUGIN_API_DIR}/IUnityGraphicsMetal.h")
  target_sources(jsui_unity PRIVATE native/jsui_unity_metal.mm)
  target_include_directories(jsui_unity PRIVATE ${JSUI_UNITY_PLUGIN_API_DIR})
  target_compile_definitions(jsui_unity PRIVATE JSUI_UNITY_GPU=1)
  set_source_files_properties(native/jsui_unity_metal.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
  target_link_libraries(jsui_unity PRIVATE "-framework Metal" "-framework Foundation")
  message(STATUS "jsui_unity: GPU path (Metal), Unity plugin API from ${JSUI_UNITY_PLUGIN_API_DIR}")
else()
  target_compile_definitions(jsui_unity PRIVATE JSUI_UNITY_GPU=0)
  message(STATUS "jsui_unity: CPU only (needs macOS, JSUI_GRAPHITE_METAL and JSUI_UNITY_PLUGIN_API_DIR)")
endif()

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
if(APPLE)
  # A directory (the .bundle).
  add_custom_command(TARGET jsui_unity POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${JSUI_UNITY_PLUGIN_DIR}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${JSUI_UNITY_PLUGIN_DIR}/${_unity_plugin_name}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${_unity_plugin} ${JSUI_UNITY_PLUGIN_DIR}/${_unity_plugin_name}
    VERBATIM)
else()
  # A single file.
  add_custom_command(TARGET jsui_unity POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${JSUI_UNITY_PLUGIN_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy ${_unity_plugin} ${JSUI_UNITY_PLUGIN_DIR}/${_unity_plugin_name}
    VERBATIM)
endif()
