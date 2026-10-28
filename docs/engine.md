# Engine internals (MT Framework Lite on GHS / Wii U)

Facts about the executable's runtime structures. Addresses are for US v32 (`config/target.txt`).

## GHS C++ ABI (matters for every struct and the recompiler)

- **vptr placement:** GHS (Cfront-style) puts the vtable pointer *after the members of the first class
  that introduces virtuals*. Classes whose root has no data (e.g. `MtObject`) have it at +0; `MtDTI` has
  it at +0x1C.
- **vtable layout:** 8-byte entries `{s32 this-delta, fnptr}`. Entry 0 is `{0, typeinfo*}`; virtual slots
  follow from +8.
- **RTTI is present:** typeinfo = `{vptr 0x1014E0B8, const char *name, …}` for every polymorphic class
  (3,953 found). This names vtables independently of MtDTI.
- **Vtable words are `{s16 value << 16, ptr}`:** header `{offset << 16, typeinfo*}` (0 for primary vtables), slots
  `{this_delta << 16, fn}`.
- **Secondary vtables** (multiple inheritance): header offset = base subobject offset; they sit right after the
  primary vtable. Their slots point at GHS **this-adjust thunks** `subi r3,r3,offset; b target` (and also carry
  delta = −offset). 40 tables / 406 slots / 382 thunks in `config/secondary_vtables.csv`
  (`ghidra/scripts/FindSecondaryVtables.java`); every (class, polymorphic base at offset > 0) pair from the
  typeinfo base lists — 23 — has one. Only 47 classes have more than one direct base, almost all in the
  network layer (`MtNet*`, `nNetwork::*`, `nn::nex`): e.g. `MtNetStorage2` with
  `MtNetRequestController::Listener` at +0x44. Game code is essentially single-inheritance.
- **Typeinfo base list** (+0x0C): `{baseTypeinfo*, u32 w}` per direct base, base offset = `w >> 16`,
  `w & 0x200` = last entry (`0x1600` / `0x1400` low bits otherwise; meaning of 0x400/0x1000 not decoded).

## MtDTI (runtime type info)

Each class registers a static `MtDTI` from a static initialiser (most are only reachable through the
startup ctor table, so Ghidra has no function for them):

```
MtDTI::MtDTI(this=&DTI, name, parent DTI, size, id, r8, r9)    // 0x02D63178
*(DTI + 0x1C) = &Class::MyDTI::__vtbl                         // overrides MtDTI::__vtbl 0x100C9644
register-dtor(…)                                               // 0x03122B28
```

| Off | Field |
|---|---|
| +00 | `const char *name` |
| +04 | next sibling |
| +08 | first child |
| +0C | parent (default `0x103D5B70` when NULL is passed) |
| +10 | hash-bucket link (buckets at `0x103D5B90`, index `id & 0xFF`) |
| +14 | bits: `size = w >> 9` (23 bits) · r9 → bits 3..8 · r8 → bits 0..2 |
| +18 | `id`; when passed 0 it is `MtDTI::calcId(name) = crc32(name, ~0) & 0x7FFFFFFF` (`0x02D63150` → `0x02DBF680`) |
| +1C | vptr |

`MyDTI` vtable slots: `{0, typeinfo}`, dtor, **`newInstance`** (allocates `size` with alignment and calls the
class constructor). The class's own vtable usually follows right after the `MyDTI` vtable.

r8 is 0 for 1,150 classes, 1/2/3 for 80 (flag/attribute bits); r9 is always 0. Meaning unknown.

## Recovered data

`ghidra/scripts/RecoverDTI.java` → `config/dti.csv` + `config/vtables.csv`; `scripts/merge_symbols.py`
folds them into `config/symbols.txt` (lines tagged `gen:dti` / `gen:rtti`).

- **1,251 DTI registrations, 0 unresolved**, 1,251 distinct DTI objects; ids verified against the CRC for
  1,230 (21 pass the id via a register that could not be traced).
- **All 45 ARC resource types** appear as DTIs with the identical id – independent confirmation of the
  ARC type-hash scheme.
- Class prefixes: `u` 535 (units), `s` 164 (singletons/systems), `n` 152 (namespaced), `c` 145, `r` 140
  (resources), `a` 31 (areas/scenes). Biggest parents: `MtObject` 249, `cResource` 73, `uIDLBase` 66,
  `uEffectCore` 61, `uEnemy` 41, `sAppSystemBase` 41, `uID` 36, `cUnit` 36. Main singleton:
  `sLtMH3GMain : sMain` (size 0x1880).
