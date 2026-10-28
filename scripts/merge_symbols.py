"""Merge generated class metadata (config/dti.csv, config/vtables.csv, config/virtuals.csv) into
config/symbols.txt.

Existing hand-written lines win: a generated symbol is only added if its address is not already named.
Generated lines end with a `gen:<source>` tag so they can be regenerated: running this again first drops
every tagged line and re-adds them from the CSVs. Remove the tag to "adopt" a line as hand-written.

Usage: python -I scripts/merge_symbols.py <repo root>
"""
import csv
import os
import re
import sys
from collections import Counter

# Engine functions found while recovering the DTI system (see docs/research-log.md).
FIXED = [
    (0x02D63178, "MtDTI::MtDTI", "func", "ctor(this, name, parent, size, id, r8, r9)"),
    (0x02D63150, "MtDTI::calcId?", "func", "crc32(name, ~0) & 0x7FFFFFFF"),
    (0x02DBF680, "MtCRC::crc32?", "func", "called by calcId with init -1"),
    (0x103D5B70, "MtDTI::root?", "data", "default parent when none is passed"),
    (0x100C9644, "MtDTI::__vtbl", "vtable", "set by ctor, overridden by each MyDTI"),
    (0x03122C18, "__pure_virtual_called?", "func", "li r3,6; b abort path - fills 689 vtable slots"),
    # MT Framework core (docs/engine.md "Core"): boot, main loop, allocators, threads.
    (0x02E84370, "__rpx_start?", "func", "ELF e_entry; GHS crt: init, ctor table, calls main, exit"),
    (0x02000020, "main", "func", "sApp on the stack; createMain(sLtMH3GMain DTI); run; ~sApp"),
    (0x02B99E78, "sApp::sApp", "func", "stores this into sApp instance pointer 0x103D2E60"),
    (0x02B9AF24, "sApp::createMain?", "func", "dti->newInstance() (MyDTI slot 1) -> this+0x2438"),
    (0x02B9AF98, "sApp::run?", "func", "main loop: while(!main->+0x74) main->vf6(); vf18 on request"),
    (0x02003954, "sLtMH3GMain::update?", "func", "per-frame: sMain::vf6 then vf6/vf9/vf10/vf12 of every system"),
    (0x0294B8DC, "sMain::framePacing?", "func", "OSGetSystemInfo ticks + OSSleepTicks"),
    (0x02D63E50, "MtDTI::getAllocator?", "func", "MtMemoryAllocator table[(dti->w14 >> 3) & 0x3F]"),
    (0x103D6100, "MtMemoryAllocator::mpAllocator?", "data", "allocator pointer table, indexed by DTI bits 3..8"),
    (0x02D7C9DC, "MtThread::createOSThread?", "func", "OSCreateThread wrapper, priority clamped to 31"),
    # Render submission (docs/engine.md "Render submission").
    (0x02BBED98, "sLtRender::buildDisplayList?", "func", "draw records -> GX2 display list; only GX2DrawEx/SetShader caller"),
    (0x02BBC8A4, "sLtRender::beginDisplayList?", "func", "GX2BeginDisplayListEx + GX2SetContextState"),
    (0x02BBED28, "sLtRender::endDisplayList?", "func", "GX2EndDisplayList"),
    (0x02BBE358, "sLtRender::setShaders?", "func", "only GX2SetVertexShader/GX2SetPixelShader caller"),
    (0x02BBF9D0, "sLtRender::present?", "func", "GX2DirectCallDisplayList, copy to TV+DRC scan buffers, swap, flush, vsync"),
    (0x02BC014C, "sLtRender::endFrame?", "func", "called by sLtRender::vf10/vf6; leads to present"),
    (0x02BC2278, "sLtRender::ExecuteThread::signalAndWait?", "func", "OSSignalEvent(done); OSWaitEvent(start)"),
    (0x02BC1E34, "sApp::presentStandalone?", "func", "second GX2 present path, called from sApp code"),
    # File loading (docs/engine.md "File loading").
    (0x02BD75E0, "sResource::loaderThread?", "func", "OS thread entry: dequeue, open MtFile/MtFileStream, cResource::load"),
    (0x02D7D0B4, "MtThread::createOSThreadEx?", "func", "used by sResource ctor: stack 0x40000, prio 15, affinity"),
    (0x02BD73A0, "sResource::buildPath?", "func", "sprintf: root BACKSLASH name . getExtension() (format at 0x100B07AC)"),
    (0x02BD7554, "sResource::loadFromStream?", "func", "res->vf10() then res->load(stream)"),
    (0x02D97FC4, "MtFile::MtFile", "func", "(path, mode); writes MtFile vtable"),
    (0x03125658, "sprintf?", "func", "called with printf-style formats, e.g. 0x100B07AC"),
    (0x102CBD9C, "gMutex?", "data", "OSMutex used by sResource tables, sLtRender and others (556 refs)"),
]

