# Roadmap

## Strategy

Byte-matching decompilation is impractical: the game was built with Green Hills (GHS) MULTI,
which we don't have, and MH3U's executable is large (MT Framework engine + game). Instead:

1. **Static recompilation** – translate every PPC function in the RPX into C++ that runs on x64,
   operating on an emulated guest memory space and register context. This gets the *whole game*
   running early.
2. **HLE runtime** – implement the Cafe OS imports (coreinit, gx2, vpad, snd_core, nn_*, …) natively
   on PC instead of emulating the OS.
3. **Progressive decompilation** – replace recompiled functions with readable hand-written C++
   (`src/`) one at a time, verified against the recompiled version. Over time the guest-memory
   layer can shrink and the code becomes a real port.

This is the model used by XenonRecomp (Unleashed Recompiled) and N64Recomp, and is far more
reachable than a pure decomp.

## Phases

### Phase 0 – Groundwork
- [x] Dump game + update, convert to `.wua`, extract (`scripts/extract.ps1`)
- [x] Fingerprint target build (`config/target.txt`); decide which version we support → US update v32
- [x] Ghidra project with RPX loader; full auto-analysis (`ghidra/MH3U`, via `scripts/ghidra-headless.ps1`)
- [x] Inventory RPL imports → `config/imports.csv` (every Cafe OS function the game calls)
- [x] Resolve the 31 `OSDynLoad_FindExport` call sites → runtime-loaded imports (add to `imports.csv`) → 53 from swkbd/erreula
- [x] Inventory the asset tree in `content/` → `docs/formats/README.md` (+ `inventory.md`, `arc.md`, `config/resource_types.csv`)

### Phase 0.5 – Asset follow-ups (found during inventory)
- [x] Identify what `rEmulationShaderPackage` (`shaderPack.spkg`) and `.es` (`ESX`) shaders contain → GX2 binaries emulating the 3DS DMPGL pipeline; 667 unique shaders ([shaders.md](formats/shaders.md))
- [x] Decide shader strategy → **offline Latte→SPIR-V for all 667** (plan: [shaders.md § Translation plan](formats/shaders.md))
- [x] `rTexture` (`TEX`) header + mip layout → [tex.md](formats/tex.md), verified 17,052/17,052 (`scripts/mt_tex.py verify`)
- [x] Texture deswizzle (addrlib 2D/1D thin1) + decode to confirm BC1/BC3/RGBA8 (`scripts/mt_tex.py export`)
- [x] Cube-map layout (192 textures, +0x6C float block, 6→8 slices, per-slice rotation)
- [x] Filesystem overlay: update content over base content in the runtime `fs` layer (`runtime/fs/overlay_fs.*`, test `fs_overlay`)

### Phase 1 – Understanding the binary
- [x] Identify MT Framework core: `MtObject`, `MtDTI`, allocators, `cResource`, `cSystem` singletons, threads (no job system in MT Lite) → [engine.md § Core](engine.md), `config/singletons.csv`
- [x] Use MtDTI tables + RTTI-ish strings to name classes & vtables → `config/symbols.txt` (1,251 DTIs, 3,946 vtables; [engine.md](engine.md))
- [x] Name virtual functions from vtable slots and resolve overrides → 26,236 functions (`config/virtuals.csv`)
- [x] Create Ghidra functions for all vtable targets + static initialisers (ctor table `0x101632C4`, 1,191); re-analyse → 79,851 functions
- [x] Apply `config/symbols.txt` to the Ghidra project (`ApplySymbols.java`, verified by `CheckCoverage.java`)
- [x] Catalogue secondary (multiple-inheritance) vtables → `config/secondary_vtables.csv` (40 tables, 382 this-adjust thunks; network layer only)
- [x] Find the property/`createProperty` virtual → decode XFS files generically (MtObject slot 3; `scripts/mt_xfs.py`, 848/848, [xfs.md](formats/xfs.md))
- [x] Locate main loop, scene/task system, render submission, file loading → [engine.md § Core / Render submission / File loading](engine.md) (no separate task system: the per-frame system list is it)
- [x] Function boundary list → `config/functions.csv` (88,225 functions tiling `.text`; `ExportFunctions.java`)
- [x] Find indirect-jump tables (switch statements) and vtable call sites → `config/switches.csv` (1,102), `config/indirect_calls.csv` (34,325)

