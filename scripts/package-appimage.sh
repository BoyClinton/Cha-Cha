#!/usr/bin/env bash
# Packages a Linux build (scripts/build-linux.sh) as build/release/Cha-Cha-<version>-x86_64.AppImage (docs/linux.md).
# Usage: scripts/package-appimage.sh [build dir] [version]
# linuxdeploy copies the libraries the binary needs (SDL3, FFmpeg's libavcodec and theirs) into the AppImage; the
# Vulkan loader, the GPU drivers and glibc stay the system's. Like the Windows zip it holds no game files, but the
# binary contains the recompiled game code.
set -euo pipefail
src="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$HOME/cha-cha-build}"
version="${2:-dev-$(git -C "$src" rev-parse --short HEAD)}"
out="$src/build/release"
app="$build/AppDir"
tools="$HOME/.cache/cha-cha-tools"
bin="$build/runtime/boot/Cha-Cha"
[ -x "$bin" ] || { echo "no Linux build at $bin - run scripts/build-linux.sh first" >&2; exit 1; }

rm -rf "$app"
mkdir -p "$app/usr/bin" "$app/usr/share/applications" "$app/usr/share/icons/hicolor/256x256/apps" "$app/usr/share/doc/cha-cha" "$out" "$tools"
install -m 755 "$bin" "$app/usr/bin/Cha-Cha"
cat > "$app/usr/share/applications/cha-cha.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Cha-Cha
Comment=Monster Hunter 3 Ultimate, natively on PC (bring your own dump)
Exec=Cha-Cha
Icon=cha-cha
Categories=Game;
Terminal=false
EOF
# the icon: the largest image in icon/icon.ico (PNG entries are stored as they are)
python3 - "$src/icon/icon.ico" "$app/usr/share/icons/hicolor/256x256/apps/cha-cha.png" <<'PY'
import struct, sys
ico = open(sys.argv[1], 'rb').read()
count = struct.unpack_from('<H', ico, 4)[0]
best = None
for i in range(count):
    w, h, _, _, _, _, size, offset = struct.unpack_from('<BBBBHHII', ico, 6 + 16 * i)
    w, h = w or 256, h or 256
    data = ico[offset:offset + size]
    if data.startswith(b'\x89PNG') and (best is None or w * h > best[0]):
        best = (w * h, data)
if best is None:
    sys.exit('icon.ico has no PNG image')
open(sys.argv[2], 'wb').write(best[1])
PY
for f in LICENSE input.example.ini docs/release/THIRD_PARTY_NOTICES.md; do cp "$src/$f" "$app/usr/share/doc/cha-cha/"; done
sed "s/{VERSION}/$version/" "$src/docs/release/README.txt" > "$app/usr/share/doc/cha-cha/README.txt"

# guard: nothing game-derived besides the binary
if find "$app" -type f \( -iname '*.wua' -o -iname '*.rpx' -o -iname '*.rpl' -o -iname '*.elf' -o -iname '*.arc' -o -iname '*.spv' \) | grep -q .; then
    echo "refusing to package game files" >&2
    exit 1
fi

# linuxdeploy (continuous build from its GitHub releases), run without FUSE
ld="$tools/linuxdeploy-x86_64.AppImage"
if [ ! -x "$ld" ]; then
    curl -sSfL -o "$ld" https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
    chmod +x "$ld"
fi
export APPIMAGE_EXTRACT_AND_RUN=1 LDAI_OUTPUT="$out/Cha-Cha-$version-x86_64.AppImage"
# the Vulkan loader must be the system's (it finds the GPU drivers installed there)
"$ld" --appdir "$app" --exclude-library 'libvulkan.so*' --desktop-file "$app/usr/share/applications/cha-cha.desktop" \
      --icon-file "$app/usr/share/icons/hicolor/256x256/apps/cha-cha.png" --output appimage
ls -l "$LDAI_OUTPUT"
