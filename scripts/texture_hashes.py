"""Lists every texture in the game's archives with its level-0 content hash as the renderer computes it
(runtime/gfx/renderer.cpp hash_words over the GX2-tiled level 0, the trace's "h0"), so a texture seen in a frame trace
can be named and its kind found on other maps (e.g. the water textures for reflections on water only).

Usage: python -I scripts/texture_hashes.py [hash ...] [--size WxH] [--grep word ...]  (--grep last)
  no arguments: prints "hash  w x h  fmt  arc  entry" for every texture
  hashes: prints only those; --grep: only entries whose path contains one of the words
Reads extracted/ (update over base); writes nothing.
"""
import os
import struct
import sys
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "extracted")
BASE, UPDATE = "0005000010118300_v0", "0005000e10118300_v32"
ARC_MAGIC, TEX_MAGIC, TEXTURE = 0x00435241, 0x00584554, 0x241F5DEB
ENTRY = 0x50
M64 = (1 << 64) - 1


def hash_words(p):
    n = len(p)
    h = [0x9E3779B97F4A7C15 ^ n, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9, 0x27D4EB2F165667C5]
    i = 0
    full = n - n % 32
    for i in range(0, full, 32):
        w = struct.unpack_from("<4Q", p, i)
        for k in range(4):
            x = ((h[k] ^ w[k]) * 0x100000001B3) & M64
            h[k] = x ^ (x >> 29)
    i = full
    r = (h[0] ^ (h[1] * 31) ^ (h[2] * 961) ^ (h[3] * 29791)) & M64
    while i + 8 <= n:
        (w,) = struct.unpack_from("<Q", p, i)
        x = ((r ^ w) * 0x100000001B3) & M64
        r = x ^ (x >> 29)
        i += 8
    while i < n:
        r = ((r ^ p[i]) * 0x100000001B3) & M64
        i += 1
    return r


def align(x, a):
    return (x + a - 1) // a * a


def archives():
    files = {}
    for title in (BASE, UPDATE):
        content = os.path.join(ROOT, title, "content")
        for d, _, names in os.walk(content):
            for n in names:
                if n.endswith(".arc"):
                    full = os.path.join(d, n)
                    files[os.path.relpath(full, content).replace("\\", "/")] = full
    return files


def textures(sizes=None, words=None):
    for rel, full in sorted(archives().items()):
        data = open(full, "rb").read()
        if len(data) < 12 or struct.unpack_from(">I", data, 0)[0] != ARC_MAGIC:
            continue
        count = struct.unpack_from(">H", data, 6)[0]
        for i in range(count):
            e = 12 + i * ENTRY
            name = data[e:e + 64].split(b"\0")[0].decode("ascii", "replace")
            kind, csize, size, off = struct.unpack_from(">IIII", data, e + 64)
            if kind != TEXTURE or off + csize > len(data):
                continue
            blob = data[off:off + csize]
            try:
                tex = zlib.decompress(blob)
            except zlib.error:
                tex = blob
            if len(tex) < 16 or struct.unpack_from(">I", tex, 0)[0] != TEX_MAGIC:
                continue
            b, c = struct.unpack_from(">II", tex, 8)
            w, h, faces, fmt = (b >> 13) & 0x1FFF, b & 0x1FFF, c >> 24, (c >> 16) & 0xFF
            if fmt == 0x03:
                block, bpe = 1, 4
            elif fmt == 0x0B:
                block, bpe = 4, 8
            elif fmt == 0x0C:
                block, bpe = 4, 16
            else:
                continue
            if faces != 1 or not w or not h:
                continue
            if sizes and (w, h) not in sizes:
                continue
            if words and not any(x in (rel + "/" + name).lower() for x in words):
                continue
            ew, eh = (w + block - 1) // block, (h + block - 1) // block
            tiled = not (w < 32 and h < 16)
            pitch, rows = (align(ew, 32), align(eh, 16)) if tiled else (align(ew, 8), align(eh, 8))
            level0 = pitch * rows * bpe
            if 16 + level0 > len(tex):
                continue
            yield hash_words(tex[16:16 + level0]), w, h, fmt, rel, name


def main():
    args = sys.argv[1:]
    words = []
    if "--grep" in args:
        k = args.index("--grep")
        words, args = [w.lower() for w in args[k + 1:]], args[:k]
    sizes = set()
    if "--size" in args:
        k = args.index("--size")
        sizes = {tuple(int(v) for v in args[k + 1].split("x"))}
        args = args[:k] + args[k + 2:]
    want = {int(a, 16) for a in args}
    for h, w, hh, fmt, rel, name in textures(sizes, words):
        if want and h not in want:
            continue
        print(f"{h:016x}  {w}x{hh}  {fmt:02X}  {rel}  {name}")


if __name__ == "__main__":
    main()
