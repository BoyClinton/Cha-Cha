#!/usr/bin/env bash
# Builds the folder release for Linux: one self-contained folder (as MHGU Recomp's), zipped as
# Cha-Cha-<version>-linux-x86_64.tar.gz. Usage: scripts/package-folder.sh <build dir> <version> [out dir]
#
#   Cha-Cha-<version>/
#     Cha-Cha                the program (RUNPATH $ORIGIN: the libraries below are found next to it)
#     libSDL3.so.0 ...       SDL3 and the shader compiler (FFmpeg's libavcodec and Vulkan come from the system)
#     Cha-Cha.ini            settings; its presence makes this folder the data folder (README "The folder release")
#     game/                  put your dump here (the game and update folders, or a .wua)
#     mods/                  mods, Damage Numbers included
#     licenses/              Cha-Cha's GPL-3.0 and the bundled libraries' licenses
#     README.txt
#   created on first start: cache/ save/ quests/ texture_replacements/ log.txt input.ini mods.ini
set -euo pipefail
src="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:?build dir}"
version="${2:?version}"
out="${3:-$src/build/release}"
name="Cha-Cha-$version"
stage="$out/$name"
rm -rf "$stage"
mkdir -p "$stage/game" "$stage/mods" "$stage/licenses"

install -m 755 "$build/runtime/boot/Cha-Cha" "$stage/Cha-Cha"
# libraries the system may not have: SDL3, shaderc (+ SPIRV-Tools)
for lib in libSDL3.so.0 libshaderc_shared.so.1 libSPIRV-Tools.so libSPIRV-Tools-opt.so; do
    path="$(ldd "$stage/Cha-Cha" | awk -v l="$lib" '$1 == l { print $3 }')"
    if [ -n "$path" ] && [ -f "$path" ]; then cp -L "$path" "$stage/$lib"; fi
done

# the mods built with this tree (<build>/mods/<mod>: mod.ini + plugins/), with their READMEs
for mod in "$build"/mods/*/; do
    m="$(basename "$mod")"
    [ -f "$mod/mod.ini" ] || continue  # CMake's own folders
    cp -r "$mod" "$stage/mods/$m"
    [ -f "$src/mods/$m/README.md" ] && cp "$src/mods/$m/README.md" "$stage/mods/$m/"
done

cat > "$stage/Cha-Cha.ini" <<'EOF'
# Cha-Cha settings. This file next to Cha-Cha makes this folder Cha-Cha's data folder: settings, saves, caches,
# mods and quests are kept here. Delete a line to get its default back.
#
# game = your own dump of Monster Hunter 3 Ultimate (US) + update v32: a .wua file or game folders (code, content,
# meta). Leave it out and put the dump in the game folder next to Cha-Cha instead.
EOF
cat > "$stage/game/put-your-game-here.txt" <<'EOF'
Put your own dump of Monster Hunter 3 Ultimate (US) with update v32 in this folder:
  - the game folder and the update folder (each with code, content and meta), or
  - a .wua file.
Cha-Cha reads the files where they are; nothing is copied or changed.
EOF

cp "$src/LICENSE" "$stage/licenses/Cha-Cha (GPL-3.0).txt"
cp "$src/include/chacha_plugin.h" "$stage/mods/chacha_plugin.h"  # for making plugins
lic() { [ -f "$2" ] && cp "$2" "$stage/licenses/$1.txt" || true; }
lic SDL3 /usr/share/licenses/SDL3/LICENSE.txt
lic SDL3 "$build/_deps/sdl3-src/LICENSE.txt"
lic "Dear ImGui" "$build/_deps/imgui-src/LICENSE.txt"
lic zstd "$build/_deps/zstd-src/LICENSE"
lic shaderc /usr/share/licenses/libshaderc/LICENSE
lic SPIRV-Tools /usr/share/licenses/spirv-tools/LICENSE

cat > "$stage/README.txt" <<EOF
Cha-Cha $version - Monster Hunter 3 Ultimate, running natively on PC
=====================================================================

No game files are included: you need your own Monster Hunter 3 Ultimate (US) with update v32.

Start
  1. Put your dump in the game folder (see game/put-your-game-here.txt), or choose it on the first start.
  2. Run ./Cha-Cha (chmod +x Cha-Cha if needed). The first start translates the game's shaders (a few seconds).

Needs: x86-64 Linux, a Vulkan 1.2 driver, FFmpeg's libavcodec (for the movies). On another distribution than the
one this was built on, use the AppImage instead.

This folder
  Cha-Cha.ini     settings (the launcher's SETTINGS page writes it)
  game/           your dump
  mods/           mods: each in its own folder, switched on and off on the Mods page (F2 in game)
  cache/          translated shaders and pipelines (made on first start)
  save/           your saves
  quests/         event and challenge quests (the Quests page)
  texture_replacements/   texture packs
  log.txt         what Cha-Cha printed (when not started from a terminal)
  licenses/       licenses of Cha-Cha and the libraries it ships

Keys: F1 settings, F2 mods, F3 GamePad screen, F11 fullscreen / window. Settings -> Controls: bindings, mouse sensitivity, camera speed.

Contains no game files: the menus are drawn with the game's own GUI textures, read from your dump at start.

Online (very experimental): talk to Neko in Port Tanzia, then Host a Hunt or Join a Hunt. To play across your network,
first turn on Settings -> Gameplay -> Online -> Play over the network, only with people you trust. Steps are in the
README on GitHub. Please report problems: https://github.com/SilversRayGA/Cha-Cha-Plus/issues

Mods: see mods/damage-numbers/README.md for an example. Making mods: docs/modding.md in Cha-Cha's source.

Cha-Cha is free software under the GNU GPL-3.0 (licenses/). A fan project, not affiliated with or endorsed by
Capcom or Nintendo.
EOF

tar -C "$out" -czf "$out/$name-linux-x86_64.tar.gz" "$name"
echo "folder: $stage"
echo "archive: $out/$name-linux-x86_64.tar.gz"
