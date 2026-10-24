# jsui_unity: the native plugin of the Unity package (package/), built with
# -DJSUI_BUILD_UNITY=ON. It is the static jsui library plus a few helpers
# (native/jsui_unity.cpp), linked into a loadable module that the package's
# C# code P/Invokes: jsui_unity.bundle on macOS, libjsui_unity.so on Linux,
# jsui_unity.dll on Windows. The build copies it into
# package/Plugins/<platform>/, where Unity picks it up for the Editor and
# standalone players.
add_library(jsui_unity MODULE native/jsui_unity.cpp)
set_target_properties(jsui_unity PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(WIN32)
  # Every jsui_* function is exported from the DLL: compiled dllexport into
  # the static library (as for jsui_shared, which the tools still link
  # statically) and linked whole, so the objects C++ code here does not
  # reference are in too. JSUI_BUILDING: jsui_unity.cpp's own jsuiu_* too.
  if(CMAKE_VERSION VERSION_LESS 3.24)
    message(FATAL_ERROR "integrations/unity on Windows needs CMake 3.24 or newer (WHOLE_ARCHIVE linking)")
  endif()
  target_compile_definitions(jsui PRIVATE JSUI_SHARED)
  target_compile_definitions(jsui_unity PRIVATE JSUI_SHARED JSUI_BUILDING)
  target_link_libraries(jsui_unity PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,jsui>")
else()
  target_link_libraries(jsui_unity PRIVATE jsui)
endif()

# GPU path: the plugin is also a Unity native rendering plugin
# (native/jsui_unity_plugin.cpp) and Skia draws into a RenderTexture on
# Unity's own graphics device, with one backend per graphics API
# (native/jsui_unity_gpu.h), each built when the jsui library has it:
#
#   Metal              macOS, JSUI_GRAPHITE_METAL   native/jsui_unity_metal.mm
#   Vulkan             Linux / Windows, JSUI_VULKAN native/jsui_unity_vulkan.cpp
#   Direct3D 11 / 12   Windows, JSUI_D3D            native/jsui_unity_d3d.cpp
#
# (all three from a Skia built by scripts/build-skia-desktop.sh). The plugin
# API they call is declared in native/unity_api/jsui_unity_api.h, so no Unity
# install is needed; set JSUI_UNITY_PLUGIN_API_DIR (an editor's PluginAPI
# folder: Unity.app/Contents/PluginAPI, Editor/Data/PluginAPI) to build
# against Unity's own headers instead, which also compiles a check that ours
# match them (native/unity_api/check_unity_api.cpp / .mm).
set(JSUI_UNITY_PLUGIN_API_DIR "" CACHE PATH
    "Optional: Unity's native plugin API headers (an editor's PluginAPI folder) to build the GPU path against")
target_include_directories(jsui_unity PRIVATE native/unity_api)
set(_unity_gpu_apis "")
set(_unity_api_headers IUnityGraphics.h IUnityInterface.h)
if(APPLE AND JSUI_GRAPHITE_METAL)
  enable_language(OBJCXX)  # per directory: another subdirectory enabling it does not count here
  target_sources(jsui_unity PRIVATE native/jsui_unity_metal.mm)
  target_compile_definitions(jsui_unity PRIVATE JSUI_UNITY_METAL=1)
  set_source_files_properties(native/jsui_unity_metal.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
  target_link_libraries(jsui_unity PRIVATE "-framework Metal" "-framework Foundation")
  list(APPEND _unity_gpu_apis Metal)
  list(APPEND _unity_api_headers IUnityGraphicsMetal.h)
endif()
if(JSUI_VULKAN AND NOT APPLE)
  target_sources(jsui_unity PRIVATE native/jsui_unity_vulkan.cpp)
  target_compile_definitions(jsui_unity PRIVATE JSUI_UNITY_VULKAN=1)
  list(APPEND _unity_gpu_apis Vulkan)
  list(APPEND _unity_api_headers IUnityGraphicsVulkan.h)
endif()
if(WIN32 AND JSUI_D3D)
  target_sources(jsui_unity PRIVATE native/jsui_unity_d3d.cpp)
  target_compile_definitions(jsui_unity PRIVATE JSUI_UNITY_D3D=1)
  list(APPEND _unity_gpu_apis "Direct3D 11" "Direct3D 12")
  list(APPEND _unity_api_headers IUnityGraphicsD3D11.h IUnityGraphicsD3D12.h)
endif()

if(_unity_gpu_apis)
  target_sources(jsui_unity PRIVATE native/jsui_unity_plugin.cpp)
  target_compile_definitions(jsui_unity PRIVATE JSUI_UNITY_GPU=1)
  string(REPLACE ";" ", " _unity_gpu_list "${_unity_gpu_apis}")
  if(JSUI_UNITY_PLUGIN_API_DIR)
    foreach(_h ${_unity_api_headers})
      if(NOT EXISTS "${JSUI_UNITY_PLUGIN_API_DIR}/${_h}")
        message(FATAL_ERROR "JSUI_UNITY_PLUGIN_API_DIR: no ${_h} in ${JSUI_UNITY_PLUGIN_API_DIR}")
      endif()
    endforeach()
    target_include_directories(jsui_unity PRIVATE ${JSUI_UNITY_PLUGIN_API_DIR})
    target_compile_definitions(jsui_unity PRIVATE JSUI_UNITY_SDK_HEADERS=1)
    # Not linked into anything: it only has to compile. The .mm adds the
    # Metal checks to the .cpp's.
    if(APPLE AND JSUI_GRAPHITE_METAL)
      add_library(jsui_unity_api_check OBJECT native/unity_api/check_unity_api.mm)
    else()
      add_library(jsui_unity_api_check OBJECT native/unity_api/check_unity_api.cpp)
    endif()
    target_include_directories(jsui_unity_api_check PRIVATE native/unity_api ${JSUI_UNITY_PLUGIN_API_DIR})
    # Skia's Vulkan headers (include/third_party/vulkan), as jsui uses them.
    target_include_directories(jsui_unity_api_check PRIVATE ${JSUI_SKIA_DIR})
    target_compile_definitions(jsui_unity_api_check PRIVATE
      $<$<BOOL:${JSUI_VULKAN}>:JSUI_UNITY_CHECK_VULKAN=1> $<$<BOOL:${JSUI_D3D}>:JSUI_UNITY_CHECK_D3D=1>)
    add_dependencies(jsui_unity jsui_unity_api_check)
    message(STATUS "jsui_unity: GPU path (${_unity_gpu_list}), Unity's plugin API from ${JSUI_UNITY_PLUGIN_API_DIR}")
  else()
    message(STATUS "jsui_unity: GPU path (${_unity_gpu_list}), plugin API from native/unity_api")
  endif()
else()
  target_compile_definitions(jsui_unity PRIVATE JSUI_UNITY_GPU=0)
  message(STATUS "jsui_unity: CPU only (the GPU path needs a jsui with JSUI_GRAPHITE_METAL, JSUI_VULKAN or JSUI_D3D)")
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
  set(_unity_platform Windows/x86_64)
  set(_unity_plugin $<TARGET_FILE:jsui_unity>)
  set(_unity_plugin_name jsui_unity.dll)
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
if(WIN32 AND JSUI_ICU_DATA)
  # Skia's ICU reads icudtl.dat from beside the executable or the DLL
  # (SkLoadICU): the Editor loads the plugin from the package, so it goes
  # next to it; for players, Editor/JsuiBuildPostprocess.cs copies it next to
  # the player's copy of the DLL.
  add_custom_command(TARGET jsui_unity POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${JSUI_ICU_DATA} ${JSUI_UNITY_PLUGIN_DIR}/icudtl.dat
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${JSUI_ICU_DATA} $<TARGET_FILE_DIR:jsui_unity>/icudtl.dat
    VERBATIM)
endif()

# A test of the Vulkan backend without Unity (native/test/unity_vulkan_test.cpp):
# it loads the built plugin, plays Unity's part over its own Vulkan device
# (IUnityInterfaces, IUnityGraphics, IUnityGraphicsVulkanV2, the render
# events) and compares what the plugin drew with the CPU raster.
if(JSUI_BUILD_TESTS AND JSUI_VULKAN AND UNIX AND NOT APPLE)
  add_executable(jsui-unity-vulkan-test native/test/unity_vulkan_test.cpp)
  target_include_directories(jsui-unity-vulkan-test PRIVATE native/unity_api ${CMAKE_SOURCE_DIR}/include
    ${JSUI_SKIA_DIR})
  target_link_libraries(jsui-unity-vulkan-test PRIVATE ${CMAKE_DL_LIBS})
  add_dependencies(jsui-unity-vulkan-test jsui_unity)
  target_compile_definitions(jsui-unity-vulkan-test PRIVATE JSUI_UNITY_PLUGIN_PATH="$<TARGET_FILE:jsui_unity>")
endif()
