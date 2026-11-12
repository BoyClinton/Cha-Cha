# Dear ImGui (MIT) for the launcher overlay: fetched at configure time (pinned tag), SDL3 + Vulkan backends.
include(FetchContent)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.9b
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)
add_library(mh3u_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
set_property(TARGET mh3u_imgui PROPERTY COMPILE_OPTIONS "$<IF:$<BOOL:${MSVC}>,/utf-8;/w,-w>")  # third-party: not held to our /W4
target_include_directories(mh3u_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
find_package(Vulkan REQUIRED)
target_link_libraries(mh3u_imgui PUBLIC SDL3::SDL3 Vulkan::Vulkan)

# The game executable: recompiled code + HLE runtime.
set(boot_sources main.cpp plugins.cpp plugins.h profiler.cpp read_watch.cpp read_watch.h app_paths.cpp app_paths.h quests.cpp quests.h
  shader_prep.cpp shader_prep.h loading_screen.cpp loading_screen.h graphics_menu.cpp graphics_menu.h texture_dump.cpp texture_dump.h
  controls_menu.cpp controls_menu.h dlc_content.cpp dlc_content.h
  launcher_image.cpp launcher_image.h setup.cpp setup.h game_skin.cpp game_skin.h skin_decode.cpp skin_decode.h)  # the fork's overlay (F1)
if(ANDROID)
  # libmain.so: SDL's Java activity (android/) loads it and calls SDL_main (main.cpp includes SDL_main.h there)
  add_library(mh3u SHARED ${boot_sources})
else()
  add_executable(mh3u WIN32 ${boot_sources})
endif()
if(WIN32)
  # GUI subsystem (no console window for players) with the standard main(); output: setup_output() in main.cpp
  target_link_options(mh3u PRIVATE /ENTRY:mainCRTStartup)
  # Cha-Cha.map next to the exe: a crash report's "at Cha-Cha.exe+0x..." (guest_memory.cpp) maps to a function with
  # it ("-map:" spelling: the Visual Studio generator turns /MAP into GenerateMapFile, which the ClangCL toolset drops)
  target_link_options(mh3u PRIVATE "-map:$<TARGET_FILE_DIR:mh3u>/Cha-Cha.map")
elseif(ANDROID)
  # 16 KB pages (Android 15+ devices): segments aligned for both page sizes
  target_link_options(mh3u PRIVATE "-Wl,-z,max-page-size=16384")
  target_link_libraries(mh3u PRIVATE android log)
else()
  # crash reports name the function themselves (dladdr); libSDL3.so is looked for next to the binary. Releases link
  # with the install path (CMAKE_BUILD_WITH_INSTALL_RPATH): lib/ of the folder release, ../lib of the AppImage.
  target_link_options(mh3u PRIVATE -rdynamic)
  set_target_properties(mh3u PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN/lib:$ORIGIN/../lib")
endif()
# players see Cha-Cha.exe (the project's name); the target stays mh3u for the build scripts
if(ANDROID)
  set_target_properties(mh3u PROPERTIES OUTPUT_NAME "main")
else()
  set_target_properties(mh3u PROPERTIES OUTPUT_NAME "Cha-Cha")
endif()
# exe icon from icon/icon.ico (resource id 1, also used for the SDL window)
if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/icon/icon.ico")
  target_sources(mh3u PRIVATE mh3u.rc)
endif()
target_include_directories(mh3u PRIVATE ${CMAKE_SOURCE_DIR}/include)  # chacha_plugin.h (the mod API)
target_link_libraries(mh3u PRIVATE mh3u_recomp_game mh3u_recomp_core mh3u_os mh3u_gfx mh3u_audio mh3u_core mh3u_fs mh3u_latte mh3u_imgui mh3u_game_src SDL3::SDL3)
if(WIN32)
  target_link_libraries(mh3u PRIVATE shell32 ole32)
  # SDL3.dll from the Vulkan SDK next to the executable
  add_custom_command(TARGET mh3u POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$ENV{VULKAN_SDK}/Bin/SDL3.dll" $<TARGET_FILE_DIR:mh3u>)
else()
  target_link_libraries(mh3u PRIVATE ${CMAKE_DL_LIBS})
endif()
# version (crash reports, logs): git describe of the checkout at configure time
find_package(Git QUIET)
set(MH3U_VERSION "dev")
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} describe --always --dirty WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE MH3U_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
target_compile_definitions(mh3u PRIVATE MH3U_VERSION="${MH3U_VERSION}")

add_executable(quests_test quests_test.cpp quests.cpp quests.h)
add_test(NAME boot_quests COMMAND quests_test)

add_executable(skin_decode_test skin_decode_test.cpp skin_decode.cpp skin_decode.h ${CMAKE_SOURCE_DIR}/runtime/gfx/tiling.cpp)
target_include_directories(skin_decode_test PRIVATE ${CMAKE_SOURCE_DIR}/runtime/gfx)
add_test(NAME boot_skin_decode COMMAND skin_decode_test)
