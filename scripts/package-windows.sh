#!/usr/bin/env bash
# Packages a Windows build cross-compiled on Linux (docs/windows-cross.md) as build/release/Cha-Cha-<version>-windows-x64.zip,
# like scripts/package-release.ps1 does on Windows. The CRT is linked statically, so no Visual C++ runtime DLLs ship.
# Usage: scripts/package-windows.sh <build dir> <version>
set -euo pipefail
src="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:?build dir}"
version="${2:?version}"
out="$src/build/release"
name="Cha-Cha-$version-windows-x64"
stage="$out/$name"
exe="$build/runtime/boot/Cha-Cha.exe"
[ -f "$exe" ] || { echo "no Windows build at $exe" >&2; exit 1; }
rm -rf "$stage" "$out/$name.zip"
mkdir -p "$stage/licenses" "$stage/data/mods"

cp "$exe" "$build/runtime/boot/SDL3.dll" "$stage/"
sed "s/{VERSION}/$version/" "$src/docs/release/README.txt" > "$stage/README.txt"
cat > "$stage/portable.txt" <<'EOF'
This file makes Cha-Cha portable: it keeps its settings, saves, caches and mods in the data folder next to Cha-Cha.exe.
Delete it to use %APPDATA%\Cha-Cha instead.
EOF
cp "$src/input.example.ini" "$stage/"
cp "$src/LICENSE" "$stage/LICENSE.txt"
cp "$src/docs/release/THIRD_PARTY_NOTICES.md" "$stage/licenses/"
for f in "$build/_deps/zstd-src/LICENSE:zstd-LICENSE.txt" "$build/_deps/imgui-src/LICENSE.txt:DearImGui-LICENSE.txt" \
         "$build/_deps/sdl3-src/LICENSE.txt:SDL3-LICENSE.txt" "${VULKAN_SDK:-/nonexistent}/licenses/SDL3/LICENSE.txt:SDL3-LICENSE.txt" \
         "${SHADERC_SRC:-/nonexistent}/LICENSE:shaderc-LICENSE.txt" \
         "${SHADERC_SRC:-/nonexistent}/third_party/LICENSE.glslang:glslang-LICENSE.txt" \
         "${SHADERC_SRC:-/nonexistent}/third_party/LICENSE.spirv-tools:SPIRV-Tools-LICENSE.txt"; do
    [ -f "${f%%:*}" ] && cp "${f%%:*}" "$stage/licenses/${f##*:}"
done
# mods that come with Cha-Cha, ready in the portable data folder; the plugin header for making more
for mod in "$build"/mods/*/; do
    m="$(basename "$mod")"
    [ -f "$mod/mod.ini" ] || continue
    cp -r "$mod" "$stage/data/mods/$m"
    [ -f "$src/mods/$m/README.md" ] && cp "$src/mods/$m/README.md" "$stage/data/mods/$m/"
done
cp "$src/include/chacha_plugin.h" "$stage/data/mods/"
cat > "$stage/BUILDINFO.txt" <<EOF
Version: $version
Commit: $(git -C "$src" rev-parse --short HEAD)$(git -C "$src" diff --quiet HEAD -- . 2>/dev/null || echo " (with uncommitted changes)")
Built: $(date -u '+%Y-%m-%d %H:%M:%S') UTC
Platform: Windows x86-64 (cross-compiled on Linux with clang-cl, static CRT)
EOF

# guard: nothing game-derived besides the program itself
if find "$stage" -type f \( -iname '*.wua' -o -iname '*.rpx' -o -iname '*.rpl' -o -iname '*.elf' -o -iname '*.arc' -o -iname '*.spv' -o -iname '*.bin' \) | grep -q .; then
    echo "refusing to package game files" >&2
    exit 1
fi
(cd "$out" && python3 -I -c "
import os, sys, zipfile
name = sys.argv[1]
with zipfile.ZipFile(name + '.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, _, files in os.walk(name):
        for f in files:
            z.write(os.path.join(d, f))
" "$name")
ls -l "$out/$name.zip"
