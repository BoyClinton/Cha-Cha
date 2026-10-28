# Runtime (Phase 3)

`mh3u` (`runtime/boot/main.cpp`) runs the recompiled game (`recomp/out`, library `mh3u_recomp_game`) on HLE
implementations of the Cafe OS libraries (`runtime/os`). Run from the repo root: `build/cmake/runtime/boot/Debug/Cha-Cha.exe .`
(`MH3U_TRACE=1` prints the first calls of every import and HLE details such as file opens and heap carving;
`--headless` runs without a window; `MH3U_VK_VALIDATION=1` enables the Vulkan validation layer;
`MH3U_EXIT_AFTER_FRAMES=N` stops cleanly after N frames and prints frame/time/import statistics).

## Boot
1. Load + relocate `MH3G_Cafe_US.elf` with the recompiler's loader; copy `.syscall/.text/.rodata/.data/.module_id`,
   zero `.bss/.ipc_smem` into guest memory.
2. Commit the other regions (below), create the MEM2 expanded heap and the MEM1 / foreground frame heaps, write the
   data imports (function pointers for `MEMAllocFromDefaultHeap(Ex)`/`MEMFreeToDefaultHeap`, `__gh_FOPEN_MAX` = 20
   (assumed), `environ` = 0), register every HLE function by name (`ppc::register_import`).
3. Main thread: 1 MB stack at 0x60000000, guest `OSThread` in the HLE area, `argv = {"MH3G_Cafe_US.rpx"}`.
4. Open the window (`runtime/gfx/presenter`: SDL3 + Vulkan device/swapchain) on the host main thread, which then
   only pumps window events; the guest boots on its own host thread.
5. Call the ELF entry `__rpx_start` (0x02E84370). The guest crt itself runs the static constructor table
   (`0x02E842EC` walks `0x101632C4` until a null entry) and then `main`; `exit()` unwinds back as `GuestExit`.

## Guest memory map (`runtime/core/guest_memory.h`)
The whole 32-bit space is one 4 GB host reservation (guest address A = base + A, as the generated code assumes);
regions are committed explicitly, so a stray access faults and is reported with its guest address.

| Range | Use |
|---|---|
| 0x02000000–0x031963A4 | `.syscall` + `.text` |
| 0x10000000–0x104D8FC0 | `.rodata` `.data` `.module_id` `.bss` `.ipc_smem` |
| 0x10600000–0x50000000 | MEM2 arena = default/base expanded heap (`MEMGetBaseHeapHandle(1)`) |
| 0x60000000–0x64000000 | guest thread stacks (main thread: first 1 MB) |
| 0x70000000–0x71000000 | runtime-owned guest objects (OSThread, argv, OSSystemInfo, scratch) |
| 0xC0000000–0xC0010000 | import area: data imports are read at their symbol addresses (e.g. 0xC0007F88) |
| 0xC1000000– | fake code addresses for host functions reachable through guest pointers (never mapped) |
| 0xE0000000 + 40 MB | foreground bucket (frame heap, `MEMGetBaseHeapHandle(2)`; size assumed) |
| 0xF4000000 + 32 MB | MEM1 (frame heap, `MEMGetBaseHeapHandle(0)`) |

