# Shaders: `.es`, `.spkg`, `.lfx`

Status: **all shaders translated** – `tools/shaderc-latte` decodes the 668 unique shaders (by runtime key) and turns
every one into GLSL → SPIR-V, `glslc` + `spirv-val` clean (§ Translator). The runtime draws with them
(`docs/runtime.md` § Rendering, stage 2); the boot screens' shaders render correctly.

## Key facts

- MH3U (Wii U) runs on **MT Framework Lite** (the 3DS branch: `Lt*` classes) with a **DMPGL compatibility
  layer**: the renderer drives 3DS-style fixed-function state (`dmp_TexEnv[0..5]`, fragment lighting
  LUTs, gas fog, shadow textures), and the Wii U build picks a precompiled GX2 pixel shader for each state
  combination. Capcom calls these "emulation shaders" (`rEmulationShader`).
- **Every shader is a precompiled GX2 binary** (Nintendo GFD/`.gsh` container, `Gfx2` v7.1, R600-family
  Latte microcode). No source, no IR. The executable embeds **no** shaders (no `Gfx2` in the RPX).
- The set is **closed and small: 667 unique shaders** (582 pixel, 78 vertex, 7 vertex+geometry), all
  shipped as files.

## Files

| File | Class | Format |
|---|---|---|
| `nativeCafe/es/<id>.es` ×967 | `rEmulationShader` | 16-byte header + one GFD (pixel shader) |
| `nativeCafe/system/ltshader/null.es`, `uber.es` | `rEmulationShader` | same; ids `0xFFFFFFFF` and `0` |
| `nativeCafe/shaderPack.spkg` | `rEmulationShaderPackage` | `PKG` header (u32 count = 967) + the `.es` GFDs bundled, each preceded by its u32 id. 965/967 identical to the loose `.es`; 580 unique contents (deduplicated) |
| `nativeCafe/LtUserShader/LtUserShaderPackage.lfx` | `rLtShader` | MT `MFX` effect file: 92 GFDs (vertex; 14 also carry geometry + GS copy shader) + the effect's metadata |

All big-endian. Magics shown readable (stored as byte-reversed BE u32s, see `README.md`).

### `.es` header (0x10 bytes)

| Off | Type | Value |
|---|---|---|
| 0x00 | u32 | magic `ESX` (`0x00585345`) |
| 0x04 | u32 | version `0x0A` |
| 0x08 | u32 | id = 32-bit state hash; equals the file name (`0000c977.es`) |
| 0x0C | u32 | 0 |
| 0x10 | GFD | `Gfx2` header + `BLK{` blocks: 6 = pixel shader header, 7 = pixel program, 1 = EOF |

Uniform blocks in every emulation pixel shader: `CBTexComb` (`PSTexComb.env_w[]`), `CBLighting`
(`PSLighting.fv0..7/uv0..7`, `ambient`), `CBGasFog` (`fogColor`, `fogTable[0..127]`), `CBMiscOp`,
`CBConstant` (`_PSConstant.texComb/lightEnv/lighting/gasFog/miscOp`). Samplers: `Texture0..3`,
`TextureCube0`, `TextureShadow`, `LutD0`, `LutD1`, `LutFR`, `LutRR`, `LutRG`, `LutRB` – exactly the
PICA200 fragment-lighting LUT set.

### `.lfx` (MFX)

Header magic `MFX` (`0x0058464D`), then name tables. Readable names cover the whole effect system:
techniques (`TMaterialStd`, `TMaterialToon`, `TBloomFilter`, …), shader names (`VS_*`, `PS_*`), input
layouts (`IA_OBJ_4WT`, …), constant buffers (`CBMaterial`, `CBJointMatrix`, `CBViewProjection`, …), sampler /
blend / depth / raster states (`SS*`, `BS*`, `DS*`, `RS*`), and DMP fixed-function names (`DMP_TexEnv0..5`,
`DMP_FragmentLighting`, `DMP_LightEnv*`, `DMP_Fog`). Vertex-shader constants are a flat `c[0..95]` array –
the PICA200's 96 float uniform registers – so the vertex shaders are 3DS vertex programs carried over.
The `.lfx` contains **no pixel shader GFDs** although it names `PS_*` shaders: hypothesis – `PS_*`
techniques resolve to emulation shaders via DMPGL state.

