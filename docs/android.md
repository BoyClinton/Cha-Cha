# Android

Cha-Cha builds as an arm64 APK (`build/android/Cha-Cha-<version>-android-arm64.apk`) with `scripts/build-android.ps1` on Windows.
Status: **builds; not yet run on a device** (2026-11-11).

## What the APK is

- `libmain.so`: the game (recompiled code + runtime), the same code as the PC builds compiled with the NDK's clang for
  arm64-v8a. Loaded by SDL's Java activity (`android/app/.../ChaChaActivity.java` extends `SDLActivity`), which calls
  its `SDL_main` (`runtime/boot/main.cpp` includes `SDL_main.h` on Android).
- `libSDL3.so`: SDL 3.4.16 built from `tools/bin/android/SDL3`; its Java glue is compiled from the same source tree,
  so the two always match.
- FFmpeg's H.264 decoder (libavcodec/libavutil 9.0.2, static, nothing else enabled) and shaderc (from the NDK's
  sources, static), both linked into `libmain.so`.
- No game files, as with the PC releases. Requirements: Android 10 (API 29), arm64, Vulkan 1.1.

## Where things go on the device

The data folder is the app's own folder on shared storage, `Android/data/io.github.boyclinton.chacha/files`
(`SDL_GetAndroidExternalStoragePath`; no storage permission needed). It holds `config.ini`, `save/`, `cache/`,
`log.txt` (stdout + stderr) and the player's dump in a `game` folder (a `.wua` or the code / content / meta folders,
found by the same rule as next to the PC exe). From a PC over USB the folder is visible as
`Internal storage/Android/data/io.github.boyclinton.chacha/files`; Android 11+ hides it from on-device file managers.
`bonus/`, `custom/`, `mods/`, `texture_replacements/` work as on PC.

## Controls

Through SDL3's gamepad API, unchanged from PC: a handheld's built-in controls, Bluetooth / USB controllers and
clip-on controllers all appear as gamepads; remapping works. Mouse and keyboard work where Android has them (DeX,
Chromebooks). **No touch controls yet**: a phone needs a controller (plan: an on-screen virtual pad in the game's
style feeding the same input path, menus by tap through `menu_mouse.cpp`).

## Building

One-time setup is in `tools/README.md` § Android (Android Studio + SDK, NDK 30, CMake 4.1.2, the Linux NDK and FFmpeg
source in WSL, SDL3 and Vulkan-Headers in `tools/bin/android`). The PC build must be configured in `build/cmake`.

```
scripts/build-android.ps1                                     # debug-signed, for adb install / testing
scripts/build-android.ps1 -Release -Keystore my.jks -KeyAlias chacha   # release, signed with your own key
```

Steps, each skipped when up to date:
1. FFmpeg for arm64 in WSL (`scripts/android/build-ffmpeg.sh`, installed under `~/android-deps` and copied to
   `build/android/deps/arm64-v8a`).
2. shaderc with `ndk-build` (work tree `%LOCALAPPDATA%\cha-cha-android`: its generator scripts break on the spaces in
   the checkout path; the script merges the component libraries itself because ndk-build's own merge step quotes its
   `llvm-ar` script wrongly on Windows).
3. The game code: the PC tree's `recomp_emit` writes `build/android/recomp-out` (a cross build cannot run it, so
   `recomp/CMakeLists.txt` only checks the files exist when `CMAKE_CROSSCOMPILING`).
4. CMake + Ninja with the NDK toolchain into `build/android/cmake`; the stripped libraries go to
   `build/android/jniLibs/arm64-v8a`.
5. Gradle (`android/`, wrapper → Gradle 9.9.1, AGP 9.5.1, JBR = Java 25) packages the APK; the launcher icon is the
   PNG inside `icon/icon.ico`.

Install on a device with USB debugging: `adb install -r build/android/Cha-Cha-<version>-android-arm64.apk`; read the log with
`adb shell cat /sdcard/Android/data/io.github.boyclinton.chacha/files/log.txt`.

## Platform notes

- Guest memory (`runtime/core/guest_memory_posix.inc`) reserves 4 GB with `mmap(PROT_NONE)`, as on Linux. The fault
  handler reads the write bit from the arm64 signal frame's `esr_context` (ESR WnR) and the PC from `uc_mcontext.pc`.
- 16 KB-page devices (some Android 15+ phones): `commit()` works in whole host pages, and write tracking (4 KB
  mprotect granularity) is switched off, so `written_since()` always answers "written" and texture caches re-read
  guest memory (slower, correct). `libmain.so` is linked with 16 KB segment alignment.
- Vulkan: the renderer asks for 1.3 and loads its 1.3 calls through `vkGetDeviceProcAddr` (`vk_compat.cpp`), so
  linking against the API-29 `libvulkan.so` works; headers come from Vulkan-Headers 1.4.363 (the NDK's are older).
