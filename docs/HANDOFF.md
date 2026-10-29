# Handoff – Cha-Cha (2026-10-28)

Read `CLAUDE.md` first, then `docs/ROADMAP.md` and the tail of `docs/research-log.md`. This file is the state of play
at the end of the session that shipped 0.1.0.

## Where things stand

- **Released.** Cha-Cha 0.1.0 is public: <https://github.com/BoyClinton/Cha-Cha>, release
  <https://github.com/BoyClinton/Cha-Cha/releases/tag/v0.1.0> with `Cha-Cha-0.1.0-windows-x64.zip` (24 MB) and
  `Cha-Cha-0.1.0-x86_64.AppImage` (72 MB). GPL-3.0. README rewritten for GitHub (no emulator named in user-facing
  pages); its "What's next" lists only local network play, online play and the quest maker (user, 2026-10-28).
- **Playable on Windows**: boot → title → New Game / Continue → Moga Village (item box, carving and gathering seen
  by the user); saves load and write; intro movie; audio; keyboard + mouse and controllers. A full quest hunt end to
  end is still an open Phase 4 item.
- **Built into the game, always on** (no MODS switches, user decision): Item Sets in the item box menu (house box and
  small boxes; register / load, 5 sets per pouch, smart load, pouch-window preview with d-pad paging), hold to gather
  (every carve / gather / mine through `sCockpit::obtainItem?` 0x020ECA2C), monster HP scaling in Port Tanzia (MH4U
  table 0.65 / 0.80 / 0.90 / 1.0, every rank), DLC quests from files (MODS → Quests).
- **Graphics**: Tri look fitted to a Tri screenshot, screen-space GI, AO, enhanced DOF, vignette, texture packs and
  dumping, internal resolution up to 4×, unlimited FPS.
- **Front end**: launcher (PLAY / SETTINGS / MODS / QUIT) over the game's title; settings panel redesigned this
  session with the impeccable skill (Adwaita-style rounded groups, switches, tab pill, side pane, one 4/8/16/24/32
  spacing scale, equal margins); d-pad left/right change values (the row owns the keys so ImGui nav no longer jumps to
  Apply); footer control guide built from the current bindings and controller type (PlayStation symbols drawn).
  `PRODUCT.md` records the design decisions.
- **Linux**: ported this session (`docs/linux.md`). Builds in WSL2 Ubuntu 22.04, boots headless and in a WSLg window
  to the title (llvmpipe software Vulkan in WSL). Not yet tried on real Linux hardware / Steam Deck; sound, movies
  (libavcodec) and controllers there are unverified.

## Last commits (local `main`)

