# recomp/ – static recompiler (Espresso PPC → C++)

Our own recompiler (decision in `CLAUDE.md`). Pipeline, as it gets built:

1. **Load** the decompressed RPX (`extracted/.../code/MH3G_Cafe_US.elf`): `src/elf.*` reads sections,
   `src/loader.*` applies `.rela.text/.rela.rodata/.rela.data` and records import references (done, below).
2. **Decode** every word of every function in `config/functions.csv`: `src/espresso.*` (done, below).
3. **Emit** C++ per function → `recomp/out/` (generated, not committed, ~235 MB): `src/emitter.*`, driver
   `tools/recomp_emit.cpp`. **Every function of the game is emitted** (see Emitter below); nothing is stubbed.

## Guest model (`include/ppc_context.h`)
- `Context`: `r[32]`, `f[32]` as `{ps0, ps1}` doubles, `cr[8]` as `{lt, gt, eq, so}` bytes, `lr`, `ctr`, XER bits
  (`so/ov/ca`), `fpscr`, `gqr[8]`. Memory is flat: guest address A = `base + A`; `ld*/st*` byte-swap (big-endian),
  `*_le` for `lwbrx`-style ops. Every function is `PPC_FUNC(sub_XXXXXXXX)` = `void(Context& ctx, uint8_t* base)`.
- FP lanes follow Gekko: `lfs` and single-precision ops (`fadds`, `frsp`, …) write both lanes; `lfd`, `fmr` and
  double ops write ps0 only; `ps_merge10` swaps – which is what GHS's ps1 save/restore relies on.
- Host-safety choices: `divw` by 0 / `INT_MIN/-1` give Dolphin's results instead of trapping; cache ops are no-ops
  except `dcbz` (zeroes 32 bytes). `fres`/`frsqrte` are exact, not the hardware estimate; `fctiw` assumes
  round-to-nearest (FPSCR rounding mode not modelled yet).

## Emitter (`src/emitter.h`)
- One C++ block per instruction with the canonical disassembly as a comment; in-body branches become `goto loc_X`;
  `bc` decrements CTR / tests CR bits per BO/BI; record forms update CR0; carries update XER[CA].
- **Calls use the host stack.** `bl f` → `ctx.lr = pc+4; sub_f(ctx, base);`, `bclr` → `return` (also when LR was
  saved/restored through the guest stack, which only round-trips the same value), `b`/`bc` to another function's
  entry → tail call `sub_f(ctx, base); return;` (LR untouched, so the callee still returns to our caller), `bl` to the
  next instruction → only sets LR (get-PC idiom). Assumes ABI-conforming LR use: code that `mtlr`s a computed
  address and `blr`s to it would be mistranslated – none found yet, not proven absent.
- **Imports**: `b`/`bl` sites in `Image::import_calls` call `import_<rpl>_<name>(ctx, base)`; generated
  `imports.cpp` stubs report `unimplemented_import` until the HLE runtime provides them (578 functions).
- **Switches**: a `bctr` listed in `config/switches.csv` becomes `switch (ctx.ctr) { case <slot>: goto <slot>; … }`
  over its inline table slots (each slot is a `b case` instruction, emitted as goto or tail call); an address
  outside the table hits `unimplemented_function`.
- **Indirect calls/jumps**: `bctrl`/`bclrl` → `ppc::call_indirect(ctx, base, target)` with LR set; other `bctr` → the
  same as a tail call. `call_indirect` (`support/indirect.cpp`) binary-searches the generated function table (88,225
  entries) and import table (578 stubs, for function pointers to imports); unknown targets are reported.
- **Multi-range bodies** (135, entry range + chunks from `functions.csv`) are emitted as one C++ function, ranges in
  address order, `goto` the entry when a chunk lies below it; a range that does not end in a jump continues with a
  `goto` to the following address, or – when that address starts another function (47 functions, GHS lets code
  run on into the next function) – with a tail call to it.
- **Atomics**: `lwarx` records a reservation (address + raw word) in the context; `stwcx.` succeeds only through a
  host `std::atomic_ref` compare-exchange against that word, so it stays atomic with guest threads on host threads
  (ABA accepted). `lswi`/`stswi` follow the architected register/byte order; `tw`/`twi` evaluate TO and call
  `ppc::trap` (report + stop) when it holds. Calls to `$UNDEF` weak symbols call address 0 through `call_indirect`
  (reported if ever reached; the crt tests these pointers first). `.syscall` (0x02000000, `nop; blr`) is emitted as
  a function: crt code calls it.
