"""MT Framework rTexture (TEX v0xA5, Wii U) header parsing and mip layout.

See docs/formats/tex.md. The layout model is empirical: it reproduces the exact data size of every
texture in the game (`verify` mode). Deswizzling uses addrlib R6xx thin1 addressing with
pipe/bank swizzle 0 and per-slice rotation for cube faces, verified visually and against downscaled
level 0. Covers every texture in the game, cube maps included.
Pure stdlib, so slow (~30 s per 1024x1024 texture) - fine for tools and checks, not for the runtime.

Usage:
  python -I scripts/mt_tex.py verify <repo root>     check the model against every rTexture in the ARCs
  python -I scripts/mt_tex.py export <repo root> <name substring> <out dir> [level]
                                                     deswizzle + decode matching textures to PNG
Exported PNGs are game data: write them outside the repo (or to build/), never commit them.
"""
import os
import struct
import sys
import zlib

TEX_MAGIC = 0x00584554      # "TEX\0" read little-endian
TEX_TYPE_HASH = 0x241F5DEB  # rTexture
HEADER_SIZE = 0x10

# MT format code -> (name, block dim, bytes per element). Confirmed by deswizzling + decoding real
# textures (docs/formats/tex.md). GX2 equivalents: RGBA8 = UNORM_R8_G8_B8_A8 (0x1A), BC1 = UNORM_BC1
# (0x31), BC3 = UNORM_BC3 (0x33); sRGB-ness is not in the header.
FORMATS = {
    0x03: ("RGBA8", 1, 4),
    0x0B: ("BC1", 4, 8),
    0x0C: ("BC3", 4, 16),
}

MACRO_W, MACRO_H = 32, 16  # 2D-tiled alignment, in elements, for all formats seen
MICRO = 8                  # 1D-tiled alignment (8x8 elements)


def parse_header(data):
    magic, a, b, c = struct.unpack(">IIII", data[:HEADER_SIZE])
    if magic != TEX_MAGIC:
        raise ValueError("not a TEX: magic 0x%08X" % magic)
    return {
        "version": a >> 20,          # 0xA5
        "flags": a & 0xFFFFF,        # 0x2 normal, 0x4006 cube; 0x402 / 0x3002 seen rarely
        "mips": b >> 26,
        "width": (b >> 13) & 0x1FFF,
        "height": b & 0x1FFF,
        "faces": c >> 24,            # 1, or 6 for cube maps
        "format": (c >> 16) & 0xFF,
        "unk": c & 0xFFFF,           # always 8
    }


def _align(x, a):
    return (x + a - 1) // a * a


CUBE_SH_SIZE = 0x6C  # cube maps: 27 floats (spherical harmonics?) between header and texel data


def data_offset(hdr):
    """File offset of level 0 texel data."""
    return HEADER_SIZE + (CUBE_SH_SIZE if hdr["faces"] == 6 else 0)


