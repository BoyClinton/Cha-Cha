# Research log

Dated, append-only notes of discoveries. Newest at the bottom. Each entry: what was found, where
(address / file), how it was confirmed, and what it unblocks.

## 2026-10-22
- Project skeleton created. No dump analysed yet.
- Extracted `game/mh3u.wua` (5.8 GB) → `extracted/` (6.2 GB). Contains two titles, no DLC:
  - `0005000010118300_v0` – base game, US (`MH3G_Cafe_US.rpx`, 9.5 MB → 25.2 MB ELF)
  - `0005000e10118300_v32` – update v32 (`MH3G_Cafe_US.rpx`, 11.2 MB → 28.0 MB ELF)
  - Internal name `MH3G` (Japanese title MH3G). Assets under `content/nativeCafe/arc/**.arc`.
  - Both ELFs have the Cafe header (OSABI `0xCA`, e_type `0xFE01` = RPX); hashes in `config/target.txt`.
- **Canonical target decided: update v32 RPX** (`extracted/0005000e10118300_v32/code/MH3G_Cafe_US.rpx`,
  SHA1 `d97aabfc5a45362f88dfacb09f23c6dcae6faac2`). All addresses in `config/` refer to this build.
- Ghidra 12.0 headless import + full auto-analysis of v32 RPX into `ghidra/MH3U` (≈12 min, 6 GB heap).
  Loader: Maschell GhidraRPXLoader 0.9.2; language `PowerPC:BE:32:Gekko_Broadway` (GameCubeLoader).
  Summary via `ghidra/scripts/ProgramSummary.java`:
  - `.text` 0x02000020–0x031963A3 (~19.6 MB code), `.rodata` 0x10000000, `.data` 0x10164640,
    `.bss` 0x1030A740–0x10470F23. Import stubs live in `.fimport_*` blocks at 0xC0006040+.
  - 60,690 functions; 13,630 paired-single (`ps_*`/`psq_*`) instructions decoded correctly.
  - 578 imports from 22 RPLs: coreinit 156, gx2 101, snd_user 37, snd_core 37, nn_fp 32, nsysnet 29,
    padscore 29, nn_uds 28, nlibcurl 24, vpad 20, nn_act 17, nn_ac 13, h264 12, nsyshid 9, nn_save 9,
    usb_mic 9, mic 8, proc_ui 7, nn_ndm 5, dmae 2, sysapp 2, nn_acp 2.
  - Analysis warnings: "Unable to resolve constructor" decompiler errors only inside `.fimport_*`
    stubs (0xC0007xxx–0xC0009xxx) – harmless. "Demangle CodeWarrior" analyzer crashed (NPE, from
    GameCubeLoader); irrelevant, the binary is GHS-built.
  - Notable: `nn_uds` (local wireless) and `h264` (video decode) imports; `nlibcurl`/`nsysnet` = online.
- Import inventory → `config/imports.csv` (via `ghidra/scripts/ExportImports.java`): 588 entries =
  578 function stubs + 10 data imports (all coreinit: default-heap function pointers
  `MEMAllocFromDefaultHeap[Ex]`/`MEMFreeToDefaultHeap`, GHS libc `_iob`, `environ`, `__gh_FOPEN_MAX`,
  C++ exception/atexit hooks). Every import has ≥1 reference.
  - References by library: coreinit 6205, gx2 309, snd_core 192, nlibcurl 130, snd_user 110, nsysnet 70,
    nn_fp 59, nn_uds 40, padscore 37, others < 35.
  - Hottest: memcpy 2252, OSUnlockMutex 872, OSLockMutex 782, memset 575, OSReport 190,
    OSGetSystemInfo 178, MEMAllocFromAllocator 159, MEMFreeToAllocator 135, OSSleepTicks 92.
    → memcpy/memset and mutexes must be fast native implementations.
  - **`OSDynLoad_FindExport` has 31 call sites**: the game resolves extra functions at runtime, so
    `imports.csv` is not the complete HLE surface. Next: find the export-name strings passed to it.
  - Libc is GHS's (`__gh_*`, `__ghs_flock_*`), supplied by coreinit – confirms GHS toolchain.
