# JsuiDemo for Android: the native library of a NativeActivity app (no Java)
# that renders a JSUI bundle with Skia Graphite on Vulkan (libvulkan.so is
# opened at run time), Ganesh on GLES through EGL and the CPU raster into the
# window's buffer as the fallbacks, and forwards touch, keys and
# gamepads. scripts/build-android.sh builds it with the NDK's CMake toolchain
# and packages libjsuidemo.so, AndroidManifest.xml and the UI bundle (as
# assets/ui/) into an APK.

set(JSUI_ANDROID_GLUE "${ANDROID_NDK}/sources/android/native_app_glue")
add_library(jsui_native_app_glue STATIC ${JSUI_ANDROID_GLUE}/android_native_app_glue.c)
target_include_directories(jsui_native_app_glue PUBLIC ${JSUI_ANDROID_GLUE})

add_library(jsuidemo SHARED main.cpp)
target_link_libraries(jsuidemo PRIVATE jsui jsui_native_app_glue android log EGL GLESv3)
# The NativeActivity entry point lives in the glue: keep it exported.
target_link_options(jsuidemo PRIVATE "-u" "ANativeActivity_onCreate" "-Wl,-z,max-page-size=16384")
