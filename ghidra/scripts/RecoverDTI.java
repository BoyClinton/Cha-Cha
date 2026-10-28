// Recovers MT Framework class metadata from two sources:
//  1. MtDTI registrations: every call to MtDTI::MtDTI(this, name, parent, size, id, r8, r9) in the static
//     initialisers, plus the MyDTI vtable stored to this+0x1C right after.
//  2. GHS C++ RTTI: typeinfo objects {tiVptr, const char *name, ...} and vtables that start with the
//     header pair {0, typeinfo*} followed by {delta, fn} pairs.
// Writes two CSVs: <dti.csv> (one row per registered class) and <vtables.csv> (one row per vtable).
//
// MtDTI layout (0x20 bytes, from the ctor at MTDTI_CTOR):
//   +00 name  +04 next  +08 child  +0C parent  +10 hash link
//   +14 bits: size = w>>9 (23 bits) | r9 field in bits 3..8 | r8 field in bits 0..2
//   +18 id (crc32(name, ~0) & 0x7FFFFFFF when passed as 0)  +1C vptr (GHS: vptr after base members)
// Args: <dti.csv> <vtables.csv>
// @category MH3U
import java.io.PrintWriter;
import java.util.*;

import ghidra.app.plugin.core.analysis.ConstantPropagationContextEvaluator;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.Reference;
import ghidra.program.util.SymbolicPropogator;

public class RecoverDTI extends GhidraScript {
    static final long MTDTI_CTOR = 0x02d63178L;
    static final long DTI_ROOT = 0x103d5b70L;   // ctor's default parent when none is passed

    private final Map<Address, SymbolicPropogator> propsByStart = new HashMap<>();
    private MemoryBlock text;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        text = currentProgram.getMemory().getBlock(".text");

        // ---- 1. DTI registrations ---------------------------------------------------------------
        List<long[]> dtis = new ArrayList<>();         // dti, name, parent, size, id, r8, r9, myVt, call
        Map<Long, String> dtiName = new HashMap<>();
        dtiName.put(DTI_ROOT, "<root>");
        int unresolved = 0;
        for (Reference r : getReferencesTo(toAddr(MTDTI_CTOR))) {
            if (!r.getReferenceType().isCall()) continue;
            Address call = r.getFromAddress();
            Long dti = reg(call, "r3"), name = reg(call, "r4"), parent = reg(call, "r5"), size = reg(call, "r6");
            Long id = reg(call, "r7"), r8 = reg(call, "r8"), r9 = reg(call, "r9");
            String nm = name == null ? null : readString(name);
            if (dti == null || nm == null) {
                if (unresolved++ < 15) println("unresolved DTI ctor call at " + call + " r3=" + dti + " r4=" + name);
                continue;
            }
            Long myVt = vtableStoreAfter(call);
            dtis.add(new long[] { dti, name, parent == null ? -1 : parent, size == null ? -1 : size,
                                  id == null ? -1 : id, r8 == null ? -1 : r8, r9 == null ? -1 : r9,
                                  myVt == null ? -1 : myVt, call.getOffset() });
            dtiName.put(dti, nm);
        }

        // ---- 2. RTTI typeinfos and vtables ------------------------------------------------------
        Memory mem = currentProgram.getMemory();
        List<MemoryBlock> dataBlocks = new ArrayList<>();
        for (String b : new String[] { ".rodata", ".data" }) dataBlocks.add(mem.getBlock(b));
        // typeinfo candidates: X with *(X+4) -> identifier string; vtable headers: {0, X}
        Map<Long, String> typeinfo = new HashMap<>();
        Map<Long, Integer> tiVptrs = new HashMap<>();
        List<long[]> headers = new ArrayList<>();        // vtable addr, typeinfo addr
        for (MemoryBlock b : dataBlocks) {
            for (long a = b.getStart().getOffset(); a + 8 <= b.getEnd().getOffset(); a += 4) {
                if (getIntU(a) != 0) continue;
                long ti = getIntU(a + 4);
                MemoryBlock tb = mem.getBlock(toAddr(ti));
                if (tb == null || !dataBlocks.contains(tb) || (ti & 3) != 0) continue;
                String nm = typeinfo.get(ti);
                if (nm == null) {
                    long np = getIntU(ti + 4);
                    nm = identifier(np);
                    if (nm == null) continue;
                    typeinfo.put(ti, nm);
                    tiVptrs.merge(getIntU(ti), 1, Integer::sum);
                }
                // must be followed by at least one {delta, fn-in-.text} pair
                if (!isText(getIntU(a + 12))) continue;
                headers.add(new long[] { a, ti });
            }
        }
        println("typeinfo vptr histogram: " + tiVptrs);