### Phase 2 – Recompiler
- [x] PPC decoder covering Espresso: integer, FPU, paired singles, cache ops → `recomp/src/espresso.*`; all 4.61M words decode, 0 mismatches vs Ghidra on 640,600 distinct words
- [x] Loader: apply `.rela.*` relocations, import table + call sites → `recomp/src/loader.*`; equals Ghidra's memory byte-for-byte except 28 `$UNDEF` (→ 0) sites
- [x] Emit C++ per function; big-endian guest memory with byteswapping loads/stores → 88,226/88,226 functions, 100% of instructions, compile and link
- [x] Last 392 functions: atomics, multi-range bodies, fall-through into the next function, `lswi`, `tw`, `$UNDEF` calls, `.syscall` → **100%: 88,226/88,226 functions, 0 stubs**
- [x] Handle: switch tables, function pointers (lookup table), `mtctr/bctr` → `switch (ctx.ctr)` + `ppc::call_indirect`
- [x] setjmp/longjmp, exceptions if any → **none**: no EH sections or throw/setjmp runtime, no code reloads r1 from a buffer (only the GHS `or r1,r11,r11` epilogue), no fiber/context-switch imports; `std::exception`/`bad_alloc` exist only as library vtables
- [x] Output to `recomp/out/`, compiles cleanly under clang-cl → the whole tree builds with the VS ClangCL toolset (`cmake -B build/clang -T ClangCL`): 0 warnings, 9/9 tests, the game reaches the main menu; generated code needed no changes

### Phase 3 – Runtime (boot to first frame)
- [x] Boot: guest memory map, sections, stacks, heaps, data imports; `__rpx_start` runs the ctor table + `main` → [runtime.md](runtime.md)
- [x] coreinit: memory heaps (MEM*), threads (OSThread → host threads), mutex/cond/event/semaphore/message queue, alarms, time → enough for boot to the main menu (no fibers in this game; `partial` in imports.csv marks calls with simplified semantics, e.g. priorities)
- [x] FS: map `/vol/content` → `extracted/.../content`, `/vol/save` → user dir → overlay + synchronous `FS*`/`SAVE*` HLE; exercised by boot (archives, shaders, movies, sound) and save creation (`save/80000001/...`)
- [x] GX2: command buffer interpretation → Vulkan; texture tiling/untiling; surface formats → Vulkan renderer: draws with translated shaders, GPU surface cache for render targets, clears/copies/scan-out on the GPU, textures with all mip levels via our Latte surface layout (`runtime/gfx/addrlib/`), blending/depth/stencil/cull/alpha test, RECTS, GamePad screen inset (F1); every screen up to character creation matches Cemu. Render targets are never read back to guest memory: `MH3U_WATCH_TARGETS` found no CPU access to target memory from boot to character creation (re-check in gameplay)
- [x] Shader translator `tools/shaderc-latte/` (offline, all shaders → SPIR-V), in milestones ([shaders.md § Translator](formats/shaders.md)):
  - [x] GFD header parser + Latte disassembler → 668/668 unique shaders (by runtime key) decode
  - [x] Pixel shaders 581/581 → GLSL → SPIR-V, `spirv-val` clean
  - [x] Vertex shaders 82/82 (fetch-shader GPR convention: GPR i+1 ← attribute sq_vtx_semantic[i])
  - [x] VS+GS pairs 5/5 (rings → stage interface + copy shader)
  - [x] Runtime lookup by key; fetch shader → vertex input (CPU attribute conversion); draws on Vulkan
  - [x] Render comparison vs Cemu across more screens → 7 screens from boot to character creation, pixel-compared
    with Cemu's native screenshots: boot prompts bit-exact or near, the rest differ only by animation phase
    ([runtime.md § Render comparison](runtime.md), `scripts/compare-frames.ps1`)
