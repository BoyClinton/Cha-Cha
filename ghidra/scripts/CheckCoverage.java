// Checks that config/symbols.txt is reflected in the program: every func line is a function with the
// expected leaf name, every vtable slot target is a function, and prints totals.
// Args: <symbols.txt> <vtables.csv>
// @category MH3U
import java.io.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class CheckCoverage extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        int funcLines = 0, isFunc = 0, nameOk = 0;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(args[0]), "UTF-8"))) {
            for (String line; (line = r.readLine()) != null; ) {
                if (line.startsWith("#") || line.isBlank()) continue;
                String[] f = line.split("\\s+", 4);
                if (!f[2].equals("func")) continue;
                funcLines++;
                Function fn = getFunctionAt(toAddr(Long.decode(f[0])));
                if (fn == null) continue;
                isFunc++;
                String full = fn.getName(true);
                if (full.replace(" ", "").equals(f[1]) || f[1].endsWith("::" + fn.getName()) || f[1].equals(fn.getName())) nameOk++;
            }
        }
        int slots = 0, slotFuncs = 0;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(args[1]), "UTF-8"))) {
            r.readLine();
            for (String line; (line = r.readLine()) != null; ) {
                String[] cols = line.split(",");
                long vt = Long.decode(cols[0]);
                int n = Integer.parseInt(cols[cols.length - 2]);
                for (int i = 0; i < n; i++) {
                    slots++;
                    long t = getInt(toAddr(vt + 12 + 8L * i)) & 0xFFFFFFFFL;
                    if (getFunctionAt(toAddr(t)) != null) slotFuncs++;
                }
            }
        }
        println(String.format("functions in program=%d", currentProgram.getFunctionManager().getFunctionCount()));
        println(String.format("symbols.txt func lines=%d are functions=%d with matching name=%d", funcLines, isFunc, nameOk));
        println(String.format("vtable slots=%d pointing at a function=%d", slots, slotFuncs));
        for (long a : new long[] { 0x029CEF0CL, 0x029CE290L, 0x02D63178L, 0x03122C18L }) {
            Function fn = getFunctionAt(toAddr(a));
            println(String.format("  %08X -> %s", a, fn == null ? "(none)" : fn.getName(true)));
        }
    }
}