## Graphics
`runtime/gfx/gx2.cpp`: GX2Init and the setup the game performs – TV (1080p, RGBA8 `0x1A`, triple-buffered) and
GamePad (854x480) scan buffers (`GX2CalcTVSize/DRCSize`, `GX2SetTVBuffer/DRCBuffer`, scale/gamma/enable), swap
interval, context state, `GX2CalcSurfaceSizeAndAlignment` (our Latte surface layout, `runtime/gfx/addrlib/`, test `gfx_addrlib`:
exact level count, sizes, pitch, alignment, per-level tile mode and mip offsets – an earlier approximation padded every
mip level to 0x800 bytes, so the game's back-to-back `.tex` mip chains were read from wrong offsets), HiZ and
fetch-shader/ring-buffer sizes, GX2R surfaces from the default heap, timestamps, vsync (16.7 ms pacing),
`GX2SwapScanBuffers` → presents the TV scan-out image. Tiling apertures return the tiled image
as-is. Device used here: NVIDIA GeForce RTX 3060.

### Rendering, stage 1 (CPU) – now the fallback
- **Display lists.** Command-emitting GX2 calls are registered through `recordable<F>`: while a list is open on the
  calling thread (`GX2BeginDisplayListEx`) the call is appended (function + r3..r10 + f1..f8) instead of run;
  `GX2EndDisplayList` stores the list by buffer address and returns a nominal size (16 bytes/command, ≥ 0x20, capped
  to the buffer) – a zero size had made `sLtRender` skip `GX2DirectCallDisplayList` entirely. DirectCall replays it.
- CPU versions of clears (RGBA8, RGB10A2, RGBA16F, R32F; D24S8/D32F/D16), `GX2CopySurface` (detile → retile with
  `runtime/gfx/tiling.*`, addrlib R6xx thin1, checked against `scripts/mt_tex.py` in the `gfx_tiling` test) and TV
  scan-out (detile + convert to RGBA8) remain for surfaces the renderer cannot take (unsupported formats, no device).

### Rendering, stage 2 (Vulkan) – `runtime/gfx/renderer.*`
The window and the device come from `Presenter`; `--headless` creates the same device without a window, so frame
dumps render too. Required: Vulkan 1.3 (dynamic rendering, demote-to-helper, synchronization2) and
`VK_KHR_push_descriptor`-capable hardware; `VK_EXT_depth_clip_control` is used for GX2's -1..1 clip-space depth.
- **State.** gx2.cpp collects a `DrawState`: shaders (VS/PS, GS when `GX2SetShaderModeEx` mode 2), fetch-shader
  streams, attribute buffers, per-stage uniform blocks, pixel textures + samplers, colour/depth buffers, viewport,
  scissor, blend (`GX2SetColorControl`/`BlendControl`/`TargetChannelMasks`/constant), depth, cull, alpha test,
  point size, clip control, stencil (`GX2SetDepthStencilControl` – arguments 9-13 come from the caller's stack
  parameter area, which display lists now record too – and `GX2SetStencilMask`; GX2 compare/stencil-op enums equal
  Vulkan's; masks and reference are dynamic state).
- **Shaders** are looked up per guest shader struct by the translator's key (FNV-1a 64 over register block + program,
  `tools/shaderc-latte`), loaded from `build/shaders/<key>.<stage>.spv` with its `.meta` (uniform blocks, samplers,
  attribute semantics), which defines the descriptor set layout. A missing file skips the draw with a warning.
- **Fetch shaders.** `GX2InitFetchShaderEx` generates no microcode: it copies the `GX2AttribStream` array into the
  program buffer. At draw time each VS input semantic is matched to a stream (location), and the CPU converts the
  referenced vertices (big-endian attribute data; UNORM/SNORM/INT/SCALED/FLOAT formats; the stream's component
  selection) into `uvec4` GPR values. Indexed draws are rebased to the index range; QUADS/QUAD_STRIP/LINE_LOOP become
  index lists; RECTS (3 corners per rectangle) get their fourth corner as v1 + v2 - v0 from the decoded attributes
  – exact for vertex shaders affine in their inputs – and become two triangles.
- **Uniform blocks are already little-endian** in guest memory (the game swaps them for the GPU, unlike vertex
  data): they are copied as is into a per-frame host-visible upload buffer.
- **Surfaces.** Render targets live in a cache keyed by guest image address. Clears, `GX2CopySurface` and
  TV scan-out of cached targets run on the GPU (copy/blit into a TV image the presenter blits to the swapchain);
  nothing is written back to guest memory. Textures are detiled with addrlib addressing (every mip level and
  slice; a macro-tiled mip chain takes its swizzle from bits 8-10 of the mipmaps pointer) and uploaded, re-uploaded
  when a hash of level 0 + mip chain changes; samplers use the guest min/max LOD and mip filter; a texture whose image is a cached target samples that image (an R32F target sampled
  with depth compare goes through a D32F copy). A draw that samples its own colour target samples a copy taken just
  before it.
- **GamePad screen.** `GX2CopyColorBufferToScanBuffer` to the DRC goes to a second scan-out image; the presented
  frame shows it not at all (default – the port plays on the TV with a Pro Controller), as a 30%-width inset in
  the bottom-right corner, or instead of the TV image: F2 in the window cycles, `MH3U_GAMEPAD_VIEW=off|pip|only` sets
  the start. Frame dumps include the inset.
- **Pipelines** are cached by shaders, vertex layout, topology, target formats, blend/write masks, depth and cull
  state; viewport (flipped: negative height, GX2 viewports are y-up), scissor and blend constant are dynamic.
- **Debugging.** `MH3U_DUMP_FRAME=N[,N...]` writes `build/frame_N.bmp` (read back from the GPU), `build/frame_N_draws.txt`
  with the frame's ordered events (clears, draws, copies, scan-out) and a per-draw trace (shader keys, decoded first
  vertices per attribute, targets, textures, uniform values); `MH3U_DUMP_TARGET=<hex image address>` also writes
  `build/target_<addr>.bmp`. `MH3U_RENDER_DEBUG` flags: 1 no alpha test, 2 no blending, 8 only geometry-shader draws,
  16 log the write-watched texture memory.
  `MH3U_VK_VALIDATION=1` enables the validation layer (clean over 600 frames, headless and windowed).

