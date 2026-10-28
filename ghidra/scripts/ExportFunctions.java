// Exports function boundaries for the recompiler and checks how well they tile .text.
// Writes <out.csv>: start,end,name,status,chunks
//   start = entry, end = end of the body range holding the entry (exclusive); chunks = every other body
//   range (including code laid out before the entry) as "start-end" pairs, space-separated, end exclusive.
// Prints: overlaps, gaps in .text (zero padding / undisassembled / instructions outside functions), and
// branches that enter another function anywhere but its entry (the recompiler can only tail-call entries).
// Args: <out.csv>
// @category MH3U
import java.io.*;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;

public class ExportFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        long tStart = text.getStart().getOffset(), tEnd = text.getEnd().getOffset() + 1;
        Listing listing = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();

        List<long[]> ranges = new ArrayList<>();
        int count = 0, multi = 0, outside = 0, chunkCount = 0;
        StringBuilder csv = new StringBuilder("start,end,name,status,chunks\n");
        for (Function fn : fm.getFunctions(true)) {
            if (fn.isExternal()) continue;
            long s = fn.getEntryPoint().getOffset();
            if (s < tStart || s >= tEnd) { outside++; continue; }
            count++;
            AddressSetView body = fn.getBody();
            AddressRange first = body.getRangeContaining(fn.getEntryPoint());
            long e = first.getMaxAddress().getOffset() + 1;
            StringBuilder chunks = new StringBuilder();
            for (AddressRange r : body.getAddressRanges()) {
                long rs = r.getMinAddress().getOffset(), re = r.getMaxAddress().getOffset() + 1;
                ranges.add(new long[] { rs, re });
                if (r.equals(first)) {
                    if (rs == s) continue;
                    re = s;  // part of the entry range that lies before the entry
                }
                chunkCount++;
                chunks.append(chunks.length() == 0 ? "" : " ").append(String.format("%08X-%08X", rs, re));
            }
            if (chunks.length() > 0) multi++;
            String name = fn.getName(true).replace(" ", "");
            if (name.contains(",") || name.contains("\"")) name = "\"" + name.replace("\"", "\"\"") + "\"";
            csv.append(String.format("0x%08X,0x%08X,%s,recomp,%s\n", s, e, name, chunks));
        }
        try (Writer w = new OutputStreamWriter(new FileOutputStream(out), "UTF-8")) { w.write(csv.toString()); }
        println(String.format("functions in .text=%d (entry outside .text=%d) with chunks=%d chunks=%d", count, outside, multi, chunkCount));

        // Overlaps and gaps over all body ranges.
        ranges.sort(Comparator.comparingLong(r -> r[0]));
        long cursor = tStart, gapBytes = 0, padBytes = 0, undisBytes = 0, codeBytes = 0;
        int overlaps = 0, gaps = 0;
        List<String> gapSamples = new ArrayList<>(), ovSamples = new ArrayList<>();
        for (long[] r : ranges) {
            if (r[0] < cursor) {
                overlaps++;
                if (ovSamples.size() < 15) ovSamples.add(String.format("%08X starts inside previous range ending %08X", r[0], cursor));
            } else if (r[0] > cursor) {
                gaps++;
                gapBytes += r[0] - cursor;
                long pad = 0, undis = 0, code = 0;
                for (long a = cursor; a < r[0]; a += 4) {
                    if (listing.getInstructionAt(toAddr(a)) != null) code += 4;
                    else if (getInt(toAddr(a)) == 0) pad += 4;
                    else undis += 4;
                }
                padBytes += pad; undisBytes += undis; codeBytes += code;
                if (gapSamples.size() < 20) gapSamples.add(String.format("%08X-%08X len=%d code=%d undis=%d pad=%d", cursor, r[0], r[0] - cursor, code, undis, pad));
            }
            cursor = Math.max(cursor, r[1]);
        }
        if (cursor < tEnd) gapBytes += tEnd - cursor;
        println(String.format(".text %08X-%08X size=%d", tStart, tEnd, tEnd - tStart));
        println(String.format("overlaps=%d gaps=%d gapBytes=%d (zero pad=%d, undisassembled nonzero=%d, instructions outside functions=%d)",
                overlaps, gaps, gapBytes, padBytes, undisBytes, codeBytes));
        for (String s : ovSamples) println("  overlap: " + s);
        for (String s : gapSamples) println("  gap: " + s);

        // Cross-function control flow that does not land on an entry.
        Map<String, Integer> bad = new TreeMap<>();
        List<String> badSamples = new ArrayList<>();
        Set<Long> badTargets = new TreeSet<>();
        for (Instruction ins : listing.getInstructions(text.getStart(), true)) {
            if (ins.getAddress().getOffset() >= tEnd) break;
            if (!ins.getFlowType().isJump() && !ins.getFlowType().isCall()) continue;
            Function from = fm.getFunctionContaining(ins.getAddress());
            for (Address t : ins.getFlows()) {
                if (!text.contains(t)) continue;
                Function to = fm.getFunctionContaining(t);
                if (to == null || to == from || to.getEntryPoint().equals(t)) continue;
                String k = ins.getMnemonicString();
                bad.merge(k, 1, Integer::sum);
                badTargets.add(t.getOffset());
                if (badSamples.size() < 20) badSamples.add(String.format("%s %s -> %s inside %s", ins.getAddress(), ins, t, to.getName()));
            }
        }
        println(String.format("branches into the middle of another function: %s (distinct targets=%d)", bad, badTargets.size()));
        for (String s : badSamples) println("  mid: " + s);
    }
}