- **3,946 primary vtables** (45,289 virtual slots) for 3,646 class names via RTTI – 1,226 DTI classes plus
  2,420 RTTI-only classes (templates such as `MtTypedArray<…>`, `nUtil::LinkList<…>`, SDK classes).
- 25 DTI classes have no vtable match – small nested config/interface structs (`CreateConfig`,
  `KeyboardConfig`, …) whose RTTI name is qualified differently or which have no virtuals.

## Virtual functions (`ghidra/scripts/NameVirtuals.java` → `config/virtuals.csv`)

- **Typeinfo:** `{0x1014E0B8, name, uniq*, baseList*, …}`; `baseList` = `{baseTypeinfo*, u32 w}` entries
  (offset = `w >> 16`, `0x200` = last; see GHS ABI above); `baseList = 0` for roots (`MtObject` etc.).
  **Typeinfos are duplicated per translation unit**, so a base pointer often lands on a copy no vtable
  uses – resolve bases **by name**.
- **Slot naming:** slot *i* = vtable entry at offset `8 + 8*i`. Same function as the primary base's slot →
  inherited; different → override of the slot's introducer; beyond the base's slot count → new virtual.
  Functions are named after the first class owning them non-inherited: `Class::vf<i>`.
- **Slot 0 is the GHS deleting destructor** `~X(this, flags)` (`if (flags & 1) delete this`) for
  3,866 of 3,942 classes (shape-checked, not assumed) → named `Class::~Class`. The other 76 are classes
  whose first virtual is something else (getters, empty stubs).
- `0x03122C18` = `__pure_virtual_called?` (`li r3,6` → abort path), filling 689 slots. Functions that fill
  slots of several unrelated classes (empty `blr` stubs etc.) are named `vfshared_<n>_<addr>` (777).
- Results: 3,490 vtables with a resolved base + 125 roots (331 bases have no detected vtable);
  45,289 slots = 16,899 inherited + 23,635 overrides + 2,601 new across **26,236 distinct functions**.
  Example: `MtObject` introduces slots 0–4, `cResource` 5–14, `rTexture` overrides all 15.
- **15,535 of the 26,236 virtual targets were never disassembled by Ghidra** (reachable only via vtables).
  They need to become functions in the project – and in the recompiler's function list.

## Static constructor table

`0x101632C4`–`0x1016455C` in `.rodata`: **1,191 pointers** to static initialisers (DTI registrations and
other global constructors), run at startup. Found by searching data for pointers to computed initialiser
starts (`ghidra/scripts/FindCtorTable.java`). The runtime/recompiler must call these in order before `main`.

## Ghidra project state

`ghidra/scripts/CreateFunctions.java` (pre-analysis) created **18,501 functions** for the ctor-table
entries and every vtable slot target; re-analysis then brought the project from 60,690 to **79,851
functions**. `ghidra/scripts/ApplySymbols.java` applied `config/symbols.txt` (26,241 function names, 9,199
labels, 3,739 namespaces, `SourceType.IMPORTED`). `ghidra/scripts/CheckCoverage.java` verifies: every
symbols.txt function exists with its name and all 45,289 vtable slots point at functions.

**Function boundaries (Phase 1).** `RepairBodies.java` (backup in `build/ghidra-backup-phase1`) wired the
1,102 switch tables into their functions, carved 9,359 functions out of `.text` gaps (code reached only
through data/pointers) and split 122 entry points out of 13 functions that Ghidra had glued together
(switch tables whose entries `b` straight into other functions). Result: **88,225 functions in `.text`**,
bodies tile `.text` except 6 data words (`0x00400000` in front of the GHS FPR save/restore helpers around
`0x0312CF14`), and no branch enters a function anywhere but its entry. `ExportFunctions.java` re-checks all
of this and writes `config/functions.csv`.

**Indirect control flow.** GHS switches: `cmplwi idx,N; bgt|bge default; rlwinm idx<<2; base via
lis+addic or addis+addi; mtctr; bctr` with the table = N(+1) inline `b case` instructions right after the
`bctr` (no data tables at all). Virtual calls: `lwz r12,0(this); lwz r0,off(r12); mtctr r0; bctrl`, the
`{delta}` half is not read when the compiler knows it is 0; slot = `(off-12)/8`. Most-called slots: 12
(9,073 sites, mostly on the object returned by `0x02D63E50`), 6 (3,377 – `(size, 0x10)` arguments, looks
like an allocator `alloc`), 7 (2,998). `config/switches.csv`, `config/indirect_calls.csv`.

