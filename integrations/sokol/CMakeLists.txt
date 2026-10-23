# jsui-sokol: the shared HUD over a sokol_gfx scene on the GL backend
# (main.cpp). Built with -DJSUI_BUILD_SOKOL=ON; the sokol headers are
# downloaded at a pinned commit. JSUI_SOKOL_DIR skips the download.
include(${CMAKE_SOURCE_DIR}/integrations/shared/fetch.cmake)

# floooh/sokol has no releases: a commit of its master branch (2026-10-23).
set(JSUI_SOKOL_COMMIT ba938930bd77e7ebcbe3871740a3e0c9d00c96c0)
jsui_fetch_archive(_sokol_src NAME sokol
  URL https://github.com/floooh/sokol/archive/${JSUI_SOKOL_COMMIT}.tar.gz
  SHA256 91e2b5b806a70e0c19671d60de7e693b6e5c50c7ba26af2442417466df237ba7
  DIR sokol-${JSUI_SOKOL_COMMIT})
if(NOT _sokol_src OR NOT EXISTS ${_sokol_src}/sokol_gfx.h)
  message(WARNING "sokol not available: skipping jsui-sokol (set JSUI_SOKOL_DIR or -DJSUI_BUILD_SOKOL=OFF)")
  return()
endif()

add_executable(jsui-sokol main.cpp sokol_impl.c)
target_include_directories(jsui-sokol PRIVATE ${_sokol_src})
target_compile_definitions(jsui-sokol PRIVATE SOKOL_GLCORE GL_SILENCE_DEPRECATION)
jsui_example_target(jsui-sokol)
if(APPLE)
  enable_language(OBJC)
  set_source_files_properties(sokol_impl.c PROPERTIES LANGUAGE OBJC)
  target_link_libraries(jsui-sokol PRIVATE "-framework Cocoa" "-framework QuartzCore" "-framework OpenGL")
elseif(WIN32)
  target_link_libraries(jsui-sokol PRIVATE opengl32)
  set_target_properties(jsui-sokol PROPERTIES WIN32_EXECUTABLE OFF)
else()
  find_package(X11 REQUIRED)
  find_package(OpenGL REQUIRED)
  find_package(Threads REQUIRED)
  target_link_libraries(jsui-sokol PRIVATE X11::X11 X11::Xi X11::Xcursor OpenGL::GL Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
