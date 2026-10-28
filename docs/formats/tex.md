# `rTexture` / `.tex` – MT texture (Wii U, version 0xA5)

Status: **complete for every texture in the game** – header, mip layout (17,244/17,244 sizes incl. 192 cube
maps), deswizzle and BC1/BC3/RGBA8 decode verified (`scripts/mt_tex.py verify` / `export`).

Big-endian. 16-byte header, then texture data (GX2 tiled), no per-level table.

## Header

Three u32 bitfields, packed most-significant-bit first (the same C bitfields as the little-endian
3DS/PC TEX, laid out by a big-endian compiler).

| Off | Bits | Field | Observed |
|---|---|---|---|
| 0x00 | 32 | magic `0x00584554` (`TEX\0` as LE u32) | all |
| 0x04 | 31..20 | version | `0xA5` for all (same version number as MT 3DS textures) |
| 0x04 | 19..0 | flags/type | `0x2` (16,953), `0x4006` cube (192), `0x3002` (52), `0x402` (47) |
| 0x08 | 31..26 | mip count | 1–12 |
| 0x08 | 25..13 | width | |
| 0x08 | 12..0 | height | |
| 0x0C | 31..24 | faces | 1, or 6 for cube maps |
| 0x0C | 23..16 | format | see below |
| 0x0C | 15..0 | ? | always `8` |

## Formats

Only three appear. **Confirmed by decoding** (see Verification).

| Code | Count | Format | GX2 surface format | Element |
|---|---:|---|---|---|
| `0x0C` | 8,905 | BC3 (DXT5) | `UNORM_BC3` 0x33 | 4×4 block, 16 B |
| `0x0B` | 7,941 | BC1 (DXT1) | `UNORM_BC1` 0x31 | 4×4 block, 8 B |
| `0x03` | 398 | RGBA8, byte order R,G,B,A | `UNORM_R8_G8_B8_A8` 0x1A | 1 texel, 4 B |

BC blocks are stored little-endian as on every other platform (no byte swap); only the header is BE.

## Mip layout (verified model)

Levels are stored back to back from level 0, no extra alignment between levels. Sizes follow GX2/addrlib
tiling with these parameters, which hold for all three formats:

- **2D tiled** (macro tile): dimensions in elements aligned to **32 × 16**.
- **1D tiled** (micro tile): dimensions in elements aligned to **8 × 8**.
- Level 0 is 2D unless *both* texel width < 32 *and* texel height < 16 (then 1D).
- A later level degrades from 2D to 1D (permanently) once its width in elements < 32 *or* height < 16.

Example, BC1 128×128 with 8 mips: L0 32×32 elements 2D = 8,192 B; L1 16×16 → 1D = 2,048 B; L2–L7 one
micro tile each = 6 × 512 B; total 13,312 B, exactly as stored.

So the data is the GX2 surface image + mip chain as the hardware wants it (2D = `GX2_TILE_MODE_TILED_2D_THIN1`,
1D = `TILED_1D_THIN1`). The engine uploads it directly; our runtime must **deswizzle** it to linear
BCn/RGBA data for Vulkan.

## Deswizzle (verified)

AMD addrlib R6xx `ComputeSurfaceAddrFromCoord{MicroTiled,MacroTiled}` for THIN1, with Wii U parameters
2 pipes, 4 banks, 256-byte pipe interleave; coordinates are in elements (4×4 blocks for BCn) and bpp is the
element size in bits (32 / 64 / 128). **Pipe and bank swizzle are 0** for these textures. Macro tile =
8·banks × 8·pipes = 32×16 elements, which independently matches the size fit above. Implementation:
`addr_1d_thin1` / `addr_2d_thin1` in `scripts/mt_tex.py`.

## Verification
- Visual: a BC1 1024² GUI background, a BC3 1024² icon atlas (alpha correct) and an RGBA8 mip level 1
  (512²) decode to clean images with no tile seams.
- 1D path: on 40 BC textures whose level 1 is 1D-tiled, decoded L1 vs. 2× box-downscaled L0 differs by
  **2.7/255** on average (worst 6.8); treating L1 as linear instead gives 34.2 (worst 95.1).
- Exported PNGs are game data – generate them outside the repo, never commit them.

## Cube maps (verified)

192 textures, flags `0x4006`, 6 faces, all BC1, sizes 64×64/7 mips and 256×256/9 mips.

- After the 16-byte header: **0x6C bytes = 27 floats** (plausible values; most likely 9 RGB
  spherical-harmonic coefficients, as on other MT titles – meaning not yet confirmed), then texel data.
- Each level stores the faces as consecutive **slices**: **6 at level 0, padded to 8 (next power of two) at
  every later level**. Slice size = aligned level size from the rules above; tile mode per level is chosen
  exactly as for single-face textures.
- 2D-tiled slices use addrlib's per-slice bank/pipe rotation: `bankPipe ^= slice * 2` (rotation =
  pipes × (banks/2 − 1)); slice offset = slice × aligned slice bytes, added before the pipe/bank bits are
  inserted. 1D-tiled slices are simply consecutive.
- Verified: model sizes match **192/192** cube maps; on both cube sizes every face's level 1 (1D) matches
  its 2× downscaled level 0 (2D) within 0.7–2.3/255. Without the rotation, faces 1, 2, 3, 5 decode wrong
  (0 and 4 have rotation ≡ 0 mod 8). Face order is the slice order (presumably +X, −X, +Y, −Y, +Z, −Z –
  check when rendering).

## Open questions
- Meaning of flag values `0x402`, `0x3002`, and the constant `8` in word 3.
- sRGB vs linear (not in the header – probably decided by the material/sampler).
- Meaning of the 27 cube-map floats; cube face orientation.
