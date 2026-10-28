# Asset formats

One file per format (`arc.md`, `tex.md`, `mod.md`, …). For each: magic, endianness (Wii U = big-endian),
header layout, compression, which game code loads it (function address), and tool support.

- **[inventory.md](inventory.md)** – generated overview of every file type and every resource type inside
  ARCs (counts, sizes, magics). Regenerate with `python -I scripts/asset_inventory.py .`
- **`config/resource_types.csv`** – ARC type hash → resource class, machine-readable.

Magics: MT Framework writes 4-char magics as little-endian u32s; on Wii U they're stored big-endian, so
raw bytes read backwards (`\0XET` = `TEX`). Tables below use the readable form.

## Top-level picture (base v0 + update v32 = 7,807 files, ~6 GB)

| Share | What |
|---|---|
| 2.3 GB | 16 `.mp4` movies (`nativeCafe/movie`) – standard MP4/H.264, play with any decoder |
| 1.9 GB | 4,609 `.arc` archives → 58,609 resources; 3.4 GB of the uncompressed content is `rTexture` |
| 1.8 GB | 323 `.dspw` streams (BGM + SE) – `rSoundSourceDSPADPCM`, Nintendo DSP-ADPCM |
| small | quest data, sound sequencing, shaders, GUI, fonts, collision, enemy AI tables |

## Formats

| Ext / class | Magic | Description | Doc | Status |
|---|---|---|---|---|
| `.arc` | `ARC` | MT archive v16, zlib entries | [arc.md](arc.md) | **verified** |
| `rTexture` (`.tex`) | `TEX` | Texture v0xA5: 16-byte header + GX2-tiled data; BC1/BC3/RGBA8 | [tex.md](tex.md) | **verified** for all 17,244 incl. cube maps |
| `rModel` (`.mod`) | `MOD` | Model | | unknown |
| `rMaterial` | `MRL` | Material | | unknown |
| `rVramLoadList` | `rvl` | Per-model VRAM upload list (Wii U specific?) | | unknown |
| `rMotionList` | `LMT` | Animation list | | unknown |
| `rSoundSourceDSPADPCM` (`.dspw`) | `WPSD` | DSP-ADPCM audio | | unknown |
| `rEmulationShader` (`.es`) | `ESX` | One precompiled GX2 pixel shader per 3DS (DMPGL) fixed-function state combination; id = state hash | [shaders.md](shaders.md) | container known |
| `rEmulationShaderPackage` (`.spkg`) | `PKG` | The `.es` shaders bundled (id + GFD) | [shaders.md](shaders.md) | container known |
| `rLtShader` (`.lfx`) | `MFX` | MT effect file: techniques, states, layouts + 92 vertex/geometry GFDs | [shaders.md](shaders.md) | partly known |
| XFS-based (`.pop`, `.supp`, `.list`, `rStageData`, `rTexLayout`, …) | `XFS` | MT's reflection-serialised objects; the file embeds its schema (createProperty list) → [xfs.md](xfs.md), `scripts/mt_xfs.py` | | decoded (848/848) |
| `memory.ini` | text | Allocator/heap budgets per platform: Win32, CTR (3DS), CAFE (Wii U) | | readable |

Full list of all 45 ARC resource types and 26 loose extensions: [inventory.md](inventory.md).
