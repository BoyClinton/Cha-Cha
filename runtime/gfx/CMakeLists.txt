# Graphics: SDL3 window + Vulkan presentation, GX2 HLE. SDL3 comes with the Vulkan SDK (cmake/SDL3Config.cmake).
find_package(Vulkan REQUIRED)

add_subdirectory(addrlib)

add_library(mh3u_gfx STATIC presenter.cpp presenter.h gx2.cpp gx2.h renderer.cpp renderer.h shader_abi.h texture_pack.cpp texture_pack.h tiling.cpp tiling.h)
target_include_directories(mh3u_gfx PUBLIC .)
target_link_libraries(mh3u_gfx PUBLIC mh3u_os Vulkan::Vulkan PRIVATE SDL3::SDL3 mh3u_latte PUBLIC mh3u_addrlib)

add_executable(tiling_test tiling_test.cpp tiling.cpp)
add_test(NAME gfx_tiling COMMAND tiling_test)
