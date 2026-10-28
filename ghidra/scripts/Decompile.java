// Decompiles functions to C. Args: <output file> <hex address>... (each address may be anywhere in the function).
// Example: scripts/ghidra-headless.ps1 -process MH3G_Cafe_US.rpx -readOnly -noanalysis
//            -postScript Decompile.java C:\abs\out.c 0x02A44ED4 0x02A44334
// Output is our working notes (decompiled game code): keep it under build/, never in git.
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.PrintWriter;

public class Decompile extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: Decompile.java <out file> <addr>...");
            return;
        }
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
            for (int i = 1; i < args.length; i++) {
                Address a = toAddr(Long.decode(args[i]));
                Function f = getFunctionContaining(a);
                if (f == null) {
                    out.println("// no function at " + args[i]);
                    continue;
                }
                DecompileResults r = dec.decompileFunction(f, 120, monitor);
                out.println("// ---- " + f.getName() + " @ " + f.getEntryPoint());
                out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
            }
        }
        dec.dispose();
    }
}