        Map<String, List<long[]>> vtByClass = new HashMap<>();
        try (PrintWriter w = new PrintWriter(args[1], "UTF-8")) {
            w.println("vtable,class,typeinfo,slots,first_fn");
            for (long[] h : headers) {
                int slots = 0;
                long first = 0;
                for (long a = h[0] + 8; ; a += 8) {
                    long d = getIntU(a), f = getIntU(a + 4);
                    if (d == 0 && typeinfo.containsKey(f)) break;          // next vtable's header
                    if (!isText(f) || (d != 0 && (int) d > 0 && d > 0x10000)) break;
                    if (slots == 0) first = f;
                    slots++;
                }
                String cls = typeinfo.get(h[1]);
                vtByClass.computeIfAbsent(cls, k -> new ArrayList<>()).add(new long[] { h[0], slots });
                w.println(String.format("0x%08X,%s,0x%08X,%d,0x%08X", h[0], q(cls), h[1], slots, first));
            }
        }

        // ---- 3. DTI csv, joined with the class vtable found via RTTI ----------------------------
        int withVt = 0;
        try (PrintWriter w = new PrintWriter(args[0], "UTF-8")) {
            w.println("class,dti,parent_dti,parent,size,id,id_crc_ok,r8,r9,mydti_vtable,class_vtable,new_instance,ctor_call");
            // Header-defined classes get a guarded initialiser per translation unit: same DTI object, many
            // ctor calls. Keep one row per DTI address.
            dtis.sort(Comparator.comparingLong((long[] x) -> x[0]).thenComparingLong(x -> x[8]));
            Set<Long> seen = new HashSet<>();
            for (long[] d : dtis) {
                if (!seen.add(d[0])) continue;
                String nm = dtiName.get(d[0]);
                String parent = d[2] < 0 ? "?" : dtiName.getOrDefault(d[2], "?");
                long crc = crc(nm);
                long id = d[4] <= 0 ? crc : d[4];  // 0 = "compute from name"; <0 = r7 not resolved
                long newInst = d[7] < 0 || !isText(getIntU(d[7] + 0x14)) ? -1 : getIntU(d[7] + 0x14);  // MyDTI slot 2
                List<long[]> vts = vtByClass.get(nm);
                String classVt = "";
                if (vts != null) {
                    long best = -1, bestSlots = -1;
                    for (long[] v : vts) if (v[1] > bestSlots) { best = v[0]; bestSlots = v[1]; }
                    classVt = String.format("0x%08X", best);
                    withVt++;
                }
                w.println(String.format("%s,0x%08X,%s,%s,0x%X,0x%08X,%s,%d,%d,%s,%s,%s,0x%08X",
                    q(nm), d[0], d[2] < 0 ? "?" : String.format("0x%08X", d[2]), q(parent), d[3], id,
                    d[4] < 0 ? "unverified" : (d[4] == 0 || d[4] == crc) ? "yes" : "no", d[5], d[6],
                    d[7] < 0 ? "?" : String.format("0x%08X", d[7]), classVt,
                    newInst < 0 ? "?" : String.format("0x%08X", newInst), d[8]));
            }
        }
        println(String.format("dti_ctor_calls=%d unresolved=%d unique_dtis=%d with_class_vtable=%d typeinfos=%d vtables=%d",
            dtis.size(), unresolved, dtiName.size() - 1, withVt, typeinfo.size(), headers.size()));
    }

    // CSV-quote names that contain commas (template arguments) or quotes.
    private static String q(String s) {
        return s.contains(",") || s.contains("\"") ? "\"" + s.replace("\"", "\"\"") + "\"" : s;
    }

    private long getIntU(long a) {
        try {
            return getInt(toAddr(a)) & 0xFFFFFFFFL;
        } catch (Exception e) {  // .bss and other uninitialised memory
            return 0;
        }
    }

    private boolean isText(long a) {
        return text.contains(toAddr(a));
    }

    private Long reg(Address at, String regName) throws Exception {
        // Many static initialisers are only reachable through the ctor table, so auto-analysis never made
        // them functions. Fall back to the range starting after the previous blr.
        Function f = getFunctionContaining(at);
        Address start;
        ghidra.program.model.address.AddressSetView body;
        if (f != null) {
            start = f.getEntryPoint();
            body = f.getBody();
        } else {
            // start: just after the previous blr / unconditional b / gap in the code
            Instruction ins = getInstructionAt(at);
            for (int i = 0; i < 400; i++) {
                Instruction prev = ins.getPrevious();
                if (prev == null || prev.getMnemonicString().equals("blr") || prev.getMnemonicString().equals("b")
                        || !prev.getMaxAddress().add(1).equals(ins.getAddress())) break;
                ins = prev;
            }
            start = ins.getAddress();
            // end: the next blr, so one propagation covers every DTI ctor call in the initialiser
            Instruction end = getInstructionAt(at);
            for (int i = 0; i < 2000 && end.getNext() != null && !end.getMnemonicString().equals("blr"); i++) {
                end = end.getNext();
            }
            body = new ghidra.program.model.address.AddressSet(start, end.getMaxAddress());
        }
        SymbolicPropogator p = propsByStart.get(start);
        if (p == null) {
            p = new SymbolicPropogator(currentProgram);
            p.flowConstants(start, body, new ConstantPropagationContextEvaluator(monitor, true), true, monitor);
            propsByStart.put(start, p);
        }
        SymbolicPropogator.Value v = p.getRegisterValue(at, currentProgram.getRegister(regName));
        if (v == null || v.isRegisterRelativeValue()) return null;
        return v.getValue() & 0xFFFFFFFFL;
    }

    // After the ctor call the initialiser stores the MyDTI vtable: stw rX,0x1c(rDTI).
    private Long vtableStoreAfter(Address call) throws Exception {
        Instruction ins = getInstructionAt(call);
        for (int i = 0; i < 10 && ins != null; i++) {
            ins = ins.getNext();
            if (ins == null) break;
            if (ins.getMnemonicString().equals("stw") && ins.toString().contains("0x1c(")) {
                Object[] in = ins.getOpObjects(0);
                if (in.length > 0 && in[0] instanceof Register) return reg(ins.getAddress(), ((Register) in[0]).getName());
            }
        }
        return null;
    }

    private String readString(long addr) {
        try {
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 128; i++) {
                byte b = getByte(toAddr(addr + i));
                if (b == 0) break;
                if (b < 0x20 || b > 0x7E) return null;
                sb.append((char) b);
            }
            return sb.length() > 0 ? sb.toString() : null;
        } catch (Exception e) {
            return null;
        }
    }

    // C++ class name, possibly qualified/templated: starts with a letter or '_'.
    private String identifier(long addr) {
        String s = readString(addr);
        if (s == null || s.length() < 2) return null;
        char c = s.charAt(0);
        if (!(Character.isLetter(c) || c == '_')) return null;
        for (char ch : s.toCharArray()) {
            if (!(Character.isLetterOrDigit(ch) || "_:<>, *&".indexOf(ch) >= 0)) return null;
        }
        return s;
    }

    private static long crc(String s) {
        java.util.zip.CRC32 c = new java.util.zip.CRC32();
        c.update(s.getBytes());
        // crc32 with init ~0 and no final xor == ~zlib_crc32; then keep 31 bits
        return (~c.getValue()) & 0x7FFFFFFFL;
    }
}
