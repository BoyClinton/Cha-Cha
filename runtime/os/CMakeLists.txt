# HLE implementations of Cafe OS libraries (coreinit, ...), registered by name with the recompiler's import registry.
add_library(mh3u_os STATIC
  hle.cpp hle.h
  exp_heap.cpp exp_heap.h
  coreinit_base.cpp
  coreinit_memory.cpp
  coreinit_sync.cpp
  coreinit_time.cpp
  nn_act.cpp
  coreinit_fs.cpp
  nn_save.cpp
  coreinit_config.cpp
  coreinit_thread.cpp
  input.cpp
  coreinit_dynload.cpp
  applets.cpp
  offline.cpp
  coreinit_misc.cpp
  h264.cpp
)
target_include_directories(mh3u_os PUBLIC .)
target_link_libraries(mh3u_os PUBLIC mh3u_core mh3u_fs mh3u_ppc_context mh3u_recomp_support mh3u_input)
if(WIN32)  # H.264: Media Foundation
  target_link_libraries(mh3u_os PRIVATE mfplat mfuuid wmcodecdspuuid ole32 oleaut32 strmiids)
else()     # H.264: FFmpeg's libavcodec
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec libavutil)
  target_link_libraries(mh3u_os PRIVATE PkgConfig::AVCODEC)
endif()

add_executable(exp_heap_test exp_heap_test.cpp exp_heap.cpp)
target_include_directories(exp_heap_test PRIVATE .)
add_test(NAME os_exp_heap COMMAND exp_heap_test)