- Output (`recomp/out/`): `recomp_00..63.cpp`, `functions.h` (all declarations), `stubs.cpp` (rejected functions →
  `unimplemented_function(addr)` with the reason as a comment), `imports.cpp`, `function_table.cpp` (all 88,225
  `{addr, fn}` sorted – for indirect calls), `emit_report.txt`.
- **Result on the game: all 88,226 functions (88,225 + `.syscall`) emitted – 4,610,269 of 4,610,269 instructions
  (100%), 0 stubs.** The program compiles with MSVC /W4 with **0 warnings** (~5 min, ~370 MB generated) and **links**
  (`link_check`: 88,226-entry function table, sorted, every reference resolved).
- `emit_selftest`: 23 hand-encoded functions emitted at build time, compiled and executed: CTR loop, rotate/insert,
  64-bit carry, signed compare, big-endian loads/stores with update, `fmadds`/`fctiwz`/`stfiwx`/`fcmpu`,
  `srawi`+`addze`, `divw` incl. /0, record-form early return, `ps_merge10` lane save, call + tail call (callee sees
  the caller's LR), import call, a GHS stack frame saving/restoring LR through guest memory, a GHS inline switch (including
  default and a tail-call case), an indirect `bctrl` through the function table, an atomic increment loop
  (`lwarx`/`stwcx.`/`bne`), `lswi`, two-range bodies (chunk after and before the entry), fall-through into the
  next function, and a non-trapping `twi`.

## Loader (`src/loader.h`)
- 524,610 relocations, 4 types: `ADDR32` 138,659 · `ADDR16_LO` 200,260 · `ADDR16_HA` 178,490 · `REL24` 7,201.
  Internal ones go through section symbols (`$TEXT` = 0x02000000, `$DATA` = 0x10000000) + addend and **change
  nothing** – the file is already linked at its final addresses (checked: 0 changed). Only the 7,298 import
  relocations change bytes.
- `Image::imports` = the 588 static imports (exactly `config/imports.csv` func+data); `import_calls` maps the 7,189
  `bl`/`b` sites to their import, `import_at(addr)` answers for any relocated instruction (calls and `lis`/`addi`
  halves of import-data addresses). Import stubs (0xC000xxxx) are out of branch range: like Ghidra the branch keeps
  only the low 26 bits of the displacement – the emitter must use `import_calls`, not the branch target.
- `$UNDEF` (undefined weak symbols, e.g. optional crt hooks in `__rpx_start?`) resolves to **0** as on the Cafe
  loader; 28 sites, recorded in `undefined_refs`. Ghidra uses a placeholder (`0xC001D000`) there.
- Verified (`loader_check <repo> <dir>` with `ghidra/scripts/ExportBlocks.java`): `.text`, `.rodata`, `.data` equal
  Ghidra's relocated memory **byte for byte** apart from the 28 `$UNDEF` words.

## Espresso decoder (`src/espresso.h`)
- Canonical ops only (no simplified mnemonics); 224 ops: integer, branch/CR, load/store (incl. byte-reversed,
  multiple, string), FPU, all paired-single forms (`ps_*`, `psq_*`, `dcbz_l`), supervisor/cache ops. Fields are
  decoded by bit position (`d/a/b/c/e`, `imm`, `spr`, `crf`, `gqr`, `w`, `rc/oe/lk/aa`); the op gives them meaning.
- `format()` prints canonical text for debugging (`addi r3,r1,0x8`, `bclr 20,0`, `psq_l f1,0x8(r3),0,2`).

## Verification
- `espresso_test`: hand-checked encodings (words from the game as Ghidra shows them + hand-encoded PS forms).
- `decoder_coverage <repo> [out.tsv]`: decodes all **4,610,267** words in the 88,225 function bodies (entry ranges +
  chunks): **0 invalid**, 647,537 distinct words, 134 ops used.
- Cross-check vs Ghidra: `ghidra/scripts/ExportMnemonics.java` + `scripts/check_decoder.py ours.tsv ghidra.tsv` –
  **640,600 distinct words compared, 0 op mismatches, 0 register mismatches** (Ghidra's simplified mnemonics are
  mapped back to canonical ops). The ~7k words only on one side are relocation sites (see Load).

## Findings that shape the emitter
- **No paired-single math in the game.** Opcode 4 occurs only as `ps_merge10 fN,fN,fN` (17,209×) and opcodes
  56/57/60/61 (`psq_l/st`) never: GHS saves/restores the ps1 half of non-volatile FPRs in prologues/epilogues
  (`ps_merge10` + `stfs`). The emitter still needs a two-lane FPR model for correctness of that save/restore, but no
  GQR quantisation path is exercised by game code.
- Hottest ops: addi 660k, lwz 532k, b 396k, or 395k, stw 336k, bc 299k, lfs 237k.