- [x] Audio: AX/snd_core voices → SDL3 mixing → ADPCM/PCM decode, SRC, envelope, device mix → stereo SDL3 stream; intro, title music and effects confirmed by ear (2026-10-24). No AUX reverb (AXFX effects are stubs, as in Cemu without firmware files)
- [x] Input: VPAD (GamePad) + KPAD/WPAD (Pro Controller) → SDL gamepad/keyboard → keyboard + mouse look, Xbox/PlayStation/SDL controllers, DualSense touchpad → touch, rumble, `input.ini` bindings; Pro Controller on WPAD channel 0 (`MH3U_CONTROLLER=pro` / `pro-only`), which the game uses when its controller option says so – both drive boot, title and menus (docs/input.md). Gameplay button actions: Phase 4
- [x] Stub online (nn_ac, nsysnet, nn_act) to "offline" cleanly (+ nlibcurl, NSSL, nn_uds, nn_ndm, nn_acp, nn_fp pending)
- [x] Milestone: main loop runs (constructors, `main`, start-up loading, per-frame draws + `GX2SwapScanBuffers` at 60 Hz) – nothing rendered yet
- [x] Milestone: title screen renders (frame ~1000 with scripted A presses: "PRESS A", Ver. 1.02; H.264 movies via Media Foundation)
- [x] Milestone: main menu renders (New Game / Continue / Options / Data Transfer / DLC)
- [x] Milestone: character creation renders (player model with mipmapped textures)

