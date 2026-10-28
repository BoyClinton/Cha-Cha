"""Inventory of the game's content/ tree (base title overlaid with the update).

Writes build/asset_inventory.json (full data), docs/formats/inventory.md (summary tables) and
config/resource_types.csv (ARC type hash -> resource class).
Only names, counts, sizes and magics are recorded - no asset data.

ARC (MT Framework archive, big-endian on Wii U) layout as observed:
  0x00 u32 magic 0x00435241 ("ARC\\0" when read little-endian)
  0x04 u16 version (0x10)    0x06 u16 entry count    0x08 u32 padding
  0x0C entries[count], 0x50 bytes each:
       char path[64]; u32 type_hash; u32 comp_size; u32 size_flags; u32 offset
  size_flags is checked here: decompressed size = size_flags >> 3, flags = size_flags & 7 (hypothesis,
  verified by decompressing samples and comparing lengths).

Type hashes are named by hashing every class-like string found in the executable.

Usage: python -I scripts/asset_inventory.py <repo root>
"""
import json
import os
import re
import struct
import sys
import zlib
from collections import Counter, defaultdict

BASE = "extracted/0005000010118300_v0"
UPDATE = "extracted/0005000e10118300_v32"
ELF = UPDATE + "/code/MH3G_Cafe_US.elf"
ARC_MAGIC = 0x00435241
ENTRY_SIZE = 0x50


def effective_files(root):
    """content-relative path -> (absolute path, source) with update files overriding base."""
    files = {}
    for src, title in (("base", BASE), ("update", UPDATE)):
        content = os.path.join(root, title, "content")
        for dirpath, _, names in os.walk(content):
            for n in names:
                full = os.path.join(dirpath, n)
                rel = os.path.relpath(full, content).replace("\\", "/")
                files[rel] = (full, src)
    return files


def magic_str(b):
    """Show a 4-byte magic both as stored and byte-reversed (MT writes LE magics as BE u32)."""
    if len(b) < 4:
        return b.hex()
    rev = b[:4][::-1]
    printable = lambda x: all(32 <= c < 127 or c == 0 for c in x)
    if printable(rev) and rev.rstrip(b"\0"):
        return rev.rstrip(b"\0").decode("ascii")
    if printable(b[:4]) and b[:4].rstrip(b"\0"):
        return b[:4].rstrip(b"\0").decode("ascii")
    return "0x" + b[:4].hex()


def class_hashes(elf_path):
    """Hash every class-like identifier in the executable with the common MT hash variants."""
    data = open(elf_path, "rb").read()
    names = set(m.group().decode() for m in re.finditer(rb"(?<![\x21-\x7e])[a-z][A-Z][A-Za-z0-9_]{2,60}(?=\x00)", data))
    table = {}
    for n in names:
        c = zlib.crc32(n.encode()) & 0xFFFFFFFF
        for variant, h in (("jamcrc&7fffffff", (~c) & 0x7FFFFFFF), ("crc32&7fffffff", c & 0x7FFFFFFF),
                           ("jamcrc", (~c) & 0xFFFFFFFF), ("crc32", c)):
            table.setdefault(h, (n, variant))
    return table, len(names)


