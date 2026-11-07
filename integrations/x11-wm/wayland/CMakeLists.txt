# jsui-wl: the same shell on Wayland, as a wlroots 0.17 compositor
# (main.cpp). Linux only; built with -DJSUI_BUILD_WAYLAND_WM=ON. Needs
# libwlroots-dev (0.17), wayland-protocols, libxkbcommon-dev,
# libpixman-1-dev and libdrm-dev.
find_package(PkgConfig REQUIRED)
pkg_check_modules(WLR REQUIRED IMPORTED_TARGET wlroots>=0.17 wlroots<0.18 wayland-server xkbcommon pixman-1 libdrm)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)
set(_gen ${CMAKE_CURRENT_BINARY_DIR}/protocols)
file(MAKE_DIRECTORY ${_gen})
add_custom_command(OUTPUT ${_gen}/xdg-shell-protocol.h
  COMMAND ${WAYLAND_SCANNER} server-header ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml ${_gen}/xdg-shell-protocol.h
  VERBATIM)
# text-input-unstable-v2 (KDE / Qt; not in wayland-protocols): the text
# input Qt 5's Wayland client speaks (text_input_v2.h).
set(_ti2 ${CMAKE_CURRENT_SOURCE_DIR}/protocols/text-input-unstable-v2.xml)
add_custom_command(OUTPUT ${_gen}/text-input-unstable-v2-protocol.h ${_gen}/text-input-unstable-v2-protocol.c
  COMMAND ${WAYLAND_SCANNER} server-header ${_ti2} ${_gen}/text-input-unstable-v2-protocol.h
  COMMAND ${WAYLAND_SCANNER} private-code ${_ti2} ${_gen}/text-input-unstable-v2-protocol.c
  DEPENDS ${_ti2}
  VERBATIM)
# wp_fractional_scale_v1 (staging): apps that render at the shell's scale
# (Chromium / Electron, GTK 4, Qt 6) learn it from here (fractional_scale_v1.h).
set(_fs ${WAYLAND_PROTOCOLS_DIR}/staging/fractional-scale/fractional-scale-v1.xml)
add_custom_command(OUTPUT ${_gen}/fractional-scale-v1-protocol.h ${_gen}/fractional-scale-v1-protocol.c
  COMMAND ${WAYLAND_SCANNER} server-header ${_fs} ${_gen}/fractional-scale-v1-protocol.h
  COMMAND ${WAYLAND_SCANNER} private-code ${_fs} ${_gen}/fractional-scale-v1-protocol.c
  DEPENDS ${_fs}
  VERBATIM)
add_executable(jsui-wl main.cpp ${_gen}/xdg-shell-protocol.h ${_gen}/text-input-unstable-v2-protocol.h
  ${_gen}/text-input-unstable-v2-protocol.c ${_gen}/fractional-scale-v1-protocol.h ${_gen}/fractional-scale-v1-protocol.c)
target_include_directories(jsui-wl PRIVATE ${_gen})
target_compile_definitions(jsui-wl PRIVATE WLR_USE_UNSTABLE JSUI_WM_BUNDLE="${JSUI_WM_BUNDLE}")
target_link_libraries(jsui-wl PRIVATE jsui PkgConfig::WLR)
jsui_copy_icu_data(jsui-wl)
set_target_properties(jsui-wl PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# --mir (Ubuntu Touch): present through a host Mir server with mirclient.
# Mir 1.x as packaged by UBports is mir1client; upstream Mir 1.x is mirclient.
pkg_check_modules(MIRCLIENT IMPORTED_TARGET mir1client)
if(NOT MIRCLIENT_FOUND)
  pkg_check_modules(MIRCLIENT IMPORTED_TARGET mirclient)
endif()
if(MIRCLIENT_FOUND)
  message(STATUS "jsui-wl: Mir client support (--mir) with mirclient ${MIRCLIENT_VERSION}")
  target_compile_definitions(jsui-wl PRIVATE JSUI_WL_MIR=1)
  target_link_libraries(jsui-wl PRIVATE PkgConfig::MIRCLIENT)
  # GPU presentation (EGL/GLES on the Mir window; libhybris on the device).
  pkg_check_modules(MIRGL IMPORTED_TARGET egl glesv2)
  if(MIRGL_FOUND)
    message(STATUS "jsui-wl: --mir renders on the GPU (EGL/GLES)")
    target_compile_definitions(jsui-wl PRIVATE JSUI_WL_MIR_GL=1)
    target_link_libraries(jsui-wl PRIVATE PkgConfig::MIRGL)
    # Android's ANativeWindow (UBports' android-headers): Mir's android
    # window is one, and its perform() takes a buffer count
    # (JSUI_WL_MIR_BUFFERS, see mir_host.h).
    find_path(JSUI_WL_ANDROID_HEADERS system/window.h PATHS /usr/include/android-30 /usr/include/android NO_DEFAULT_PATH)
    if(JSUI_WL_ANDROID_HEADERS)
      target_include_directories(jsui-wl SYSTEM PRIVATE ${JSUI_WL_ANDROID_HEADERS})
      target_compile_definitions(jsui-wl PRIVATE JSUI_WL_ANDROID_WINDOW=1)
    endif()
  endif()
  # Skia Graphite on Vulkan (libhybris' libvulkan, loaded at run time): needs
  # a JSUI built with JSUI_VULKAN and the Vulkan headers.
  find_path(JSUI_WL_VULKAN_INCLUDE vulkan/vulkan.h)
  if(JSUI_VULKAN AND JSUI_WL_VULKAN_INCLUDE)
    message(STATUS "jsui-wl: --mir renders with Skia Graphite on Vulkan when it can")
    target_compile_definitions(jsui-wl PRIVATE JSUI_WL_MIR_VK=1)
    target_link_libraries(jsui-wl PRIVATE ${CMAKE_DL_LIBS})
  endif()
endif()
