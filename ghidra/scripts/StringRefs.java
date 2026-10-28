// For each argument string, finds its exact NUL-terminated occurrences in memory and dumps the code
// around every reference to it. Used to discover how the engine consumes names (e.g. MtDTI ctors).
// Args: <string> [<string> ...]
// @category MH3U
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;

public class StringRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        for (String s : getScriptArgs()) {
            byte[] pat = (s + "\0").getBytes("US-ASCII");
            Address a = mem.getMinAddress();
            int hits = 0;
            while ((a = mem.findBytes(a, pat, null, true, monitor)) != null && hits < 10) {
                // require a NUL (or block start) before, so "rTexture" doesn't match "...rTexture"
                boolean start = false;
                try { start = mem.getByte(a.subtract(1)) == 0; } catch (Exception e) { start = true; }
                if (start) {
                    hits++;
                    Reference[] refs = getReferencesTo(a);
                    println(String.format("\"%s\" @ %s  refs=%d", s, a, refs.length));
                    int shown = 0;
                    for (Reference r : refs) {
                        if (shown++ >= 3) break;
                        Function f = getFunctionContaining(r.getFromAddress());
                        println(String.format("  ref from %s in %s", r.getFromAddress(), f == null ? "?" : f.getName() + "@" + f.getEntryPoint()));
                        Instruction ins = getInstructionAt(r.getFromAddress());
                        if (ins == null) continue;
                        for (int i = 0; i < 6 && ins.getPrevious() != null; i++) ins = ins.getPrevious();
                        for (int i = 0; i < 16 && ins != null; i++, ins = ins.getNext()) {
                            StringBuilder refsOut = new StringBuilder();
                            for (Reference rr : ins.getReferencesFrom()) refsOut.append(" ->").append(rr.getToAddress());
                            println("     " + ins.getAddress() + "  " + ins + refsOut);
                        }
                    }
                }
                a = a.add(1);
            }
        }
    }
}