At boot the game shows its first screen correctly: the "Wii U GamePad has been detected" notice (clear, a
full-screen pass, a translucent panel, 8 gold frame sprites and 2 text runs – all point sprites expanded by the
`1e2db7fd…` geometry shader – then a copy to the GamePad buffer), and waits there for a button. Debug build: ~44 fps
headless.

## Audio
`runtime/audio/ax.cpp`: AX (snd_core) with a software mixer modelled on Cemu's `ax_mix.cpp`. 96 guest `AXVoice`s.
Every 3 ms AX frame (96 samples at 32 kHz) each playing voice is decoded (DSP ADPCM with its coefficients, predictor
and loop context; PCM16 big-endian; PCM8) through linear sample-rate conversion (16.16 ratio), scaled by its VE
envelope (volume + per-sample delta), low-passed if enabled, and mixed into the TV main bus by its device mix
(`AXSetVoiceDeviceMix`: 6 channels L R SL SR C LFE x 4 buses of {u16 volume 1.15, s16 delta}); the game sends each
voice to a single channel. Voices with no TV mix but a GamePad (DRC 0) mix play through their DRC L/R volumes. The
bus is downmixed to stereo (C and surrounds at -3 dB, LFE dropped) and queued on an SDL3 audio stream; frames are
produced whenever fewer than ~40 ms are queued, so AX time follows the audio clock. State and offsets are mirrored
into the guest structs, then the game's frame callback runs on its own guest stack.
- Not done: AUX buses / AXFX reverb (callbacks registered, never run), biquad, final-mix callbacks, 5.1 output.
- `--headless` or `MH3U_AUDIO=0`: no device, a steady 3 ms clock. `MH3U_AUDIO_WAV=<file>` records the stereo output.
- Checked by recording the boot (music and effects at the expected times, tonal rather than noise) and by ear:
  intro, title music and menu effects sound right.

## Frame pacing
The game runs on a variable timestep: `sMain::framePacing?` (`0x0294B8DC`) sets `sMain+0x7C` to the real time since
the last frame in 1/60 s units (`+0x4C` = 60, `+0x76` fixed-step flag = 0, its own minimum interval `60/+0x50` =
1/3000 s). Its frame rate is only bounded by the swap interval it requests (`GX2SetSwapInterval(2)`: 30 FPS in game),
so uncapping does not speed the game up (at ~147 FPS the delta reads 0.41).

`GX2SwapScanBuffers` submits and presents the frame, then sleeps to the next flip time (the GPU works meanwhile):
- `MH3U_FPS` – default in a window: the display's refresh rate; headless: `game` (60 Hz / swap interval, as the
  hardware flip). `MH3U_FPS=<n>` paces at n FPS, `max` (or 0) is unlimited.
- Presenting never waits for vsync on top of that: MAILBOX, else IMMEDIATE; `MH3U_VSYNC=1` forces FIFO. Two presents
  in flight.
- The window title shows the FPS (game swaps per second), updated twice a second.

Moga Village, headless, RTX 3060 (`MH3U_FPS=max`): 12–16 FPS → ~155 FPS after the October 2026 work (research log);
GPU ~2 ms per frame; the game thread's remaining cost is the game's own update (~6 ms at 30 FPS) plus our draw
translation and Vulkan driver work.

## Profiling
- `MH3U_PROFILE=1` samples every thread's stack at ~1 kHz and prints at exit, per thread (named: guest threads by
  `OSThread` address/entry/priority): top functions (self), the functions on the stack most often (inclusive), and
  the blocking sites of samples outside Cha-Cha.exe. Also logs the FPS every 5 s (headless too). Build `RelWithDebInfo`
  for names (`build/clang/runtime/boot/RelWithDebInfo/Cha-Cha.exe`).
- The renderer's exit summary includes GPU ms/frame (timestamp queries) and MB streamed per frame over the last 600
  frames, write-watch passes, texture hashes and vertex/index conversions. `MH3U_RENDER_DEBUG=16` logs the
  write-watched texture memory.
- `MH3U_INPUT_CLOCK=time` makes `MH3U_INPUT_SCRIPT` numbers 1/60 s of wall time instead of swap counts, so scripted
  routes work at any `MH3U_FPS`. Village benchmark on a save with a character:
  `MH3U_INPUT_SCRIPT="1000:A;1200:A;1450:A;1650:A;1850:A;2100:A;2350:A;2600:A;2850:A"` (Continue → slot 1 → Yes), in
  the village after ~40 s.

