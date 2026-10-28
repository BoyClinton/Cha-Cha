// Dumps the instructions of every function that references an import stub.
// Args: <import name> [max functions]. Useful to eyeball call patterns before writing a resolver.
// @category MH3U
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.*;

public class DumpCallers extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String name = args[0];
        int max = args.length > 1 ? Integer.parseInt(args[1]) : 5;
        for (Symbol s : currentProgram.getSymbolTable().getSymbols(name)) {
            MemoryBlock b = getMemoryBlock(s.getAddress());
            if (b == null || !b.getName().startsWith(".fimport_")) continue;
            Set<Address> seen = new TreeSet<>();
            for (Reference r : getReferencesTo(s.getAddress())) {
                Function f = getFunctionContaining(r.getFromAddress());
                println(String.format("ref from %s (%s) in %s", r.getFromAddress(), r.getReferenceType(),
                    f == null ? "<no function>" : f.getEntryPoint() + " " + f.getName()));
                if (f != null) seen.add(f.getEntryPoint());
            }
            int n = 0;
            for (Address fa : seen) {
                if (n++ >= max) break;
                Function f = getFunctionAt(fa);
                println("== " + f.getName() + " @ " + fa + " size=" + f.getBody().getNumAddresses());
                int i = 0;
                for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                    if (i++ > 40) { println("   ..."); break; }
                    StringBuilder refs = new StringBuilder();
                    for (Reference r : ins.getReferencesFrom()) refs.append(" ->").append(r.getToAddress());
                    println("   " + ins.getAddress() + "  " + ins + refs);
                }
            }
        }
    }
}
