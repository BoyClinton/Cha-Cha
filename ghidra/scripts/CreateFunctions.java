// Creates functions Ghidra's auto-analysis missed because they are only reachable through tables:
//   - static initialisers listed in the ctor table (CTOR_TABLE .. CTOR_TABLE_END, .rodata)
//   - every slot of every vtable in config/vtables.csv (includes MyDTI dtor/newInstance)
// Disassembles first where needed. Run as -preScript (writable project) so auto-analysis follows up.
// Args: <vtables.csv>
// @category MH3U
import java.io.*;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;

public class CreateFunctions extends GhidraScript {
    static final long CTOR_TABLE = 0x101632C4L, CTOR_TABLE_END = 0x1016455CL;  // inclusive, 1191 entries

    private MemoryBlock text;
    private int created, existed, inside, failed;

    @Override
    public void run() throws Exception {
        text = currentProgram.getMemory().getBlock(".text");
        Set<Long> targets = new TreeSet<>();
        for (long a = CTOR_TABLE; a <= CTOR_TABLE_END; a += 4) targets.add(u32(a));
        int ctorTargets = targets.size();

        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(getScriptArgs()[0]), "UTF-8"))) {
            r.readLine();
            for (String line; (line = r.readLine()) != null; ) {
                // vtable,class,typeinfo,slots,first_fn - class may be quoted with commas, so index from the end
                String[] cols = line.split(",");
                long vt = Long.decode(cols[0]);
                int slots = Integer.parseInt(cols[cols.length - 2]);
                for (int i = 0; i < slots; i++) targets.add(u32(vt + 12 + 8L * i));
            }
        }
        // MyDTI vtables are listed in vtables.csv too (class "X::MyDTI"), so their slots are covered.
        println("targets: ctor table " + ctorTargets + ", total distinct " + targets.size());

        for (long t : targets) {
            if (!text.contains(toAddr(t))) { failed++; continue; }
            make(toAddr(t));
        }
        println(String.format("created=%d already_function=%d inside_other_function=%d failed=%d",
            created, existed, inside, failed));
    }

    private void make(Address a) {
        if (getFunctionAt(a) != null) { existed++; return; }
        Function host = getFunctionContaining(a);
        if (host != null) {
            // An existing function swallowed this entry point (typical for code after a tail call).
            inside++;
            return;
        }
        if (getInstructionAt(a) == null && !disassemble(a)) { failed++; return; }
        if (createFunction(a, null) != null) created++;
        else failed++;
    }

    private long u32(long a) {
        try { return getInt(toAddr(a)) & 0xFFFFFFFFL; } catch (Exception e) { return 0; }
    }
}