To refresh after symbols change, re-run only the apply step:
`scripts/ghidra-headless.ps1 -process MH3G_Cafe_US.rpx -noanalysis -postScript ApplySymbols.java <symbols.txt>`

## Core (boot, main loop, allocators, singletons, threads)

**Boot.** `__rpx_start?` `0x02E84370` (ELF `e_entry`, GHS crt) runs the static ctor table and calls
`main` `0x02000020`:
```
sApp app;                       // 0x02B99E78, 0x2D30 bytes on main's stack
...                             // 0x02000338, 0x02000E6C (not yet identified)
app.createMain(&sLtMH3GMain::DTI, 0x10000000);  // 0x02B9AF24: dti->newInstance() -> app+0x2438
app.run();                      // 0x02B9AF98
app.~sApp(2);                   // 0x02B9A2CC
```
**Main loop** (`sApp::run?`, loop at `0x02B9B00C`): waits (`OSSleepTicks`) until startup finishes, then
`while (!main->byte+0x74) { main->vf6(); if (app+0x2448 != app+0x244C) main->vf18(...); }`.
`sLtMH3GMain::update?` (`0x02003954`, = slot 6) calls `sMain::framePacing?` (`0x0294B8DC`, timing via
`OSGetSystemInfo` + `OSSleepTicks`) and then 85 calls into the systems held in the array below — mostly their
slot 6 (cSystem per-frame update), plus slots 9/10/12 on render/GUI/collision systems (meaning not yet known).