## Engine side (executable strings)

`rEmulationShader`, `rEmulationShaderPackage`, `rLtShader`, `sLtShader` (all with `::MyDTI`), `DMPGL`,
`dmp_TexEnv[n].{srcRgb,operandRgb,combineRgb,scaleRgb,constRgba,bufferColor,bufferInput,…}`,
paths `LtUserShader\LtUserShaderPackage`, `system\ltshader\LtShaderPackage`, and the messages
`"Emulation shader compile failed. %s"`, `"Shader object id conflict. %s : %s"`.

## Translation plan (decided: offline Latte → SPIR-V for all 667)

Rejected alternative: a PICA200 fragment generator for the 582 pixel shaders (would need the DMPGL
state-hash and uniform packing reversed first, and two shader paths to maintain).

**Pipeline** – a host tool `tools/shaderc-latte/` (our code, C++ or Python), run once per game build:

1. **Input:** `build/shaders.json` (from `scripts/shader_inventory.py`) – each unique GFD by SHA-1.
2. **Parse GFD:** shader header block (`GX2VertexShader` / `GX2PixelShader` / `GX2GeometryShader` struct:
   register state, uniform blocks, uniform vars, samplers, attribs, varyings/semantics) + program block.
   Struct layouts from wut `gx2/shaders.h`.
3. **Decode Latte microcode** (R600/R700 ISA, "Evergreen-lite"): CF clauses → ALU (VLIW5 groups, literal
   constants, PV/PS forwarding, predication), TEX and VTX fetch clauses, exports. References: AMD R600/R700
   ISA docs, Cemu `LatteDecompiler*` (MPL-2.0), decaf-emu `latte` (GPL – read only).
4. **Emit GLSL 4.50** (Vulkan dialect) per shader, then compile with `glslc` (Vulkan SDK) → **SPIR-V**.
   GLSL as the intermediate keeps output human-debuggable; can switch to direct SPIR-V emission later.
5. **Mapping rules** (shared with the runtime GX2 layer, so put them in one header):
   - uniform blocks → UBO binding = Latte block index; loose uniforms (`c[0..95]` in VS) → one UBO
   - samplers → combined image samplers, binding = Latte sampler slot
   - VS inputs: Latte vertex shaders read attributes via a **fetch shader** the game builds at runtime with
     `GX2InitFetchShaderEx`; the runtime turns that into Vulkan vertex-input state, and the translator
     assumes the fetch shader's GPR layout (attribute N → R(N+1) convention, confirm per shader)
   - VS→PS varyings: match by Latte semantic IDs from the shader headers
   - GS: Latte uses ring buffers + a VS-side "copy shader"; translate to a native Vulkan geometry shader
     (only 7 unique VS+GS pairs – handle as a special case)
   - Clip-space: Latte uses GL-style depth (-1..1) → emit fix-up or use `VK_EXT_depth_clip_control`
6. **Output:** `build/shaders/<key>.{vert,frag,geom}.spv` (key: see § Translator). The runtime hashes the shader
   the game hands to `GX2SetVertexShader`/`GX2SetPixelShader` and loads the file; a miss is a hard error (set is
   closed).
   Generated SPIR-V is game-derived – never committed, regenerated from the user's dump.

**Verification**
- Tool must translate **667/667**; `glslc` + `spirv-val` clean for all.
- Disassembler mode (Latte → text) for diffing against Cemu's shader dumps / logs.
- Per-shader render tests once the runtime draws: same draw captured in Cemu vs our build (RenderDoc).

