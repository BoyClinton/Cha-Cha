// Decompiles every function that references a given address and prints those whose C contains a text pattern,
// with the matching lines (and lines containing an optional second pattern for context).
// Args: <hexaddr> <pattern> [context pattern]   e.g. 10312bb0 "0x11c) & 0x60" FUN_020e24b8
// @category MH3U
import java.util.*;

import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class FindPattern extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Address target = toAddr(Long.parseLong(args[0], 16));
        String pattern = args[1];
        String context = args.length > 2 ? args[2] : null;
        Set<Function> fns = new TreeSet<>(Comparator.comparing(Function::getEntryPoint));
        for (Reference r : getReferencesTo(target)) {
            Function f = getFunctionContaining(r.getFromAddress());
            if (f != null) fns.add(f);
        }
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        println("functions referencing " + target + ": " + fns.size());
        for (Function f : fns) {
            DecompileResults res = di.decompileFunction(f, 60, monitor);
            if (!res.decompileCompleted()) continue;
            String c = res.getDecompiledFunction().getC();
            if (!c.contains(pattern)) continue;
            println("== " + f.getEntryPoint() + " " + f.getName(true));
            for (String line : c.split("\n"))
                if (line.contains(pattern) || (context != null && line.contains(context))) println("   " + line.trim());
        }
        di.dispose();
    }
}
