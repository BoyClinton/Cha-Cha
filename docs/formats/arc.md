# `.arc` – MT Framework archive (Wii U, version 16)

Status: **header and entry table verified** on all 4,609 archives (58,609 entries) by
`scripts/asset_inventory.py`; decompressed-size field verified on 9,824 sampled entries.

All fields are **big-endian**.

## Header (0x0C bytes)

| Off | Type | Value |
|---|---|---|
| 0x00 | u32 | magic `0x00435241` (`"ARC\0"` as a little-endian u32, i.e. MT's LE magic stored BE) |
| 0x04 | u16 | version = `0x10` (16) for every archive in MH3U |
| 0x06 | u16 | entry count |
| 0x08 | u32 | 0 (padding) |

## Entry (0x50 bytes, `count` entries starting at 0x0C)

| Off | Type | Meaning |
|---|---|---|
| 0x00 | char[64] | path without extension, `\` separators, NUL-padded (e.g. `player\mod\m\face000\m_face000`) |
| 0x40 | u32 | resource type hash → `config/resource_types.csv` |
| 0x44 | u32 | compressed size |
| 0x48 | u32 | `size_flags`: **decompressed size = `size_flags >> 3`**, low 3 bits = flags (observed values in `build/asset_inventory.json`) |
| 0x4C | u32 | absolute file offset of the data |

The low-bits packing (`>> 3`) differs from the little-endian PC/3DS ARCs (size in low 29 bits); it is the
same C bitfield laid out by a big-endian compiler.

Data is zlib (RFC 1950) compressed. Same-named entries with different type hashes are common
(a model's `rModel` + `rMaterial` + `rVramLoadList` share one path).

## Type hash

`type_hash = ~crc32(class_name) & 0x7FFFFFFF` (JAMCRC, top bit cleared), where `class_name` is the
MT resource class (`rTexture`, `rModel`, …). All 45 hashes in the game resolve this way against class
names found in the executable – see `docs/formats/inventory.md`.

## Open questions
- Meaning of the 3 flag bits (likely load priority/quality, as on other MT titles).
- Whether any entries are stored uncompressed (none seen so far; the script falls back if zlib fails).
