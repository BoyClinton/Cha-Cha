// Locates the static-constructor table: computes the start of a few DTI static initialisers (code after the
// previous blr/b), searches data for words pointing at them, and measures the run of .text pointers there.
// @category MH3U
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.Reference;

public class FindCtorTable extends GhidraScript {
    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        MemoryBlock text = mem.getBlock(".text");
        int found = 0;
        for (Reference r : getReferencesTo(toAddr(0x02d63178L))) {
            if (getFunctionContaining(r.getFromAddress()) != null) continue;
            Address start = initStart(r.getFromAddress());
            byte[] pat = new byte[] { (byte) (start.getOffset() >> 24), (byte) (start.getOffset() >> 16),
                                      (byte) (start.getOffset() >> 8), (byte) start.getOffset() };
            for (MemoryBlock b : mem.getBlocks()) {
                if (!b.isInitialized() || b == text || b.isExecute()) continue;
                Address hit = mem.findBytes(b.getStart(), b.getEnd(), pat, null, true, monitor);
                if (hit == null) continue;
                // measure the contiguous run of words pointing into .text around the hit
                Address lo = hit, hi = hit;
                while (text.contains(toAddr(getInt(lo.subtract(4)) & 0xFFFFFFFFL))) lo = lo.subtract(4);
                while (text.contains(toAddr(getInt(hi.add(4)) & 0xFFFFFFFFL))) hi = hi.add(4);
                println(String.format("init %s -> pointer at %s in %s; table run %s..%s (%d entries)",
                    start, hit, b.getName(), lo, hi, (hi.subtract(lo)) / 4 + 1));
                found++;
            }
            if (found >= 4) break;
        }
    }

    private Address initStart(Address at) {
        Instruction ins = getInstructionAt(at);
        for (int i = 0; i < 400; i++) {
            Instruction prev = ins.getPrevious();
            if (prev == null || prev.getMnemonicString().equals("blr") || prev.getMnemonicString().equals("b")
                    || !prev.getMaxAddress().add(1).equals(ins.getAddress())) break;
            ins = prev;
        }
        return ins.getAddress();
    }
}
