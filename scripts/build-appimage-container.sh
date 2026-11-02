#!/usr/bin/env bash
# Builds the portable AppImage in an Ubuntu 22.04 container (glibc 2.35: runs on most distributions, Steam Deck too)
# from any Linux host with podman (or docker).
# Usage: scripts/build-appimage-container.sh <version> [build root]   (default build root ~/cha-cha-appimage)
#
# Needs your dump in extracted/ (scripts/build-linux.sh's layout; symlinks are fine - their targets are mounted too)
# and a shaderc source tree with its dependencies (git clone https://github.com/google/shaderc, then
# utils/git-sync-deps) at $SHADERC_SRC (default <build root>/shaderc). The image comes from scripts/appimage/Containerfile.
# Output: build/release/Cha-Cha-<version>-x86_64.AppImage
set -euo pipefail
src="$(cd "$(dirname "$0")/.." && pwd)"
version="${1:?version}"
root="${2:-$HOME/cha-cha-appimage}"
shaderc="${SHADERC_SRC:-$root/shaderc}"
engine="$(command -v podman || command -v docker)"
mkdir -p "$root/sdk/include" "$root/sdk/lib"
# Vulkan headers from the host (newer than Ubuntu 22.04's; the loader stays the system's at run time)
[ -d "$root/sdk/include/vulkan" ] || cp -r /usr/include/vulkan /usr/include/vk_video "$root/sdk/include/"
[ -d "$shaderc/third_party/glslang" ] || { echo "no shaderc source with dependencies at $shaderc" >&2; exit 1; }
"$engine" image exists cha-cha-appimage 2>/dev/null || "$engine" build -t cha-cha-appimage -f "$src/scripts/appimage/Containerfile" "$src/scripts/appimage"

# mount the checkout, the build root, shaderc and every directory the dump's symlinks point into, at the same paths
mounts=(-v "$src:$src" -v "$root:$root" -v "$shaderc:$shaderc:ro")
while IFS= read -r t; do
    d="$(dirname "$t")"
    mounts+=(-v "$d:$d:ro")
done < <(find "$src/extracted" -maxdepth 3 -type l -exec readlink -f {} \; | sort -u)

"$engine" run --rm --security-opt label=disable --userns=keep-id -e HOME="$root/home" "${mounts[@]}" -w "$src" \
    cha-cha-appimage bash -euo pipefail -c "
mkdir -p '$root/home' '$root/sdk/include' '$root/sdk/lib'
if [ ! -f '$root/sdk/lib/libshaderc_combined.a' ]; then
    cmake -S '$shaderc' -B '$root/shaderc-build' -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-15 \
        -DCMAKE_CXX_COMPILER=clang++-15 -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON \
        -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_SKIP_TESTS=ON -DENABLE_GLSLANG_BINARIES=OFF -DENABLE_CTEST=OFF -DBUILD_TESTING=OFF
    ninja -C '$root/shaderc-build' shaderc_combined
    cp '$root/shaderc-build/libshaderc/libshaderc_combined.a' '$root/sdk/lib/'
    cp -r '$shaderc/libshaderc/include/shaderc' '$root/sdk/include/'
fi
export VULKAN_SDK='$root/sdk' CC=clang-15 CXX=clang++-15
cmake -S '$src' -B '$root/out' -G Ninja -DCMAKE_BUILD_TYPE=Release -DMH3U_RECOMP_OUT='$root/recomp-out' \
      -DVulkan_LIBRARY=/usr/lib/x86_64-linux-gnu/libvulkan.so -DVulkan_INCLUDE_DIR='$root/sdk/include' \
      -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld-15 -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld-15 -DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=lld-15
cmake --build '$root/out' --target mh3u damage_numbers -j \${JOBS:-8}
bash scripts/package-appimage.sh '$root/out' '$version'
"
ls -l "$src/build/release/Cha-Cha-$version-x86_64.AppImage"
