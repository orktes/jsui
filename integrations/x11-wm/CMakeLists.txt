# jsui-wm: an X11 window manager / compositor with a JSUI desktop shell
# (main.cpp, ui/shell.jsx). Linux only; built with -DJSUI_BUILD_X11WM=ON.
# Needs the X11 development packages (Debian / Ubuntu: libx11-dev
# libxcomposite-dev libxdamage-dev libxfixes-dev libxext-dev libxtst-dev
# libxi-dev). The shell bundle is built with
#   npx jsui build integrations/x11-wm/ui/shell.jsx -o integrations/x11-wm/ui/dist/shell.js
find_package(X11 REQUIRED)
foreach(_c Xcomposite Xdamage Xfixes Xext Xtst Xi)
  if(NOT X11_${_c}_FOUND)
    message(FATAL_ERROR "jsui-wm needs lib${_c} (install its -dev package)")
  endif()
endforeach()

set(JSUI_WM_BUNDLE "${CMAKE_CURRENT_SOURCE_DIR}/ui/dist/shell.js" CACHE FILEPATH
    "Default shell bundle of jsui-wm (npx jsui build integrations/x11-wm/ui/shell.jsx -o <this>)")

add_executable(jsui-wm main.cpp)
target_compile_definitions(jsui-wm PRIVATE JSUI_WM_BUNDLE="${JSUI_WM_BUNDLE}")
target_link_libraries(jsui-wm PRIVATE jsui X11::X11 X11::Xcomposite X11::Xdamage X11::Xfixes X11::Xext X11::Xtst X11::Xi)
jsui_copy_icu_data(jsui-wm)
set_target_properties(jsui-wm PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
