// Classifies every bctr/bctrl in .text (gaps are disassembled in memory first) and decodes switch tables.
//   GHS switches: idx is bounds-checked (cmplwi idx,N), shifted by 2 and added to a constant base; the
//   base is usually bctr+4, i.e. the "table" is N+1 inline `b case` instructions. lwzx tables in data
//   are handled too.
//   Indirect calls: vcall = lwz fn,off(lwz vptr,voff(obj)); ptmf = fn loaded from vtable+idx*8+4
//   (pointer to member); fptr = anything else.
// Writes <switches.csv>: bctr,function,kind,table,cases,bound,targets (targets space-separated)
//        <indirect.csv>: addr,function,kind,branch,vptr_off,fn_off   (branch = call|tail)
// Args: <switches.csv> <indirect.csv>
// @category MH3U
import java.io.*;
import java.util.*;
import java.util.regex.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.*;

public class FindIndirect extends GhidraScript {
    static final Pattern OPS = Pattern.compile("^(\\S+)\\s*(.*)$");
    static final int BACK = 24;
    Listing listing;
    long tStart, tEnd;

    static String[] ops(Instruction i) {
        Matcher m = OPS.matcher(i.toString());
        m.find();
        String rest = m.group(2).trim();
        return rest.isEmpty() ? new String[0] : rest.split(",");
    }

    static boolean writesFirst(String mn) {
        return !(mn.startsWith("st") || mn.startsWith("cmp") || mn.startsWith("b") || mn.equals("mtspr") || mn.startsWith("dcb")
                || mn.startsWith("ps_cmp") || mn.startsWith("fcmp") || mn.equals("sync") || mn.equals("isync"));
    }

    static boolean terminator(Instruction i) {
        String mn = i.getMnemonicString();
        return mn.equals("b") || mn.equals("blr") || mn.equals("bctr") || mn.equals("rfi");
    }

    /** Nearest earlier instruction (same straight-line run) that writes reg. */
    Instruction def(Instruction from, String reg) {
        Instruction p = from;
        for (int k = 0; k < BACK; k++) {
            p = listing.getInstructionBefore(p.getAddress());
            if (p == null || terminator(p)) return null;
            String mn = p.getMnemonicString();
            if (mn.equals("bl") || mn.equals("bctrl") || mn.equals("blrl")) {
                if (reg.matches("r([0-9]|1[0-2])")) return null;  // volatile, clobbered by the call
                continue;
            }
            String[] o = ops(p);
            if (o.length > 0 && writesFirst(mn) && o[0].equals(reg)) return p;
            // update forms (lwzu/stwu) also write the base register
            if ((mn.endsWith("u") || mn.endsWith("ux")) && o.length > 1 && o[1].contains("(" + reg + ")")) return p;
        }
        return null;
    }

    static long imm(String s) {
        s = s.trim();
        boolean neg = s.startsWith("-");
        if (neg) s = s.substring(1);
        long v = s.startsWith("0x") ? Long.parseLong(s.substring(2), 16) : Long.parseLong(s);
        return neg ? -v : v;
    }

    /** Constant value of reg at `at`, or null. */
    Long constOf(Instruction at, String reg, int depth) {
        if (depth > 6) return null;
        Instruction d = def(at, reg);
        if (d == null) return null;
        String mn = d.getMnemonicString();
        String[] o = ops(d);
        try {
            switch (mn) {
                case "lis": return (imm(o[1]) << 16) & 0xFFFFFFFFL;
                case "li": return imm(o[1]);
                case "addi": case "addic": case "subi": case "subic": case "addis": {
                    Long b = constOf(d, o[1], depth + 1);
                    if (b == null) return null;
                    long v = imm(o[2]);
                    if (mn.startsWith("sub")) v = -v;
                    if (mn.equals("addis")) v <<= 16;
                    return (b + v) & 0xFFFFFFFFL;
                }
                case "ori": {
                    Long b = constOf(d, o[1], depth + 1);
                    return b == null ? null : (b | imm(o[2]));
                }
                case "or":
                    if (o[1].equals(o[2])) return constOf(d, o[1], depth + 1);
                    return null;
                default: return null;
            }
        } catch (RuntimeException e) {
            return null;
        }
    }

