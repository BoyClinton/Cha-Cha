#!/usr/bin/env bash
# Two copies of Cha-Cha on this PC for trying Online (docs/online.md): each has its own data folder, so its own save.
# Usage: scripts/online-two-copies.sh [path to Cha-Cha]   (default: ~/cha-cha-build/out/runtime/boot/Cha-Cha)
# The first copy uses your data folder as it is. The second gets a folder of its own, with a copy of your settings and saves made once.
# A controller reaches every copy, whichever window has focus, so the second copy ignores the first one found: play it with the keyboard.
set -euo pipefail
bin="${1:-$HOME/cha-cha-build/out/runtime/boot/Cha-Cha}"
data="${XDG_DATA_HOME:-$HOME/.local/share}/Cha-Cha"
second="${data}-second"
if [ ! -d "$second" ]; then
    mkdir -p "$second"
    cp "$data/config.ini" "$second/" 2>/dev/null || true
    cp "$data/input.ini" "$second/" 2>/dev/null || true
    cp -r "$data/save" "$second/" 2>/dev/null || true
fi
# mods follow the first copy each time, so both have the same ones
rm -rf "$second/mods"
cp -r "$data/mods" "$second/" 2>/dev/null || true
cp "$data/mods.ini" "$second/" 2>/dev/null || true
ignore=""
if [ -e /sys/class/input/js0/device/id/vendor ]; then
    ignore="0x$(cat /sys/class/input/js0/device/id/vendor)/0x$(cat /sys/class/input/js0/device/id/product)"
fi
cd "$(dirname "$bin")"
"$bin" --data-dir "$data" &
first=$!
sleep 5
SDL_GAMECONTROLLER_IGNORE_DEVICES="$ignore" "$bin" --data-dir "$second" &
second_pid=$!
wait "$first" "$second_pid"