# Names for virtual slots, applied to the introducer and every override (key: introducing class, slot).
# Evidence in docs/engine.md "Core".
SLOT_NAMES = {
    ("MtObject", 3): "createProperty",  # (this, MtPropertyList*): base call, then one addProperty per field
    ("MtObject", 4): "getDTI",       # returns the class's MtDTI (MtThread::vf4, sMain::vf4)
    ("MtAllocator", 6): "alloc",     # (size, align): MEMAllocFromExpHeapEx in MtCafeHeapAllocator
    ("MtAllocator", 12): "free",     # MEMFreeToExpHeap
    ("MtAllocator", 13): "getSize",  # MEMGetSizeForMBlockExpHeap
    ("cResource", 6): "getExtension",  # rArchive -> "arc", rTexture -> "tex"; used to build the file path
    ("cResource", 9): "load",          # load(MtStream*), called by the loader thread and sync create
    ("sResource", 11): "requestAsync?",  # lookup under mutex, else create + queue for the loader thread
    ("sResource", 12): "create?",        # lookup, else dti->newInstance() and load synchronously
    ("sResource", 13): "find?",          # lookup only
}


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    sym_path = os.path.join(root, "config/symbols.txt")
    lines = open(sym_path, encoding="utf-8").read().splitlines()
    header = [l for l in lines if l.startswith("#")]
    kept = [l for l in lines if l.strip() and not l.startswith("#") and not re.search(r"\bgen:\w+$", l)]
    taken = {int(l.split()[0], 16) for l in kept}

    gen = []

    def add(addr, name, kind, note, src):
        if addr in taken:
            return
        taken.add(addr)
        name = name.replace(" ", "")  # symbols.txt is space-separated; "Foo<int, 128U>" -> "Foo<int,128U>"
        gen.append("0x%08X %s %s # %s gen:%s" % (addr, name, kind, note, src) if note else
                   "0x%08X %s %s # gen:%s" % (addr, name, kind, src))

    for addr, name, kind, note in FIXED:
        add(addr, name, kind, note, "dti")

    with open(os.path.join(root, "config/dti.csv"), encoding="utf-8") as f:
        dti = list(csv.DictReader(f))
    names = Counter(r["class"] for r in dti)
    for r in dti:
        cls = r["class"] if names[r["class"]] == 1 else "%s@%s" % (r["class"], r["dti"][2:])
        add(int(r["dti"], 16), cls + "::DTI", "data",
            "MtDTI parent=%s size=%s id=%s" % (r["parent"], r["size"], r["id"]), "dti")
        if r["mydti_vtable"] not in ("", "?"):
            add(int(r["mydti_vtable"], 16), cls + "::MyDTI::__vtbl", "vtable", "", "dti")
        if r["new_instance"] not in ("", "?"):
            add(int(r["new_instance"], 16), cls + "::MyDTI::newInstance", "func", "", "dti")

    with open(os.path.join(root, "config/vtables.csv"), encoding="utf-8") as f:
        vts = list(csv.DictReader(f))
    seen = Counter()
    for r in sorted(vts, key=lambda r: r["vtable"]):
        seen[r["class"]] += 1
        suffix = "" if seen[r["class"]] == 1 else "#%d" % seen[r["class"]]
        add(int(r["vtable"], 16), "%s::__vtbl%s" % (r["class"], suffix), "vtable", "%s slots" % r["slots"], "rtti")
        add(int(r["typeinfo"], 16), "%s::__typeinfo" % r["class"], "data", "", "rtti")

    # Virtual functions (ghidra/scripts/NameVirtuals.java): override notes point at the introducing class.
    vf_path = os.path.join(root, "config/virtuals.csv")
    if os.path.exists(vf_path):
        with open(vf_path, encoding="utf-8") as f:
            for r in csv.DictReader(f):
                if r["name"].startswith("vfshared_"):
                    note = "shared by %s vtable slots" % r["owners"]
                elif r["kind"] == "override":
                    note = "overrides %s slot %s" % (r["introduced_by"], r["slot"])
                else:
                    note = "introduces slot %s" % r["slot"]
                name = r["name"]
                slot_name = SLOT_NAMES.get((r["introduced_by"] or r["class"], int(r["slot"])))
                if slot_name and not name.startswith("vfshared_"):
                    name = "%s::%s" % (r["class"], slot_name)
                add(int(r["fn"], 16), name, "func", note, "vf")

    # Secondary (multiple-inheritance) vtables (ghidra/scripts/FindSecondaryVtables.java). Their slots point at
    # GHS this-adjust thunks (`subi r3,r3,offset; b target`), named __thunk_<offset>_<target leaf>; a target that
    # is still unnamed is named after the base slot it implements (Class::BaseLeaf_<slot name>).
    sv_path = os.path.join(root, "config/secondary_vtables.csv")
    if os.path.exists(sv_path) and os.path.exists(vf_path):
        base_slot = {}  # (primary vtable, slot) -> leaf name
        with open(vf_path, encoding="utf-8") as f:
            for r in csv.DictReader(f):
                for vt in r["vtable"].split("|"):
                    base_slot.setdefault((vt, int(r["slot"])), r["name"].split("::")[-1])
        prim = {}
        for r in vts:
            prim.setdefault(r["class"], r["vtable"])
        names = {int(l.split()[0], 16): l.split()[1] for l in kept + gen}
        with open(sv_path, encoding="utf-8") as f:
            rows = list(csv.DictReader(f))
        for r in rows:
            add(int(r["vtable"], 16), "%s::__vtbl@%s" % (r["class"], r["base"]), "vtable",
                "secondary: %s subobject at +%s, %s slots" % (r["base"], r["offset"], r["slots"]), "rtti2")
            bvt = prim.get(r["base"])
            leaf = r["base"].split("::")[-1].split("<")[0]
            for i, (fn, tgt) in enumerate(zip(r["slot_fns"].split(), r["targets"].split())):
                fn, tgt = int(fn, 16), int(tgt, 16)
                slot_name = base_slot.get((bvt, i), "vf%d" % i).replace("~", "dtor_")
                note = "%s slot %d via secondary vtable" % (r["base"], i)
                if tgt not in names:
                    add(tgt, "%s::%s_%s" % (r["class"], leaf, slot_name), "func", note, "vf2")
                    names[tgt] = "%s::%s_%s" % (r["class"], leaf, slot_name)
                if tgt != fn:
                    target_leaf = names[tgt].split("::")[-1]
                    add(fn, "%s::__thunk_%s_%s" % (r["class"], r["offset"], target_leaf), "func",
                        "this -= %s; b 0x%08X (%s)" % (r["offset"], tgt, note), "vf2")

    # Singleton instance pointers (config/singletons.csv, ghidra/scripts/FindSingletons.java).
    sg_path = os.path.join(root, "config/singletons.csv")
    if os.path.exists(sg_path):
        with open(sg_path, encoding="utf-8") as f:
            for r in csv.DictReader(f):
                if not r["instance"]:
                    continue
                q = "?" if r["uncertain"] else ""
                note = "ctor stores this; %s reads" % r["refs"]
                if r["main_slot"]:
                    note += "; sLtMH3GMain+%s" % r["main_slot"]
                add(int(r["instance"], 16), "%s::mpInstance%s" % (r["class"], q), "data", note, "core")
                add(int(r["ctor"], 16), "%s::%s%s" % (r["class"], r["class"], q), "func", "", "core")

    body = sorted(kept + gen, key=lambda l: int(l.split()[0], 16))
    with open(sym_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(header + body) + "\n")
    print("kept %d hand-written, generated %d, total %d" % (len(kept), len(gen), len(body)))


if __name__ == "__main__":
    main()