    /** If reg = idx << shift (rlwinm idx,shift,0,31-shift), returns idx register name. */
    String shiftedIndex(Instruction at, String reg, int shift) {
        Instruction d = def(at, reg);
        if (d == null || !d.getMnemonicString().equals("rlwinm")) return null;
        String[] o = ops(d);
        if (imm(o[2]) != shift || imm(o[4]) != 31 - shift) return null;
        return o[1];
    }

    /** cmplwi [crN,]idx,N before `at` → N+1 cases. */
    Integer bound(Instruction at, String idx) {
        Instruction p = at;
        long rebase = 0;  // idx = x - rebase after hopping through subi/addi
        for (int k = 0; k < BACK; k++) {
            p = listing.getInstructionBefore(p.getAddress());
            if (p == null || terminator(p)) return null;
            String mn = p.getMnemonicString();
            String[] o = ops(p);
            if (mn.equals("cmplwi")) {
                int r = o[0].startsWith("cr") ? 1 : 0;
                if (o.length > r + 1 && o[r].equals(idx)) {
                    // bgt/ble guard → cases 0..N; bge/blt guard → 0..N-1
                    int n = (int) (imm(o[r + 1]) - rebase);
                    Instruction q = p;
                    for (int j = 0; j < 4; j++) {
                        q = q.getNext();
                        if (q == null) break;
                        String qm = q.getMnemonicString();
                        if (qm.startsWith("bge") || qm.startsWith("blt")) return n;
                        if (qm.startsWith("bgt") || qm.startsWith("ble")) return n + 1;
                    }
                    return n + 1;
                }
            }
            if (o.length > 0 && writesFirst(mn) && o[0].equals(idx) && !mn.equals("rlwinm")) {
                // idx was produced here: allow one hop through a move/sub (e.g. subi idx,x,base)
                if (mn.equals("or") && o.length > 2 && o[1].equals(o[2])) idx = o[1];
                else if ((mn.equals("subi") || mn.equals("addi")) && o.length > 2) {
                    rebase += mn.equals("subi") ? imm(o[2]) : -imm(o[2]);
                    idx = o[1];
                } else return null;
            }
        }
        return null;
    }

