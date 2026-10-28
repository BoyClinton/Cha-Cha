// Catalogues secondary (multiple-inheritance) vtables. GHS vtable words are {s16 value << 16, ptr}: the header
// is {offset << 16, typeinfo*} (0 for primary vtables, config/vtables.csv), slots are {this_delta << 16, fn}.
// A header with offset > 0 is the vtable of the base subobject at that offset; its slots that adjust `this`
// carry delta = -offset. The base is the most-derived polymorphic base at that offset, from the typeinfo base
// lists ({baseTypeinfo*, w}: offset = w >> 16, 0x200 = last entry); its slot count bounds the table.
// Writes <out.csv>: vtable,class,typeinfo,offset,slots,base,thunks,first_fn,count_from,slot_fns,targets
// (targets = where each slot's `subi r3,r3,offset; b target` thunk goes; the slot fn itself when not a thunk),targets
// Args: <vtables.csv> <out.csv>
// @category MH3U
import java.io.*;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.*;

public class FindSecondaryVtables extends GhidraScript {
    static final long TI_VPTR = 0x1014E0B8L;
    static final String NL = String.valueOf((char) 10);
    Memory mem;
    long tStart, tEnd;

    long u32(long a) {
        try { return mem.getInt(toAddr(a)) & 0xFFFFFFFFL; } catch (Exception e) { return -1; }
    }

