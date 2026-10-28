#!/usr/bin/env bash
# Builds Cha-Cha for Linux (docs/linux.md): from WSL2 on the Windows checkout, or on a Linux machine.
# Usage: scripts/build-linux.sh [build dir]   (default ~/cha-cha-build; keep it on a Linux disk, not /mnt/...)
# Needs: clang 15+, Ninja, libvulkan-dev, CMake 3.25+ (/opt/cmake), the LunarG Vulkan SDK (/opt/vulkan/<version>/x86_64), FFmpeg
# (libavcodec-dev), the SDL3 build dependencies. The recompiled game code is generated into the build directory
# from your own dump (extracted/ in the checkout), so recomp/out of a Windows build is left alone.
set -euo pipefail
src="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$HOME/cha-cha-build}"
export PATH="/opt/cmake/bin:$PATH"
export VULKAN_SDK="${VULKAN_SDK:-$(ls -d /opt/vulkan/*/x86_64 | sort -V | tail -1)}"
export CC="${CC:-clang-15}" CXX="${CXX:-clang++-15}"
lld="$(command -v ld.lld || command -v ld.lld-15)"
# the Linux SDK ships headers and tools but not the loader: the system's libvulkan (libvulkan-dev)
vulkan_lib="$(ls "$VULKAN_SDK"/lib/libvulkan.so 2>/dev/null || ls /usr/lib/x86_64-linux-gnu/libvulkan.so)"
cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DMH3U_RECOMP_OUT="$build/recomp-out" \
      -DVulkan_LIBRARY="$vulkan_lib" \
      -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=$lld" -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=$lld"
# the generated game code compiles in large files (about 1-2 GB of memory each): JOBS limits the parallel compilers
cmake --build "$build" --target mh3u -j "${JOBS:-6}"