### Phase 4 – Playable
- [x] Second screen (GamePad touch UI) → composited/secondary window or overlay → decided against: the port plays on the TV with a Pro Controller and no GamePad (default); the GamePad image is still available as an optional inset / full view (F2, `MH3U_GAMEPAD_VIEW`) for debugging
- [x] Text entry → PC keyboard instead of the Wii U software keyboard (swkbd HLE; Enter/A confirm, Esc/B cancel); character naming works
- [ ] Mouse: hovering highlights menu options on keyboard + mouse (needs the game's GUI/menu system mapped)
- [x] Saves load/write → a new character (Character Creation → Touch Panel Settings → Character Select slot 1) is written to `save/80000001/user1` (35,364 bytes) with `system` updated; after a fresh boot, Continue lists it (name, gender, play time, zenny, HR) and "Use this character? → Yes" loads it back into Moga Village
- [x] Milestone: in game – Moga Village renders (opening Village Chief scene, NPCs, player in starting gear)
- [ ] Hunt in a quest end-to-end; compare with Cemu
- [ ] Performance: framerate unlock investigation, resolution scaling — framerate done (variable-timestep game,
  `MH3U_FPS`, village ~155 FPS uncapped on an RTX 3060); resolution scaling not started

### Distribution (asset-free, Yakumo-style: a release exe + the user's own dump)
- [x] Read the dump in place: `.wua`, decrypted title folders (Dumpling/loadiine), folders of titles, Cemu `mlc01`;
  titles identified by `code/app.xml`; RPX inflated in memory and checked against the v32 SHA-1
  ([formats/dumps.md](formats/dumps.md), [formats/wua.md](formats/wua.md))
- [x] Data folder (portable `data\` / `%APPDATA%\MH3U-PC`), `config.ini`, `--install` / `--game`
- [x] First-start setup screen (Dear ImGui): pick + verify the dump, translate all 668 shaders in-process (~4 s)
- [x] Pipelines persist across runs (`pipelines.bin` + `VkPipelineCache`), prebuilt with a progress bar at start-up
- [x] `scripts/package-release.ps1`: portable zip with exe, SDL3, VC++ runtime, notices; guard against game files
- [ ] Encrypted dumps (`.wud`/`.wux`, NUS) with user-supplied keys
- [ ] Build from the dump too: `recomp_emit`, tests and tools read the RPX from a `.wua` instead of `extracted/`
- [ ] Full third-party license texts in the package (SDL3, shaderc/glslang/SPIRV-Tools); project license: GPL-3.0 (2026-10-28)
- [ ] Linux AppImage (user 2026-10-28; plan in [linux.md](linux.md)): port guest memory (mmap / mprotect, SIGSEGV dirty
  tracking), movies (libavcodec), WIC / BCrypt / Windows fonts / %APPDATA%; build in WSL2 Ubuntu 22.04; package with
  linuxdeploy. Release 0.1.0 (zip + AppImage) and the public GitHub push wait for it. 2026-10-28: ported, builds in
  WSL2, the AppImage boots headless to the title; not yet seen in a window or on a real Linux machine

### Player experience (requested 2026-10-26; Dusklight-style front end)
1. Controllers
   - [x] PS5 DualSense recognised: controllers that announced themselves to the launcher are now opened by host input
     too (`SDL_GetGamepads` at init); "DualSense Wireless Controller connected" in the log (driving the game: confirm)
2. No console window
   - [x] Players never see the black terminal: GUI-subsystem exe; logs go to `<data>/log.txt`; `--console` (or
     launching from a terminal) still shows output for development
3. Launcher / title screen (reference: Dusklight 2.0)
   - [x] Full-window front end before the game: logo, menu PLAY / SETTINGS / MODS / QUIT (mouse, keyboard, gamepad)
   - [x] Status line bottom-left ("Game ready." + region/version, or what is missing) and the build version bottom-right
   - [x] Shader preparation as a small "Compiling N shaders" toast with a progress bar instead of a separate screen;
     PLAY waits for it (the pipeline prebuild still shows its bar in the game window)
   - [x] Game picker moves into this front end (CHOOSE GAME on first start; Settings > change game / folder)
   - [x] Background: the game itself. It boots as soon as its shaders are ready and runs behind the launcher (intro,
     title screen), which is drawn over its frames in the same window (ImGui, Vulkan backend, recorded by the
     presenter); the game gets no input until PLAY, and buttons held then are ignored until released. The warm
     backdrop only stands in before the game shows frames (first start, loading)
   - [x] Pipeline prebuild as a launcher toast ("Compiling N pipelines") instead of the full-window bar: it runs on
     the guest thread before __rpx_start
   - [x] Display mode, bloom, DOF, AO and look apply live from the launcher; resolution, frame rate, VSync and a
     changed game say "next start"
   - [ ] Checked by eye in a window (runs clean with validation; reached the title states behind the overlay)
   - [x] Settings mid-game: F1 or the controller's Home / guide (or mic / capture) button opens them over the game
     (game input suspended, buttons held on closing ignored until released); F2 now cycles the GamePad view
   - [x] Controls page: keyboard / mouse and controller rebinding per game button (press to bind, Esc cancels,
     Delete clears), controller layout, mouse sensitivity, invert Y, reset; saved to input.ini, live in game
     (`bindings_test`)
4. Settings (stored in `config.ini`, applied on PLAY)
   - [ ] Display mode: windowed / borderless / fullscreen
   - [x] Internal resolution as a multiple of 1280x720: 0.5x, 0.75x, 1x (native), 1.5x, 2x, 3x (4K), 4x; independent of
     the window size
   - [x] Bloom on/off (uBloomFilter draw skipped; village frame compared)
   - [x] Vignette on/off (ours, in the post pass with the Tri grade; switched live, APPLY)
   - [ ] Depth of field Off / Original / Enhanced. The game's uDOFFilter/uBokehFilter never draw from the intro to Moga
     Village (Off skips them where the game does use them: not yet seen)
   - [x] Enhanced DOF (ours): background blur beyond the auto-focused hunter, from the scene depth buffer; village
     frame checked (far fence/wall soft, hunter and NPCs sharp), +0.4 ms GPU at 1080p
   - [x] Ambient occlusion on/off (ours, screen-space from depth; the game has none): contact shadows under slats,
     around barrels and posts in the village frame, +1.5 ms GPU at 1080p (half-resolution AO above 1440p)
   - [x] Look: Original (MH3U) / Tri (Wii-style: hazier, softer contrast, lifted shadows, glow; references: Wii
     screenshots of Moga Village and the Deserted Island)
   - [ ] Frame rate limit: display refresh / 30 / 60 / 120 / 144 / 240 / unlimited (the game is variable-timestep)
   - [ ] VSync on/off (FIFO vs mailbox/immediate presentation)
5. Skip boot notices
   - [x] The "game will begin set to the Wii U Pro Controller" notice (and the other aGame system notices) handled by
     rewritten game code: src/game/boot_prompts.cpp
   - [x] The "create save data?" / extended-data prompts answered Yes without being shown (fresh start, no input:
     Capcom logo -> intro movie, save written)
6. Resolution scaling (renderer)
   - [x] Render targets created at the chosen scale (viewports, scissors, copies, scan-out scaled; shaders that use
     texel coordinates: none, the translator samples normalised); village verified at 0.5x, 2x (3840x2160) and 4x
     (GPU 0.8 / 6.6 / 26 ms per frame on an RTX 3060)

### Cha-Cha front end (requested 2026-10-27)
1. Name
   - [x] The project is **Cha-Cha**: window title, launcher, executable (`Cha-Cha.exe`), release package, data folder
     (`%APPDATA%\Cha-Cha`, the old `MH3U-PC` folder is taken over), docs
2. Title screen (revised 2026-10-27: no key art or logo)
   - [ ] The game boots straight away and plays its own intro / title behind the launcher, which is a light overlay:
     only "CHA-CHA PRESENTS..." and the menu (PLAY / SETTINGS / MODS / QUIT), status and toasts; the game gets no input
     until PLAY
3. Settings (reference: Dusklight 2.0's settings window)
   - [x] One centred panel with tabs (General, Video, Input), label-left / value-right rows, a description pane for
     the selected row, a close button and APPLY (changes wait for it); the same panel before the game and over it
     (F1 / Home); window size separate from the internal resolution
   - [x] The controller drives the launcher with the game's bindings (A selects, B back, L/R tabs, Start applies)
   - [ ] Closing the panel mid-game (close button) leaves nothing behind on screen
   - [x] Settings redesign (user 2026-10-28): a cleaner, modern look in the spirit of GNOME Adwaita (rounded grouped
     rows, generous spacing, clear hierarchy, quiet controls) but keeping our own colours, not Adwaita's scheme; use
     the `/impeccable` design skill for the pass. 2026-10-28: done in setup.cpp (rounded groups with hairlines,
     switches, a tab pill in a header bar, side pane, footer; PRODUCT.md records the decisions); checked by the user
     with a controller
   - [x] D-pad / stick left and right change the highlighted row's value (up / down still move between rows)
   - [x] A control guide in the panel (e.g. "○ Confirm  ✕ Cancel  L/R Tabs") drawn from the current bindings, so it
     updates with remapping and with the Nintendo / Xbox layout
4. Text entry
   - [x] An on-screen keyboard drawn in the game window whenever the game asks for text (character name, chat):
     controller (d-pad / stick, A types, B deletes, Start confirms) and mouse; replaces typing into the window title
   - [ ] Button prompts on the keyboard (which controller button types, deletes, spaces, shifts, confirms)
5. Sound
   - [ ] Launcher sound effects: move, select, back, tab switch, apply, keyboard keys (generated, nothing shipped)
6. Frame rate
   - [ ] In-engine cutscenes (area introductions, arriving at Port Tanzia) are capped at 30 FPS: run them uncapped
     like the rest of the game
7. Texture packs (requested 2026-10-27)
   - [x] `texture_replacements` in the data folder: DDS files (BC1-BC7, RGBA8/BGRA8, own mips) named
     `tex_<w>x<h>_<CRC32>.dds` replace the game texture with that level-0 CRC, at any resolution; TEXTURE PACKS
     on/off in VIDEO, live (`docs/texture-packs.md`)
   - [x] DUMP TEXTURES in VIDEO (live, not saved): writes the textures the game uses to `texture_dump` under those
     names, with toasts (folder, running count) over the game; MH3U_DUMP_TEXTURES for runs
   - Packs made for the 3DS release are not supported and won't be converted (user decision 2026-10-29, after
     issue #5)
8. Mods panel (requested 2026-10-27)
   - [x] MODS from the title menu and over the game (F2; F1 switches to the settings, F3 now cycles the GamePad view),
     tabs QUESTS and GAMEPLAY
   - [x] Quest library `<data>/quests`: loose `q_<id>.quest` files and `quest1..4` files from a Wii U save; the QUESTS
     tab installs / removes each into the save's DLC slots (quest1-3 event, quest4 challenge, 20 slots each)
     (`runtime/boot/quests.*`, `quests_test`)
   - [x] Installed quests show at the Port Tanzia Gathering Hall (user, 2026-10-27: a quest installed before
     loading the save appears)
   - [ ] Quests installed while the game runs show without reloading: Cha-Cha writes the slot into the copy the game
     read (save_reads); check in game
   - [x] Packs (top-level folders, pack.json, cover) with All / Choose / Off; official DLC (from save files) defaults
     to All and installs itself; per-quest pictures and descriptions for custom quests
   - [x] Map where max HP is set: uEnemy::setMaxHp? 0x0259FB84 = base HP (em_status) x quest stat row (x0.55 for
     flagged quests); overridden to log every spawn (research log, "Monster HP")
   - [ ] Monster HP scaling by the number of hunters on the quest (1-4): find what marks a Gathering Hall quest (log
     a village and a hub quest) and the hunter count, then scale max HP in the override. Always on, no MODS switch
     (user 2026-10-28)
     Progress 2026-10-28: done in src/game/monster_hp.cpp - hub quests (sGame+0x1B9 != 1) not already x0.55, MH4U
     table 0.65 / 0.80 / 0.90 / 1.0 for 1-4 hunters (1 until online play), every rank. To check in a hub quest
     (log line "game_hp -> hp") at Low, High and G rank
   - [x] Item sets (MHGU style) in the game's own item box menu: a new option **Item Sets** when the box is opened,
     with a drop-down sub-menu **Register Item Set** (pouch -> set) / **Load Item Set** (refill the pouch from the box);
     needs the box menu, its option list and drop-down UI, and the pouch / box structures mapped (user 2026-10-27;
     always on, no MODS switch - 2026-10-28: the GAMEPLAY tab is gone)
     Progress: box menu mapped (sLobby box state, option tables, pouch / box); option 6 Item Sets with Register /
     Load Item Set implemented (src/game/item_sets.cpp, sets in save/80000001/cha-cha-item-sets.ini, always on);
     everything below seen working in a window by the user (2026-10-28)
     2026-10-27, user: menu, sub-list and help work; Item Sets drawn in row 3 under Combine/Manage Items. Still to do:
     - [x] the separator line crosses the Item Sets row: grow the upper (tan) part by a row, move the line and the
       three rows below it (blue part) and the scroll's bottom down (seen in a window 2026-10-27, Equipment Sets
       shown in row 7)
     - [x] the small box (Store / Remove / Combine, e.g. Port Tanzia's dock) gets Item Sets as a 4th row, its scroll
       a row longer (seen in a window 2026-10-27)
     - [x] several sets: choosing Register / Load opens another list of set slots, with a preview of the set's items
       (help box)
     - [x] pouch preview window (user 2026-10-27): the menus stay as they are (Register / Load Item Set, then the set
       list), plus the game's pouch window (uIDCommonPauch, as in Store Items) showing the highlighted set's loadout -
       Load: what it would put in the pouch from what the pouch and box hold (smart load); Register: the set as
       stored. D-pad left / right turn its pages (game page sound). Seen working in a window 2026-10-28.
       (Earlier idea, replaced: the Equipment Set screen's list as a screen of its own - Equipment Set state
       sLobby+0x72B4, states 0x10-0x13.)
     - [x] smart load: only what the pouch and box hold (1 + 3 with 10 in the set -> 4)
     - [x] Register read an empty pouch: the pouch was empty then; reads checked in a window (3 items)
   - [x] Hold to gather: holding the button keeps carving / gathering / mining until the spot is used up (every
     gatherable, user 2026-10-28; always on, no MODS switch). The obtain step every gatherable goes through
     (FUN_020ECA2C) makes the input layer repeat the held A (src/game/carving.cpp, input::repeat_held); seen working
     in a window 2026-10-28

### Phase 5 – Decompile & enhance
- [ ] Replace hot / important functions with hand-written code in `src/`
- [ ] Widescreen/high-res, uncapped FPS, mod loading from loose files
- [ ] Global illumination and a Tri look that really looks like Tri (user 2026-10-28: the current Tri shader "looks
  ugly"); see VIDEO effects (`runtime/gfx/renderer.cpp` scene effects)
  Progress: Tri reworked as a scene pass (warm grade, richer colour, soft glow, depth haze; HUD untouched) and
  screen-space GI added (VIDEO > GLOBAL ILLUMINATION); checked headless in Moga Village, to judge in play

### Phase 5.5 – In-game menus instead of ImGui (user 2026-10-28)
No launcher overlay and no Dear ImGui: the game starts as the game, and Cha-Cha's settings live in the game's own
menus, drawn with its own widgets. Order (user): the GFX tab first. Item Sets (`src/game/item_sets.cpp`) is the
model for adding to a game menu; MH3U_TRACE_TEXT finds a menu's code from its labels.
- [ ] Pause menu (village / field): a third tab **GFX** after Hunter / Info, an Options-style list (label left, value
  box right, help box below, page arrows) holding the graphics settings (resolution, frame rate, VSync, look, GI, AO,
  DOF, bloom, vignette, texture packs, ...)
- [ ] Title → Options (Game Options): a seventh box **Graphics Settings** with the same rows
- [ ] Game Options → Controller Settings: our remapping (keyboard / mouse and controller per game button, layout,
  sensitivity, invert Y), drawn in the game's UI style
- [ ] Main menu → DLC becomes the mods menu: Capcom's DLC download screen, listing the official DLC and the custom
  quests (packs) from `<data>/quests`, installed / removed from there
- [ ] Game Options holds Cha-Cha's own boxes next to the game's six (user 2026-10-28: "they could be here"):
  Graphics Settings, and the texture dump (below); more rows if the screen needs a scroll or a second column
- [ ] The game's DLC download loading screen and progress bar (find it from the DLC menu's download flow) reused for
  Cha-Cha's long jobs: shader translation / pipeline prebuild at boot, and the texture dump
- [ ] Texture dump of the whole game: every `rTexture` in every archive of the content (and update) tree, written as
  `tex_<w>x<h>_<CRC32>.dds` like today's dump, instead of only what the game draws while dumping is on; started
  from Game Options, progress on that loading screen
- [ ] On-screen keyboard redrawn in the game's style (our renderer, no ImGui)
- [ ] First start without ImGui: the system folder picker chooses the dump; shader / pipeline preparation on the
  game's loading screen (above); then every start is the game straight away
- [ ] Dear ImGui removed from the build (setup.cpp launcher, settings panel, MODS panel, toasts)

### Phase 6 – Online and quest maker (last phase; user decision 2026-10-28)
- [ ] Online play, option B of `docs/online.md`: replace the game's NEX backend with Cha-Cha's own - a backend behind
  the game's session interface (`MtNet::Session` / `nNetwork::Session`), matchmaking through a small Cha-Cha lobby
  server, data over direct UDP or a relay. Steps: map the session interface and how the Gathering Hall drives it
  (create / join / search, members, messages); `nsysnet` real sockets; the lobby server; friends reworked
  (replaces the old "local/LAN multiplayer reimplementation" item)
- [ ] Local network play (user 2026-10-28, listed in the README's What's next): hunt with players on the same
  network - the game's local-play backend (MtNet has Nex / Uds / Snow; `docs/online.md`) or the online backend
  below with LAN discovery instead of the lobby server
- [ ] Multiplayer hub quest scaling with real hunter counts: HP scaling (built for 1 hunter, `src/game/monster_hp.cpp`)
  takes the count from the online session; damage etc. if wanted
- [ ] Quest maker with share codes (user decision 2026-10-29, replaces the external tool writing quest packs):
  - [ ] Document `rQuestData` (`.quest`, `SDTQ`, 824 files in `nativeCafe/quest/{eu,us}`), `rQuestList` (`.list`),
    `.quest_btl` (`QTDB`) and `.supp` in `docs/formats/`: which fields the maker sets, their id tables
  - [ ] Maker in the game's own menus (no ImGui, like Phase 5.5): monsters, map, objectives, time, rewards, title
  - [ ] Share code: on saving, the maker shows a generated code with a Copy button (system clipboard). The code holds
    the maker's choices, not the SDTQ bytes (a quest file is up to 0x2000 bytes): a compact versioned record with a
    checksum, written in an unambiguous alphabet in short groups; the game rebuilds the quest from it, so the same
    code gives the same quest on Windows and Linux, and old codes keep loading after format changes
  - [ ] Enter code: paste (clipboard, on-screen keyboard as fallback), preview the quest, install it; bad or
    tampered codes are rejected with a message, never a crash
  - [ ] Made and received quests land in the MODS > QUESTS library (a "Shared quests" pack), quest numbers
    assigned locally; folder packs keep working for DLC and hand-made `.quest` files

## Risks
- ~~Paired-singles + GQR quantized loads are heavily used~~ – not in this game: only `ps_merge10` save/restore (Phase 2 finding).
- GX2/Latte is the biggest single chunk of work; use Cemu heavily as a reference for behaviour, but write our own code (no Cemu source is vendored).
- Threading: the game assumes 3 cores with specific affinities; races may surface on PC.