    String cstr(long a) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < 300; i++) {
            try {
                byte c = mem.getByte(toAddr(a + i));
                if (c == 0) break;
                b.append((char) (c & 0xFF));
            } catch (Exception e) { return null; }
        }
        return b.toString();
    }

    boolean isCode(long v) { return v >= tStart && v < tEnd && (v & 3) == 0; }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        mem = currentProgram.getMemory();
        MemoryBlock text = mem.getBlock(".text");
        tStart = text.getStart().getOffset();
        tEnd = text.getEnd().getOffset() + 1;

        // primary vtables: address -> slot count and class
        Map<Long, Integer> primSlots = new HashMap<>();
        Map<String, List<Long>> primByClass = new HashMap<>();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(args[0]), "UTF-8"))) {
            r.readLine();
            for (String line; (line = r.readLine()) != null; ) {
                String[] c = line.split(",(?=(?:[^\"]*\"[^\"]*\")*[^\"]*$)");
                long vt = Long.decode(c[0]);
                primSlots.put(vt, Integer.parseInt(c[c.length - 2]));
                primByClass.computeIfAbsent(c[1].replace("\"", ""), k -> new ArrayList<>()).add(vt);
            }
        }

        // all typeinfos in initialised data
        List<MemoryBlock> data = new ArrayList<>();
        for (MemoryBlock b : mem.getBlocks())
            if (b.isInitialized() && !b.isExecute() && b.getStart().getOffset() >= 0x10000000L) data.add(b);
        Set<Long> tis = new HashSet<>();
        for (MemoryBlock b : data)
            for (long a = b.getStart().getOffset(); a + 8 <= b.getEnd().getOffset() + 1; a += 4)
                if (u32(a) == TI_VPTR && cstr(u32(a + 4)) != null) tis.add(a);
        println("typeinfos=" + tis.size());

        // direct bases with their subobject offsets: base list entries {baseTypeinfo*, u32 w}, offset = w >> 16,
        // flag 0x200 = last entry (e.g. JobDataStoreCheckNotification: HttpEventListener w=0x00681600 -> +0x68)
        Map<String, List<Object[]>> direct = new HashMap<>();
        int multiBaseClasses = 0;
        for (long ti : tis) {
            long bl = u32(ti + 12);
            if (bl <= 0 || bl == 0xFFFFFFFFL) continue;
            List<Object[]> bases = new ArrayList<>();
            for (int i = 0; i < 32; i++) {
                long bti = u32(bl + 8L * i), w = u32(bl + 8L * i + 4);
                if (!tis.contains(bti)) break;
                bases.add(new Object[] { cstr(u32(bti + 4)), (int) (w >>> 16) });
                if ((w & 0x200) != 0) break;
            }
            if (bases.size() > 1) multiBaseClasses++;
            direct.merge(cstr(u32(ti + 4)), bases, (x, y) -> x.size() >= y.size() ? x : y);
        }
        println("classes with >1 direct base=" + multiBaseClasses);

        StringBuilder out = new StringBuilder("vtable,class,typeinfo,offset,slots,base,thunks,first_fn,count_from,slot_fns,targets" + NL);
        int found = 0, withBase = 0, totalSlots = 0, thunkSlots = 0;
        Map<Integer, Integer> offHist = new TreeMap<>();
        for (MemoryBlock b : data) {
            for (long a = b.getStart().getOffset(); a + 16 <= b.getEnd().getOffset() + 1; a += 4) {
                long ti = u32(a + 4);
                if (!tis.contains(ti) || primSlots.containsKey(a)) continue;
                long hdr = u32(a);
                if ((hdr & 0xFFFF) != 0) continue;
                int off = (short) (hdr >>> 16);
                if (off <= 0) continue;  // secondary: header = subobject offset << 16
                String cls = cstr(u32(ti + 4));
                // most-derived polymorphic base located at this offset (walk bases transitively)
                String base = "";
                int baseSlots = -1;
                Deque<Object[]> work = new ArrayDeque<>();
                work.add(new Object[] { cls, 0 });
                Set<String> seen = new HashSet<>();
                while (!work.isEmpty()) {
                    Object[] cur = work.poll();
                    for (Object[] bb : direct.getOrDefault((String) cur[0], List.of())) {
                        String bn = (String) bb[0];
                        int bo = (Integer) cur[1] + (Integer) bb[1];
                        if (!seen.add(bn + "@" + bo)) continue;
                        if (bo == off && primByClass.containsKey(bn)) {
                            int sc = primSlots.get(primByClass.get(bn).get(0));
                            if (sc > baseSlots) { baseSlots = sc; base = bn; }
                        }
                        work.add(new Object[] { bn, bo });
                    }
                }
                // slots: {s16 delta << 16, fn}
                int n = 0, thunks = 0;
                long first = 0;
                StringBuilder fns = new StringBuilder(), tgts = new StringBuilder();
                int limit = baseSlots > 0 ? baseSlots : 400;
                while (n < limit) {
                    long d = u32(a + 8 + 8L * n), fn = u32(a + 12 + 8L * n);
                    if ((d & 0xFFFF) != 0 || !isCode(fn) || primSlots.containsKey(a + 8 + 8L * n)) break;
                    if (n == 0) first = fn;
                    fns.append(n == 0 ? "" : " ").append(String.format("%08X", fn));
                    tgts.append(n == 0 ? "" : " ").append(String.format("%08X", thunkTarget(fn)));
                    if (d != 0) thunks++;
                    n++;
                }
                if (n == 0) continue;
                String from = baseSlots > 0 ? (n == baseSlots ? "base" : "short") : "scan";
                if (!base.isEmpty()) withBase++;
                found++;
                totalSlots += n;
                thunkSlots += thunks;
                offHist.merge(off, 1, Integer::sum);
                out.append(String.format("0x%08X,%s,0x%08X,0x%X,%d,%s,%d,0x%08X,%s,%s,%s" + NL, a, q(cls), ti, off, n, q(base), thunks, first, from, fns, tgts));
                a += 8L * n + 4;  // skip header + slots
            }
        }
        // coverage: every class with a polymorphic direct base at a non-zero offset needs a secondary vtable there
        Set<String> foundKeys = new HashSet<>();
        for (String line : out.toString().split(NL)) {
            String[] c = line.split(",(?=(?:[^\"]*\"[^\"]*\")*[^\"]*$)");
            if (c.length > 3 && c[0].startsWith("0x")) foundKeys.add(c[1].replace("\"", "") + "@" + c[3]);
        }
        int expected = 0, missing = 0;
        for (Map.Entry<String, List<Object[]>> e : direct.entrySet()) {
            if (!primByClass.containsKey(e.getKey())) continue;
            for (Object[] bb : e.getValue()) {
                int bo = (Integer) bb[1];
                if (bo == 0 || !primByClass.containsKey((String) bb[0])) continue;
                expected++;
                if (!foundKeys.contains(e.getKey() + "@" + String.format("0x%X", bo))) {
                    missing++;
                    if (missing <= 10) println(String.format("  missing: %s base %s @0x%X", e.getKey(), bb[0], bo));
                }
            }
        }
        println(String.format("coverage: expected %d (class, polymorphic base at offset>0), missing %d", expected, missing));
        try (Writer w = new OutputStreamWriter(new FileOutputStream(args[1]), "UTF-8")) { w.write(out.toString()); }
        println(String.format("secondary vtables=%d (base identified %d), slots=%d (this-adjusting %d)", found, withBase, totalSlots, thunkSlots));
        StringBuilder h = new StringBuilder("subobject offsets:");
        offHist.entrySet().stream().limit(24).forEach(e -> h.append(String.format(" 0x%X:%d", e.getKey(), e.getValue())));
        println(h.toString());
    }

    /** GHS this-adjust thunk `addi r3,r3,-k; b target` -> target; otherwise fn itself. */
    long thunkTarget(long fn) {
        long w0 = u32(fn), w1 = u32(fn + 4);
        if ((w0 & 0xFFFF0000L) != 0x38630000L || (w1 >>> 26) != 18 || (w1 & 3) != 0) return fn;
        long li = w1 & 0x03FFFFFCL;
        if ((li & 0x02000000L) != 0) li -= 0x04000000L;
        return (fn + 4 + li) & 0xFFFFFFFFL;
    }

    static String q(String s) { return s.contains(",") ? "\"" + s + "\"" : s; }
}
