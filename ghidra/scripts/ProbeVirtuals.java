// Diagnostics for NameVirtuals: (1) vtables whose typeinfo base list doesn't lead to a known vtable,
// (2) slot-0 functions that don't reference their own vtable. Prints a few samples of each.
// Args: <vtables.csv>
// @category MH3U
import java.io.*;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;

public class ProbeVirtuals extends GhidraScript {
    @Override
    public void run() throws Exception {
        Map<Long, String> tiName = new HashMap<>();
        List<long[]> rows = new ArrayList<>();
        List<String> names = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(getScriptArgs()[0]))) {
            r.readLine();
            for (String line; (line = r.readLine()) != null; ) {
                String[] f = line.split(",");
                long vt = Long.decode(f[0]);
                long ti = Long.decode(f[f.length - 3]);
                tiName.put(ti, line.substring(line.indexOf(',') + 1, line.lastIndexOf(",0x", line.lastIndexOf(",", line.lastIndexOf(",") - 1) - 1)));
                rows.add(new long[] { vt, ti });
                names.add(line);
            }
        }
        Map<Long, Integer> missingBaseTi = new HashMap<>();
        int shown = 0;
        for (int k = 0; k < rows.size(); k++) {
            long ti = rows.get(k)[1];
            long bl = u32(ti + 12);
            if (bl == 0) continue;
            long bti = u32(bl);
            if (!tiName.containsKey(bti)) {
                missingBaseTi.merge(bti, 1, Integer::sum);
                if (shown++ < 4) {
                    println("no-base-vtable: " + names.get(k));
                    StringBuilder s = new StringBuilder("   baselist words:");
                    for (int i = 0; i < 6; i++) s.append(String.format(" %08x", u32(bl + 4L * i)));
                    s.append("   base ti words:");
                    for (int i = 0; i < 4; i++) s.append(String.format(" %08x", u32(bti + 4L * i)));
                    s.append("  name@+4: ").append(str(u32(bti + 4)));
                    println(s.toString());
                }
            }
        }
        println("distinct missing base typeinfos: " + missingBaseTi.size() + "  vtables affected: " +
            missingBaseTi.values().stream().mapToInt(Integer::intValue).sum());
        shown = 0;
        for (int k = 0; k < rows.size() && shown < 4; k++) {
            long vt = rows.get(k)[0];
            long fn = u32(vt + 12);
            Function f = getFunctionAt(toAddr(fn));
            boolean refs = false;
            if (f != null) {
                for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true))
                    for (Reference r : ins.getReferencesFrom()) if (r.getToAddress().getOffset() == vt) refs = true;
            }
            if (!refs && k % 37 == 0) {
                shown++;
                println("slot0-no-vtref: " + names.get(k) + (f == null ? "  (no function at slot0)" : "  size=" + f.getBody().getNumAddresses()));
                if (f != null) {
                    int i = 0;
                    for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                        if (i++ > 14) break;
                        StringBuilder r2 = new StringBuilder();
                        for (Reference rr : ins.getReferencesFrom()) r2.append(" ->").append(rr.getToAddress());
                        println("     " + ins.getAddress() + "  " + ins + r2);
                    }
                }
            }
        }
    }

    private String str(long a) {
        try {
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 60; i++) { byte b = getByte(toAddr(a + i)); if (b == 0) break; sb.append((char) b); }
            return sb.toString();
        } catch (Exception e) { return "?"; }
    }

    private long u32(long a) {
        try { return getInt(toAddr(a)) & 0xFFFFFFFFL; } catch (Exception e) { return 0; }
    }
}
