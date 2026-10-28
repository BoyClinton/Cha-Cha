# CLAUDE.md – working guide for Cha-Cha, the MH3U PC port

Read this first every session, then `docs/ROADMAP.md` (what phase we're in) and the tail of
`docs/research-log.md` (what was learned last).

## Goal
Native PC (Windows first) port of Monster Hunter 3 Ultimate (Wii U, MT Framework engine) via
**static recompilation + HLE Cafe OS runtime + progressive decompilation**. Functional
equivalence, not byte-matching (GHS compiler unavailable).

## Hard rules
- **Never commit or publish game data**: `.wua`, RPX/RPL/ELF, extracted assets, or large
  verbatim disassembly dumps. Only our code, addresses, names, and format notes go in git.
- **Addresses are build-specific.** Before using any address, check `config/target.txt`. If the
  RPX hash there differs from what's in `extracted/`, stop and tell the user.
- **Don't guess facts about the binary.** Confirm in Ghidra/disassembly or in Cemu before writing a
  name into `config/symbols.txt`. Mark uncertain names with a `?` suffix and a `# note`.
- **Licenses:** Cemu (MPL-2.0) is a **reference only** – read it to understand behaviour, write our own code; no
  Cemu source lines in the repo (user decision, 2026-10-24). decaf-emu is GPL-3.0 – compatible with the project's
  GPL-3.0 since 2026-10-28, but still read for understanding only: the README promises "no emulator code exists in
  this project", so ask the user before copying any.
- Log every non-trivial discovery in `docs/research-log.md` (date, address, evidence).
- Large dumps (`game/`, `extracted/`) are several GB on a spinning HDD (repo lives at
  `D:\Projects\Claude\Monster Hunter 3 Ultimate` since 2026-10-24); avoid churning them (don't re-extract
  needlessly).

## Environment
- Windows 11, PowerShell primary (Bash also available). Scripts in `scripts/` are PowerShell;
  Python helpers should run with `python -I`.
- Tool binaries in `tools/bin/`; see `tools/README.md` for what's installed.
- C++ build: CMake with the Visual Studio 2026 generator into `build/cmake` (see README "Building the
  runtime"). CMake/ctest live in VS's `Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin` (not on PATH
  outside the Developer shell). C++20, MSVC `/W4` must stay warning-free.
- Ghidra 12.0 is installed at `D:\Projects\Tools\Ghidra - 12.0` (since 2026-10-26).
- Ghidra headless: always `scripts/ghidra-headless.ps1` with an absolute path (the shell's cwd drifts);
  add `-readOnly -noanalysis` unless the project is meant to change. Backup before writable runs.

## Target facts (fill in as confirmed)
- CPU: Espresso, 3× PPC750CL-derived cores, big-endian, paired singles + GQRs – but GHS emits **no paired-single
  math**: only `ps_merge10` in FPR save/restore (17,209×), no `psq_l/st` at all (`recomp/README.md`).
- GPU: Latte (R7xx-family), driven via GX2; shaders are compiled R600-style microcode.
- Executable: **US update v32** `extracted/0005000e10118300_v32/code/MH3G_Cafe_US.rpx`
  (SHA1 `d97aabfc…`; full hashes in `config/target.txt`). Base v0 is kept only for diffing. Internal name `MH3G`.
- Engine: MT Framework **Lite** (3DS branch) with a DMPGL layer. MtDTI + GHS RTTI are recovered
  (`docs/engine.md`, `config/dti.csv`, `config/vtables.csv`): 1,251 classes, 3,946 named vtables.
- **GHS C++ ABI:** vptr sits after the members of the first polymorphic class (often +0, but `MtDTI`
  has it at +0x1C); vtable entries are 8 bytes `{delta, fn}` with entry 0 = `{0, typeinfo}`.
- Most static initialisers are not Ghidra functions (reached only via the ctor table) – scripts that
  propagate constants must handle code outside functions.
- Asset container: `.arc` (MT Framework). Formats documented in `docs/formats/`.

## How to tackle a work session
1. Check `ROADMAP.md` checkboxes → pick the first unchecked item in the current phase (or what the
   user asks).
2. Gather evidence: Ghidra (headless via `analyzeHeadless` + scripts in `ghidra/scripts/` when
   automating), Cemu debugger for runtime behaviour, string/DTI scans with our Python tools.
3. Record results in machine-readable form (`config/*.txt|csv`) *and* a short log entry.
4. Tick the ROADMAP box only when the result is verified.

## Conventions
- `config/symbols.txt`: `0x0200ABCD cResource::load func # from DTI vtable slot 3`
- Class names keep MT Framework style (`cFoo`, `uBar`, `rBaz` for resources, `sQux` for singletons).
- Recompiled functions are named `sub_<ADDR>` until a symbol exists; the recompiler reads
  `config/symbols.txt` to emit real names.
- Hand-decompiled replacements in `src/` carry a header comment: original address, status
  (`verified` / `wip`), and how it was verified (side-by-side run, unit test against recomp output).
- Runtime HLE functions in `runtime/<area>/` are named after the Cafe OS export they implement
  (`OSCreateThread`, `GX2DrawEx`) and recorded in `config/imports.csv` with status
  `stub` / `partial` / `done`.

## Directory map
`game/` dump · `extracted/` unpacked dump · `tools/` tool list+bins · `scripts/` our scripts ·
`config/` binary knowledge · `ghidra/` RE project · `recomp/` recompiler cfg + `out/` · `runtime/`
Cafe OS replacement (os, gfx, audio, input, fs, net) · `src/` decompiled game code · `include/`
headers · `assets/` format tools · `docs/` plans & notes · `build/` output.

## Open decisions (ask the user, don't assume)
- None right now.

## Decided
- Releases: ship the exe as a zip on GitHub Releases, Yakumo-style (`scripts/package-release.ps1`; the exe contains
  the recompiled game code, no game files). First release **0.1.0** (user decision 2026-10-28).
- License: **GPL-3.0** (`LICENSE`, user decision 2026-10-28). The public GitHub history starts from a fresh
  single commit (branch `github`), so the briefly vendored Cemu code of 2026-10-23 is not published.
- Project name: **Cha-Cha** (user decision 2026-10-27); user-facing text and the exe use it, internal names (`mh3u` target, `MH3U_*` env) stay.
- Canonical target: US release (`0005000010118300`), update v32.
- Graphics/platform: Vulkan + SDL3.
- Shaders: **offline Latte → SPIR-V translation of all 667 shipped shaders** (no runtime JIT, no PICA200
  fragment generator). Plan in `docs/formats/shaders.md` § Translation plan.
- Recompiler: **our own** (C++20, in `recomp/`), not an adapted XenonRecomp – built around Espresso paired
  singles/GQRs, the Wii U ABI, and what Phase 1 mapped (inline `b` switch tables, GHS this-adjust thunks,
  `config/functions.csv`). Pipeline: ELF `.text` → Espresso decoder (`recomp/src/espresso.*`) → C++ emitter.
