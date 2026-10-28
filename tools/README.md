# Tools

Third-party binaries go in `tools/bin/` (gitignored). Note the version you installed
in the table so results are reproducible.

## Required

| Tool | Purpose | Where it goes | Version |
|---|---|---|---|
| [Cemu](https://cemu.info) | Reference emulator: run the game, convert dump → `.wua`, debugger, compare behaviour against our port | installed anywhere | |
| [ZArchive](https://github.com/Exzap/ZArchive) (`zarchive.exe`) | Extract `.wua` → `code/ content/ meta/` | `tools/bin/zarchive.exe` | |
| [wiiurpxtool](https://github.com/0CBH0/wiiurpxtool) | Decompress RPX/RPL sections → plain ELF | `tools/bin/wiiurpxtool.exe` | |
| [Ghidra](https://ghidra-sre.org) (12.0) + JDK 21 | Main disassembler/decompiler | installed anywhere (here: `D:\Projects\Tools\Ghidra - 12.0`, the default in `scripts/ghidra-headless.ps1`; override with `-Ghidra`) | |
| [GhidraRPXLoader (Maschell)](https://github.com/Maschell/GhidraRPXLoader/releases) | Loads RPX/RPL into Ghidra with imports resolved. Use the release zip built for your exact Ghidra version (v0.9.2 = Ghidra 12.0 fixes). The cemu-project fork has no releases | Ghidra extension | |
| [Ghidra-GameCube-Loader (Cuyler36)](https://github.com/Cuyler36/Ghidra-GameCube-Loader) | Supplies the Gekko/Broadway PowerPC language (**paired singles**); without it Ghidra mis-disassembles `ps_*`/`psq_*` | Ghidra extension | |
| Python 3.11+ | Our scripts (splitting, symbol tools, asset tools) | PATH | |
| CMake 3.25+ & MSVC (VS 2026 "Desktop development with C++" workload, which bundles CMake) or clang-cl | Build the PC runtime | PATH | |
| Git | Version control (code only, never game data) | PATH | |

## Recompilation / runtime (pick once we commit to the approach, see `docs/ROADMAP.md`)

| Tool | Purpose |
|---|---|
| A PowerPC → C static recompiler (custom, modelled on [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) / [N64Recomp](https://github.com/N64Recomp/N64Recomp)) | Translate Espresso (PPC750CL + paired singles) code to C++ |
| SDL3 | Window, input, audio on PC |
| Vulkan SDK (or D3D12 via [Plume](https://github.com/renderbag/plume)) | Graphics backend for the GX2 replacement |
| [Cemu source](https://github.com/cemu-project/Cemu) (MPL-2.0) | **Reference** for Cafe OS HLE + GX2/Latte behaviour and shader decompiling |
| [decaf-emu source](https://github.com/decaf-emu/decaf-emu) (GPL-3.0) | Reference for Cafe OS internals; note license before copying code |
| [wut](https://github.com/devkitPro/wut) headers | Cafe OS function signatures & struct layouts |

## Asset tooling (MT Framework)

| Tool | Purpose |
|---|---|
| ARCtool / own Python `.arc` reader | Unpack MT Framework `.arc` archives (zlib) |
| Noesis + MT Framework plugins | Inspect models/textures (`.mod`, `.tex`) |
| [WiiUDownloader](https://github.com/Xpl0itU/WiiUDownloader)-style `meta.xml` reading | Title IDs / versions |

## Useful-but-optional

- **IDA / Binary Ninja** – if you have them; Ghidra is the shared baseline.
- **objdiff** – function-level diffing if we ever pursue matching decomp.
- **Green Hills MULTI (GHS) compiler** – what Nintendo SDK titles were built with. Not freely available, so
  byte-matching decompilation is *not* the plan; we target functional equivalence.
