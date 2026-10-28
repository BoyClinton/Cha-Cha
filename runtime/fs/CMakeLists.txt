# zstd (BSD) for .wua archives: only the decoder sources are compiled, fetched at configure time (pinned tag).
include(FetchContent)
FetchContent_Declare(zstd
  GIT_REPOSITORY https://github.com/facebook/zstd.git
  GIT_TAG v1.5.7
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR do-not-add)  # no add_subdirectory: we build the decoder ourselves
FetchContent_MakeAvailable(zstd)
set(zstd_lib ${zstd_SOURCE_DIR}/lib)
add_library(mh3u_zstd STATIC
  ${zstd_lib}/common/debug.c ${zstd_lib}/common/entropy_common.c ${zstd_lib}/common/error_private.c
  ${zstd_lib}/common/fse_decompress.c ${zstd_lib}/common/xxhash.c ${zstd_lib}/common/zstd_common.c
  ${zstd_lib}/decompress/huf_decompress.c ${zstd_lib}/decompress/zstd_ddict.c
  ${zstd_lib}/decompress/zstd_decompress.c ${zstd_lib}/decompress/zstd_decompress_block.c)
set_property(TARGET mh3u_zstd PROPERTY COMPILE_OPTIONS "")  # third-party: not held to our /W4
target_compile_definitions(mh3u_zstd PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_MULTITHREAD=0)
target_include_directories(mh3u_zstd PUBLIC ${zstd_lib})

add_library(mh3u_fs tree.h host_tree.cpp zarchive.cpp zarchive.h overlay_fs.cpp overlay_fs.h game_image.cpp game_image.h)
target_include_directories(mh3u_fs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mh3u_fs PRIVATE mh3u_zstd $<$<BOOL:${WIN32}>:bcrypt>)

add_executable(fs_overlay_test overlay_fs_test.cpp)
target_link_libraries(fs_overlay_test PRIVATE mh3u_fs)

# Runs against the user's own dump (extracted/ and, if present, game/*.wua); reports "skipped" without it.
add_test(NAME fs_overlay COMMAND fs_overlay_test "${MH3U_REPO_ROOT}")
set_tests_properties(fs_overlay PROPERTIES SKIP_RETURN_CODE 77)
