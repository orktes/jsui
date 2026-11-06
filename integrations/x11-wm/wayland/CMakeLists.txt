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
add_executable(jsui-wl main.cpp ${_gen}/xdg-shell-protocol.h)
target_include_directories(jsui-wl PRIVATE ${_gen})
target_compile_definitions(jsui-wl PRIVATE WLR_USE_UNSTABLE JSUI_WM_BUNDLE="${JSUI_WM_BUNDLE}")
target_link_libraries(jsui-wl PRIVATE jsui PkgConfig::WLR)
jsui_copy_icu_data(jsui-wl)
set_target_properties(jsui-wl PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
