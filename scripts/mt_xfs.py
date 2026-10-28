"""MT Framework XFS (reflection-serialised objects) reader. Format: docs/formats/xfs.md.

An XFS file carries its own schema: one definition per serialised class (DTI id + the property list that the
class's createProperty() registers: name, type, attribute, size), followed by the root object. So any XFS file
decodes without per-type code.

Usage:
  python -I scripts/mt_xfs.py scan [repo]            parse every XFS file (loose + inside .arc), report coverage
  python -I scripts/mt_xfs.py dump <file> [entry]     print a file (or an .arc entry, by name) as JSON
"""
import json
import os
import struct
import sys
import zlib
from collections import Counter, defaultdict

XFS_MAGIC = 0x00534658  # "XFS\0" written as a big-endian u32
ARC_MAGIC = 0x00435241
BASE = "extracted/0005000010118300_v0"
UPDATE = "extracted/0005000e10118300_v32"

# Property type codes (the MtProperty type, +4 bits 16..23). 5 and 6 are confirmed from createProperty helpers
# (0x020711C0 writes 5 for a u16 field, 0x0207126C writes 6 for a u32 field); the rest follow from the value
# sizes stored in the files and are checked by `scan` (every object must consume exactly its size field).
TYPE_NAMES = {
    0x01: "class", 0x02: "classref", 0x03: "bool", 0x04: "u8", 0x05: "u16", 0x06: "u32", 0x07: "u64",
    0x08: "s8", 0x09: "s16", 0x0A: "s32", 0x0B: "s64", 0x0C: "f32", 0x0D: "f64", 0x0E: "string",
    0x0F: "color", 0x10: "point", 0x11: "size", 0x12: "rect", 0x13: "matrix", 0x14: "vector3",
    0x15: "vector4", 0x16: "quaternion", 0x20: "cstring", 0x40: "vector2?", 0x80: "resource",
}
OBJECT_TYPES = (0x01, 0x02)
STRING_TYPES = (0x0E, 0x20)
SCALAR_FMT = {0x03: ">?", 0x04: ">B", 0x05: ">H", 0x06: ">I", 0x07: ">Q", 0x08: ">b", 0x09: ">h", 0x0A: ">i",
              0x0B: ">q", 0x0C: ">f", 0x0D: ">d"}
FLOAT_VECTORS = (0x14, 0x15, 0x16, 0x13)


CLASS_NAMES = {}


class XfsError(Exception):
    pass


class Prop:
    __slots__ = ("name", "type", "attr", "size")

    def __init__(self, name, type_, attr, size):
        self.name, self.type, self.attr, self.size = name, type_, attr, size

    @property
    def nbytes(self):
        return self.size >> 1  # size field = value bytes * 2 (bool 2, u16 4, u32 8, vector3 0x20)


