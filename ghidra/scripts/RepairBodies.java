// Makes function bodies tile .text, for the recompiler's function list.
//  1. Switches from <switches.csv> (FindIndirect): add COMPUTED_JUMP refs bctr→table slots, drop refs past
//     the decoded table (Ghidra overshoots some), disassemble, and re-derive the owning function's body.
//  2. Gaps (bytes in no function body): a gap that starts right after a terminator (blr/b/bctr/rfi) is
//     a function nobody calls directly → create one. A gap after a non-terminator is a body Ghidra cut
//     short (usually a call it wrongly thinks never returns) → report, and with "apply" clear that
//     callee's no-return flag and re-derive the caller's body.
// Args: <switches.csv> dry|apply
// @category MH3U
import java.io.*;
import java.util.*;

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.*;

public class RepairBodies extends GhidraScript {
    Listing listing;

    boolean terminator(Instruction i) {
        String mn = i.getMnemonicString();
        return mn.equals("b") || mn.equals("blr") || mn.equals("bctr") || mn.equals("rfi");
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        boolean apply = args[1].equals("apply");
        listing = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();
        ReferenceManager rm = currentProgram.getReferenceManager();
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        long tStart = text.getStart().getOffset(), tEnd = text.getEnd().getOffset() + 1;

        // --- 1. switches ---
        int swFixed = 0, refsAdded = 0, refsRemoved = 0;
        Set<Function> refresh = new HashSet<>();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(args[0]), "UTF-8"))) {
            r.readLine();
            for (String line; (line = r.readLine()) != null; ) {
                String[] c = line.split(",", -1);
                Address bctr = toAddr(Long.decode(c[0]));
                long base = Long.decode(c[3]);
                int n = c[4].isEmpty() ? 0 : Integer.parseInt(c[4]);
                Set<Address> want = new HashSet<>();
                for (int k = 0; k < n; k++) want.add(toAddr(c[2].equals("inline") ? base + 4L * k : Long.parseLong(c[6].split(" ")[k], 16)));
                Set<Address> have = new HashSet<>();
                boolean changed = false;
                for (Reference ref : rm.getReferencesFrom(bctr)) {
                    if (ref.getReferenceType() != RefType.COMPUTED_JUMP) continue;
                    have.add(ref.getToAddress());
                    if (!want.contains(ref.getToAddress())) {
                        if (apply) rm.delete(ref);
                        refsRemoved++; changed = true;
                    }
                }
                for (Address t : want) {
                    if (have.contains(t)) continue;
                    if (apply) {
                        if (listing.getInstructionAt(t) == null) disassemble(t);
                        rm.addMemoryReference(bctr, t, RefType.COMPUTED_JUMP, SourceType.ANALYSIS, 0);
                    }
                    refsAdded++; changed = true;
                }
                if (changed) {
                    swFixed++;
                    Function f = fm.getFunctionContaining(bctr);
                    if (f != null) refresh.add(f);
                }
            }
        }
        if (apply) for (Function f : refresh) CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
        println(String.format("switches changed=%d refs added=%d removed=%d functions re-derived=%d", swFixed, refsAdded, refsRemoved, refresh.size()));

        // --- 2. gaps ---
        int created = 0, failed = 0, cut = 0;
        Map<String, Integer> cutCallees = new TreeMap<>();
        List<String> cutSamples = new ArrayList<>(), failSamples = new ArrayList<>();
        AddressSet inBodies = new AddressSet();
        for (Function f : fm.getFunctions(true)) inBodies.add(f.getBody());
        AddressSet gaps = new AddressSet(text.getStart(), text.getEnd()).subtract(inBodies);
        println(String.format("gap ranges=%d bytes=%d", gaps.getNumAddressRanges(), gaps.getNumAddresses()));
        Set<Function> noRet = new HashSet<>();
        for (AddressRange g : gaps.getAddressRanges()) {
            Address a = g.getMinAddress();
            Instruction prev = listing.getInstructionBefore(a);
            boolean afterTerm = prev == null || terminator(prev) || prev.getMaxAddress().getOffset() + 1 != a.getOffset();
            if (!afterTerm) {
                cut++;
                String callee = "?";
                if (prev.getMnemonicString().equals("bl")) {
                    Address t = prev.getFlows().length > 0 ? prev.getFlows()[0] : null;
                    Function cf = t == null ? null : fm.getFunctionAt(t);
                    callee = cf == null ? String.valueOf(t) : cf.getName() + (cf.hasNoReturn() ? "[noreturn]" : "");
                    if (cf != null && cf.hasNoReturn()) noRet.add(cf);
                }
                cutCallees.merge(prev.getMnemonicString() + " " + callee, 1, Integer::sum);
                if (cutSamples.size() < 20) cutSamples.add(String.format("%s after %s: %s", g, prev.getAddress(), prev));
                if (!apply) continue;
                // e.g. code after a conditional return that was never disassembled: extend the caller's body.
                // After a genuine no-return call the body stays put and the carving below takes the rest.
                if (listing.getInstructionAt(a) == null) disassemble(a);
                Function owner = fm.getFunctionContaining(prev.getAddress());
                if (owner != null) CreateFunctionCmd.fixupFunctionBody(currentProgram, owner, monitor);
            }
            if (!apply) continue;
            // carve consecutive functions out of the gap
            long cur = a.getOffset(), end = g.getMaxAddress().getOffset() + 1;
            while (cur < end) {
                Address ca = toAddr(cur);
                if (fm.getFunctionContaining(ca) != null) { cur += 4; continue; }
                if (listing.getInstructionAt(ca) == null) disassemble(ca);
                Function f = listing.getInstructionAt(ca) == null ? null : createFunction(ca, null);
                if (f == null) {
                    failed++;
                    if (failSamples.size() < 20) failSamples.add(String.format("%08X", cur));
                    cur += 4;
                    continue;
                }
                created++;
                AddressRange first = f.getBody().getRangeContaining(ca);
                cur = Math.max(cur + 4, first.getMaxAddress().getOffset() + 1);
            }
        }
        println(String.format("gaps after a terminator → functions created=%d failed=%d; gaps after a non-terminator (cut bodies)=%d", created, failed, cut));
        cutCallees.entrySet().stream().sorted((x, y) -> y.getValue() - x.getValue()).limit(25)
                .forEach(e -> println(String.format("  %5d  %s", e.getValue(), e.getKey())));
        for (String s : cutSamples) println("  cut: " + s);
        for (String s : failSamples) println("  fail: " + s);
        println("noreturn callees implicated=" + noRet.size());

        // --- 3. split at mid-function branch targets ---
        // A b/bc/bl from one function into the middle of another usually means Ghidra glued a tail-called
        // function onto its caller (GHS emits switch tables whose entries `b` straight to other functions).
        // Every such target becomes an entry; the owner is recreated (keeping its name) so bodies re-tile.
        for (int pass = 1; pass <= 6; pass++) {
            Map<Function, Set<Address>> byOwner = new HashMap<>();
            for (Instruction ins : listing.getInstructions(text.getStart(), true)) {
                if (ins.getAddress().getOffset() >= tEnd) break;
                if (!ins.getFlowType().isJump() && !ins.getFlowType().isCall()) continue;
                Function from = fm.getFunctionContaining(ins.getAddress());
                for (Address t : ins.getFlows()) {
                    if (!text.contains(t) || listing.getInstructionAt(t) == null) continue;
                    Function to = fm.getFunctionContaining(t);
                    if (to == null || to == from || to.getEntryPoint().equals(t)) continue;
                    byOwner.computeIfAbsent(to, x -> new TreeSet<>()).add(t);
                }
            }
            int targets = byOwner.values().stream().mapToInt(Set::size).sum();
            println(String.format("split pass %d: owners=%d targets=%d", pass, byOwner.size(), targets));
            if (byOwner.isEmpty() || !apply) break;
            int made = 0, lost = 0;
            for (Map.Entry<Function, Set<Address>> en : byOwner.entrySet()) {
                Function owner = en.getKey();
                Address entry = owner.getEntryPoint();
                String name = owner.getName();
                ghidra.program.model.symbol.Namespace ns = owner.getParentNamespace();
                boolean named = owner.getSymbol().getSource() != SourceType.DEFAULT;
                fm.removeFunction(entry);
                for (Address t : en.getValue()) if (createFunction(t, null) != null) made++;
                Function again = createFunction(entry, null);
                if (again == null) { lost++; println("  could not recreate " + entry + " " + name); continue; }
                if (named) {
                    again.setParentNamespace(ns);
                    again.setName(name, SourceType.USER_DEFINED);
                }
            }
            // targets created first stop the owners' flow; re-derive everyone touched so bodies are final
            for (Map.Entry<Function, Set<Address>> en : byOwner.entrySet()) {
                for (Address t : en.getValue()) {
                    Function f = fm.getFunctionAt(t);
                    if (f != null) CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
                }
            }
            println(String.format("  created=%d owners lost=%d", made, lost));
        }
    }
}
