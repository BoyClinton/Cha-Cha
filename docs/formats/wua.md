# .wua (ZArchive)

Cemu's archive for Wii U titles (ZArchive by Exzap, format version 1). Our reader: `runtime/fs/zarchive.*` (own code;
zstd decoder from facebook/zstd). Verified against `zarchive.exe`'s extraction of the user's `mh3u.wua`: every file of
the base and update titles under `code/` and `content/` reads back byte for byte (`fs_overlay` test; the full ~6 GB
comparison with `MH3U_TEST_FULL_WUA=1`).

All integers are **big-endian**.

## Footer (last 144 bytes of the file)

| Offset | Size | Field |
|---|---|---|
| 0x00 | 6 × 16 | section table: {u64 offset, u64 size} for compressed data, offset records, names, file tree, meta directory, meta data |
| 0x60 | 32 | integrity hash (SHA-256; not checked) |
| 0x80 | 8 | total file size (must equal the file's size) |
| 0x88 | 4 | version `0x61BF3A01` |
| 0x8C | 4 | magic `0x169F52D6` |

## Data

The archive's files are concatenated into one uncompressed stream, cut into **64 KiB blocks**. Each block is
zstd-compressed on its own, or stored raw when its "compressed" size would be 64 KiB.

**Offset records**, one per 16 blocks (40 bytes): u64 offset of the first block (relative to the compressed-data
section), then 16 × u16 `compressed size - 1`. A block's offset is the record's base plus the sizes of the blocks
before it in the record.

## File tree

16-byte nodes; node 0 is the root directory.

| Offset | Size | File | Directory |
|---|---|---|---|
| 0 | 4 | bit 31 = 1; bits 0-30 name offset | bit 31 = 0; bits 0-30 name offset (`0x7FFFFFFF` = no name) |
| 4 | 4 | offset in the stream, low 32 bits | first child node index |
| 8 | 4 | size, low 32 bits | child count |
| 12 | 2 | size, high 16 bits | reserved |
| 14 | 2 | offset, high 16 bits | reserved |

A directory's children are contiguous nodes. Names are case-insensitive for lookup.

**Name table**: at the name offset, one length byte (bit 7 set: 15-bit length with the next byte as bits 7-14), then
the name bytes (no terminator).

## Layout of a title archive

The root holds one folder per title, named `<titleid>_v<version>` (`0005000010118300_v0`,
`0005000e10118300_v32`), each with `code/ content/ meta/` as on the console. The folder name is only informative: the
port identifies titles by `code/app.xml` ([dumps.md](dumps.md)).

## Performance

The reader keeps the last decompressed block per thread and reads with positional `ReadFile`, so sequential reads
of a file decompress each block once. Debug build, spinning disk: ~16 MB/s with the comparison against the
extracted files in the same loop. The game reads its archives at start-up and on area changes only.
