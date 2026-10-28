// Finds the instance pointer of each class given on the command line (class=vtable pairs, or a csv of
// class,vtable rows): the functions that reference the class vtable are its ctor/dtor; a global that the
// ctor stores `this` into and the dtor clears (stores 0 into) is the singleton's mpInstance.
// Prints: class,instance,ctor,dtor (instance empty when no unique candidate).
// Args: <in.csv with class,vtable columns> <out.csv>
// @category MH3U
import java.io.*;
import java.util.*;
import java.util.regex.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;

public class FindSingletons extends GhidraScript {
    static final Pattern LIS = Pattern.compile("^lis (r\\d+),(-?0x[0-9a-f]+|-?\\d+)$");
    static final Pattern STW = Pattern.compile("^stw (r\\d+),(-?0x[0-9a-f]+|-?\\d+)\\((r\\d+)\\)$");
    static final Pattern MOV = Pattern.compile("^or (r\\d+),(r\\d+),\\2$");
    static final Pattern LI0 = Pattern.compile("^li (r\\d+),0x0$");

    static long imm(String s) {
        boolean neg = s.startsWith("-");
        if (neg) s = s.substring(1);
        long v = s.startsWith("0x") ? Long.parseLong(s.substring(2), 16) : Long.parseLong(s);
        return neg ? -v : v;
    }

    /** Global addresses stored into: value "this" (r3 or a copy of it) or "zero". */
    Map<Long, Set<String>> globalStores(Function f) {
        Map<Long, Set<String>> out = new HashMap<>();
        Map<String, Long> hi = new HashMap<>();
        Set<String> thisRegs = new HashSet<>(List.of("r3"));
        Set<String> zeroRegs = new HashSet<>();
        for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
            String t = ins.toString();
            Matcher m;
            if ((m = LIS.matcher(t)).matches()) { hi.put(m.group(1), (imm(m.group(2)) << 16) & 0xFFFFFFFFL); thisRegs.remove(m.group(1)); zeroRegs.remove(m.group(1)); continue; }
            if ((m = MOV.matcher(t)).matches()) {
                if (thisRegs.contains(m.group(2))) thisRegs.add(m.group(1)); else thisRegs.remove(m.group(1));
                continue;
            }
            if ((m = LI0.matcher(t)).matches()) { zeroRegs.add(m.group(1)); thisRegs.remove(m.group(1)); continue; }
            if ((m = STW.matcher(t)).matches()) {
                Long h = hi.get(m.group(3));
                if (h != null) {
                    long a = (h + imm(m.group(2))) & 0xFFFFFFFFL;
                    String v = thisRegs.contains(m.group(1)) ? "this" : zeroRegs.contains(m.group(1)) ? "zero" : null;
                    if (v != null) out.computeIfAbsent(a, x -> new HashSet<>()).add(v);
                }
                continue;
            }
            if (ins.getFlowType().isCall()) { thisRegs.retainAll(Set.of("r31", "r30", "r29", "r28")); zeroRegs.clear(); }
            // any other write to a tracked register ends its meaning
            String[] parts = t.split("[ ,]");
            if (parts.length > 1 && parts[1].matches("r\\d+") && !t.startsWith("st") && !t.startsWith("cmp")) {
                thisRegs.remove(parts[1]); zeroRegs.remove(parts[1]); hi.remove(parts[1]);
            }
        }
        return out;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        StringBuilder out = new StringBuilder("class,instance,ctor,dtor\n");
        int found = 0, total = 0;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(args[0]), "UTF-8"))) {
            r.readLine();
            for (String line; (line = r.readLine()) != null; ) {
                String[] c = line.split(",");
                total++;
                Address vt = toAddr(Long.decode(c[1]));
                Set<Function> users = new LinkedHashSet<>();
                for (Reference ref : getReferencesTo(vt)) {
                    Function f = getFunctionContaining(ref.getFromAddress());
                    if (f != null) users.add(f);
                }
                Map<Long, String> ctorOf = new HashMap<>(), dtorOf = new HashMap<>();
                for (Function f : users) {
                    for (Map.Entry<Long, Set<String>> e : globalStores(f).entrySet()) {
                        if (e.getValue().contains("this")) ctorOf.put(e.getKey(), String.format("0x%08X", f.getEntryPoint().getOffset()));
                        if (e.getValue().contains("zero")) dtorOf.put(e.getKey(), String.format("0x%08X", f.getEntryPoint().getOffset()));
                    }
                }
                Set<Long> both = new TreeSet<>(ctorOf.keySet());
                both.retainAll(dtorOf.keySet());
                Long inst = both.size() == 1 ? both.iterator().next() : ctorOf.size() == 1 ? ctorOf.keySet().iterator().next() : null;
                if (inst != null) found++;
                out.append(String.format("%s,%s,%s,%s\n", c[0], inst == null ? "" : String.format("0x%08X", inst),
                        inst == null ? "" : ctorOf.get(inst), inst == null || !dtorOf.containsKey(inst) ? "" : dtorOf.get(inst)));
                if (inst == null) println(String.format("  %s: users=%d ctor-stores=%s dtor-stores=%s", c[0], users.size(), hex(ctorOf.keySet()), hex(dtorOf.keySet())));
            }
        }
        try (Writer w = new OutputStreamWriter(new FileOutputStream(args[1]), "UTF-8")) { w.write(out.toString()); }
        println(String.format("instances found %d/%d", found, total));
    }

    static String hex(Set<Long> s) {
        StringBuilder b = new StringBuilder("[");
        for (long v : s) b.append(String.format("%08X ", v));
        return b.append("]").toString();
    }
}
