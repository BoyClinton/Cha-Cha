# Host input: keyboard + mouse and SDL gamepads merged into one Wii U GamePad state (docs/input.md).

add_library(mh3u_input STATIC host_input.cpp host_input.h)
target_include_directories(mh3u_input PUBLIC .)
target_link_libraries(mh3u_input PRIVATE SDL3::SDL3)

add_executable(bindings_test bindings_test.cpp)
target_link_libraries(bindings_test PRIVATE mh3u_input SDL3::SDL3)
if(WIN32)
  add_custom_command(TARGET bindings_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$ENV{VULKAN_SDK}/Bin/SDL3.dll" $<TARGET_FILE_DIR:bindings_test>)
endif()
add_test(NAME input_bindings COMMAND bindings_test)