## Render caches (what keeps a frame cheap)
- Vertex data (expanded to vec4 per attribute) and rebased index lists are cached by stream layout / index buffer and
  reused while a hash of the raw guest bytes is unchanged; static entries live in a device-local arena (256 MB, then
  retired whole), entries that change on consecutive frames are streamed.
- Textures are re-hashed only when their pages were written: guest memory is reserved with `MEM_WRITE_WATCH` and a
  background thread stamps written pages (`GuestMemory::set_watched/written_since`). A texture the CPU writes within
  ~1 ms before it is drawn shows up a frame later.
- A colour target sampled with depth compare (the shadow map) is copied to its D32F alias only when written since.
- Render targets: other format/size views of the same address are kept (`spare_targets_`), not reallocated.
- Display-list command vectors are recycled; replay takes the GX2 state lock once per list; indirect calls use a
  direct-mapped table over the code range.

## Threads and synchronisation
Each guest `OSThread` is a host `std::thread` with its own `ppc::Context` on the stack the game passed to
`OSCreateThread` (created suspended, started by `OSResumeThread`, joined by `OSJoinThread`, `OSExitThread` unwinds).
`OSMutex` (recursive), `OSEvent` (manual/auto reset), `OSCond`, `OSSemaphore` and `OSMessageQueue` block on host
primitives keyed by the guest object address. Priorities/affinity are accepted but scheduling is the host's.

## HLE so far (status `partial` in `config/imports.csv`)
- coreinit: threads, sync, time (`OSGetTime` etc., 62,156,250 ticks/s), `OSGetSystemInfo`, `OSReport`/`OSPanic`
  (guest printf), MEM expanded/frame heaps + allocators + base heaps, `OSGetMemBound`, foreground bucket, FS*
  (synchronous API on `runtime/fs` OverlayFs: `/vol/content` update-over-base, `/vol/save` = `<repo>/save`),
  UC sys-config (English, USA), MCP product settings (USA), GHS crt locks, memcpy/memset/memmove, exit.
- nn_act: one local offline account (slot 1, persistent id 0x80000001, no NNID; server calls fail).
- nn_save: save paths over FS (`/vol/save/common`, `/vol/save/80000001`). proc_ui: always foreground.
- Library static initialisers imported from system RPLs (`__sti___*`) are no-ops.
- vpad: GamePad connected, nothing pressed; padscore (WPAD/KPAD): no controllers.
- OSDynLoad (swkbd, erreula) → applets created but never shown (all 53 exports).
- Offline/absent: nsyshid, mic, usb_mic (no devices), nsysnet/nlibcurl/NSSL (no network), nn_ac (not connected),
  nn_uds (unavailable), nn_ndm (accepted), nn_acp network time, sysapp launches, h264 (unavailable for now).
- dmae copies (with endian swap), IM dimming (disabled), IPC buffer pools, OSAlarm (host alarm thread calling the
  guest callback on its own stack), hardware registers (read 0, every access logged once).

## Where boot stands
**The game reaches its main loop and runs it.** After the constructors and `main` it carves its heaps, starts its
threads, sets up GX2, input, applets and audio, loads its start-up data through FS (shader packages
`LtUserShaderPackage.lfx` and `shaderArc.arc`, system textures `sysfont_AM_NOMIP.tex` / `DefaultCube_CM.tex` /
`HbmIcon_GSM_HQ_NOMIP.tex`, the iWnn IME dictionaries, sound banks `initial.scsr` / `initial_d.sdsr`, the resident
`arc/ID/ID_resi_eng.arc` and `arc/shell/shell.arc`, jingles, `quest.stqr`), then every frame records display lists,
issues draws, copies the colour buffer to the scan buffers and calls `GX2SwapScanBuffers`. Its sound engine plays
voices. 900 frames run without a fault or a missing import; 228 distinct imports are exercised and ~400,000 GX2
calls accepted per 900 frames. Display lists are recorded and replayed, clears/copies/scan-out execute on the CPU and the window shows the TV
scan-out image. Draws run on Vulkan with the offline-translated shaders: the game's first screen (the GamePad
notice) renders correctly; with input (docs/input.md) the boot continues through the extended-data save prompt and
the intro movie (h264, below) to the **title screen** ("PRESS A").

