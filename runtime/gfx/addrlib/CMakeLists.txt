# Latte surface layout (latte_surface.*) and GX2 surface sizing / per-level addressing on top of it (gx2_surface.*).
add_library(mh3u_addrlib STATIC latte_surface.cpp latte_surface.h gx2_surface.cpp gx2_surface.h)
target_include_directories(mh3u_addrlib PUBLIC .)
target_compile_features(mh3u_addrlib PUBLIC cxx_std_20)

add_executable(addrlib_test addrlib_test.cpp addrlib_sweep.h ../tiling.cpp)
target_include_directories(addrlib_test PRIVATE ..)
target_link_libraries(addrlib_test PRIVATE mh3u_addrlib)
add_test(NAME gfx_addrlib COMMAND addrlib_test)
