// Histogram of the instruction shapes leading up to every bctr/bctrl in .text (disassembling gaps
// in memory first; nothing is saved with -readOnly), to see how GHS emits switches and indirect calls.
// Args: [windowSize] [samplesPerShape]
// @category MH3U
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.*;

public class ProbeBctr extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        int win = args.length > 0 ? Integer.parseInt(args[0]) : 5;
        int samples = args.length > 1 ? Integer.parseInt(args[1]) : 2;
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        long s = text.getStart().getOffset(), e = text.getEnd().getOffset() + 1;
        Listing listing = currentProgram.getListing();
        int disas = 0;
        for (long a = s; a < e; a += 4) {
            int w = getInt(toAddr(a));
            if ((w == 0x4E800420 || w == 0x4E800421) && listing.getInstructionAt(toAddr(a)) == null) {
                // disassemble backwards window too: find previous terminator and disassemble from after it
                long b = a - 4;
                while (b > s && a - b < 0x400) {
                    int pw = getInt(toAddr(b));
                    if (pw == 0x4E800020 || listing.getInstructionAt(toAddr(b)) != null) break;
                    b -= 4;
                }
                disassemble(toAddr(b + 4));
                disas++;
            }
        }
        Map<String, Integer> hist = new TreeMap<>();
        Map<String, List<String>> ex = new HashMap<>();
        int bctr = 0, bctrl = 0, withComputed = 0;
        for (long a = s; a < e; a += 4) {
            int w = getInt(toAddr(a));
            if (w != 0x4E800420 && w != 0x4E800421) continue;
            Instruction ins = listing.getInstructionAt(toAddr(a));
            if (ins == null) continue;
            if (w == 0x4E800420) bctr++; else bctrl++;
            boolean computed = false;
            for (Reference r : ins.getReferencesFrom()) if (r.getReferenceType() == RefType.COMPUTED_JUMP) computed = true;
            if (computed) withComputed++;
            StringBuilder shape = new StringBuilder(w == 0x4E800420 ? "J " : "C ");
            Instruction p = ins;
            List<String> ctx = new ArrayList<>();
            for (int i = 0; i < win && p.getPrevious() != null; i++) {
                p = p.getPrevious();
                ctx.add(0, p.toString());
            }
            for (String c : ctx) shape.append(c.split(" ")[0]).append(' ');
            shape.append(computed ? "[computed]" : "");
            String k = shape.toString();
            hist.merge(k, 1, Integer::sum);
            List<String> l = ex.computeIfAbsent(k, x -> new ArrayList<>());
            if (l.size() < samples) l.add(String.format("%08X: %s", a, String.join(" ; ", ctx)));
        }
        println(String.format("disassembled windows=%d bctr=%d bctrl=%d with COMPUTED_JUMP refs=%d", disas, bctr, bctrl, withComputed));
        hist.entrySet().stream().sorted((x, y) -> y.getValue() - x.getValue()).limit(40).forEach(en -> {
            println(String.format("%6d  %s", en.getValue(), en.getKey()));
            for (String x : ex.get(en.getKey())) println("          " + x);
        });
    }
}
