# Latte -> SPIR-V shader translator (docs/formats/shaders.md § Translation plan). The library is linked into the
# runtime, which translates the user's shaders on first start; shaderc comes from the Vulkan SDK (static).
find_package(Vulkan REQUIRED)
add_library(mh3u_latte STATIC gfd.cpp gfd.h latte.cpp latte.h glsl.cpp glsl.h translate.cpp translate.h)
if(WIN32)
  target_include_directories(mh3u_latte PUBLIC . ${CMAKE_SOURCE_DIR}/runtime/gfx PRIVATE "$ENV{VULKAN_SDK}/Include")
  target_link_libraries(mh3u_latte PRIVATE
    "$ENV{VULKAN_SDK}/Lib/$<IF:$<CONFIG:Debug>,shaderc_combinedd,shaderc_combined>.lib")
  # the SDK ships the libraries without their PDBs
  target_link_options(mh3u_latte INTERFACE /ignore:4099)
else()  # the Linux SDK: x86_64/{include,lib}
  target_include_directories(mh3u_latte PUBLIC . ${CMAKE_SOURCE_DIR}/runtime/gfx PRIVATE "$ENV{VULKAN_SDK}/include")
  find_package(Threads REQUIRED)
  target_link_libraries(mh3u_latte PRIVATE "$ENV{VULKAN_SDK}/lib/libshaderc_combined.a" Threads::Threads)
endif()

add_executable(shaderc-latte main.cpp)
target_link_libraries(shaderc-latte PRIVATE mh3u_latte mh3u_fs)

# Decodes every shader in the user's dump; skipped (77) without it.
add_test(NAME latte_decode COMMAND shaderc-latte "${MH3U_REPO_ROOT}/extracted")
set_tests_properties(latte_decode PROPERTIES SKIP_RETURN_CODE 77)
