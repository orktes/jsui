# JsuiDemo: iOS example host. A UIKit app with one MTKView that renders a
# JSUI bundle with Skia Ganesh on Metal (CPU raster + blit as the fallback),
# forwarding touches, hardware keys and MFi / Xbox / DualSense controllers.
#
#   scripts/build-ios.sh            # Skia slice + bundle + app for the simulator
#
# The UI bundle (JSUI_IOS_BUNDLE, built by `jsui build`, assets/ next to it)
# is copied into the app as ui/bundle.js.

set(JSUI_IOS_BUNDLE "${CMAKE_SOURCE_DIR}/integrations/ios/build-ui/bundle.js" CACHE FILEPATH
    "JSUI bundle shipped in the app (its directory is copied as ui/)")
set(JSUI_IOS_BUNDLE_ID "com.orktes.jsui.demo" CACHE STRING "Bundle identifier of the example app")
set(JSUI_IOS_TEAM "" CACHE STRING "Development team for device signing (empty: ad-hoc, simulator)")

enable_language(OBJCXX)

add_executable(JsuiDemo MACOSX_BUNDLE main.mm)
target_link_libraries(JsuiDemo PRIVATE jsui "-framework UIKit" "-framework Metal" "-framework MetalKit"
  "-framework QuartzCore" "-framework GameController")
target_compile_options(JsuiDemo PRIVATE -fobjc-arc)
set_target_properties(JsuiDemo PROPERTIES
  OUTPUT_NAME JsuiDemo
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
  MACOSX_BUNDLE_GUI_IDENTIFIER ${JSUI_IOS_BUNDLE_ID}
  MACOSX_BUNDLE_BUNDLE_NAME JsuiDemo
  MACOSX_BUNDLE_BUNDLE_VERSION 1
  MACOSX_BUNDLE_SHORT_VERSION_STRING 0.1
  XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${JSUI_IOS_TEAM}"
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")

get_filename_component(_jsui_ios_ui_dir "${JSUI_IOS_BUNDLE}" DIRECTORY)
if(EXISTS "${JSUI_IOS_BUNDLE}")
  add_custom_command(TARGET JsuiDemo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${_jsui_ios_ui_dir}" "$<TARGET_BUNDLE_CONTENT_DIR:JsuiDemo>/ui"
    COMMENT "Copying the UI bundle into JsuiDemo.app/ui")
else()
  message(WARNING "No UI bundle at ${JSUI_IOS_BUNDLE}: the app starts with -JsuiURL only "
                  "(npx jsui build <app.jsx> -o ${JSUI_IOS_BUNDLE})")
endif()

# Ninja / Makefile builds are not signed by Xcode: ad-hoc sign for the
# simulator (devices need JSUI_IOS_TEAM with the Xcode generator).
if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
  add_custom_command(TARGET JsuiDemo POST_BUILD
    COMMAND codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:JsuiDemo>"
    COMMENT "Ad-hoc signing JsuiDemo.app")
endif()