**Milestones**
1. GFD header parser + microcode disassembler for all 667 (no codegen) – proves decode coverage.
2. Pixel shaders (`ps`, 582) – single pattern family, highest leverage.
3. Vertex shaders (78) incl. fetch-shader convention.
4. VS+GS (7).

Open: whether `uber.es` is ever bound at runtime (if so it is just another of the 667).

## Translator (`tools/shaderc-latte/`)

```
build/cmake/tools/shaderc-latte/Debug/shaderc-latte.exe <repo> [--disasm] [--stats] [--glsl] [--spv]
```

Scans the dump itself (same sources as `shader_inventory.py`), writes to `build/shaders/` (game-derived, never
committed): `<key>.<stage>.txt` listings, `<key>.{vert,frag,geom}` GLSL, `<key>.<stage>.spv`, `varyings.txt`.
`--spv` runs `glslc --target-env=vulkan1.3 -O` and `spirv-val` in parallel (~4.5 min for 668 in Debug).
ctest `latte_decode` checks that every shader decodes.

- **Key:** FNV-1a 64 over the header's register block (VS 0xD0, PS 0xA4, GS 0x4C bytes, big-endian as in guest
  memory) followed by the program bytes – what the runtime can compute from the `GX2*Shader` struct it is given.
  668 keys vs 667 GFD SHA-1s: a VS+GS GFD gives two shaders, and some GFDs differ only outside regs/program.
- **Microcode** is little-endian. Decoder: `latte.cpp` (CF, ALU with VLIW unit assignment and literals, TEX, VTX).
- **Bindings / interface:** `runtime/gfx/shader_abi.h` – set per stage, UBO binding = block index (the game's
  blocks are already little-endian; `ivec4 d[]`), sampler binding = 32 + slot, VS inputs `uvec4` GPR-ready bits at
  location = attribute semantic, varyings at a game-wide dense location (`varyings.txt`), alpha test (PS) and point
  size (VS) via push constants. `<key>.<stage>.meta` lists the blocks, samplers and attribute semantics a shader
  uses, for the runtime's descriptor layouts.
- **Codegen model** (`glsl.cpp`): GPRs are `ivec4` bit patterns; each VLIW group reads all operands, then writes
  GPRs and PV/PS. Non-IEEE MUL/MULADD/DOT4 keep 0×anything = 0. Branching emulates the active-mask stack per
  invocation (`m0..mN`); JUMP/ELSE targets are checked statically (stack depth at the target must equal depth at the
  jump minus POP_COUNT) so jumps can be dropped. DX10 loops → `for` with a 4096-iteration cap. KILL → `demote`.
  CUBE + cube SAMPLE round-trip through face/s/t back to a direction. SIN/COS take revolutions (R700).
- **GS pairs:** the VS's ES→GS ring writes become outputs `es<i>` (i = dword offset / 4), read by the GS as
  `in vec4 es<i>[]` (input is points: only vertex 0 is fetched). GS ring writes go to a local `gsr[]`; each
  EMIT_VERTEX calls `copy_vertex(k * GS_ITEMS)`, the translated copy shader, then `EmitVertex()`. k is the number of
  vertices emitted so far at run time: the sprite GS's branches each write their 4 vertices at the same ring offsets.
- **Unchecked semantics** (need a render comparison): Latte `gl_FragCoord` origin/w, depth range (plan:
  `VK_EXT_depth_clip_control`), quad behaviour of WQM clauses under divergent masks, FLT_TO_INT rounding.

## Open questions
- How the 32-bit `.es` id is computed from DMPGL state (find `%08x` formatting near `rEmulationShader` load).
- What `uber.es` is used for (fallback for missing permutations?).
- Where `system\ltshader\LtShaderPackage` comes from – no such loose file; probably inside an ARC.
- Meaning of the `PKG` header words after the count (`0x9EA400`, `0x2D2400`, `0x3A9FC` – look like sizes).
- The 2 shaders that differ between `shaderPack.spkg` and the loose `.es` files.