def read_arc(path, stats, samples):
    with open(path, "rb") as f:
        head = f.read(12)
        magic, version, count = struct.unpack(">IHH", head[:8])
        if magic != ARC_MAGIC:
            return {"error": "bad magic " + head[:4].hex()}
        table = f.read(count * ENTRY_SIZE)
        entries = []
        for i in range(count):
            raw = table[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE]
            name = raw[:64].split(b"\0", 1)[0].decode("ascii", "replace")
            th, comp, sf, off = struct.unpack(">IIII", raw[64:80])
            entries.append((name, th, comp, sf, off))
        for name, th, comp, sf, off in entries:
            s = stats[th]
            s["count"] += 1
            s["comp"] += comp
            s["size"] += sf >> 3
            s["flags"][sf & 7] += 1
            if "sample" not in s:
                s["sample"] = name
            # Decompress the first entry of every type and the first entry of every archive.
            if th not in samples or name == entries[0][0]:
                f.seek(off)
                blob = f.read(comp)
                try:
                    out = zlib.decompress(blob)
                    ok = len(out) == (sf >> 3)
                    samples.setdefault(th, magic_str(out[:4]))
                except zlib.error:
                    out, ok = blob, comp == (sf >> 3)  # stored uncompressed?
                    samples.setdefault(th, magic_str(blob[:4]) + " (raw)")
                s["checked"] += 1
                s["size_ok"] += ok
        return {"version": version, "count": count}


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    files = effective_files(root)

    by_ext = defaultdict(lambda: {"count": 0, "bytes": 0, "magics": Counter(), "dirs": Counter()})
    for rel, (full, src) in files.items():
        ext = rel.rsplit(".", 1)[-1].lower() if "." in os.path.basename(rel) else "(none)"
        e = by_ext[ext]
        e["count"] += 1
        e["bytes"] += os.path.getsize(full)
        with open(full, "rb") as f:
            e["magics"][magic_str(f.read(4))] += 1
        e["dirs"]["/".join(os.path.dirname(rel).split("/")[:3]) or "."] += 1

    stats = defaultdict(lambda: {"count": 0, "comp": 0, "size": 0, "flags": Counter(), "checked": 0, "size_ok": 0})
    samples = {}
    arc_versions = Counter()
    arc_errors = []
    for rel, (full, src) in sorted(files.items()):
        if rel.endswith(".arc"):
            r = read_arc(full, stats, samples)
            if "error" in r:
                arc_errors.append((rel, r["error"]))
            else:
                arc_versions[r["version"]] += 1

    hashes, n_names = class_hashes(os.path.join(root, ELF))
    types = []
    for th, s in sorted(stats.items(), key=lambda kv: -kv[1]["count"]):
        name, variant = hashes.get(th, (None, None))
        types.append({"hash": "0x%08X" % th, "class": name, "hash_variant": variant, "magic": samples.get(th),
                      "count": s["count"], "comp_bytes": s["comp"], "bytes": s["size"],
                      "flags": dict(s["flags"]), "size_checked": s["checked"], "size_ok": s["size_ok"],
                      "sample": s["sample"]})

    result = {
        "files": len(files),
        "from_update": sum(1 for _, src in files.values() if src == "update"),
        "by_ext": {k: {"count": v["count"], "bytes": v["bytes"], "magics": dict(v["magics"]),
                       "dirs": dict(v["dirs"].most_common(5))} for k, v in by_ext.items()},
        "arc_versions": dict(arc_versions),
        "arc_errors": arc_errors,
        "class_names_hashed": n_names,
        "arc_types": types,
    }
    os.makedirs(os.path.join(root, "build"), exist_ok=True)
    with open(os.path.join(root, "build/asset_inventory.json"), "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1)
    write_markdown(root, result)
    with open(os.path.join(root, "config/resource_types.csv"), "w", encoding="utf-8", newline="\n") as f:
        f.write("type_hash,class,magic,arc_entries\n")
        for t in sorted(types, key=lambda t: t["hash"]):
            f.write("%s,%s,%s,%d\n" % (t["hash"], t["class"] or "?", t["magic"], t["count"]))

    checked = sum(t["size_checked"] for t in types)
    ok = sum(t["size_ok"] for t in types)
    print("files=%d (update %d) arcs=%s errors=%d entries=%d types=%d named=%d size_check=%d/%d" % (
        result["files"], result["from_update"], dict(arc_versions), len(arc_errors),
        sum(t["count"] for t in types), len(types), sum(1 for t in types if t["class"]), ok, checked))
    print("hash variants used:", Counter(t["hash_variant"] for t in types if t["class"]))


def mb(n):
    return "%.1f" % (n / 1048576)


def write_markdown(root, r):
    L = ["# Content inventory", "",
         "Generated by `scripts/asset_inventory.py` from base v0 overlaid with update v32. Do not edit by hand.", "",
         "Effective files: **%d** (%d from the update). ARC versions: %s. Unreadable ARCs: %d." % (
             r["files"], r["from_update"], r["arc_versions"], len(r["arc_errors"])), "",
         "## Loose files by extension", "",
         "| Ext | Files | MB | Magic(s) | Main folders |", "|---|---:|---:|---|---|"]
    for ext, e in sorted(r["by_ext"].items(), key=lambda kv: -kv[1]["bytes"]):
        L.append("| `.%s` | %d | %s | %s | %s |" % (ext, e["count"], mb(e["bytes"]),
                 ", ".join("`%s`×%d" % kv for kv in e["magics"].items()),
                 ", ".join("`%s`" % d for d in e["dirs"])))
    L += ["", "## Resource types inside ARCs", "",
          "Class names come from hashing identifiers found in the executable (variant column says which hash). "
          "Magic = first 4 bytes of a decompressed sample. Size check = entries whose decompressed length "
          "matched `size_flags >> 3`.", "",
          "| Type hash | Class | Magic | Entries | Uncompressed MB | Size check | Sample path |",
          "|---|---|---|---:|---:|---|---|"]
    for t in r["arc_types"]:
        L.append("| `%s` | %s | `%s` | %d | %s | %d/%d | `%s` |" % (
            t["hash"], "`%s`" % t["class"] if t["class"] else "?", t["magic"], t["count"], mb(t["bytes"]),
            t["size_ok"], t["size_checked"], t["sample"]))
    with open(os.path.join(root, "docs/formats/inventory.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L) + "\n")


if __name__ == "__main__":
    main()