- `9e14504` README: What's next → local network play, online play, quest maker
- `315cca6` README for the release (what's new, getting started on both systems, DLC, texture packs)
- `efb634a` AppImage: system Vulkan loader; per-platform archive handle
- `8f1461c` Linux port: builds and boots to the title in WSL2 (+ `52cb1ba` LF for `*.sh`)
- `e84fbad` / `3ff6921` / `f713625` settings redesign, spacing, margins
- `e7e4b14` GPL-3.0, version 0.1.0 (exe version resource), release packaging
- Verified at the end: MSVC and clang-cl Release build warning-free, ctest 12/12, Linux build + AppImage boot.

## Publishing (important)

- Local `main` keeps the full history, which still contains Cemu code vendored on 2026-10-23 (removed 2026-10-24).
  **Never push local `main`.** The public history is the orphan branch `github` (tracks `origin/main`; started as one
  fresh commit, now `dd5ae30`).
- To publish later work: `git checkout github`, `git checkout main -- .` (plus `git rm` for files deleted on main),
  check for game data / emulator code, commit, `git push origin github:main`, `git checkout main`. Ask the user
  before every push or release. `gh` is at `C:\Program Files\GitHub CLI\gh.exe`, signed in as BoyClinton.
- Release files: `scripts/package-release.ps1 -Version x.y.z` (Windows zip from `build/clang`) and, in WSL,
  `scripts/build-linux.sh` then `scripts/package-appimage.sh ~/cha-cha-build x.y.z`. Bump the version in
  `runtime/boot/mh3u.rc` too. `gh release create vX.Y.Z <zip> <AppImage> --repo BoyClinton/Cha-Cha`.

## What's next (user's priorities)

0. **Phase 5.5 (user 2026-10-28, goals set, not started):** replace the ImGui launcher / panels with the game's own
   menus - GFX tab in the pause menu (first), Graphics Settings in Game Options, our remapping in Controller
   Settings, the main menu's DLC screen as the mods menu, a game-style on-screen keyboard, native folder picker on
   first start, then ImGui out of the build. Checklist in `docs/ROADMAP.md` § Phase 5.5.

1. **Local network play** and **online play** (Phase 6, option B of `docs/online.md`): replace the game's NEX
   backend behind `MtNet::Session` / `nNetwork::Session` with Cha-Cha's own; LAN discovery for local play, a small
   lobby server for online; HP scaling then takes the real hunter count.
2. **Quest maker** (Phase 6, plan changed by the user 2026-10-29): quests are made in game and shared as a
   generated code another player copies and pastes to get the same quest; no external tool. Document `rQuestData`
   (`.quest`, `SDTQ`), `rQuestList`, `.quest_btl`, `.supp` first. Checklist in `docs/ROADMAP.md` § Phase 6.
3. Smaller open items: HP scaling checked in a hub quest at Low / High / G rank; Linux on real hardware; a minimal
   FFmpeg (H.264 decoder only) to shrink the AppImage; a font to bundle on Linux (Bahnschrift can't ship; decision);
   the rest of the unchecked ROADMAP items.

## How to build / run

- Windows MSVC: `cmake -S . -B build/cmake`, `cmake --build build/cmake --config Release`; clang-cl: `build/clang`
  (`-T ClangCL`), faster for the generated code. CMake/ctest live in VS's
  `Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin`. Run `build/cmake/runtime/boot/Release/Cha-Cha.exe`.
- Windowed UI captures (ask the user first): `MH3U_LAUNCHER_OPEN=settings:video|input|general` or `mods`,
  `MH3U_SHOT=6`, `MH3U_EXIT_AFTER_SECONDS=7` → `build/shot_6.bmp`; convert with System.Drawing.
- Linux (WSL2 `Ubuntu-22.04`, user `boycl`): toolchain installed (clang 15, Ninja, `/opt/cmake` 3.31.6,
  `/opt/vulkan/1.4.363.0` LunarG SDK, libvulkan-dev, libavcodec-dev, SDL3 deps). Build tree `~/cha-cha-build`
  (generated game code in `~/cha-cha-build/recomp-out`, not the HDD's `recomp/out`). Test data folders
  `~/cha-data`, `~/cha-data-appimage`. linuxdeploy cached in `~/.cache/cha-cha-tools`.
- Debug switches: see `docs/runtime.md` and `docs/input.md` (`MH3U_EXIT_AFTER_SECONDS`, `MH3U_INPUT_SCRIPT`,
  `MH3U_GUI_TEST`, `MH3U_DUMP_GUI`, `MH3U_GI`, `MH3U_TRI_*`, ...).

## Gotchas

- **WSL from PowerShell:** inline `bash -lc "..."` gets mangled (PowerShell expands `$x`, Windows splits on spaces in
  the repo path). Write a script file to the scratchpad and run `wsl.exe -d Ubuntu-22.04 -- bash /mnt/c/.../x.sh`.
- Bash heredocs with Python: tricky quoting fails silently or with syntax errors; prefer the Edit tool or a script file.
- `docs/research-log.md` contains a stray 0x96 byte: edit it at the byte level (Python, binary mode).
- On Linux, guest memory pages in watched ranges are read-only (write tracking): kernel writes into them fail with
  EFAULT, so file reads go through a bounce buffer (`fs/host_tree.cpp`). Keep that in mind for any new syscall that
  writes guest memory.
- The auto-mode permission check blocks downloading and running outside tools and creating public things; ask the user.
- Windowed runs pop up on the user's desktop: ask first (memory). Headless runs are fine.

## Local state (not in git)

- `save/80000001/`: the user's save with item sets (`cha-cha-item-sets.ini`), DLC quests installed from
  `quests/DLC MH3U EU`. Backups in `build/save_*` (latest `build/save_backup_2026-10-28`).
- `build/release/`: the 0.1.0 zip and AppImage (plus an old `MH3U-PC-dev-9278e6a` zip). `build/ui/`: panel captures.
- `tools/bin/`: Cemu (reference only), wiiurpxtool, zarchive.
