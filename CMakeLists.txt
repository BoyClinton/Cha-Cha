cmake_minimum_required(VERSION 3.25)
project(mh3u_pc LANGUAGES C CXX)  # C: the zstd decoder (runtime/fs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  # clang-cl's /W4 includes -Wextra: Vulkan's `VkFooInfo x{VK_STRUCTURE_TYPE_FOO}` idiom (rest zero-initialised on
  # purpose) would warn at every struct.
  add_compile_options(-Wno-missing-field-initializers)
endif()
if(NOT MSVC)
  # the recompiled code was written against MSVC's semantics: no type-based alias analysis, wrapping signed overflow
  add_compile_options(-fno-strict-aliasing -fwrapv)
endif()

# SDL3: on Windows the Vulkan SDK's copy (SDL3.dll next to the exe); elsewhere built from source (pinned tag, the
# same version), shared so the AppImage carries it as libSDL3.so
if(WIN32)
  find_package(SDL3 REQUIRED CONFIG PATHS "$ENV{VULKAN_SDK}/cmake" NO_DEFAULT_PATH GLOBAL)
else()
  include(FetchContent)
  set(SDL_SHARED ON CACHE BOOL "" FORCE)
  set(SDL_STATIC OFF CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.16
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(SDL3)
  # the Microsoft C runtime functions the code uses (_dupenv_s, fopen_s, ...), for C++ only (SDL is C)
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-include>" "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_SOURCE_DIR}/runtime/core/win_compat.h>")
endif()

enable_testing()

# Repo root, so tests can find the user's extracted dump (extracted/ is gitignored).
set(MH3U_REPO_ROOT "${CMAKE_SOURCE_DIR}")

add_subdirectory(runtime)
add_subdirectory(recomp)
add_subdirectory(src)
add_subdirectory(tools/shaderc-latte)