def level_layout(hdr):
    """[(level, width, height, tile, offset, size, slices)]; offset is relative to data_offset(hdr),
    size covers all slices, tile is '2d' or '1d'. Cube maps store faces as slices: 6 at level 0, padded
    to 8 (next power of two) at every later level."""
    _, bd, bpe = FORMATS[hdr["format"]]
    w, h = hdr["width"], hdr["height"]
    tiled2d = not (w < MACRO_W and h < MACRO_H)  # level 0 tests texel dims, both must be small
    out, off = [], 0
    for lvl in range(hdr["mips"]):
        lw, lh = max(w >> lvl, 1), max(h >> lvl, 1)
        ew, eh = -(-lw // bd), -(-lh // bd)
        if tiled2d and lvl > 0 and (ew < MACRO_W or eh < MACRO_H):
            tiled2d = False  # degrade to 1D, never back
        if tiled2d:
            slice_size = _align(ew, MACRO_W) * _align(eh, MACRO_H) * bpe
        else:
            slice_size = _align(ew, MICRO) * _align(eh, MICRO) * bpe
        slices = 1 if hdr["faces"] == 1 else (hdr["faces"] if lvl == 0 else 8)
        out.append((lvl, lw, lh, "2d" if tiled2d else "1d", off, slice_size * slices, slices))
        off += slice_size * slices
    return out


def data_size(hdr):
    """Bytes after the 16-byte header."""
    lay = level_layout(hdr)
    return data_offset(hdr) - HEADER_SIZE + lay[-1][4] + lay[-1][5]


# --- GX2 / R600 addrlib thin1 addressing (Wii U: 2 pipes, 4 banks, 256-byte pipe interleave) ---------
# Follows AMD addrlib's R6xx ComputeSurfaceAddrFromCoord{MicroTiled,MacroTiled} for THIN1 modes only,
# non-depth, single sample. x/y are in elements (4x4 blocks for BCn), bpp in bits.
NUM_PIPES, NUM_BANKS = 2, 4
PIPE_INTERLEAVE_BITS = 8  # 256 bytes
PIPE_BITS, BANK_BITS = 1, 2


def _pixel_index_in_micro_tile(x, y, bpp):
    if bpp == 8:
        bits = (x & 1, (x >> 1) & 1, (x >> 2) & 1, (y >> 1) & 1, y & 1, (y >> 2) & 1)
    elif bpp == 16:
        bits = (x & 1, (x >> 1) & 1, (x >> 2) & 1, y & 1, (y >> 1) & 1, (y >> 2) & 1)
    elif bpp == 64:
        bits = (x & 1, y & 1, (x >> 1) & 1, (x >> 2) & 1, (y >> 1) & 1, (y >> 2) & 1)
    elif bpp == 128:
        bits = (y & 1, x & 1, (x >> 1) & 1, (x >> 2) & 1, (y >> 1) & 1, (y >> 2) & 1)
    else:  # 32 bpp and the rest
        bits = (x & 1, (x >> 1) & 1, y & 1, (x >> 2) & 1, (y >> 1) & 1, (y >> 2) & 1)
    return sum(b << i for i, b in enumerate(bits))


def addr_1d_thin1(x, y, bpp, pitch, height=0, slice=0):
    micro_tile_bytes = 64 * bpp // 8
    micro_offset = micro_tile_bytes * ((x >> 3) + (y >> 3) * (pitch >> 3))
    slice_offset = slice * (pitch * height * bpp // 8)
    return slice_offset + micro_offset + (bpp * _pixel_index_in_micro_tile(x, y, bpp) >> 3)


def addr_2d_thin1(x, y, bpp, pitch, height=0, slice=0, pipe_swizzle=0, bank_swizzle=0):
    elem_offset = bpp * _pixel_index_in_micro_tile(x, y, bpp) >> 3

    pipe = ((y >> 3) ^ (x >> 3)) & 1
    bank = (((y // (16 * NUM_PIPES)) ^ (x >> 3)) & 1) | ((((y // (8 * NUM_PIPES)) ^ (x >> 4)) & 1) << 1)
    bank_pipe = pipe + NUM_PIPES * bank
    rotation = NUM_PIPES * ((NUM_BANKS >> 1) - 1)  # per-slice bank/pipe rotation for 2D thin modes
    bank_pipe ^= pipe_swizzle + NUM_PIPES * bank_swizzle + slice * rotation
    bank_pipe %= NUM_PIPES * NUM_BANKS
    pipe, bank = bank_pipe % NUM_PIPES, bank_pipe // NUM_PIPES

    macro_w, macro_h = 8 * NUM_BANKS, 8 * NUM_PIPES  # 32 x 16 elements
    macro_tile_bytes = bpp * macro_w * macro_h // 8
    macro_offset = ((x // macro_w) + (pitch // macro_w) * (y // macro_h)) * macro_tile_bytes
    slice_offset = slice * (pitch * height * bpp // 8)

    swizzle_bits = PIPE_BITS + BANK_BITS
    total = elem_offset + ((macro_offset + slice_offset) >> swizzle_bits)
    group_mask = (1 << PIPE_INTERLEAVE_BITS) - 1
    return ((bank << (PIPE_BITS + PIPE_INTERLEAVE_BITS)) | (pipe << PIPE_INTERLEAVE_BITS)
            | (total & group_mask) | ((total & ~group_mask) << swizzle_bits))


def deswizzle_level(data, hdr, level, face=0):
    """Linear element data (row-major, unpadded) for one mip level of one face (cube-map slice)."""
    _, bd, bpe = FORMATS[hdr["format"]]
    lvl, lw, lh, tile, off, size, slices = level_layout(hdr)[level]
    ew, eh = -(-lw // bd), -(-lh // bd)
    bpp = bpe * 8
    base = data_offset(hdr) + off
    src = data[base:base + size]
    out = bytearray(ew * eh * bpe)
    if tile == "2d":
        pitch, height = _align(ew, MACRO_W), _align(eh, MACRO_H)
        addr = lambda x, y: addr_2d_thin1(x, y, bpp, pitch, height, face)
    else:
        pitch, height = _align(ew, MICRO), _align(eh, MICRO)
        addr = lambda x, y: addr_1d_thin1(x, y, bpp, pitch, height, face)
    for y in range(eh):
        for x in range(ew):
            a = addr(x, y)
            d = (y * ew + x) * bpe
            out[d:d + bpe] = src[a:a + bpe]
    return bytes(out), lw, lh


# --- Decoders -> RGBA8 -----------------------------------------------------------------------------
def _rgb565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def _bc1_colors(block, punchthrough):
    # BC blocks are little-endian regardless of the console's byte order.
    c0, c1, idx = struct.unpack("<HHI", block)
    p0, p1 = _rgb565(c0), _rgb565(c1)
    if c0 > c1 or not punchthrough:
        pal = [p0 + (255,), p1 + (255,),
               tuple((2 * a + b) // 3 for a, b in zip(p0, p1)) + (255,),
               tuple((a + 2 * b) // 3 for a, b in zip(p0, p1)) + (255,)]
    else:
        pal = [p0 + (255,), p1 + (255,), tuple((a + b) // 2 for a, b in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
    return [pal[(idx >> (2 * i)) & 3] for i in range(16)]


def _bc3_alpha(block):
    a0, a1 = block[0], block[1]
    bits = int.from_bytes(block[2:8], "little")
    if a0 > a1:
        pal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    else:
        pal = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
    return [pal[(bits >> (3 * i)) & 7] for i in range(16)]


def decode_rgba(linear, hdr, w, h):
    """RGBA8 rows for a deswizzled level."""
    name, bd, bpe = FORMATS[hdr["format"]]
    px = bytearray(w * h * 4)
    if bd == 1:  # RGBA8: stored bytes taken as R,G,B,A
        return bytes(linear[:w * h * 4])
    bw = -(-w // 4)
    for i in range(len(linear) // bpe):
        blk = linear[i * bpe:(i + 1) * bpe]
        if bpe == 8:
            texels = _bc1_colors(blk, True)
        else:
            alpha = _bc3_alpha(blk[:8])
            texels = [c[:3] + (a,) for c, a in zip(_bc1_colors(blk[8:], False), alpha)]
        bx, by = (i % bw) * 4, (i // bw) * 4
        for t, c in enumerate(texels):
            x, y = bx + (t & 3), by + (t >> 2)
            if x < w and y < h:
                o = (y * w + x) * 4
                px[o:o + 4] = bytes(c)
    return bytes(px)


def write_png(path, rgba, w, h):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def smoothness(rgba, w, h):
    """Mean absolute difference between horizontally adjacent texels (low = image-like, high = noise)."""
    total = n = 0
    for y in range(0, h, max(1, h // 64)):
        row = rgba[y * w * 4:(y + 1) * w * 4]
        for x in range(0, (w - 1) * 4, 4):
            total += sum(abs(row[x + c] - row[x + 4 + c]) for c in range(3))
            n += 1
    return total / max(n, 1)


def export(data, out_png, level=0, face=0):
    hdr = parse_header(data)
    linear, w, h = deswizzle_level(data, hdr, level, face)
    rgba = decode_rgba(linear, hdr, w, h)
    write_png(out_png, rgba, w, h)
    return hdr, w, h, smoothness(rgba, w, h)


def iter_arc_textures(root):
    files = {}
    for title in ("extracted/0005000010118300_v0", "extracted/0005000e10118300_v32"):
        content = os.path.join(root, title, "content")
        for dirpath, _, names in os.walk(content):
            for n in names:
                full = os.path.join(dirpath, n)
                files[os.path.relpath(full, content)] = full
    for rel, full in sorted(files.items()):
        if not rel.endswith(".arc"):
            continue
        with open(full, "rb") as f:
            count = struct.unpack(">H", f.read(12)[6:8])[0]
            table = f.read(count * 0x50)
            for i in range(count):
                e = table[i * 0x50:(i + 1) * 0x50]
                th, comp, _, off = struct.unpack(">IIII", e[64:80])
                if th == TEX_TYPE_HASH:
                    f.seek(off)
                    yield rel, e[:64].split(b"\0", 1)[0].decode("ascii", "replace"), zlib.decompress(f.read(comp))


def verify(root):
    ok, bad = {1: 0, 6: 0}, {1: 0, 6: 0}
    for arc, name, d in iter_arc_textures(root):
        hdr = parse_header(d)
        kind = hdr["faces"]
        if data_size(hdr) == len(d) - HEADER_SIZE:
            ok[kind] += 1
        else:
            bad[kind] += 1
            if sum(bad.values()) <= 10:
                print("mismatch", arc, name, hdr, "model", data_size(hdr), "actual", len(d) - HEADER_SIZE)
    print("single-face textures: %d match, %d mismatch; cube maps: %d match, %d mismatch"
          % (ok[1], bad[1], ok[6], bad[6]))
    return sum(bad.values()) == 0


def export_matching(root, pattern, out_dir, limit=5, level=0):
    os.makedirs(out_dir, exist_ok=True)
    n = 0
    for arc, name, d in iter_arc_textures(root):
        if pattern.lower() not in name.lower():
            continue
        for face in range(parse_header(d)["faces"]):
            out = os.path.join(out_dir, name.replace("\\", "_") + "_F%d_L%d.png" % (face, level))
            hdr, w, h, s = export(d, out, level, face)
            print("%s  %s %dx%d mips=%d face=%d smoothness=%.1f  -> %s"
                  % (name, FORMATS[hdr["format"]][0], w, h, hdr["mips"], face, s, out))
        n += 1
        if n >= limit:
            break


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "verify":
        sys.exit(0 if verify(sys.argv[2]) else 1)
    if len(sys.argv) >= 5 and sys.argv[1] == "export":
        lvl = int(sys.argv[5]) if len(sys.argv) > 5 else 0
        export_matching(sys.argv[2], sys.argv[3], sys.argv[4], limit=20, level=lvl)
        sys.exit(0)
    print(__doc__)