**Systems.** `cSystem` subclasses are singletons: the ctor stores `this` into a static `mpInstance`
(usually right after the class's MtDTI object, e.g. `sMain` DTI `0x103CEA24`, instance `0x103CEA44`).
`ghidra/scripts/FindSingletons.java` → `config/singletons.csv`: 70 instance pointers + ctors (6 marked
uncertain: ≤2 reads). `sLtMH3GMain` (0x1880 bytes = `sMain` 0x1770 + 0x110) holds one pointer per system at
`+0x1770…+0x1878` (`main_slot` column), filled by the ctor `0x020013DC` as `new(size)` → ctor → store; 67
slots, from `sResource` (+0x1770), `sLtRender` (+0x1778), `sPad`, `sScene`, … to `sGSound` (+0x1878).
`sApp` (instance `0x103D2E60`) is the owner object on main's stack, not in the array.

**Allocators.** `MtAllocator` (0x6C) subclasses: Default, Heap, Block, Physical (→ `nDraw::VramAllocator`),
Virtual, CafeHeap, ExHeap, Scalable. Slots (from `MtCafeHeapAllocator`, which calls coreinit directly):
6 = `alloc(size, align)` (`MEMAllocFromExpHeapEx`; zero-fills when flag `0x80` at +0x36), 7/8 = forward to
alloc, 12 = `free(ptr)` (`MEMFreeToExpHeap`), 13 = `getSize(ptr)` (`MEMGetSizeForMBlockExpHeap`); MtCafeHeap
keeps a mutex at +0x3C, the ExpHeap handle at +0x6C, bytes in use at +4. Every class's allocator is chosen
by its DTI: `MtDTI::getAllocator?` `0x02D63E50` = `mpAllocator[(dti->w14 >> 3) & 0x3F]`, table at
`0x103D6100` (entry 1 `0x103D6104` is used directly by many deleting destructors).

**MtObject.** Slots 0–4: 0 deleting dtor, 4 `getDTI` (returns the class's DTI; e.g. `MtThread::vf4` →
`0x103F3370`). Slot 3 = **`createProperty(MtPropertyList*)`** (confirmed: `rPopData::createProperty` `0x02071904` calls the
base, then for each field fills a 0x38-byte `MtProperty` — `+0 name`, `+4 type<<16|attr`, `+8 owner` — via a
per-type helper (`0x020711C0` writes type 5 for u16, `0x0207126C` type 6 for u32) and adds it with
`0x02071530`; the names/types/order equal the XFS schema). XFS = this list serialised (`docs/formats/xfs.md`). `merge_symbols.py` `SLOT_NAMES` names
every override of these slots (1,392 functions).

**Threads / jobs.** No job system in this MT Lite build (no job/task-scheduler classes in DTI or RTTI apart
from AI task lists and material "Vjobs"). Concurrency = `MtThread` (0x1C30) subclasses: `sLtRender::ExecuteThread`,
`sSound::UpdateThread`/`LoadThread`, `sID::LoadThread`, `sSavedata::cStorageThread`, `sMovie::FileOpenCloseThread`,
`uMovie::Player::*Thread` (4), keyboard/IME/error-viewer threads. `MtThread::createOSThread?` `0x02D7C9DC`
wraps `OSCreateThread` (priority clamped to 31); resume at `0x02D7CC74`, join at `0x02D7CD40`, affinity
at `0x02D7CFA8`/`0x02D7D114`. `sMain` (ctor resumes, `sMain::vf10` joins), `sResource` and `sApp` also own
OS threads directly. Only 9 `OSCreateThread` call sites exist in total.

## Render submission

All GX2 calls live in the `sLtRender` code (`0x02BB9…`–`0x02BC2…`, plus one present path in sApp code). Systems
record draw records into `sLtRender`; the records are turned into one GX2 display list, which is executed and
presented at the end of the frame:
- `sLtRender::buildDisplayList?` `0x02BBED98`: `beginDisplayList?` (`GX2BeginDisplayListEx` +
  `GX2SetContextState`) → for each record, compare with the previous record (r31 vs r25) and only re-emit what
  changed: textures `0x02BBCBBC`, samplers `0x02BBCF18`, uniforms `0x02BBD29C`, `setShaders?` `0x02BBE358`
  (the **only** `GX2SetVertexShader`/`GX2SetPixelShader` caller) → `GX2DrawEx`/`GX2DrawIndexedEx` (the only
  callers in the game), `GX2ClearColor`… → `endDisplayList?` (`GX2EndDisplayList`).
- Runs on `sLtRender::ExecuteThread` (run = slot 5 `0x02BC22B0`: `OSWaitEvent(start)` → build →
  `0x02BC2278` `OSSignalEvent(done)` + wait), or inline from `sLtRender::vf6` when not threaded.
- Frame: main frame calls `sLtRender` slot 9 first (begin, `0x02BC09F4`) and slot 10 last (`0x02BC0AD8` →
  `endFrame?` `0x02BC014C` → `0x02BBFEEC` → `present?` `0x02BBF9D0`: `GX2DirectCallDisplayList`,
  `GX2CopyColorBufferToScanBuffer` for TV and DRC, `GX2SwapScanBuffers`, `GX2Flush`, `GX2WaitForVsync`).
  A second display-list call is in `0x02BBFFA4`.
- Port consequence: a GX2→Vulkan backend only has to handle what `buildDisplayList?` emits — a small, closed
  set of state calls plus two draw calls — and display lists are replayed with `GX2DirectCallDisplayList`.

## File loading

`sResource` (instance `0x102CEBC4`, `sLtMH3GMain+0x1770`) owns a loader thread created in its ctor via
`MtThread::createOSThreadEx?` `0x02D7D0B4` (stack 0x40000 from the sResource allocator, priority 15, arg =
sResource, entry `sResource::loaderThread?` `0x02BD75E0`, name string "Resource Loa…" `0x100B07BC`). Per request:
1. dequeue under the global mutex `0x102CBD9C`; sleep (`OSSleepTicks`) when empty;
2. copy the 64-char resource name (`res+8`), `buildPath?` `0x02BD73A0` = `sprintf("%s\%s.%s", root, name,
   res->getExtension())` (cResource slot 6: `rArchive` → `"arc"`, `rTexture` → `"tex"`);
3. `MtFile file(path, 0x12)` (`0x02D97FC4`), `MtFileStream s(&file)` (`0x02DAA010`); size → `res+0x54`;
4. `loadFromStream?` `0x02BD7554`: `res->vf10()` then **`res->load(stream)`** (cResource slot 9);
5. close/destroy the stream; flags byte `res+0x50`: bit 0 loaded, `0x10` failed; notify `0x02BD64D4`.
Request API (sResource virtual slots): 11 `requestAsync?` (lookup in the resource table, else create + queue),
12 `create?` (lookup, else `dti->newInstance()` and load synchronously on the caller's thread via the same
`loadFromStream?`), 13 `find?`. `MtFile` wraps coreinit FS: slot 5 open (`FSOpenFile`, `FSGetStatFile`),
7 read (`FSReadFile`), 8/9 async read start/join (own OS thread), 10 seek (`FSSetPosFile`), 13 size.
Port consequence: `MtFile`'s FS calls are the only path into `/vol/content`, so the runtime `fs` overlay plus
`FSOpenFile`/`FSReadFile`/`FSSetPosFile`/`FSGetStatFile`/`FSCloseFile` HLE covers all asset loading.

## Next uses
- Struct sizes from DTI → stub `include/` class headers.
- XFS decoding: done – `createProperty` is MtObject slot 3; format in `docs/formats/xfs.md`.