## Movies (h264.rpl) – `runtime/os/h264.cpp`
The game's `uMovie::Player` demuxes the MP4 itself and feeds one Annex B access unit per `H264DECExecute` (with
`OUTPUT_PER_FRAME = 1`), expecting that unit's picture back through the output callback before it feeds the next.
The HLE decodes with the Windows Media Foundation H.264 decoder:
- **Work memory:** `H264DECMemoryRequirement` reports 20 MB up to level 4.1. The game allocates it from the heap that
  also holds its 28 NV12 frame buffers (2048x1088x1.5 each); reporting the largest level's 180 MB exhausted that heap
  and the player failed in setup (`FUN_02a4370c`).
- **One picture per Execute, in decode order** (what the Wii U decoder does; the game orders pictures for display by
  timestamp): the SPS VUI is rewritten to `max_num_reorder_frames = 0`, an access unit delimiter is appended, the
  decoder runs in low-latency single-thread mode, and a held B picture is pushed out with a drain. Pictures are matched
  to the oldest pending access unit (MF re-stamps sample times in display order).
- **Output:** NV12 at pitch = width rounded up to 256, UV after `pitch * height`, coded height (1088) with crop, in an
  `H264DECFrameOutput` record (result 100, timestamp, size, pitch, crop, image pointer) – layout as in Cemu.
- No Media Foundation → pictures are delivered black so movies still run their course.

## Render comparison against Cemu
Method (repeatable): Cemu 2.6 in portable mode (`tools/bin/Cemu/portable/`, not in git) runs `game/mh3u.wua`; a
keyboard GamePad profile (`portable/controllerProfiles/controller0.xml`: Return = A, Backspace = B, arrows = d-pad)
drives it, and **F12** saves the TV image at its native 1920x1080 to `portable/screenshots/` (keys must be held
~0.3 s to be seen by the game). The port dumps the same screens with `MH3U_DUMP_FRAME` (with `MH3U_GAMEPAD_VIEW=off`
so the inset does not cover the image; boot prompts need an empty `save/`). `scripts/compare-frames.ps1` prints
mean difference / PSNR / share of pixels off by > 24 and writes a difference map and a side-by-side image.

Results (2026-10-24, outputs in build/compare, not in git):

| Screen | Mean abs diff | PSNR | Pixels > 24 | Remaining difference |
|---|---:|---:|---:|---|
| "Wii U GamePad has been detected" | 0.00 | identical | 0 % | none (bit-exact) |
| "Save data is required" Yes/No | 0.01 | 58.7 dB | 0 % | none visible |
| "Set extended data save settings" | 0.01 | 62.3 dB | 0 % | none visible |
| Title ("PRESS A") | 0.46 | 41.4 dB | 0.14 % | logo shimmer / pulsing A (animation phase) |
| Main menu | 0.10 | 43.3 dB | 0.52 % | blinking highlight of the selected item |
| Character creation | 1.24 | 30.4 dB | 1.8 % | player idle pose; UI identical, textures/shading match at 3x zoom |
| Character creation + name notice | 1.01 | 31.0 dB | 1.4 % | same |

No rendering differences beyond animation timing were found on these screens. Not compared yet: the GamePad screen
(Cemu's view swap did not respond to Ctrl+Tab), the intro movie, and gameplay scenes.

## Debugging stalls
- With `MH3U_TRACE=1` the exit summary lists every import's call count and **every guest thread**: entry point,
  creation frame, import count, last import, and – for blocking HLE waits (mutex, cond, event, semaphore, message
  queue, join) – the object waited on, the guest call site and since which frame.
- `MH3U_EXIT_AFTER_SECONDS=N` exits (with the summary) by wall clock, for runs whose frames stop.
- `MH3U_WATCH_TARGETS=1` makes the guest pages of every cached render target no-access after each frame and logs
  the first CPU access to each (`watch: CPU read from …`), to find code that would need render targets read back
  to guest memory (none found up to character creation).
- `MH3U_PEEK="@<addr>:<off>,..."` prints u32 fields of a guest object at exit (`"<vtable>:<off>,..."` scans for
  objects by vtable; slow in Debug).
- `ghidra/scripts/Decompile.java <out file> <addr>...` decompiles functions (output stays in build/).


## Assumptions to revisit
`__gh_FOPEN_MAX` = 20; foreground bucket 40 MB; MCP `product_area`/`game_region` offsets; FSStat flag values;
`FSSetUserData` offset in the command block; nn_act / nn_uds failure Result value; bsp hardware version
0x26100010; MEM_BASE_HEAP_FG = 8 (confirmed by the game's own use); `OSCancelThread` ignored; `_iob` has no
backing storage (stdio HLE must treat `FILE*` as identity).
