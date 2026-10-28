// Names virtual functions from the RTTI vtables in config/vtables.csv and resolves overrides.
//
// GHS typeinfo: {vptr, const char *name, uniq*, baseList*, ...}; baseList = {baseTypeinfo*, u32 flags}...
// The first base entry is the primary base, whose vtable layout the class extends. For every class:
//   slot i with the same function as the primary base's slot i  -> inherited (no new name)
//   slot i with a different function, i < base slot count       -> override of the slot introduced above
//   slot i >= base slot count                                   -> new virtual introduced by this class
// A function is named after the class where it first appears non-inherited: Class::vf<i>, where i is the
// slot index after the {0, typeinfo} header (vtable offset 8 + 8*i). Slot 0 becomes Class::~Class when the
// function writes Class's own vtable address (the destructor resets the vptr) - checked, not assumed.
// Functions owned by several unrelated (class, slot) pairs (empty stubs, pure-virtual handlers) are
// reported as shared and named vfshared_<n>_<addr>.
//
// Args: <vtables.csv> <virtuals.csv out>
// @category MH3U
import java.io.*;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;

public class NameVirtuals extends GhidraScript {
    static class Vt {
        long addr, ti;
        String cls;
        long[] fns;
        Vt base;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        List<Vt> vts = new ArrayList<>();
        Map<Long, Vt> byTi = new HashMap<>();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(args[0]), "UTF-8"))) {
            r.readLine();
            for (String line; (line = r.readLine()) != null; ) {
                List<String> f = csv(line);
                Vt v = new Vt();
                v.addr = Long.decode(f.get(0));
                v.cls = f.get(1);
                v.ti = Long.decode(f.get(2));
                int slots = Integer.parseInt(f.get(3));
                v.fns = new long[slots];
                for (int i = 0; i < slots; i++) v.fns[i] = u32(v.addr + 12 + 8L * i);
                vts.add(v);
                Vt prev = byTi.get(v.ti);
                if (prev == null || prev.fns.length < slots) byTi.put(v.ti, v);  // primary = longest
            }
        }
        // GHS emits a typeinfo copy per translation unit, so base-list pointers often hit a copy that no
        // vtable uses. Resolve bases by the typeinfo's name instead; primary vtable per name = longest.
        Map<String, Vt> byName = new HashMap<>();
        for (Vt v : vts) {
            Vt prev = byName.get(v.cls);
            if (prev == null || prev.fns.length < v.fns.length) byName.put(v.cls, v);
        }
        int withBase = 0, roots = 0, unknownBase = 0;
        for (Vt v : vts) {
            long baseList = u32(v.ti + 12);
            if (baseList == 0) { roots++; continue; }
            String baseName = readString(u32(u32(baseList) + 4));
            v.base = baseName == null ? null : byName.get(baseName);
            if (v.base != null && v.base != v) withBase++;
            else { v.base = null; unknownBase++; }
        }

        // introducing class per (class, slot): walk up while the base still has the slot
        Map<Long, List<String[]>> owners = new LinkedHashMap<>();  // fn -> [class, slot, kind, introducedBy]
        int inherited = 0, overrides = 0, fresh = 0;
        for (Vt v : vts) {
            for (int i = 0; i < v.fns.length; i++) {
                long fn = v.fns[i];
                if (v.base != null && i < v.base.fns.length && v.base.fns[i] == fn) { inherited++; continue; }
                String kind;
                Vt intro = v;
                if (v.base != null && i < v.base.fns.length) {
                    kind = "override";
                    overrides++;
                    for (Vt b = v.base; b != null && i < b.fns.length; b = b.base) intro = b;
                } else {
                    kind = "new";
                    fresh++;
                }
                owners.computeIfAbsent(fn, k -> new ArrayList<>()).add(new String[] { v.cls, "" + i, kind, intro.cls, String.format("0x%08X", v.addr) });
            }
        }

        int dtorChecked = 0, dtorOk = 0, shared = 0;
        try (PrintWriter w = new PrintWriter(args[1], "UTF-8")) {
            w.println("fn,name,class,slot,kind,introduced_by,vtable,owners");
            for (Map.Entry<Long, List<String[]>> e : owners.entrySet()) {
                long fn = e.getKey();
                List<String[]> os = e.getValue();
                String[] o = os.get(0);
                String name;
                if (os.size() > 1) {
                    shared++;
                    name = String.format("vfshared_%d_%08X", os.size(), fn);
                } else if (o[1].equals("0")) {
                    dtorChecked++;
                    if (isDeletingDtor(fn)) {
                        dtorOk++;
                        String simple = o[0].replaceAll("<.*>", "");
                        simple = simple.substring(simple.lastIndexOf(':') + 1);
                        name = o[0] + "::~" + simple;
                    } else {
                        name = o[0] + "::vf0";
                        if (dtorChecked - dtorOk <= 3) dumpHead(fn, o[0]);
                    }
                } else {
                    name = o[0] + "::vf" + o[1];
                }
                w.println(String.format("0x%08X,%s,%s,%s,%s,%s,%s,%d", fn, q(name), q(o[0]), o[1], o[2], q(o[3]), o[4], os.size()));
            }
        }
        println(String.format("vtables=%d with_base=%d roots=%d unknown_base=%d slots: inherited=%d override=%d new=%d",
            vts.size(), withBase, roots, unknownBase, inherited, overrides, fresh));
        int noCode = 0;
        for (long fn : owners.keySet()) if (getInstructionAt(toAddr(fn)) == null) noCode++;
        println(String.format("functions=%d shared=%d slot0 checked=%d deleting_dtor_shape=%d; slot-0 targets that "
            + "needed disassembly=%d; virtual targets still without code=%d", owners.size(), shared, dtorChecked,
            dtorOk, undisassembled, noCode));
    }

    private int undisassembled = 0;

    // Many virtuals are reachable only through vtables, so auto-analysis never disassembled them.
    private Instruction code(long fn) {
        Instruction ins = getInstructionAt(toAddr(fn));
        if (ins == null) {
            undisassembled++;
            disassemble(toAddr(fn));
            ins = getInstructionAt(toAddr(fn));
        }
        return ins;
    }

    // GHS deleting destructor: void X::~X(X *this, int flags) { ...; if (flags & 1) delete this; }
    // Shape: flags (r4, or a copy made with "or rN,r4,r4") tested with rlwinm. rA,rN,0,31,31 before the
    // function's final blr. Trivial dtors test r4 directly; big ones (inlined member dtors) test it late.
    // Walks instructions from the entry (Ghidra may not have a function there).
    private boolean isDeletingDtor(long fn) {
        Instruction ins = code(fn);
        Set<String> flagsRegs = new HashSet<>(Collections.singleton("r4"));
        for (int i = 0; i < 2000 && ins != null; i++, ins = ins.getNext()) {
            String s = ins.toString();
            if (i < 40 && s.matches("or r\\d+,r4,r4")) flagsRegs.add(s.substring(3, s.indexOf(',')));
            if (s.startsWith("rlwinm. ")) {
                String[] ops = s.substring(8).split(",");
                if (ops.length == 5 && flagsRegs.contains(ops[1]) && s.endsWith(",0x0,0x1f,0x1f")) return true;
            }
            if (ins.getMnemonicString().equals("blr")) break;
        }
        return false;
    }

    private void dumpHead(long fn, String cls) {
        println(String.format("slot0 not matched: %s @ %08X", cls, fn));
        Instruction ins = code(fn);
        for (int i = 0; i < 14 && ins != null; i++, ins = ins.getNext()) {
            println("     " + ins.getAddress() + "  " + ins);
            if (ins.getMnemonicString().equals("blr")) break;
        }
    }

    private String readString(long a) {
        try {
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 200; i++) {
                byte b = getByte(toAddr(a + i));
                if (b == 0) break;
                if (b < 0x20 || b > 0x7E) return null;
                sb.append((char) b);
            }
            return sb.length() > 0 ? sb.toString() : null;
        } catch (Exception e) {
            return null;
        }
    }

    private long u32(long a) {
        try {
            return getInt(toAddr(a)) & 0xFFFFFFFFL;
        } catch (Exception e) {
            return 0;
        }
    }

    private static String q(String s) {
        return s.contains(",") || s.contains("\"") ? "\"" + s.replace("\"", "\"\"") + "\"" : s;
    }

    private static List<String> csv(String line) {
        List<String> out = new ArrayList<>();
        StringBuilder cur = new StringBuilder();
        boolean inQ = false;
        for (int i = 0; i < line.length(); i++) {
            char c = line.charAt(i);
            if (inQ) {
                if (c == '"' && i + 1 < line.length() && line.charAt(i + 1) == '"') { cur.append('"'); i++; }
                else if (c == '"') inQ = false;
                else cur.append(c);
            } else if (c == '"') inQ = true;
            else if (c == ',') { out.add(cur.toString()); cur.setLength(0); }
            else cur.append(c);
        }
        out.add(cur.toString());
        return out;
    }
}
