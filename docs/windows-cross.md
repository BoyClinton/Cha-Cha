# Building the Windows release on Linux

`Cha-Cha.exe` can be cross-compiled on Linux with clang-cl, no Windows PC needed (first done on Fedora 44,
2026-11-02). The C runtime is linked statically, so the zip needs no Visual C++ redistributable. Wine runs the result
for a quick check (`wine Cha-Cha.exe --headless --console ...`).

## What it needs

| Piece | Where from |
|---|---|
| clang-cl, lld-link | the distribution's clang / lld |
| llvm-lib, llvm-rc, llvm-mt | the distribution's `llvm` package. Without root: `dnf download llvm`, then `rpm2cpio llvm-*.x86_64.rpm \| cpio -idm` and point `CHACHA_LLVM_BIN` at its `usr/bin` |
| MSVC C runtime + Windows SDK | [xwin](https://github.com/Jake-Shadle/xwin): `xwin --accept-license --arch x86_64 splat --output <dir>` (Microsoft's licence; nothing of it is shipped) |
| A "Vulkan SDK" folder (`VULKAN_SDK`) | made by hand: `Include/` (the Vulkan headers and `shaderc/`), `Lib/vulkan-1.lib` (an import library: `llvm-dlltool -m i386:x86-64 -d vulkan-1.def -l vulkan-1.lib`, the .def listing the `vk*` functions of `vulkan_core.h` / `vulkan_win32.h`), `Lib/shaderc_combined.lib`, and SDL3 installed into it (`bin/SDL3.dll`, `cmake/SDL3Config.cmake`; `Bin` a symlink to `bin`) |
| SDL3 for Windows | the same SDL source the Linux build fetches, built with the toolchain and installed into the SDK folder |
| shaderc for Windows | `git clone https://github.com/google/shaderc` + `utils/git-sync-deps`, target `shaderc_combined` with `-DSHADERC_ENABLE_SHARED_CRT=OFF` |
| The generated game code | a native Linux build first: a cross build cannot run `recomp_emit`, so `MH3U_RECOMP_OUT` points at the Linux build's `recomp-out` (the C++ is the same) |

## Building

```
export CHACHA_XWIN=<xwin dir> CHACHA_LLVM_BIN=<llvm bin dir> VULKAN_SDK=<sdk dir>
cmake -S . -B <win build> -G Ninja -DCMAKE_TOOLCHAIN_FILE=scripts/windows/clang-cl.cmake -DCMAKE_BUILD_TYPE=Release \
      -DMH3U_RECOMP_OUT=<linux build>/recomp-out -DVulkan_INCLUDE_DIR=$VULKAN_SDK/Include \
      -DVulkan_LIBRARY=$VULKAN_SDK/Lib/vulkan-1.lib
cmake --build <win build> --target mh3u damage_numbers
scripts/package-windows.sh <win build> <version>     # build/release/Cha-Cha-<version>-windows-x64.zip
```

SDL3 and shaderc are configured with the same `-DCMAKE_TOOLCHAIN_FILE` and `-DCMAKE_BUILD_TYPE=Release` (SDL3 with
`-DCMAKE_INSTALL_PREFIX=$VULKAN_SDK`, then `ninja install`). The toolchain uses the release CRT only: xwin does not
fetch the debug one.

The zip holds `Cha-Cha.exe`, `SDL3.dll`, `portable.txt` (so `data/` next to the exe is the data folder), and
`data/mods/damage-numbers` ready to use.
