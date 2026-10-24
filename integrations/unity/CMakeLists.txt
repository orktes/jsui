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
# own Metal device. On by default on macOS with a jsui built with
# JSUI_GRAPHITE_METAL. The plugin API it calls is declared in
# native/unity_api/jsui_unity_api.h, so no Unity install is needed; set
# JSUI_UNITY_PLUGIN_API_DIR (an editor's Unity.app/Contents/PluginAPI) to build
# against Unity's own headers instead, which also compiles a check that ours
# match them (native/unity_api/check_unity_api.mm).
set(JSUI_UNITY_PLUGIN_API_DIR "" CACHE PATH
    "Optional: Unity's native plugin API headers (Unity.app/Contents/PluginAPI) to build the GPU path against")
target_include_directories(jsui_unity PRIVATE native/unity_api)
if(APPLE AND JSUI_GRAPHITE_METAL)
  enable_language(OBJCXX)  # per directory: another subdirectory enabling it does not count here
  target_sources(jsui_unity PRIVATE native/jsui_unity_metal.mm)
  target_compile_definitions(jsui_unity PRIVATE JSUI_UNITY_GPU=1)
  set_source_files_properties(native/jsui_unity_metal.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
  target_link_libraries(jsui_unity PRIVATE "-framework Metal" "-framework Foundation")
  if(JSUI_UNITY_PLUGIN_API_DIR)
    if(NOT EXISTS "${JSUI_UNITY_PLUGIN_API_DIR}/IUnityGraphicsMetal.h")
      message(FATAL_ERROR "JSUI_UNITY_PLUGIN_API_DIR: no IUnityGraphicsMetal.h in ${JSUI_UNITY_PLUGIN_API_DIR}")
    endif()
    target_include_directories(jsui_unity PRIVATE ${JSUI_UNITY_PLUGIN_API_DIR})
    target_compile_definitions(jsui_unity PRIVATE JSUI_UNITY_SDK_HEADERS=1)
    # Not linked into anything: it only has to compile.
    add_library(jsui_unity_api_check OBJECT native/unity_api/check_unity_api.mm)
    target_include_directories(jsui_unity_api_check PRIVATE native/unity_api ${JSUI_UNITY_PLUGIN_API_DIR})
    add_dependencies(jsui_unity jsui_unity_api_check)
    message(STATUS "jsui_unity: GPU path (Metal), Unity's plugin API from ${JSUI_UNITY_PLUGIN_API_DIR}")
  else()
    message(STATUS "jsui_unity: GPU path (Metal), plugin API from native/unity_api")
  endif()
else()
  target_compile_definitions(jsui_unity PRIVATE JSUI_UNITY_GPU=0)
  message(STATUS "jsui_unity: CPU only (the GPU path needs macOS and JSUI_GRAPHITE_METAL)")
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