- Resolved all 31 `OSDynLoad_FindExport` call sites (`ghidra/scripts/ResolveDynLoad.java`) → **53 runtime
  imports, 0 unresolved**, appended to `config/imports.csv` as `kind=dynfunc`:
  - Only two modules are loaded at runtime: **`swkbd.rpl`** (software keyboard, 33 exports) and
    **`erreula.rpl`** (error/EULA viewer, 20 exports). Both are system applets – on PC they become a simple
    text-input overlay and a message box.
  - Mechanism: loaders `0x0307DC18` (swkbd) / `0x0308FC00` (erreula) call `OSDynLoad_Acquire(name, &handle)`
    (handles at `0x1040E0F4` / `0x1043341C`), then call 31 six-instruction wrappers
    `wrapper(void **out, const char *name)` → `FindExport(*handle, 0, name, out)`. Wrappers are shared by
    exports with identical signatures, hence 31 wrappers for 53 exports. Function pointers land in
    contiguous slot tables `0x1040E06C–0x1040E0EC` and `0x104333C8–0x10433414` (added to `symbols.txt`).
  - Export names are GHS/Cfront-mangled C++ (`SwkbdCreate__3RplFPUcQ3_2nn5swkbd10RegionTypeUiP8FSClient`
    = `Rpl::SwkbdCreate(unsigned char*, nn::swkbd::RegionType, unsigned, FSClient*)`), matching the wut
    `nn::swkbd` / `nn::erreula` headers.
  - Gotcha: the Ghidra decompiler fails on these wrappers ("Could not follow disassembly flow into
    non-existing memory at 0x04007180"): the RPX branch to an import stub encodes 0x04xxxxxx while the loader
    maps stubs at 0xC0xxxxxx. Use `SymbolicPropogator` (instruction-level) instead for such call sites.
    This will matter for the recompiler too: import calls must be matched by relocation, not branch target.
- Asset inventory (`scripts/asset_inventory.py` → `docs/formats/inventory.md`, `config/resource_types.csv`):
  - Effective content = base v0 overlaid with update v32: 7,807 files. The update only adds/replaces 35
    (item/text ARCs in `arc/ID`, 4 SE ARCs, and Japanese IME dictionaries `mw_iwnn/dic` for swkbd).
    → The PC filesystem layer must overlay update over base, like Cemu does.
  - ARC format v16 fully parsed on 4,609 archives / 58,609 entries; documented in `docs/formats/arc.md`.
    Decompressed size = `size_flags >> 3` (BE bitfield), verified 9,824/9,824 samples.
  - **Type hash = `~crc32(class) & 0x7FFFFFFF`**: all 45 resource types named from identifiers in the
    executable, one consistent hash variant → names are solid. Largest: `rTexture` 17,244 entries (3.35 GB
    uncompressed), `rSoundSourceDSPADPCM` 10,983, `rModel`/`rMaterial` 6,170 each.
  - `memory.ini` is a shared MT config with allocator budgets for **Win32**, CTR (3DS) and CAFE (Wii U):
    the engine ran natively on Windows during development. Useful as the heap layout for our runtime.
  - `shaderPack.spkg` is `rEmulationShaderPackage` (12.7 MB); `nativeCafe/es` holds 969 `ESX` shaders.
    Worth checking before writing a Latte shader translator: if these hold source/IR, GX2 shader translation
    could be partly avoided.
  - Many types are `XFS` (MT reflection serialisation) – decodable generically once MtDTI is mapped.
  - Movies are plain MP4 (2.3 GB), audio is DSP-ADPCM (`.dspw`, 1.8 GB).
- Shader investigation (`docs/formats/shaders.md`, `scripts/shader_inventory.py`):
  - **"Emulation shaders" emulate the 3DS GPU, not the PC.** The Wii U renderer is MT Framework *Lite* (3DS
    branch) with a DMPGL layer: 3DS fixed-function state (`dmp_TexEnv[0..5]`, PICA200 fragment-lighting LUTs
    `LutD0/D1/FR/RR/RG/RB`, gas fog, shadow texture) selects a precompiled GX2 pixel shader by 32-bit state
    hash (`es/<hash>.es`). Strings in RPX: `DMPGL`, `dmp_TexEnv[n].combineRgb`…, `rEmulationShader`,
    `rEmulationShaderPackage`, `rLtShader`, `sLtShader`.
  - All shaders are compiled GX2 (`Gfx2` v7.1 GFD) – no source/IR. RPX embeds none. Closed set of
    **667 unique shaders**: 582 pixel, 78 vertex, 7 vertex+geometry.
  - `shaderPack.spkg` = the 967 `.es` shaders bundled with ids (965 identical, 580 unique contents).
  - `LtUserShaderPackage.lfx` (MFX) names the whole effect system (techniques, VS/PS, IA layouts, CBs, states,
    `DMP_*`); its vertex constants are `c[0..95]` = PICA200 uniform registers.
  - Strategy impact: offline Latte→SPIR-V translation of a fixed, verifiable set becomes viable; or skip the
    582 pixel shaders entirely by generating PICA200 fragment shaders from DMPGL state (3DS-emulator style).
- **Decision (user): offline Latte → SPIR-V translation of all 667 shaders.** One uniform pipeline for every
  stage; no dependence on reversing the DMPGL state hash. Plan: `docs/formats/shaders.md` § Translation plan.
- rTexture (`docs/formats/tex.md`, `scripts/mt_tex.py`):
  - Header = magic + 3 MSB-first bitfields: version `0xA5` (as on 3DS) | flags; mips:6 width:13 height:13;
    faces:8 format:8 unk:16 (=8). Consistent on all 17,244 textures.
  - Only 3 formats: `0x0C` (8,905, 1 B/texel → BC3?), `0x0B` (7,941, 0.5 B/texel → BC1?), `0x03`
    (398, 4 B/texel → RGBA8). Names by byte rate; block decode still to confirm.
  - Data is the raw GX2 tiled surface + mip chain. Fitted size model – macro tile 32×16 elements, micro
    8×8, level 0 2D unless texel w<32 *and* h<16, later levels degrade to 1D when elements w<32 *or* h<16 –
    matches **17,052/17,052** single-face textures exactly. Cube maps (192) not yet: +0x6C bytes (SH
    coefficients?) and per-level 6-slice layout.
  - Next for textures: implement addrlib thin1 deswizzle (Cemu LatteAddrLib as reference), decode one BC
    texture to confirm formats, then fit cube maps.
- Texture deswizzle done (`scripts/mt_tex.py export`): addrlib R6xx thin1 micro/macro addressing, 2 pipes /
  4 banks / 256 B interleave, **pipe/bank swizzle 0**. Formats **confirmed**: `0x0B` = BC1, `0x0C` = BC3,
  `0x03` = RGBA8 (R,G,B,A byte order); BC blocks little-endian. Checked visually (BC1 lobby background,
  BC3 icon atlas with alpha, RGBA8 mip 1) and numerically: 1D-tiled L1 vs downscaled L0 = 2.7/255 mean over
  40 textures vs 34.2 for wrong addressing. Mip offsets and the 2D→1D degrade rule are thereby confirmed too.
- Cube maps solved: header + 0x6C bytes (27 floats, SH?) + per level the faces as slices – 6 slices at level 0,
  **padded to 8 at later levels** – with addrlib per-slice bank/pipe rotation (`^= slice*2`) in 2D tiles.
  Sizes 192/192; each face's 1D level 1 matches downscaled level 0 within 0.7–2.3/255; without rotation
  faces 1,2,3,5 break. All 17,244 textures in the game are now fully decodable.
- **MtDTI + RTTI recovered** (`docs/engine.md`, `ghidra/scripts/RecoverDTI.java`, `config/dti.csv`,
  `config/vtables.csv`, `scripts/merge_symbols.py`):
  - `MtDTI::MtDTI` = `0x02D63178`, args (this, name, parent, size, id, r8, r9); layout 0x20 bytes with the
    vptr at +0x1C (GHS puts vptr after the first polymorphic class's members); size = `w14 >> 9`;
    id = `crc32(name, ~0) & 0x7FFFFFFF` when 0 is passed (`0x02D63150`).
  - Static initialisers are mostly not Ghidra functions (only reached via the ctor table) – propagate
    constants over the range from the previous `blr`/`b`/gap to the next `blr`.
  - 1,251/1,251 registrations decoded; all 45 ARC resource types match DTI ids exactly.
  - GHS emits RTTI: vtables start `{0, typeinfo*}`, typeinfo `{0x1014E0B8, name, …}` → 3,946 named
    vtables (45,289 slots), 2,420 classes beyond the DTI set.
  - `symbols.txt` grew from 57 to 10,455 entries (DTI objects, MyDTI vtables, newInstance factories,
    vtables, typeinfos).
- **Virtual functions named** (`ghidra/scripts/NameVirtuals.java`, `config/virtuals.csv`, `docs/engine.md`):
  - GHS typeinfo base list at typeinfo+0x0C = `{base typeinfo*, flags}`; typeinfos are duplicated per TU, so
    bases must be matched by name (by address only 2,232 of 3,946 resolved; by name 3,490 + 125 roots).
  - Slot 0 = deleting destructor `(this, flags)` for 3,866/3,942 classes, verified by the `flags & 1` test
    shape (`rlwinm. rX,r4|copy,0,31,31`); named `X::~X`.
  - 26,236 distinct virtual functions: 23,635 override slots, 2,601 introduce new ones; 777 shared stubs;
    `0x03122C18` = pure-virtual handler (689 slots).
  - **15,535 virtual targets had never been disassembled by Ghidra** – auto-analysis misses everything only
    reachable through vtables. Must be fixed before the function list for the recompiler is trusted.
  - `symbols.txt`: 35,440 entries, 26,241 named functions.
- **Ghidra project repaired** (first writable run; backup in `build/ghidra-backup`):
  - Static ctor table = `0x101632C4`–`0x1016455C` (.rodata), 1,191 initialiser pointers.
  - `CreateFunctions.java` created 18,501 functions (ctor table + all vtable slot targets; 8,926 already
    existed, 0 failures); re-analysis 431 s → **79,851 functions** (was 60,690).
  - `ApplySymbols.java`: all 26,241 function names + 9,199 labels applied in 3,739 namespaces, 0 failures.
  - `CheckCoverage.java`: 26,241/26,241 named functions present, 45,289/45,289 vtable slots are functions.
- First runtime code: `runtime/fs` `OverlayFs` (C++20, CMake, MSVC /W4 clean). `/vol/content` and `/vol/code`
  layered update-over-base, `/vol/save` writable; guest paths normalised (`\`, `//`, `.`, `..`; climbing above
  `/` rejected). Test `fs_overlay` runs against the real dump: replaced/added/untouched files resolve to the
  right layer; merged `arc/ID` = 79 entries (69 base + 10 update-only). Phase 0.5 complete.
- **Function boundary list + switch tables + indirect calls** (Phase 1; backup `build/ghidra-backup-phase1`):
  - `.text` (`0x02000020`–`0x031963A4`) had 1.7 MB outside any function: small functions reached only via data
    (`blr` stubs, `li r3,0; blr`, vtable tail-call thunks). No zero padding exists in `.text`.
  - GHS switches are inline: `bctr` followed by N `b case` instructions (base = `bctr+4`, e.g. `0x02006EAC`).
    Guard is `cmplwi idx,N` + `bgt` (N+1 cases) **or `bge`** (N cases); index can be rebased (`subi idx,x,0x47F`
    at `0x0288266C`, 13 cases) or guarded by a binary search (`0x024069E8`, 8 cases – Ghidra had 225).
    1,102 switches / 26,096 cases; Ghidra had found 997 and missed case 0 on 17 of them. All 1,102 now agree.
  - Some switch tables `b` straight into other functions (tail-call cases, e.g. table at `0x022BACD0`); Ghidra
    glued ~100 KB of functions onto 0x78-byte `FUN_022ae750`. Split 122 entries out of 13 functions.
  - `0x02080120` was typed as data, cutting a deleting destructor's tail; fixed by hand.
  - Result: 88,225 functions, 0 overlaps, 24 bytes uncovered (data words), 0 mid-function branch targets;
    symbols/vtables still 26,241/26,241 and 45,289/45,289 (`CheckCoverage.java`).
  - Indirect calls: 31,901 virtual (28,593 calls + 3,308 tail), 789 pointer-to-member, 1,635 plain pointer.
- **MT Framework core** (`docs/engine.md` § Core):
  - `e_entry` = `0x02E84370` → `main` `0x02000020`: `sApp` on the stack (0x2D38 frame ≥ `sApp` 0x2D30), create
    main from `sLtMH3GMain::DTI` (`0x02B9AF24` calls MyDTI slot 1 through the DTI vptr at +0x1C), `run`
    `0x02B9AF98` = main loop `while(!main->+0x74) main->vf6()`.
  - `sLtMH3GMain::vf6` `0x02003954` drives 67 systems stored at `+0x1770..+0x1878`; mapped by following each
    `new` → ctor → `stw` and identifying ctors by the vtable they write (`CtorClass` scratch script).
  - Singletons: ctor stores `this` to `mpInstance`; 70 found (`FindSingletons.java`), 64 with ≥3 reads.
  - Allocator slots from `MtCafeHeapAllocator` imports: 6 alloc (`MEMAllocFromExpHeapEx`), 12 free
    (`MEMFreeToExpHeap`), 13 getSize (`MEMGetSizeForMBlockExpHeap`). Per-class allocator =
    `0x103D6100[(dti->w14>>3)&0x3F]` (`0x02D63E50`).
  - `MtObject` slot 4 = `getDTI` (`MtThread::vf4` returns `0x103F3370`); slot 3 likely `createProperty`.
  - No job system; threads are `MtThread` subclasses over a thin `OSCreateThread` wrapper `0x02D7C9DC`.
  - symbols.txt: 35,587 entries; Ghidra re-applied, `CheckCoverage` 26,317/26,317 and 45,289/45,289.

## 2026-10-23
- **Render submission** (`docs/engine.md` § Render submission): `GX2DrawEx` has 3 call sites, all in
  `0x02BBED98` (`buildDisplayList?`), which diffs consecutive draw records and emits only changed state into a
  GX2 display list; runs on `sLtRender::ExecuteThread` (event handshake `0x02BC2278`). Present =
  `0x02BBF9D0` (DirectCallDisplayList → copy to TV/DRC → swap → flush → vsync), reached from `sLtRender` slot 10.
- **File loading** (`docs/engine.md` § File loading): sResource loader thread `0x02BD75E0` (created with
  `0x02D7D0B4`, 0x40000 stack, prio 15). Path = `sprintf("%s\%s.%s")` at `0x02BD73A0` with cResource slot 6
  (verified: `rArchive::vf6` → "arc", `rTexture::vf6` → "tex"); `MtFile`/`MtFileStream` open → cResource slot 9
  `load(stream)` (`0x02BD7554`). sResource slots 11/12/13 = async request / sync create / find (names `?`).
- symbols.txt 35,602; Ghidra re-applied, CheckCoverage 26,331/26,331 + 45,289/45,289.
- **createProperty + XFS**: MtObject slot 3 = `createProperty(MtPropertyList*)` – `rPopData::vf3`
  `0x02071904` registers `mPopDataType` (helper writes type 6), `DataList` (0x19, not serialised),
  `PopEditData` after the base `cResource` props; `PopEditData::vf3` `0x02071F64` registers `flag`/`area` at
  +0x10/+0x12 with type 5. MtProperty: +0 name, +4 type<<16|attr, +8 owner. XFS files embed exactly this list;
  object size field counts itself; type 0x80 = resource ref (`u8 2`, class name, path). `scripts/mt_xfs.py
  scan`: 848/848 files exact, 23 classes all named via dti.csv. Format: `docs/formats/xfs.md`.
- **Secondary vtables** (Phase 1 done): GHS vtable words are `{s16 << 16, ptr}` – secondary header
  `{offset << 16, typeinfo}` (e.g. `0x101610F4` `{0x00680000, JobDataStoreCheckNotification ti}`), slots
  `{-offset << 16, thunk}` with thunks `subi r3,r3,offset; b target` (`0x022492B8`). Typeinfo base-list word:
  offset `w >> 16`, `0x200` = last (earlier "651 multi-base classes" was a mis-parse; real: 47). 40 secondary
  vtables, 406 slots, 382 thunks; all 23 expected (class, base) pairs found. Names `Class::__vtbl@Base`,
  `Class::__thunk_<off>_<target>`; symbols 35,957; Ghidra re-applied, CheckCoverage 26,646/26,646.
- **Phase 2 started – own recompiler** (decision in CLAUDE.md). Espresso decoder `recomp/src/espresso.*`:
  4,610,267 words in all function bodies decode (0 invalid); vs Ghidra 640,600 distinct words, 0 op / 0 register
  mismatches (`scripts/check_decoder.py`).
  - **Paired singles are unused by game code**: opcode 4 only as `ps_merge10 fN,fN,fN` (17,209, prologue/epilogue
    ps1 save), opcodes 56/57/60/61 count 0 in raw `.text`.
  - The decompressed ELF is unrelocated at import sites: ~6.9k distinct words (`bl` to `0x0319xxxx`, import-data
    `lis/addi`) differ from Ghidra's relocated memory; `.rela.text` (0x46AB74 bytes) etc. must be applied by the loader.
- **Recompiler loader** (`recomp/src/loader.*`): 524,610 RPL relocations (ADDR32/ADDR16_LO/ADDR16_HA/REL24); all
  internal ones are no-ops (file already linked), 7,298 import relocations change bytes; 588 imports = imports.csv;
  7,189 import call sites. `$UNDEF` (undefined weak, 28 sites in the crt, e.g. `0x02E843BC`, `0x102FFFBC`) → 0,
  where Ghidra uses placeholder 0xC001D000. With that, `.text/.rodata/.data` equal Ghidra's relocated memory exactly.
- **Emitter, first cut** (`recomp/src/emitter.*`, `recomp/include/ppc_context.h`): leaf functions only. 29,319 of the
  game's functions (245,885 instructions) emit and compile warning-free; self-test executes 10 hand-encoded
  functions correctly. Largest rejection classes: calls 44,795, tail branches 7,391, `bcctr` 3,826, LR writes 2,510,
  atomics (`lwarx`) 194.
- **Emitter: calls, tail calls, LR, imports.** Host-stack calls (`bclr` = return), tail calls to entries, imports via
  the loader's call-site map. 66,913/88,225 functions (61.6% of instructions) emitted; with stubs for the rest, the
  import stubs and an 88,225-entry function table the whole program compiles (0 warnings, ~3.5 min) and links.
  Remaining blockers: `bctrl` 16,745 functions, `bcctr` 4,159. 5 crt functions call `$UNDEF` weak symbols
  (`0x031270E4`, `0x03127DCC`, `0x0312C514`, `0x0312C568`, `0x0312C830`).
- **Emitter: switches + indirect calls.** Switch `bctr`s (config/switches.csv) → `switch (ctx.ctr)` over inline slots;
  `bctrl`/`bclrl`/plain `bctr` → `ppc::call_indirect` (function + import tables). Coverage 61.6% → **99.7%** of
  instructions: 87,833/88,225 functions; program compiles (0 warnings, ~4.5 min) and links. Left: 194 atomics,
  135 multi-range, 47 falls-off-end, 9 `lswi`, 5 `$UNDEF` calls, 2 `tw`.
- **Emitter: 100%.** Atomics (reservation + host CAS), multi-range bodies, fall-through into the next function (all
  47 "falls off end" cases end where another function starts), `lswi`/`stswi`, `tw`/`twi`, `$UNDEF` calls (→ address 0
  via call_indirect), and `.syscall` (0x02000000 = `nop; blr`, called by crt `0x0312C830` after `creqv eq`) as a
  function. Result: 88,226/88,226 functions, 4,610,269 instructions, 0 stubs; compiles (0 warnings) and links.
- **Phase 3: first boot of recompiled code** (`runtime/`, `docs/runtime.md`). `mh3u` maps the image, sets up
  memory/heaps/stack and calls `__rpx_start`; the guest crt runs the 1,191-entry ctor table (`0x02E842EC`) and
  `main`. HLE written as reached: coreinit base/memory/sync/time/threads/FS/UC/MCP, nn_act, nn_save, proc_ui.
  Findings: no code uses r2/r13 small-data bases; `OSIsDebuggerInitialized` must be 0 (else `tw 31` in
  `__rpx_start`); MEM1/foreground base heaps are frame heaps; the game takes the whole MEM2 base heap and carves 11
  `MtHeapAllocator`s; threads created: `sMain` 0x0294B204, sResource loader 0x02BD75E0 (stack 0x40000, prio 15 –
  as predicted in Phase 1), MtThread 0x02D7CA84. Boot reaches `gx2.GX2Init` with 53 imports exercised, no faults.
- **GX2 setup + window.** SDL3 (from the Vulkan SDK) window, Vulkan 1.3 device (RTX 3060) and swapchain on the host
  main thread; guest on its own thread. Game's GX2 setup: TV mode 5 (1080p), format 0x1A, buffering 3, buffer in the
  foreground-bucket heap at 0xE0001000. Found: `MEMGetBaseHeapHandle(8)` is the foreground bucket (not 2) – the game
  takes all of MEM1 (frame heap) and all of FG (then MEMCreateExpHeapEx on it). Boot continued through input,
  OSDynLoad (erreula/swkbd), peripherals/network (offline), alarms, and loaded `shaderArc.arc` + iWnn dictionaries
  via FS; registers on interface 6 are probed (32-bit, regs 3..0xE). Stops at `snd_user.AXFXSetHooks` (audio).
- **Silent AX → main loop.** With AX/AXFX HLE (voices advanced by pitch at 32 kHz per 3 ms frame, frame callback
  thread) the game boots into its main loop: display lists, `GX2DrawEx`, `GX2CopyColorBufferToScanBuffer`,
  `GX2SwapScanBuffers`. Start-up files loaded via FS include `LtUserShaderPackage.lfx`, `shaderArc.arc`, system
  textures, IME dictionaries, `initial.scsr`/`initial_d.sdsr`, `ID_resi_eng.arc`, `shell.arc`, `quest.stqr`.
  Unpaced it ran ~230 fps (headless) – the game paces on the swap, so swaps now complete on a simulated 60 Hz vsync.
  900 frames: no faults, 228 imports exercised, ~400k GX2 calls. Registers touched: interface 6 regs 0..0xE,
  interface 3 regs 0/2/3, 16-bit interface 0 regs 0x2/0x6C (all read as 0).
- **GX2 rendering, stage 1.** `GX2EndDisplayList` returning 0 was why `GX2DirectCallDisplayList` was never called:
  with a nonzero size the game replays its lists (~420 commands/frame). Per boot frame: ClearColor on the 1080p RGBA8
  (fmt 0x1A, 2D-tiled, pitch 1920) target `0x1668D454` to (0.2,0.2,0.2,1), ClearColor on a 1024x1024 R32F (0x80E)
  target to 1.0, ClearDepthStencil on `0x1668D774`, ~11 `GX2DrawEx` (VS `0x1676A880`/`0x167C49C0`, PS programs 0x310
  bytes), clear to black, one 854x480 draw (GamePad), `GX2CopySurface` ×2, `GX2CopyColorBufferToScanBuffer` from
  `0x145FA4B4` (1920x1080, tile 4, swizzle 0). TV image black – consistent with draws not being rasterised. Tiling
  (addrlib thin1) matches `mt_tex.py`. 600 frames, no faults.
- **Latte shader translator** (`tools/shaderc-latte/`, [shaders.md § Translator](formats/shaders.md)). Shader
  microcode in the GFDs is **little-endian** (headers big-endian). All 668 unique shaders by runtime key (82 VS,
  581 PS, 5 GS – a VS+GS GFD yields one of each) decode with no unknown opcodes, confirming the R700 encodings
  (OP2 opcode at ALU_WORD1[17:7]). Feature use is narrow: no relative GPR/constant addressing, no PRED_SEL, no
  uniform-register mode (all mode 1 or 2), only SAMPLE/SAMPLE_C, 10 DX10 loops (with ALU_BREAK), CUBE in 7 shaders.
  Every pixel shader declares the same 12 samplers (types: Texture0-3 2D=1, TextureCube0=4, TextureShadow=6,
  Lut* 1D=0; resource id = sampler id = location). Uniform blocks are read via KCACHE banks and dynamically via
  VTX fetch from buffer 0x80+block (0x81 joint matrices in VS, 0x85 in PS). POSITION_ENA pixel shaders put the
  window position in GPR0 with a semantic-0 placeholder input. Only 10 varying semantics exist game-wide (0x80-0x89).
  One pixel shader (`a5732af121663dd9`) has 7,661 CF instructions; the median is 22. GS pairs: VS writes the ES->GS
  ring with MEM_RING (dword array_base, elem_size 3), GS reads it relative to R0.x (vertex 0 only → points in),
  writes 11-vec4 vertices (sq_gs_vert_itemsize 44 dwords) to the GS->VS ring at absolute offsets, and the copy
  shader exports position + 10 params per vertex; outputs are 4- or 16-vertex triangle strips. Result: 668/668
  translated to GLSL, compiled with glslc and clean under spirv-val (Vulkan 1.3).
- **Draws on Vulkan; first screen renders** (`runtime/gfx/renderer.*`, docs/runtime.md § Rendering, stage 2).
  Findings: **uniform blocks are little-endian in guest memory** (values read LE: -1.0, 100.0, -1/1024; read BE they
  were garbage) while vertex attributes are big-endian (float positions decode correctly with GX2's default swap).
  The boot frame's clears all come before its draws. The "points" draws at boot are 2D sprites: VS `418dfbf3…`
  writes the ES→GS ring, GS `1e2db7fd…` expands each point to a quad (attribute semantics: 0 rect x0,y0,x1,y1 in
  pixels, 1-4 corner colours UINT 8888, 5 UV rect in texels, 6 flags). Its branches write their 4 output vertices
  at the same GS→VS ring offsets, so the copy-shader vertex index must be the run-time emit count (translator fix).
  Two full-screen passes sample the render target they write (`d45d1834…` on the TV target, `e86db455…` copies
  TV → GamePad 854x480). Fonts: 1024x1024 BC2 (fmt 0x032) and 1024x512 RGBA8 atlases. The first screen is the
  "Wii U GamePad has been detected" notice (translucent panel, gold frame colour (103,80,27), white text with black
  shadow); the game waits there for a button. 600 frames headless and 400 windowed: 0 validation errors, ~44 fps
  (Debug).
- **Input (VPAD from host input)** (`runtime/input/`, docs/input.md). The game reads `VPADRead(0, buf, 16, &err)`
  once per frame from `sub_02BCBC44` and copies sample 0 (0xAC bytes) to +0x2D0 of its pad object; on error -2 it
  re-checks, on -5 it calls `VPADInit`. Scripted A presses (`MH3U_INPUT_SCRIPT`) get past the "Wii U GamePad has
  been detected" notice to "Set extended data save settings for Monster Hunter 3 Ultimate?" (Yes/No; the game's
  dialog buttons are indexed triangle strips with 0xFFFF primitive restart). After answering, the boot sits on a black screen
  (only the two full-screen passes drawn) through frame 3000 – next blocker, likely save data. A DualSense on the
  dev machine is picked up by SDL3's HIDAPI driver.
- **Intro movie → title screen.** After the save-settings prompt the boot plays `nativeCafe/movie/09_EU.mp4`
  (1920x1080 H.264 High@4.1, IPB with non-reference B frames, no audio track - music is `sound/bgm/movie/movie_01`).
  `uMovie::Player` (vtable 0x1009CD48; Parsing / VideoDecode / AudioDecode worker threads polling every 1 ms) set its
  stop flag +0x30a because setup `FUN_02a4370c` failed: work memory (`H264DECMemoryRequirement`) + 28 frame buffers
  of `align(w,256)*h*1.5` (+0xdd0) are allocated from one heap, and the HLE's 0xB400000 work size exhausted it after
  12 buffers (13 `H264DECCheckMemSegmentation` calls, then free-all). `FUN_02a4415c` creates 3 Y (R8) + 3 UV
  (R8G8, half size) movie textures; +0xd11 = video ready. The decode step `FUN_02a44ed4` converts the MP4's
  length-prefixed NALs to Annex B itself and treats `H264DECExecute` results with bits above 0xFF as errors; with
  OUTPUT_PER_FRAME it needs each unit's picture before feeding the next. With a Media Foundation decoder made to
  output in decode order (see docs/runtime.md § Movies) the movie plays and the title screen renders ("PRESS A",
  Ver. 1.02, frame ~1000 with scripted presses). 0 Vulkan validation errors through 1200 frames.
- **No setjmp/longjmp or C++ exceptions** (Phase 2 item). ELF sections: .syscall .text .rodata .data .module_id
  .bss .ipc_smem + relocations/imports/symbols - no .eh_frame or GHS exception tables. No setjmp/longjmp/__throw/__cxa
  strings. In the recompiled code no instruction loads r1 from anything but the stack (9,077 `or r1,r11,r11` + 2
  `ori r1,r11,0` epilogues), so nothing longjmps. Imports have no fiber/stack-switch calls. `std::exception`,
  `std::bad_alloc`, `std::logic_error` appear only as RTTI/vtables (0x0298B548..) from the standard library.
- **clang-cl build** (Phase 2 done). With the VS 2026 "C++ Clang tools" component, `cmake -B build/clang -T ClangCL`
  builds everything including the 64 generated `recomp/out` files with no warnings from generated code. Hand-written
  fixes: `&&`/`||` parentheses in espresso.cpp, an unused emitter helper, `getenv` → `_dupenv_s`;
  `-Wmissing-field-initializers` is off for clang (Vulkan `Vk...Info x{sType}` idiom). 9/9 tests pass; the clang
  Debug build runs ~25% faster than MSVC Debug (1200 frames: 23.4 s vs 30.9 s) and reached the **main menu** (New
  Game / Continue / Options / Data Transfer / DLC) with scripted A presses at frames 700 and 1000.
- **Audio output.** The game drives AX with `AXSetVoiceDeviceMix` (44k calls per 1300 frames) routing every
  voice to exactly one TV channel (c0..c5, bus 0) and GamePad voices to DRC 0 L/R; `AXGetDeviceMode` returning 0
  does not stop it using all 6 TV channels. HLE pitfall: writing the return value (r3) before reading the voice
  argument (also r3) silently dropped every device mix. Mixer per Cemu `ax_mix.cpp` → SDL3; recorded boot audio has
  music during the intro and on the title screen.
- **Mip offsets (addrlib).** The renderer's first mipmapped textures (character creation, e.g. a BC 128x128 x7 at
  image 1D14A000) showed `mipLevelOffset` steps of 0x800 from our approximate `GX2CalcSurfaceSizeAndAlignment`,
  while the `.tex` data is back to back (1D levels aligned to 256 B): every level from 2 on was read at the wrong
  offset. Replaced sizing and detiling with Cemu's LatteAddrLib (vendored, MPL-2.0, `runtime/gfx/addrlib/`): the
  player model's clothing went from noise to correct. Scripted A presses now reach **Character Creation**.
- **Stencil / RECTS / GamePad screen.** `GX2SetDepthStencilControl` has 13 arguments: 9-13 are read from the
  caller's parameter area (r1+8); display-list recording now saves 8 stack words per command and replays them in a
  parameter area below the replaying thread's stack pointer. The GamePad scan buffer (854x480 RGBA8, e.g. "Look at
  the Main screen" during character creation) is now shown as an inset.
- **Pro Controller (padscore).** `sPad` keeps 5 pads; `sPad::WiiPad` (vtable 0x100AFFCC) per WPAD channel at
  `sPad::mpInstance (0x102CEA34) + 0x70 + chan*0x4A0`. Connect callback 0x02BC9DCC (flags +0x498 = 1, registers
  extension callback 0x02BC9CD4, `WPADGetInfoAsync` with 0x02BC9CEC); extension type 31 → flags 0x11; vf7 0x02BCB068
  reads `KPADReadEx(chan, buf, 16)` and copies sample 0 to +0x2D0; vf8 0x02BCB108 maps status +0x60 Pro bits (A 0x10
  → internal 0x2000, same as Wii Remote A). With a connected Pro Controller the boot skips the GamePad notice, but
  Pro presses do not skip the intro movie (GamePad presses do): the active-pad selection is still to find.
- **Render comparison vs Cemu.** Cemu 2.6 (portable, keyboard GamePad profile, F12 = native 1920x1080 TV
  screenshot) and the port show the same boot flow without a save (GamePad notice → "In-game language has been
  changed" → save-data prompt → extended-data prompt → movie → title → menu → character creation). Pixel comparison:
  GamePad notice bit-exact; Yes/No prompts PSNR 59-62 dB; title/menu 41-43 dB with differences only in animated
  elements; character creation 30 dB from the model's idle pose (UI identical, textures and shading equal at 3x).
- **Comparison follow-ups.** The remaining differences against Cemu are animation phase: the selected Yes button
  pulses (port frames 1220-1292: mean brightness 46.7-53.2, period ~36 frames; Cemu's capture 51.6 lies inside);
  title logo shimmer, menu highlight and the model's idle pose likewise. The GamePad image (854x480 target
  0x15919800) matches Cemu's layout (4:3 panel between dark side panels; the Shakalaka shown varies). The
  "dark strip" on the inset was the right side panel.
- **Controller option.** With no GamePad (`MH3U_CONTROLLER=pro-only`) the game says "The set controller could not
  be detected… change your controller settings from the Options menu": the active controller is a game option in
  the save, so with a GamePad-made save Pro Controller input is ignored until that option changes – as on hardware.
  Pro A then dismisses the notice, skips the movie and reaches the main menu.
- **No CPU readback of render targets needed so far.** `MH3U_WATCH_TARGETS=1` marks the whole pages inside every
  cached render target no-access each frame: 0 accesses from boot to character creation (positive control: an
  `MH3U_PEEK` of the TV buffer at exit is caught). Partial first pages must be excluded – they hold the GX2 surface
  structs the HLE reads every frame.
- **Phase 3 closed (2026-10-24).** Audio confirmed by ear in a windowed run (intro, title music, effects). All Phase 3
  items ticked; open notes carried into Phase 4: gameplay button mapping, re-run MH3U_WATCH_TARGETS in gameplay,
  AUX reverb (AXFX) if wanted, GamePad screen and gameplay scenes not yet compared with Cemu.
- **Pro Controller as the default.** The controller option lives in the `system` save file: a save made with no
  GamePad starts "set to the Wii U Pro Controller/Classic Controller", and a GamePad-set save is switched to Pro the
  first time the game finds no GamePad (afterwards GamePad-only mode reports "The set controller could not be
  detected", and Options → Controller Settings shows Pro selected). The port now defaults to `MH3U_CONTROLLER=pro-only`
  and hides the GamePad screen; audio confirmed by ear in a windowed run of the port.
- **swkbd → PC keyboard.** The character-creation name notice stayed up forever because the game waits for the
  software keyboard to be usable: `SwkbdInitLearnDic` must succeed (Cemu fills an empty 0xA460-byte "NJDC"
  dictionary) and `SwkbdIsNeedCalcSubThreadFont/Predict` report some work after `SwkbdCreate`. With those, the game
  calls `SwkbdAppearInputForm` (max 10 characters for the hunter name), polls `SwkbdIsDecideOkButton`, reads
  `SwkbdGetInputFormString`, and the Name field shows the typed text ("R y o").
- **Saves and first in-game scene (2026-10-24).** Scripted keyboard/Pro input through New Game: name via the PC
  keyboard (swkbd HLE), Character Creation OK, Touch Panel Settings (Standard), Character Select slot 1 → loading →
  Moga Village with the Village Chief's opening dialogue (3D scene, NPCs, player in starting gear, dialogue boxes)
  rendering. The save HLE wrote `user1` (35,364 B) and updated `system`. A fresh boot → Continue shows the
  character in Character Select ("Ryo", Male, 0:00, 0z, HR 0) and loads it into the village ("Mornin', hunter!
  Ya sleep well?"). In Character Creation the first d-pad press after closing the name keyboard is swallowed (6
  presses reach OK from Name).
- **Cemu addrlib replaced by our own code (2026-10-24).** The vendored Cemu files (`LatteAddrLib*`, `LatteReg.h`,
  ~2,200 lines, MPL-2.0) and the Cemu-adapted `gx2_surface.cpp` are gone; `runtime/gfx/addrlib/latte_surface.*`
  (sizing, mip offsets, element addresses for all 16 tile modes) and a rewritten `gx2_surface.cpp` replace them.
  Equivalence: a seeded sweep (`addrlib_sweep.h`: 400k per-level sizings, 100k `GX2CalcSurfaceSizeAndAlignment`
  runs, every level/slice of 6k surfaces with per-element offset hashes – 597,700 cases, 5.93M values) gave
  identical results from the old and new code (hash `0x0F359C48B73370F2`, now pinned in the `gfx_addrlib` test).
  Notes from the rewrite: with 2 pipes the 3D tile modes rotate by 1 per slice, so they never fall back to 2D;
  GX2's unpadded-linear (mode 16) size is computed in 32 bits; a 0x0 surface gets 0 levels and keeps its old
  size fields; the old code divides by zero for an invalid format with a mip chain (ours pads nothing).
- **Live name entry � where the game takes the swkbd string (2026-10-24).** Each swkbd import goes through a tiny
  wrapper at 0x0307E250..0x0307E290 (one per `p_Swkbd*` slot). `sSoftwareKeyboard::vf6` (0x0293C0B0, per frame) polls
  `SwkbdIsDecideOkButton` and on OK/cancel calls the listener at this+0x28D0, vtable +0x34, with (0 = OK / 1 = cancel,
  `getInputFormString?` 0x0293C098); its return lands in this+0x28A8 (reset to -1 every frame). The string is only
  copied into the game's own text by 0x0211D774 (via thunk 0x0211DB48, 14 UI callers) once this+0x28A8 == 0, i.e.
  after OK � so the game's Name field cannot show letters while typing on its own: the real Wii U draws them in the
  swkbd applet's own input form. Options for live text: draw our own input box (as the applet did), or find the
  character-creation caller of 0x0211DB48 and refresh its field each change. 0x0211D774 also has a USB-keyboard
  path (`sKeyboard`, this+0x6F5; string from 0x0211B3FC(&0x103D3018)) used for chat.
- **Performance: village 12-16 FPS -> 30 FPS (2026-10-24).** New `MH3U_PROFILE=1` (runtime/boot/profiler.cpp): samples
  every thread's stack at ~1 kHz, prints per-thread top functions and blocking sites at exit, plus the FPS every 5 s;
  guest threads carry Windows thread descriptions. Renderer stats now include GPU ms/frame (timestamp queries) and MB
  streamed per frame. Headless benchmark: Continue route `1000:A;1200:A;1450:A;1650:A;1850:A;2100:A;2350:A;2600:A;2850:A`
  on a save with a character -> Moga Village by frame ~2400. Findings, in order of cost:
  1. Two pacers stacked: GX2SwapScanBuffers slept to its 60 Hz grid, then present() blocked on FIFO vsync and on the
     previous present's fence. Now: MAILBOX/IMMEDIATE (`MH3U_VSYNC=1` forces FIFO), 2 presents in flight, and the
     pacing sleep runs after submit+present. Frames in 90 s: 2327 -> 4592 (title/menus).
  2. Shadow map: the game renders depth into a 1024x1024 R32F colour target and samples it with depth compare in ~100
     draws a frame; `depth_alias` copied it (4 MB, image -> buffer -> D32F) on every one of those draws: 424 MB/frame
     streamed, GPU 75-95 ms/frame. Now copied only when the target was written since (Image::writes, bumped by
     transition() into a writable layout). GPU 4-5 ms/frame, 16 MB/frame.
  3. 0x15CF0000 is both the 1024x1024 D24S8 shadow depth and a 480x270 RGBA8 target every frame: target() destroyed
     and reallocated both twice a frame. Other views of an address are now parked in spare_targets_ and reused.
  4. Every cached texture (~150) was content-hashed every frame (23% of the game thread). Guest memory is now reserved
     with MEM_WRITE_WATCH; textures are hashed only when their pages were written (GuestMemory::next_epoch /
     written_since; only the cached textures' pages are scanned).
  Remaining on the game thread at 30 FPS: vertex attribute decode + fill (~31%), per-draw string formatting
  (std::num_put / locale, ~7%), vkGetInstanceProcAddr inside the driver per draw (~4%).
- **Uncapped frame rate: village ~155 FPS (2026-10-24).** The game is variable-timestep: `sMain::framePacing?`
  (0x0294B8DC) writes `sMain+0x7C` = real elapsed time in 1/60 s units (+0x4C = 60.0, +0x50 = 3000.0 -> its own
  minimum interval 1/3000 s, +0x76 fixed-step flag 0, +0x80/+0x84 clamps 100/16, timer 62,156,250 Hz at +0xF38).
  The 30 FPS cap is only `GX2SetSwapInterval(2)`. New `MH3U_FPS` (default: display refresh rate in a window, game
  pacing headless; `max` = unlimited); at ~147 FPS the delta reads 0.409 (= 60/147), so game speed is unchanged.
  Village (headless, unlimited, RTX 3060) after each step: 30 (CPU-bound, 33 ms: ~70% our display-list replay /
  draw translation, ~18% the game's update) -> vertex conversion cache 70 -> write-watch scan once per frame,
  direct-mapped indirect-call table, scratch buffers 75 -> recycled display-list vectors (each frame's lists were
  VirtualAlloc'd and freed) 95 -> static vertices resident in a device-local arena (15.7 -> 6.8 MB streamed per frame,
  GPU 4.6 -> 2.0 ms) 113 -> write-watch scan on a background thread 120 -> rebased-index cache 147 -> GX2 state lock
  once per display list, word-wise cache keys ~155 (147-165). Pipeline keys are raw bytes instead of an
  ostringstream. The profiler had a deadlock (walking a suspended thread's stack can need the loader lock it holds):
  it now copies the top 64 KB of the stack, resumes the thread, and unwinds the copy.
  Remaining on the game thread: Vulkan driver work per draw (~15%: descriptor set allocate/update per stage), source
  hashing for the caches (~7%), and the game's own update.
- **Asset-free distribution, Yakumo-style (2026-10-26).** mh3u.exe no longer needs `extracted/`: it reads the
  user's dump in place and checks it. `runtime/fs/zarchive.*` reads `.wua` directly (own reader of Cemu's ZArchive
  format, zstd 1.5.7 decoder via FetchContent; [formats/wua.md](formats/wua.md)): 1,545 files / 815 MB of base +
  update compared byte for byte with zarchive.exe's extraction. `recomp/src/inflate.*` (own DEFLATE/zlib) decompresses
  the RPX's SHF_RPL_ZLIB (0x08000000) sections in memory: all 39 sections equal wiiurpxtool's ELF. Dumps are
  identified by **`code/app.xml`**, not `meta/meta.xml`: the v32 update's meta.xml carries the *base* title id
  (0005000010118300, title_version 32 decimal) while app.xml has 0005000E10118300 / `0020` (hex)
  ([formats/dumps.md](formats/dumps.md)). The RPX (update over base) must hash to d97aabfc… (target.txt).
  Data folder: `portable.txt` → `data\` next to the exe, else `%APPDATA%\MH3U-PC`, the checkout when run from it.
- **Shaders translated on first start (2026-10-26).** shaderc (Vulkan SDK `shaderc_combined`, static) compiles the
  668 translated shaders in-process into `<data>/cache/shaders`: SPIR-V byte-identical to the earlier glslc output
  (668/668). spirv-opt (`-O`) is the cost: 360.7 s with it vs **3.9 s** without (Release, 12 threads); the village
  benchmark shows no GPU cost from unoptimised SPIR-V (2.05 ms vs 2.50 ms GPU/frame, 140-156 vs 120-146 FPS,
  run-to-run noise), so first start uses unoptimised SPIR-V. Debug shaderc is ~100x slower (use a Release
  shaderc-latte to fill a Debug run's cache). Cache stamp: translator version + RPX SHA-1.
- **Pipelines persist across runs (2026-10-26).** Pipeline keys are now a portable `PipelineDesc` (shader content
  keys instead of VkShaderModule handles); every new pipeline is appended to `<data>/cache/pipelines.bin` and the
  driver's `VkPipelineCache` is saved to `pipeline_cache.bin` (every 1800 frames and at exit). At start-up all listed
  pipelines are built on N-1 threads before the guest runs (progress bar in the window): village route → 93
  pipelines, rebuilt in 0.1 s on the next start, and the draw path finds them all (no duplicates).
- **Boot notices skipped by rewritten game code (2026-10-27).** New override mechanism: functions listed in
  `config/overrides.txt` are emitted as `<name>__orig` plus a dispatcher calling a replacement registered with
  `ppc::set_override` (src/). Findings: `uIDStartMenu::vf63` (0x0276969C) is only the 4-byte tail of the real
  per-frame state step 0x027695A0 (switch on uIDStartMenu +0x2A0); states 0/1 (FUN_027605e0 / FUN_027607c4) run the
  boot save setup. Save messages: FUN_020E24B8(sAppSaveData, msg, kind) closes them at once while
  sAppSaveData+0x27F931 is set (aBoot uses it while loading); Yes/No answers come from FUN_020E28A0 (1 Yes, 2 No);
  notices wait on sAppPad (0x10312BB0) trigger +0x11C & 0x60 (save system, aGame) or +0x6C & 0x20 (start menu).
  The two notices before the start menu ("will begin set to the Wii U Pro Controller", "In-game language has been
  changed") belong to aGame::vf5: handlers 0x02019C0C (Pro: sets game option 0x1D = 1), 0x02019AD4/0x02019B70
  (GamePad: option 0), 0x02019CA8, 0x02019D30 ("set controller could not be detected"), 0x02019E38 (language),
  0x0201A020; each returns 1 while waiting, 0 when decided. Found with the new `MH3U_WATCH_READ="@10312BB0+11C"`
  (guard-page read watch, runtime/boot/read_watch.*): the only non-pad reader while the notice was up was
  sub_02019C0C <- aGame::vf5. A fresh start with no input now goes Capcom logo -> intro movie, save written.
- **Screen filters (2026-10-27).** uFilter : cUnit, draw in vtable slot 11 (uFilter::vf11 empty): uBloomFilter
  0x02A7830C (also uAppBloomFilter), uDOFFilter 0x02A8504C, uBokehFilter 0x02A79F90; also uColorCorrectFilter,
  uHazeFilter, uMotionBlurFilter, uRadialBlurFilter. sLtRender+0x6B98 is a feature-flag word (FUN_0275F494).
  Skipping the bloom draw removes the glow in Moga Village, nothing else changes.
- **Resolution scaling and the Tri look (2026-10-27).** Render targets are created at scale x guest size (compared
  by guest size), viewports/scissors/point size scaled, CPU-initialised targets uploaded at guest size and blitted;
  the translated shaders never use texel coordinates, so nothing else changes. Village at 0.5x / 2x / 4x renders
  correctly (TV 960x540 / 3840x2160 / 7680x4320; GPU 0.8 / 6.6 / 26 ms). Tri look: full-screen pass on the TV image
  (25-tap glow of the bright parts + desaturate 0.8, contrast 0.86, lifted cool shadows), from Wii screenshots.
- **Depth of field and ambient occlusion (2026-10-27).** The game's uDOFFilter (0x02A8504C) and uBokehFilter
  (0x02A79F90) draws are never called from the intro movie through Moga Village (call counters on the overrides,
  4,500 frames), so gameplay has no DOF of its own. Our AO and DOF run in the renderer on the scene target once a
  frame, right before the first draw that samples it. In the village frame that is the bloom downsample (ps
  e86db455ede9f720), after all 3D and before the HUD. The scene is the TV-sized colour target drawn with depth
  writes: F4000800 RGBA8, depth F47F8800 D24S8. View space comes from depth alone (z' = 1/(1-d), eye depth in
  near-plane units) with an assumed ~48 degree vertical FOV. In a village shot the hunter is at z' ~1500 and the far
  fence at ~8500, so the near plane is very small: AO radius 200 near-plane units, strength 1.4. Tunables
  MH3U_AO_RADIUS/STRENGTH and MH3U_DOF_RADIUS/START/RANGE/NEAR; MH3U_FX_DEBUG=1 shows log depth plus the focus
  marker in place of the DOF pass. GPU at 1080p: base 1.9 ms, +AO 3.5 ms, +AO+DOF 3.6 ms.
- **Frame dumps write relative to the working directory** (`build/frame_N.bmp`). Run from the repo root: from
  inside build/ they used to fail silently and leave an old frame_N.bmp in place (now an error is printed).
- **Launcher over the running game (2026-10-27).** One window now: the presenter records an overlay callback into
  every swapchain image (dynamic rendering, load op LOAD) after the game frame is blitted. The launcher builds ImGui
  frames on the host main thread and hands over copies of the draw lists (ImDrawList::CloneOutput); the copies are
  drawn on whichever thread presents, with the queue lock held. The game boots once its shaders are current and
  runs behind the menu; input::set_suspended keeps its input empty until PLAY. The pipeline prebuild moved from the
  main thread (full-window bar) to the guest thread before __rpx_start, so the launcher can show it as a toast.
  Settings now reach the env before the window opens (MH3U_VSYNC is read when the swapchain is built) and again at
  boot, followed by a swapchain rebuild. The windowed run reaches start-menu states 0-6 behind the overlay; 40 s with
  VK_LAYER_KHRONOS_validation showed no errors.
- **Settings mid-game and rebinding (2026-10-27).** F1 / controller guide (or MISC1) toggles the launcher's settings
  over the running game (F2 now cycles the GamePad view). Controller buttons are no longer a fixed table in
  host_input: Config.pad maps SDL gamepad buttons and the two triggers (as ids 200/201) to GamePad buttons, filled
  from the layout's defaults or input.ini [controller]. The CONTROLS page binds from the next SDL event (keys,
  mouse buttons, wheel, pad buttons, triggers past half way) and saves input.ini with "clear = 1" sections;
  bindings_test covers bind / save / reset / reload.
- **Cutscene frame cap (2026-10-27).** The game calls GX2WaitForVsync from three places, all in the renderer:
  sLtRender::present (0x02BBF9D0; after the swap, while renderer flags 2 and 4 are set and a frame is still queued:
  +0x6E3C != +0x6E44), FUN_02BBFD3C (a frame limiter) and sApp::presentStandalone (0x02BC1E34). FUN_02BBFD3C runs
  while renderer flag 1 is set (FUN_02BBA714(this, 1)): it waits for the flip queue (GX2GetSwapStatus +
  GX2WaitForVsync) and then OSSleepTicks until a minimum frame time; its only caller is FUN_02BBFEEC (present, then
  the limiter), called from sLtRender::endFrame. Headless boot-to-village: no GX2WaitForVsync calls, 595
  GX2GetSwapStatus calls (the limiter's loop) in 3410 frames, GX2SetSwapInterval once. With our own pacing (windowed)
  FUN_02BBFD3C is overridden to do nothing (src/game/frame_limit.cpp) and GX2WaitForVsync returns at once, so
  cutscenes follow the FRAME RATE LIMIT setting; confirming in a window is open.
- **Launcher navigation (2026-10-27).** Dear ImGui 1.91 leaves InvisibleButton out of keyboard / gamepad navigation
  unless it has ImGuiButtonFlags_EnableNav: every launcher row, menu item and keyboard key needs the flag. The
  controller feeds ImGui from the game's own bindings (SDL3 backend in manual mode, buttons mapped through
  input::pad_buttons_for), so A / B follow the layout and rebinds. The overlay is handed to the presenter as a copy
  of the frame's draw lists: a frame that closes the overlay must hand over nothing, else its last picture stays.
- **Character colour pickers and tiling apertures (2026-10-27).** Skin tone (and hair colour) took a wrong colour:
  uIDMakeAppearance (0x0274A9F0, from uIDMakeCharacter::vf62) reads its gradient texture (512x256 RGBA8, tile mode
  4) back on the CPU through GX2AllocateTilingApertureEx (via 0x02957070: byte-swaps the view, copies it, swaps back
  in 0x029571FC, then GX2FreeTilingAperture). Our aperture returned the tiled image itself, so the picked texel was
  scrambled. It now detiles level 0 into a guest buffer (rows of `pitch` elements) and retiles it at Free. Found
  with MH3U_WATCH_READ on a word of the texture (RelWithDebInfo names); headless fresh-save run to the Skin Tone
  page: the skin stays as chosen and follows the cursor. MH3U_INPUT_SCRIPT also takes LSTICK_LEFT/RIGHT/UP/DOWN now.
- **Texture packs (2026-10-27).** Packs made for the 3DS release are keyed on hashes of 3DS texture data, so they
  never match Wii U textures; 3DS packs are not supported and won't be converted (2026-10-29). Our own scheme keys a
  replacement on the CRC32 of the texture's level 0 as it sits in guest memory (tiled) plus its size: no game hook,
  and MH3U uploads .tex data as stored, so the key is stable across runs. Headless village run: 67 textures dumped
  (the rest are video planes, render targets or CPU-animated); 59 of those swapped for 64x64 magenta DDS files gave
  69 replacements and a fully magenta village at 62.6 s (no slowdown).
- **DLC quests in the save (2026-10-27).** No eShop add-ons (no nn_aoc import): event and challenge quests were free
  in-game downloads over nlibcurl from `http://goshawk.capcom.co.jp/3ds/mh3g/` (index files DLC_Info_List_eng.txt,
  DLC_EventQuestInfo_eng.txt, DLC_ChallengeQuestInfo_eng.txt; quests `q_%05d.quest`; Wii U login
  `https://goshawk.capcom.co.jp/SSL/wiiu/mh3gu_us/login.cgi`, strings at ELF offsets 0x1ACC / 0x28928). The host no
  longer resolves. Downloaded quests live in the save files quest1..quest4 (names at 0x1BB44, sAppSaveData): file
  0x29024 bytes, 20 slots of 0x2010 from offset 0x28, a slot = QTDS data (version 5, little-endian, u32-prefixed
  UTF-8 titles per language) padded to 0x2000 + the 16-byte file name; quest1-3 event (q_201xx-q_203xx), quest4
  challenge (q_61000-q_61009). The 4 bytes at 0x14 of the file header are the same in an empty and a full file. An EU
  save backup held 38 quests. Headless boot to Moga Village does not open quest1-4 (read on demand, presumably by the
  Port Tanzia quest lists): whether the US build lists EU quests is still to be checked in game.
- **Monster HP (2026-10-27).** rEnemyStatus (`.ems`, magic "EMS\0" version 0x20110824, 91 files `enemy\emNNN\em_statusNN`)
  starts with the monster's base HP (BE u32 at file +0x10, e.g. em001 4500, em025 18500; small monsters 10-220) and a
  float (+0x14). uEnemy::loadResources? (0x0259FD68) keeps the resource at uEnemy+0x1DE0; uEnemy::getStatusData?
  (0x0259FA9C) returns its +0x64 copy of those words. At spawn uEnemy::initStatus? (0x0259FCD0) -> 0x0259FC98 ->
  uEnemy::setMaxHp? (0x0259FB84): max HP (state = uEnemy+0x1BE0, field +0x8BC) = base HP x row[0], row =
  uEnemy::getStatRow?(0x0259E3B4)(this, 0), a 0x20-byte float row of a table sEnemy keeps per quest monster set
  (FUN_020F3B9C), indexed by the byte state+0x8E8 (+1/+2 for other stats). Then, if the current quest's flags (quest
  data +0x1E4) have 0x100000 or 0x10 and sGame+0x1B9 != 1, max HP x 0.55 (float at 0x1004D7C8) - meaning of the flags
  and of sGame+0x1B9 not confirmed. Current quest data = *(*(sQuest+0xB050)+0x3C); byte +0x24F = quest rank, stored
  by sEnemy::setQuestRank? (0x020F527C) at sEnemy+0x4284 (0..3). No player count found here yet. setMaxHp is now
  overridden (src/game/monster_hp.cpp) to log every monster's inputs ("monster hp: ..."); boot-to-village headless run
  unchanged (60.5 s), no monster spawns there.
- **Item box menu and carving, first pass (2026-10-27).** Game text is rGUIMessage (`.gmd`, 3645 files; magic
  stored as "\0DMG", version 0x00010201; BE header: label count +0x14, string count +0x18, label bytes +0x1C, string
  bytes +0x20, then the name; the strings are the last block, NUL-separated, in index order). The box menu's options
  are `GUI\font\lobby\Shop_eng` strings 389-394: Store Items, Remove Items, Combine/Manage Items, Manage Equipment,
  Change Appearance, Equipment Sets (396/397 Item Box, 398/399 Blademaster's / Gunner's Pouch, 400 Combine Box Items,
  402 Reorder, 403 Sell). Text is looked up as FUN_0205AD00(group, index); the Port Tanzia house root
  (uIDLobbyMyhRoot::vf62 0x02706938) asks group 0x2E index 0x1A9 = Shop "View Talismans", so 0x2E is probably the
  Shop table. No code loads 389-394 as immediates and no data table holds them in a row, so the option labels most
  likely come from the menu's rGUI layout (GUI\lobby\...): adding "Item Sets" means extending that layout and
  hooking the menu's selection handler. uIDCockpitBox (0x025F480C...) is the in-field box prompt, not this menu;
  house UIs: uIDLobbyMyhRoot / uIDLobbyMyhBox / uIDLobbyMyhMyset (equipment My Set: register / load lists, a model
  for item sets). Carving: player class uPlayer (0x28F0, base cPlayerBase); no carve text to anchor on - the carve
  action state and the carcass's remaining-carves count are still to be found (hold-to-carve must key on them: the
  same button attacks, so a plain auto-repeat would chain attacks).
- **Game text lookups (2026-10-27).** Every text lookup ends in sID::vf37 (0x02141DE0) = sID::getText?(this, group
  < 0xAF, index): the rGUIMessage kept for the group at sID+0x78, its vf+0x54 returns the string. FUN_0205AD00(group,
  index) (302 callers) is only a wrapper that tail-calls it through sID's vtable (+0x134); most UI code calls the
  vtable itself (a boot-to-village headless trace saw 2 lookups through the wrapper, 129 through vf37). Group 0x2E =
  GUI\font\lobby\Shop (index 323 "Target" confirmed), 0x35 = lobby\NpcName (2 "Village Chief"). MH3U_TRACE_TEXT=
  <first>-<last>[:<group>] (src/game/text_trace.cpp) logs each (group, index, caller) once: the item box menu's
  option labels are Shop 389-394, so MH3U_TRACE_TEXT=389-394:46 while opening the box names the code that builds the
  menu. Not found statically: no immediate 389-394 near a text call, no data table of them (u16/u32 runs or fixed
  strides up to 64 bytes; the stride-16 hit at 0x101C5FF0 is an item data table), and rGUI layouts hold 389-394 only
  as unrelated numbers.
- **Item box menu (2026-10-27).** Found with MH3U_TRACE_TEXT in a window (all box labels from one call site,
  0x026A6C64). Draw: uIDLobbyMaki::drawBoxMenuOptions? 0x026A6B80 (labels Shop 0x185 + i for i < count, 7 row
  slots, extra rows hidden) and drawBoxMenu? 0x02687D38 (sub-lists: label base u16 table 0x10170F40 [396 397 400 424
  418 429 433], counts 0x10170F30 [2 2 4 5 6 4 4], Help bases 0x10170F50 [148 152 156 169 161 175 180]). Logic in
  sLobby, state at +0x6F10: +0 state byte (2 = option list), +2 open, +6 option, +8 sub cursor, +0xA level, +0xC
  option count (6), +0xE sub count. sLobby::boxMenuStep? 0x021F0E7C returns 1 on a chosen sub entry, 2 on leaving;
  sLobby::boxMenuUpdate? 0x02201508 dispatches options 0-5 (Store: states 3/4, Remove 5/6, Combine 7-10, Equipment
  0xB-0xF, Appearance 0x18-0x1D, Equipment Sets 0x10-0x13); other options only play a sound. Slot 6 of every table is
  unused and Shop 386/387/395 and Help 180-182 are "DUMMY". Pouch / box (sUserData+0x38 = hunter; the save file
  user1 holds the same struct from offset 0x28): u16 item + u16 count per slot; blademaster pouch hunter+0xA8 (24),
  gunner pouch hunter+0x108 (32), box sUserData+0x1C0 (= hunter+0x188, pages x 100 slots); item names = Item00 text
  index = item id (1 "(None)"). src/game/item_sets.cpp adds option 6 "Item Sets" (Register / Load Item Set) from
  these; text replaced through sID::getText? (src/game/text_trace.cpp set_text). Headless village run unchanged; the
  menu is still to be seen in a window.
- **Item Sets in a window (2026-10-27).** Register / Load and the sub-list work; with 7 options the menu drew option
  6 in row slot 7, which the layout leaves bare. Row slot 3 (under Combine/Manage Items, above the separator) is a
  complete row that drawBoxMenuOptions? hides on every draw: swapping slots 3 and 7 of the highlight (+0x2AC) and text
  (+0x2B8) arrays for the call and showing slot 3's plate (+0x2B0[3]) draws Item Sets there with its plate (seen in
  a window). The separator line still crosses that row. Pouch probe (MH3U_ITEMSET_PROBE, headless, user's save):
  hunter data in memory = the save file's from 0x28 (name +3; blademaster pouch at hunter+0xA8 holds the save's
  items), and the pouch pattern also sits at three other places (sUserData-0x1AD04, +0x66F54, +0x19516F8) - copies;
  yet Register in the box menu read 24 empty slots there in a window. Register now logs what it reads.
- **Item box menu layout: GUI objects, scroll length (2026-10-27).** The box menu widget (uIDLobbyMaki) is a tree of
  cGUIInstance (+0xF4 root; id +4, child +0x60, sibling +0x64, flags +0x54, pos +0xA0/+0xA4). The scroll (428), the
  rows (plates / highlights, cGUIInstAnimControl) and the separator (593) are animated instances: their frame (+0xE0)
  follows a variable (+0x104, cGUIVarInt value +0xC; the scroll's is 60, a preset that picks style and length - 50 / 70
  / 80 give khaki / dark / green scrolls), and what is drawn is an object tree at +0xFC (cGUIObject: child +0x68,
  sibling +0x6C; cGUIObj2D PosX +0x80, PosY +0x84, dirty 0x20000). Setting an instance's +0xA4 does not stay for
  animated ones; object PosY does, if written right before cGUIInstAnimation::updateTransforms? (vf13 0x02AE1E40),
  which redoes the tree's transforms (FUN_02AF4C78) when the root's +0x8C bit 1 is set. Scroll tree (root children,
  from per-object captures): background group, tan panel (y 270; moving it extends the tan part), its seam line, top
  roller group, bottom roller group (y 540; moving it lengthens the scroll), a texture set. The option rows: 8 slots
  54 apart (+0x2B0 plates, +0x2AC highlights, +0x2B8 texts - texts are GUI objects, hidden with FUN_02AF349C(text, 0);
  FUN_0228D1B4 / FUN_02ADB830 on them walks instance children and faulted at guest 00051020). The small box is the
  same menu with lobby+0x6F12 set (draw state +2): layout type 0x4A, 3 rows, no camera move (FUN_02201508). Item
  Sets: rows 0-2, Item Sets in slot 3, gap in slot 4, options 3-5 in slots 5-7; tan panel, seam, separator and bottom
  roller one row (54) down (small box: rows 4-7 hidden). The sub-list pop-up y is base + slot * row (0x1005C520..34).
  Crash reports now give Cha-Cha.exe+offset; build/clang/runtime/boot/Release/Cha-Cha.map maps it to a function.
- **Pouch window for the item set preview (2026-10-28).** Store Items' pouch is not the box screen widget
  (uIDLobbyMyhBox: box grid FUN_026FF5C4 "Item Box" (3, 200), its own pouch grid FUN_026FDE44 unused there) but the
  shared uIDCommonPauch (sID sub-unit 2 via FUN_0213F040, +0x94), found by tracing the title "Item List" (3, 0xCA) to
  FUN_0265F010 (frame, mode 1). Store Items draws it every frame in FUN_026697C0: *FUN_0214218C(sID) = 6 (pouch
  window selector; the box menu resets it to 0), +0x3F4 = 8, animation +0x2D4 (FUN_0203787C 0, FUN_02037600 3 0),
  flags +0xC |= 0x100000, vf+0xE4(1) open, FUN_0265E308 reset, FUN_0265F010(1), FUN_0265FAE4(8, 0), then
  FUN_026611B8(widget, slots, entries, page 1-based, pages, 0, 0x800, &out, 8 on the stack) lists 8 slots from any
  u16 id + u16 count array (entries 8 x 0x18: +1 highlighted, +2 shown, +4 slot index). Run from the box menu the
  widget also needs state bits 0x30000 at +0xC (Store Items: 44930040; the steps alone: 44900040, not drawn). Menu
  direction bits (FUN_0221A430, key repeat): 1 up, 2 down, 4 left, 8 right; page sound 6 (FUN_020431F8).
- **Gathering and carving (2026-10-28).** A full text trace (MH3U_TRACE_TEXT=0-5000) during a carve (Kelbi Horn)
  and a gather (Blue Mushroom) shows "%s obtained." = text group 2 index 8, from sCockpit::message? 0x020EF0A8
  (cockpit, kind 2, text 8, item; 52 callers). Probes on it and on 0x0285ECD4 (the in-quest add: (player, item,
  count), 0 = added) during carving, gathering spots and mining: every item came from 0x020ECA2C
  sCockpit::obtainItem? (11 callers: 0x028BA608, 0x028BC31C, 0x028BDB78, 0x028C1618, 0x0279E440, 0x02918338, and two in
  the lobby code, 0x02232BEC / 0x02234218 - the farm?). It adds the item (sGame+0x48: 1 -> 0x0285ECD4, in a quest?; 2 -> the
  sUserData pouch, the farm?) and reports < 2 as obtained. Hold to gather keys on that: no action state needed. Pouch
  adds in the lobby go through 0x0219BA48 (box menu, quest rewards via 0x021EE7F4), not in the field.
- **Tri look rework and global illumination (2026-10-28).** The first Tri look graded the whole TV image (HUD
  included) with a two-ring 12-tap glow (ghosted double edges), 20% less saturation, softer contrast and lifted grey
  shadows - washed out ("ugly", user). Now `kTriSceneFs` runs on the 3D scene target with the other scene passes
  (before the game's bloom and the HUD): depth haze (strength 0.30 from 3000 near-plane units, 1/25000 per unit, grey-blue;
  not on the sky), a 24-tap golden-spiral glow with a soft threshold (0.62-0.95 luma, radius 18 px at 1080p,
  strength 0.16), then a grade fitted to a Tri screenshot of Moga Village from the user (tone statistics of the final
  frame, same view): MH3U is more saturated (0.256 vs 0.162), yellower and darker with deeper blacks; the grade is
  blue x1.035, saturation x0.80, levels x0.975 + 0.016 (the game's bloom and tone pass after it amplify both - a
  first fit straight from the ratios overshot to mean 0.46). Result: mean 0.339 / sd 0.168 / 2nd percentile 0.116 /
  saturation 0.171 against the reference's 0.344 / 0.166 / 0.114 / 0.162. A first warm, saturated version went the
  wrong way. The TV post pass keeps only the vignette. Global illumination (`kGiFs` / `kGiApplyFs`, VIDEO > GLOBAL ILLUMINATION,
  MH3U_GI): one screen-space bounce at half resolution - 16 AO-style taps gather the colour of nearby surfaces facing
  the pixel (cosine x range falloff, radius 600 near-plane units), blurred depth-aware, added as c + c x gi x 0.6.
  Order in the scene: GI, AO, Tri, DOF. Tunables: MH3U_GI_RADIUS / STRENGTH, MH3U_TRI_GLOW / GLOW_RADIUS / HAZE /
  HAZE_START. Checked headless in Moga Village (Continue route, frame 5500): no ghosting, HUD untouched, GI subtle.

## 2026-10-28 – Linux port: write tracking without GetWriteWatch
- Windows reserves guest memory with `MEM_WRITE_WATCH`; Linux has no equivalent. Emulated in
  `runtime/core/guest_memory_posix.inc`: pages of the renderer's watched ranges are `mprotect`ed read-only; the first
  write faults, the SIGSEGV handler (`REG_ERR & 2` = write) marks the page (2), makes it writable and returns; the
  scanner (500 us passes) stamps marked pages with the new epoch, counts the mark down (2 -> 1 -> 0, so a write that
  completes just after the handler returned is stamped on the next pass too) and makes them read-only again.
  Evidence: headless title run in WSL, 945 written pages caught, textures update as on Windows.
- The kernel does not fault on its own writes: `read()`/`pread()` into a read-only guest page fails with EFAULT.
  File reads into guest memory go through a 1 MB bounce buffer (`runtime/fs/host_tree.cpp`).
- FFmpeg's H.264 decoder with `AV_CODEC_FLAG_LOW_DELAY`, one thread and the existing no-reorder SPS rewrite returns
  each picture from the `avcodec_send_packet` that fed it, like the Wii U decoder with OUTPUT_PER_FRAME;
  `apply_cropping = 0` keeps the 1088-line coded picture with the crop reported (as Media Foundation gave it).
  Not yet checked in a window (WSL).
- **Linux movies stall (2026-10-29, issue #2).** Reproduced headless in WSL with the released 0.1.0 AppImage and the
  .wua: the intro (09_EU.mp4) gets only 3 H264DECExecute calls in 180 s (AU 1 = SPS/PPS/IDR, AU 2 = P slice, nal 0x41),
  then the player waits forever - the window shows the first frame, as reported. Likely cause: libavcodec 4.4 in
  low-delay mode with the SPS rewritten to no reordering drops a picture whose POC is below the last one output
  (h264_select_output_frame, "no picture ooo"), so the non-reference B picture of AU 3 never reaches the output
  callback. Media Foundation outputs it; libavcodec cannot without reordering.
  Fix: a second libavcodec decoder with one picture of reordering (SPS rewritten to max_num_reorder_frames = 1)
  outputs each non-reference B at once; the low-delay one skips non-reference pictures (AVDISCARD_NONREF) and gives
  I / P at once; pictures matched by pts. Headless WSL run: 3,673 Executes with one picture each, the intro plays to
  the end (frame dumps 600-3000 show it moving), an A press at frame 700 reaches the title, the attract movie loops.
- **Older GPUs: Vulkan 1.2 (2026-10-29, issue #3).** The reporter's Radeon HD 7750 on AMD 23.11.1 (Windows) reports
  Vulkan 1.2.178: no VK_KHR_push_descriptor (we required it but never used it: no vkCmdPushDescriptorSetKHR, no push
  layouts), no dynamic rendering (core or KHR), synchronization2 and demote only as extensions, no depth_clip_control.
  Device setup now takes 1.3 core features or the extensions; `runtime/gfx/vk_compat.*` routes vkCmdBeginRendering /
  EndRendering / PipelineBarrier2 to the core or KHR entry points, or emulates them (render passes + framebuffers from
  the VkRenderingInfo, cached by attachment formats / ops / layouts and by views; pipelines and Dear ImGui get a
  compatible render pass; sync2 barriers converted to classic ones). Shaders now compile for Vulkan 1.2 (SPIR-V 1.5;
  translator version 2 re-translates caches once: 668 in 5.8 s in WSL). MH3U_VK_LEGACY=1 forces the emulated path.
  Checked headless with the validation layer (loader confirmed it loaded) on the Continue route to Moga Village, 200 s:
  normal path 2,186,760 draws and legacy path 2,213,554 draws, 0 skipped, 0 validation messages each, frames at 1300 /
  3600 / 5200 alike. Not checked: the launcher window (Dear ImGui in a render pass) and a real 1.2 driver.
- **Linux write-tracking race (2026-10-29, issue #3).** WSL headless New Game and Continue both died at the save load
  with "guest memory fault: write to guest address 1D15xxxx (uncommitted)" in sub_02953DF8. The scanner sets kTracked
  and queues a page for mprotect(PROT_READ); a write faulting meanwhile clears kTracked and unprotects it; the queued
  mprotect then lands and leaves the page read-only, untracked, and the next write was reported as uncommitted. The
  SIGSEGV handler now treats a write fault on any committed page (outside diagnostic watches) as a tracked write. Both
  routes then ran 300 s (character creation; Moga Village with 93 pipelines, as on Windows).
  House route (the reporter's other crash): a copy of the user's save, Continue, walk up to the red-curtain door
  + A, back out (stick holds scaled to WSL's ~12 fps: the game moves by wall time). In WSL: house loaded at frame
  3170, inside at 3250, village again at 3750, no fault through 420 s. The unfixed build never got that far (the save
  load itself faulted, 2 of 2 runs).
