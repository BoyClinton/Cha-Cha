# Third-party notices

Cha-Cha does not include any game assets or original game files: no disc image or `.wua`, no copy of the game's
executable or data, no textures, models, audio or video. You provide them from your own copy of the game.
Shaders and pipeline caches are generated on your PC from your copy and stay in your data folder.

## Compiled into Cha-Cha.exe

| Component | Version | License | Use | Source |
|---|---|---|---|---|
| zstd (decoder only) | 1.5.7 | BSD-3-Clause (`zstd-LICENSE.txt`) | Reads `.wua` archives | <https://github.com/facebook/zstd> |
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

## Android APK (experimental)

| Component | License | Use | Source |
|---|---|---|---|
| zstd (decoder only, 1.5.7) | BSD-3-Clause | Reads `.wua` archives | <https://github.com/facebook/zstd> |
| shaderc, glslang, SPIRV-Tools (the sources shipped with Android NDK r30, built for arm64) | Apache-2.0 / BSD-3-Clause and others | Compiles the translated shaders on first start | <https://github.com/google/shaderc> |
| FFmpeg `libavcodec`, `libavutil` 9.0.2 (H.264 decoder only, static, built without GPL or nonfree parts) | LGPL-2.1-or-later | Plays the game's movies | <https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz>, configured by `scripts/android/build-ffmpeg.sh` |
| `libSDL3.so` (3.4.16) and SDL's Java glue (`org.libsdl.app`) | zlib | Window, input, audio, the Android activity | <https://github.com/libsdl-org/SDL> |
| LLVM libc++ (static, from the NDK) | Apache-2.0 with LLVM exceptions | C++ standard library | <https://github.com/llvm/llvm-project> |

The Vulkan loader and the GPU driver come with the device.
