# Third-party notices

Cha-Cha does not include any game assets or original game files: no disc image or `.wua`, no copy of the game's
executable or data, no textures, models, audio or video. You provide them from your own copy of the game.
Shaders and pipeline caches are generated on your PC from your copy and stay in your data folder.

## Compiled into Cha-Cha.exe

| Component | Version | License | Use | Source |
|---|---|---|---|---|
| zstd (decoder only) | 1.5.7 | BSD-3-Clause (`zstd-LICENSE.txt`) | Reads `.wua` archives | <https://github.com/facebook/zstd> |
| Dear ImGui | 1.91.9b | MIT (`DearImGui-LICENSE.txt`) | Setup screens | <https://github.com/ocornut/imgui> |
| shaderc, glslang, SPIRV-Tools (`shaderc_combined` from the Vulkan SDK) | SDK 1.4.363.0 | Apache-2.0 / BSD-3-Clause and others (see the projects) | Compiles the translated shaders on first start | <https://github.com/google/shaderc> |

## Shipped as separate libraries

| Library | License | Source |
|---|---|---|
| `SDL3.dll` (3.4, from the Vulkan SDK) | zlib | <https://github.com/libsdl-org/SDL> |
| `msvcp140.dll`, `vcruntime140.dll`, `vcruntime140_1.dll` | Microsoft Visual C++ Redistributable license | Visual Studio |

The Vulkan loader (`vulkan-1.dll`) comes with your graphics driver.

## Linux AppImage

The AppImage carries the shared libraries the program needs, taken from Ubuntu 22.04 by linuxdeploy, each under its
own license:

| Library | License | Source |
|---|---|---|
| `libSDL3.so` (3.4.16, built from source) | zlib | <https://github.com/libsdl-org/SDL> |
| FFmpeg `libavcodec`, `libavutil`, `libswresample` (Ubuntu's build, with the libraries it links) | GPL-2.0-or-later as built by Ubuntu | <https://ffmpeg.org>, Ubuntu's `ffmpeg` source package |

The Vulkan loader, the GPU drivers and the C / C++ runtime libraries come from your system.