    long branchTarget(long a) throws Exception {
        int w = getInt(toAddr(a));
        if ((w >>> 26) != 18 || (w & 3) != 0) return -1;
        int li = (w & 0x03FFFFFC);
        if ((li & 0x02000000) != 0) li |= 0xFC000000;
        return (a + li) & 0xFFFFFFFFL;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        listing = currentProgram.getListing();
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        tStart = text.getStart().getOffset();
        tEnd = text.getEnd().getOffset() + 1;
        int disas = 0;
        for (long a = tStart; a < tEnd; a += 4) {
            if (listing.getInstructionContaining(toAddr(a)) == null && listing.getDefinedDataContaining(toAddr(a)) == null) {
                disassemble(toAddr(a));
                disas++;
            }
        }
        println("disassembly seeds in gaps=" + disas);

        StringBuilder sw = new StringBuilder("bctr,function,kind,table,cases,bound,targets\n");
        StringBuilder ic = new StringBuilder("addr,function,kind,branch,vptr_off,fn_off\n");
        Map<String, Integer> stats = new TreeMap<>();
        List<String> unknownJ = new ArrayList<>();
        int ghidraComputed = 0, ghidraAgree = 0, ghidraDisagree = 0;
        for (long a = tStart; a < tEnd; a += 4) {
            int w = getInt(toAddr(a));
            boolean isJ = w == 0x4E800420, isC = w == 0x4E800421;
            if (!isJ && !isC) continue;
            Instruction ins = listing.getInstructionAt(toAddr(a));
            if (ins == null) { stats.merge("undecodable", 1, Integer::sum); continue; }
            Function fn = getFunctionContaining(toAddr(a));
            String fname = fn == null ? "" : String.format("0x%08X", fn.getEntryPoint().getOffset());
            Set<Long> ghidraTargets = new TreeSet<>();
            for (Reference r : ins.getReferencesFrom())
                if (r.getReferenceType() == RefType.COMPUTED_JUMP) ghidraTargets.add(r.getToAddress().getOffset());

            Instruction mt = def(ins, "CTR");
            String ctrReg = null;
            if (mt != null && mt.getMnemonicString().equals("mtspr")) ctrReg = ops(mt)[1];
            if (ctrReg == null) {
                // mtspr CTR is not matched by writesFirst; look for it explicitly
                Instruction p = ins;
                for (int k = 0; k < BACK && ctrReg == null; k++) {
                    p = listing.getInstructionBefore(p.getAddress());
                    if (p == null || terminator(p)) break;
                    if (p.getMnemonicString().equals("mtspr") && ops(p)[0].equals("CTR")) { ctrReg = ops(p)[1]; mt = p; }
                }
            }
            String kind = null;
            if (ctrReg != null) {
                Instruction d = def(mt, ctrReg);
                String dm = d == null ? "" : d.getMnemonicString();
                String[] o = d == null ? new String[0] : ops(d);
                // --- switch forms ---
                if (isJ && d != null) {
                    Long base = null; String idx = null; String tkind = null;
                    if (dm.equals("add")) {
                        for (int s = 0; s < 2 && base == null; s++) {
                            String x = o[1 + s], y = o[2 - s];
                            String i2 = shiftedIndex(d, x, 2);
                            Long c = i2 == null ? null : constOf(d, y, 0);
                            if (c != null) { base = c; idx = i2; tkind = "inline"; }
                        }
                    } else if (dm.equals("addi") || dm.equals("subi") || dm.equals("addic") || dm.equals("subic")) {
                        Instruction h = def(d, o[1]);
                        if (h != null && h.getMnemonicString().equals("addis")) {
                            String[] ho = ops(h);
                            String i2 = shiftedIndex(h, ho[1], 2);
                            if (i2 != null) {
                                long lo = imm(o[2]);
                                if (dm.startsWith("sub")) lo = -lo;
                                base = ((imm(ho[2]) << 16) + lo) & 0xFFFFFFFFL;
                                idx = i2; tkind = "inline";
                            }
                        }
                    } else if (dm.equals("lwzx")) {
                        for (int s = 0; s < 2 && base == null; s++) {
                            String x = o[1 + s], y = o[2 - s];
                            String i2 = shiftedIndex(d, x, 2);
                            Long c = i2 == null ? null : constOf(d, y, 0);
                            if (c != null) { base = c; idx = i2; tkind = "words"; }
                        }
                    }
                    if (base != null) {
                        Integer n = bound(d, idx);
                        String bsrc = "cmplwi";
                        if (tkind.equals("inline")) {
                            // Inline tables are runs of `b`; the run length caps the count. Used when the
                            // guard is not in straight-line code (binary-search switches) or disagrees.
                            int run = 0;
                            while (run < 4096) {
                                long t = branchTarget(base + 4L * run);
                                if (t < 0 || Math.abs(t - a) > 0x100000) break;
                                run++;
                            }
                            if (n == null || n > run) { n = run; bsrc = "scan"; }
                        }
                        if (n == null && !ghidraTargets.isEmpty()) { n = ghidraTargets.size(); bsrc = "ghidra"; }
                        List<Long> targets = new ArrayList<>();
                        if (n != null && n > 0 && n < 4096) {
                            for (int k = 0; k < n; k++) {
                                long t;
                                if (tkind.equals("inline")) {
                                    long ea = base + 4L * k;
                                    t = branchTarget(ea);
                                    if (t < 0) {
                                        // a case may fall straight into code (last entry isn't a branch)
                                        t = ea;
                                    }
                                } else {
                                    t = getInt(toAddr(base + 4L * k)) & 0xFFFFFFFFL;
                                }
                                targets.add(t);
                            }
                        }
                        kind = "switch_" + tkind + (base == a + 4 ? "" : "_far") + (n == null ? "_unbounded" : "");
                        if (!ghidraTargets.isEmpty()) {
                            ghidraComputed++;
                            Set<Long> ours = new TreeSet<>(targets);
                            if (tkind.equals("inline")) {  // Ghidra's refs point at the table slots
                                ours.clear();
                                for (int k = 0; n != null && k < n; k++) ours.add(base + 4L * k);
                            }
                            if (ours.equals(ghidraTargets)) ghidraAgree++;
                            else {
                                ghidraDisagree++;
                                if (ghidraDisagree <= 40) println(String.format("  ghidra disagrees @%08X ours=%d ghidra=%d oursContainsGhidra=%b", a, ours.size(), ghidraTargets.size(), ours.containsAll(ghidraTargets)));
                            }
                        }
                        StringBuilder ts = new StringBuilder();
                        for (long t : targets) ts.append(ts.length() == 0 ? "" : " ").append(String.format("%08X", t));
                        sw.append(String.format("0x%08X,%s,%s,0x%08X,%s,%s,%s\n", a, fname, tkind, base,
                                n == null ? "" : n.toString(), n == null ? "none" : bsrc, ts));
                    }
                }
                // --- calls through pointers ---
                if (kind == null && d != null && dm.equals("lwz")) {
                    Matcher m = Pattern.compile("(-?0x[0-9a-f]+|-?\\d+)\\((r\\d+)\\)").matcher(o[1]);
                    if (m.find()) {
                        long off = imm(m.group(1));
                        String b = m.group(2);
                        Instruction v = def(d, b);
                        String vm = v == null ? "" : v.getMnemonicString();
                        if (vm.equals("lwz")) {
                            Matcher m2 = Pattern.compile("(-?0x[0-9a-f]+|-?\\d+)\\((r\\d+)\\)").matcher(ops(v)[1]);
                            if (m2.find() && (off & 7) == 4 && off >= 12) {
                                kind = "vcall";
                                ic.append(String.format("0x%08X,%s,vcall,%s,0x%X,0x%X\n", a, fname, isC ? "call" : "tail", imm(m2.group(1)), off));
                            }
                        } else if (vm.equals("add") && off == 4) {
                            String[] vo = ops(v);
                            if (shiftedIndex(v, vo[1], 3) != null || shiftedIndex(v, vo[2], 3) != null) {
                                kind = "ptmf";
                                ic.append(String.format("0x%08X,%s,ptmf,%s,,\n", a, fname, isC ? "call" : "tail"));
                            }
                        }
                    }
                }
            }
            if (kind == null) {
                kind = "fptr";
                ic.append(String.format("0x%08X,%s,fptr,%s,,\n", a, fname, isC ? "call" : "tail"));
                if (isJ && unknownJ.size() < 30) {
                    StringBuilder ctx = new StringBuilder();
                    Instruction p = ins;
                    for (int k = 0; k < 6 && p != null; k++) { p = listing.getInstructionBefore(p.getAddress()); if (p != null) ctx.insert(0, p + " ; "); }
                    unknownJ.add(String.format("%08X gh=%d: %s", a, ghidraTargets.size(), ctx));
                }
            }
            stats.merge((isC ? "C " : "J ") + kind, 1, Integer::sum);
        }
        try (Writer w = new OutputStreamWriter(new FileOutputStream(args[0]), "UTF-8")) { w.write(sw.toString()); }
        try (Writer w = new OutputStreamWriter(new FileOutputStream(args[1]), "UTF-8")) { w.write(ic.toString()); }
        stats.forEach((k, v) -> println(String.format("%7d  %s", v, k)));
        println(String.format("ghidra-recognised switches=%d agree=%d disagree=%d", ghidraComputed, ghidraAgree, ghidraDisagree));
        for (String s : unknownJ) println("  J fptr: " + s);
    }
}
