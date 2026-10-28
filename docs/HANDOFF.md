# Handoff â€“ MH3U PC port (2026-10-24)

Read `CLAUDE.md` first, then `docs/ROADMAP.md` and the tail of `docs/research-log.md`. This file is the state of play
when the previous sessions were archived.

## Where things stand
- **Phases 1â€“3 complete.** Phase 4 (Playable) in progress.
- The port boots to **gameplay**: GamePad/language/save prompts â†’ intro movie (H.264 via Media Foundation) â†’ title â†’
  main menu â†’ New Game â†’ character creation (name typed on the PC keyboard) â†’ Touch Panel Settings â†’ Character
  Select â†’ **Moga Village** (opening Village Chief scene renders). **Saves write and load** (Continue lists the
  character and loads back into the village).
- Rendering: Vulkan, all 668 shaders translated offline to SPIR-V; 7 screens from boot to character creation
  pixel-compared with Cemu â€“ identical except animation phase (`scripts/compare-frames.ps1`, `docs/runtime.md`
  Â§ Render comparison).
- Audio: AX mixer â†’ SDL3, confirmed by ear. No AUX reverb (AXFX stubs).
- Input: host keyboard+mouse and SDL controllers drive a **Wii U Pro Controller with no GamePad** by default.

## Last commits
- `66457bc` Roadmap: custom quest editor/loader + quest scaling by hunter count (Phase 5 items)
- `d5bbff4` Vendored Cemu addrlib replaced by our own `runtime/gfx/addrlib/latte_surface.*` (+ `gfx_addrlib` test)
- `488bf69` Saves load/write; first in-game scene
- `a16eb29` Text entry from the PC keyboard (swkbd HLE)
- `d78b86d` Phase 3 closed
- **Verified after `d5bbff4` (2026-10-24):** clang-cl and MSVC Debug both build warning-free, ctest 10/10 on both,
  headless New Game route reaches Moga Village and writes `user1` (35,364 B).

## User decisions / preferences (binding)
- **Cemu is a reference only** â€“ no Cemu source in the repo (in `CLAUDE.md`). decaf-emu: read only.
- **PC-native feel:** Pro Controller + no GamePad by default (`MH3U_CONTROLLER=pro-only`); GamePad screen hidden
  (F2 / `MH3U_GAMEPAD_VIEW=pip|only` for debugging); text entry from the physical keyboard (Enter or controller A =
  OK, Esc or B = cancel). Keyboard+mouse alone must flow naturally; with a controller, Enter and A both confirm.
- **Wanted next for KB+M:** mouse hover highlights menu options (needs the game's GUI/menu system reverse-engineered).
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; never commit game data.

## What's left (ROADMAP)
Phase 4 â€“ Playable:
- [ ] Mouse: hover highlights menu options on keyboard + mouse
- [ ] Hunt in a quest end-to-end; compare with Cemu (also re-run `MH3U_WATCH_TARGETS=1` in gameplay)
- [ ] Performance: framerate unlock investigation, resolution scaling
- Known quirk: in Character Creation the first d-pad press after closing the name keyboard is swallowed.
- Gameplay button mapping (which button does what) still to check in game.

Phase 5 â€“ Decompile & enhance: hand-written replacements in `src/`; widescreen/high-res, uncapped FPS, loose-file
mods; optional LAN multiplayer; custom quest editor + loader; quest scaling by hunter count.

## How to build / run
- clang-cl: `cmake -S . -B build/clang -G "Visual Studio 18 2026" -T ClangCL`, then
  `cmake --build build/clang --config Debug` (faster Debug); MSVC: `build/cmake`. CMake/ctest live in VS's
  `Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin`. Don't run two builds at once (PDB lock C1041).
- Run: `build/clang/runtime/boot/Debug/Cha-Cha.exe .` (windowed) or `... . --headless`.
- Shaders must exist in `build/shaders` (`tools/shaderc-latte <repo> --spv`, needs `VULKAN_SDK`).

## Debug / test switches (see docs/runtime.md, docs/input.md)
`MH3U_EXIT_AFTER_FRAMES`, `MH3U_EXIT_AFTER_SECONDS`, `MH3U_DUMP_FRAME=N[,N..]` (â†’ `build/frame_N.bmp` + draws
trace), `MH3U_DUMP_TARGET=<hex image>`, `MH3U_INPUT_SCRIPT="frame[+n]:BUTTON|..;.."` (UP/DOWN/A/â€¦; boot timing
varies Â±200 frames between runs â€“ leave slack, dump several frames), `MH3U_TEXT_INPUT=<name>` (headless swkbd),
`MH3U_CONTROLLER=pro-only|pro|gamepad`, `MH3U_GAMEPAD_VIEW`, `MH3U_AUDIO=0`, `MH3U_AUDIO_WAV=<file>`,
`MH3U_TRACE=1` (import counts + thread dump), `MH3U_PEEK`, `MH3U_WATCH_TARGETS=1`, `MH3U_VK_VALIDATION=1`,
`MH3U_RENDER_DEBUG` (16 = write-watch log), `MH3U_FPS=<n>|max|game` (default: display refresh rate windowed), `MH3U_VSYNC=1`,
`MH3U_PROFILE=1` (sampling profiler + FPS log; use the RelWithDebInfo build), `MH3U_INPUT_CLOCK=time` (script numbers = 1/60 s).

Scripted route to the village (headless, current save state): presses `1000:A;1200:A;1450:A;1650:A;1850:A;` then six
`DOWN` 60 frames apart from 2150, then `A` at 2550, 2800, 3050, 3300 (with `MH3U_TEXT_INPUT=<name>`). The
unskippable opening movie (ship â†’ Moga) then runs until ~11000; the village (Village Chief "Mornin', hunter!") is up
by frame 12000 (~275 s headless Debug). Back up `save/` first â€“ the route writes `user1`.

## Local state (not in git)
- `save/80000001/`: system + quest/card files, **no character** (slot 1 empty), controller option = Pro Controller.
- Backups in `build/`: `save_gamepad_backup` (same as current save), `save_with_ryo` (test character "Ryo"),
  `save_before_char` (also has a character), `save_pro_fresh`.
- Cemu 2.6 portable in `tools/bin/Cemu/portable/` with a keyboard GamePad profile (Return = A, Backspace = B,
  arrows = d-pad); F12 = native 1920x1080 TV screenshot. Cemu source checkout for reference: `build/ref/Cemu`
  (sparse).
- Comparison images: `build/compare/`.

## Gotchas
- Bash heredocs collapse `\\` â†’ write scripts/C++ literals with the Write/Edit tools (Python scripts in the
  scratchpad, then `python -I script.py`).
- `scripts/ghidra-headless.ps1` exits non-zero on a JVM warning but still writes the output; wait for the file.
  `ghidra/scripts/Decompile.java <abs out> <addr>...`.
- `MH3U_PEEK` full scans are slow and race the still-running guest; prefer `@addr`.
- The game's active controller is a save option: a GamePad-set save ignores Pro input until switched (pro-only
  switches it automatically after one notice).
