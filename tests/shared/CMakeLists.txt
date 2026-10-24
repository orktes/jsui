# A host of a packaged JSUI release: finds it with find_package(jsui) (the
# package's lib/cmake/jsui) and builds consumer.c against jsui::jsui.
#
#   cmake -S tests/shared -B build-consumer -DCMAKE_PREFIX_PATH=<package dir>
cmake_minimum_required(VERSION 3.20)
project(jsui_consumer C)
find_package(jsui CONFIG REQUIRED)
add_executable(consumer consumer.c)
target_link_libraries(consumer PRIVATE jsui::jsui)
if(WIN32)
  add_custom_command(TARGET consumer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:jsui::jsui> ${JSUI_ICU_DATA} $<TARGET_FILE_DIR:consumer>)
endif()