class Xfs:
    def __init__(self, data):
        self.data = data
        magic, self.major, self.minor, self.class_count, self.unk0c, ndefs, defsize = struct.unpack_from(">IHHIIII", data, 0)
        if magic != XFS_MAGIC:
            raise XfsError("bad magic %08X" % magic)
        base = 0x18
        offs = struct.unpack_from(">%dI" % ndefs, data, base)
        self.classes = []
        for off in offs:
            p = base + off
            dti, cnt2, unk = struct.unpack_from(">IHH", data, p)
            props = []
            for i in range(cnt2 >> 1):
                q = p + 8 + i * 0x28
                name_off, t, attr, size = struct.unpack_from(">IBBH", data, q)
                end = data.index(b"\0", base + name_off)
                props.append(Prop(data[base + name_off:end].decode("ascii", "replace"), t, attr, size))
            self.classes.append((dti, props))
        self.data_start = base + defsize
        self.pos = self.data_start
        self.types_seen = Counter()
        self.kinds = Counter()
        self.root = self.read_object()
        if self.pos != len(data):
            raise XfsError("trailing %d bytes after root object" % (len(data) - self.pos))

    def u(self, fmt):
        v = struct.unpack_from(fmt, self.data, self.pos)
        self.pos += struct.calcsize(fmt)
        return v[0] if len(v) == 1 else v

    def cstr(self):
        end = self.data.find(b"\0", self.pos)
        if end < 0:
            raise XfsError("unterminated string at 0x%X" % self.pos)
        s = self.data[self.pos:end].decode("utf-8", "replace")
        self.pos = end + 1
        return s

    def read_object(self):
        cls, ident = self.u(">HH")
        if not cls & 0x8000:
            if cls == 0x7FFE or cls == 0xFFFE or (cls, ident) == (0, 0):
                return None  # null reference
            raise XfsError("bad object header %04X %04X at 0x%X" % (cls, ident, self.pos - 4))
        start = self.pos
        size = self.u(">I")  # counts itself: object bytes from the size field to the end
        idx = cls & 0x7FFF
        if idx >= len(self.classes):
            raise XfsError("class index %d out of range at 0x%X" % (idx, start - 8))
        dti, props = self.classes[idx]
        out = {"__class": CLASS_NAMES.get(dti, "0x%08X" % dti), "__id": ident}
        for p in props:
            n = self.u(">I")
            vals = [self.read_value(p) for _ in range(n)]
            out[p.name] = vals[0] if n == 1 and not p.attr & 0x20 else vals
        if self.pos - start != size:
            raise XfsError("class 0x%08X: read %d bytes, size field %d (at 0x%X)" % (dti, self.pos - start, size, start - 8))
        return out

    def read_value(self, p):
        self.types_seen[(p.type, p.size)] += 1
        if p.type in OBJECT_TYPES:
            return self.read_object()
        if p.type in STRING_TYPES:
            return self.cstr()
        if p.type == 0x80:  # resource reference: u8 kind; kind 2 = class name + path
            kind = self.u(">B")
            self.kinds[kind] += 1
            if kind == 2:
                return {"type": self.cstr(), "path": self.cstr()}
            if kind in (0, 1):
                return None
            raise XfsError("resource ref kind %d at 0x%X" % (kind, self.pos - 1))
        if p.type in SCALAR_FMT and struct.calcsize(SCALAR_FMT[p.type]) == p.nbytes:
            return self.u(SCALAR_FMT[p.type])
        if p.type in FLOAT_VECTORS and p.nbytes % 4 == 0:
            return list(self.u(">%df" % (p.nbytes // 4)))
        raw = self.data[self.pos:self.pos + p.nbytes]
        self.pos += p.nbytes
        return raw.hex()


def dti_names(root="."):
    """DTI id -> class name from config/dti.csv (ids are ~crc32(name) & 0x7FFFFFFF, see docs/engine.md)."""
    path = os.path.join(root, "config/dti.csv")
    names = {}
    if os.path.exists(path):
        import csv
        with open(path, encoding="utf-8") as f:
            for r in csv.DictReader(f):
                names.setdefault(int(r["id"], 16), r["class"])
    return names


def iter_arc(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, _, count = struct.unpack_from(">IHH", data, 0)
    if magic != ARC_MAGIC:
        return
    for i in range(count):
        raw = data[12 + i * 0x50:12 + (i + 1) * 0x50]
        name = raw[:64].split(b"\0", 1)[0].decode("ascii", "replace")
        th, comp, sf, off = struct.unpack(">IIII", raw[64:80])
        blob = data[off:off + comp]
        try:
            blob = zlib.decompress(blob)
        except zlib.error:
            pass
        yield name, th, blob


def effective_files(root):
    files = {}
    for title in (BASE, UPDATE):
        content = os.path.join(root, title, "content")
        for dirpath, _, names in os.walk(content):
            for n in names:
                full = os.path.join(dirpath, n)
                files[os.path.relpath(full, content).replace("\\", "/")] = full
    return files


def iter_xfs(root):
    for rel, full in sorted(effective_files(root).items()):
        if rel.endswith(".arc"):
            for name, th, blob in iter_arc(full):
                if blob[:4] == b"\0SFX":
                    yield "%s:%s" % (rel, name), th, blob
        else:
            with open(full, "rb") as f:
                head = f.read(4)
            if head == b"\0SFX":
                with open(full, "rb") as f:
                    yield rel, None, f.read()


def scan(root):
    ok, bad = 0, []
    types = Counter()
    by_type = defaultdict(lambda: [0, 0])
    versions = Counter()
    classes = set()
    for where, th, blob in iter_xfs(root):
        key = "0x%08X" % th if th is not None else os.path.splitext(where)[1]
        try:
            x = Xfs(blob)
            ok += 1
            by_type[key][0] += 1
            types.update(x.types_seen)
            classes.update(d for d, _ in x.classes)
            versions[(x.major, x.minor)] += 1
        except (XfsError, struct.error, ValueError, IndexError) as e:
            bad.append((where, str(e)))
            by_type[key][1] += 1
    print("parsed %d, failed %d; versions %s" % (ok, len(bad), dict(versions)))
    print("classes: %d distinct, %d named via dti.csv" % (len(classes), sum(1 for c in classes if c in CLASS_NAMES)))
    for k, (g, b) in sorted(by_type.items()):
        print("  %-12s ok=%d failed=%d" % (k, g, b))
    print("property (type, size) usage:")
    for (t, s), n in sorted(types.items()):
        print("  type 0x%02X %-10s size 0x%04X (%d bytes): %d" % (t, TYPE_NAMES.get(t, "?"), s, s >> 1, n))
    for where, e in bad[:15]:
        print("  FAIL", where, e)
    return 0 if not bad else 1


def main():
    CLASS_NAMES.update(dti_names())
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    if sys.argv[1] == "scan":
        return scan(sys.argv[2] if len(sys.argv) > 2 else ".")
    if sys.argv[1] == "dump":
        path = sys.argv[2]
        if len(sys.argv) > 3:
            blob = next(b for n, _, b in iter_arc(path) if n == sys.argv[3])
        else:
            blob = open(path, "rb").read()
        x = Xfs(blob)
        schema = [{"dti": "0x%08X" % d, "class": CLASS_NAMES.get(d, "?"), "props": [[p.name, TYPE_NAMES.get(p.type, "0x%02X" % p.type), "0x%02X" % p.attr, p.nbytes] for p in ps]}
                  for d, ps in x.classes]
        print(json.dumps({"version": [x.major, x.minor], "classes": schema, "root": x.root}, indent=1))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
