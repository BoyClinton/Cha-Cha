"""Catalogue every GX2 shader shipped with the game (they are all content files; the RPX embeds none).

Sources (base v0 content; the update ships no shaders):
  nativeCafe/es/<id>.es                 rEmulationShader: 16-byte ESX header + GFD with one pixel shader
  nativeCafe/system/ltshader/*.es       null.es / uber.es, same format
  nativeCafe/shaderPack.spkg            rEmulationShaderPackage: PKG header + the .es GFDs bundled
  nativeCafe/LtUserShader/*.lfx         rLtShader: MFX effect file with vertex/geometry shader GFDs

Writes build/shaders.json: one record per GFD (source, offset, id, stage blocks, sha1) plus a summary.
Prints unique-shader counts per stage - the closed set an offline Latte->SPIR-V translator must handle.

Usage: python -I scripts/shader_inventory.py <repo root>
"""
import hashlib
import json
import os
import re
import struct
import sys
from collections import Counter

NATIVE = "extracted/0005000010118300_v0/content/nativeCafe"
# GFD block types (wut gfd.h)
STAGE = {3: "vs", 5: "vs", 6: "ps", 7: "ps", 8: "gs", 9: "gs", 10: "gs", 14: "cs", 15: "cs"}


def gfd_at(buf, s):
    """Return (bytes, block types) of the GFD starting at s, up to and including its EOF block."""
    o = s + struct.unpack(">I", buf[s + 4:s + 8])[0]
    types = []
    while buf[o:o + 4] == b"BLK{":
        hs, _, _, typ, size = struct.unpack(">IIIII", buf[o + 4:o + 24])
        types.append(typ)
        o += hs + size
        if typ == 1:
            break
    return buf[s:o], types


def scan(path, rel, ident_of):
    buf = open(path, "rb").read()
    out = []
    for m in re.finditer(rb"Gfx2", buf):
        s = m.start()
        if struct.unpack(">I", buf[s + 4:s + 8])[0] != 0x20:  # not a GFD header, just bytes that spell Gfx2
            continue
        g, types = gfd_at(buf, s)
        stages = sorted(set(STAGE[t] for t in types if t in STAGE))
        out.append({"source": rel, "offset": s, "id": ident_of(buf, s), "stages": stages,
                    "blocks": types, "size": len(g), "sha1": hashlib.sha1(g).hexdigest()})
    return out


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    nc = os.path.join(root, NATIVE)
    records = []
    es_id = lambda buf, s: "%08x" % struct.unpack(">I", buf[8:12])[0]
    for sub in ("es", "system/ltshader"):
        for n in sorted(os.listdir(os.path.join(nc, sub))):
            records += scan(os.path.join(nc, sub, n), sub + "/" + n, es_id)
    # In the pack, each GFD is preceded by its u32 id.
    records += scan(os.path.join(nc, "shaderPack.spkg"), "shaderPack.spkg",
                    lambda buf, s: "%08x" % struct.unpack(">I", buf[s - 4:s])[0])
    lfx = "LtUserShader/LtUserShaderPackage.lfx"
    records += scan(os.path.join(nc, lfx), lfx, lambda buf, s: None)

    unique = {}
    for r in records:
        unique.setdefault(r["sha1"], r)
    by_stage = Counter("+".join(r["stages"]) for r in unique.values())
    by_source = Counter(r["source"].split("/")[0] if r["source"].startswith("es/") else r["source"] for r in records)
    es_hashes = {r["sha1"] for r in records if r["source"].startswith("es/")}
    pack_hashes = {r["sha1"] for r in records if r["source"] == "shaderPack.spkg"}
    summary = {
        "gfd_total": len(records),
        "gfd_by_source": dict(by_source),
        "unique_total": len(unique),
        "unique_by_stage": dict(by_stage),
        "es_not_in_pack": len(es_hashes - pack_hashes),
        "pack_not_in_es": len(pack_hashes - es_hashes),
    }
    os.makedirs(os.path.join(root, "build"), exist_ok=True)
    with open(os.path.join(root, "build/shaders.json"), "w", encoding="utf-8") as f:
        json.dump({"summary": summary, "shaders": records}, f, indent=1)
    print(json.dumps(summary, indent=1))


if __name__ == "__main__":
    main()
