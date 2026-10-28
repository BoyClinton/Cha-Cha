// Writes one line per distinct instruction word in .text: "hexword<TAB>mnemonic<TAB>operands" as Ghidra
// disassembles it. Used by scripts/check_decoder.py to cross-check the recompiler's Espresso decoder.
// Args: <out.tsv>
// @category MH3U
import java.io.*;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;

public class ExportMnemonics extends GhidraScript {
    @Override
    public void run() throws Exception {
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        Set<Integer> seen = new HashSet<>();
        int n = 0;
        try (Writer w = new BufferedWriter(new OutputStreamWriter(new FileOutputStream(getScriptArgs()[0]), "UTF-8"))) {
            for (Instruction ins : currentProgram.getListing().getInstructions(text.getStart(), true)) {
                if (!text.contains(ins.getAddress())) break;
                int word = getInt(ins.getAddress());
                n++;
                if (!seen.add(word)) continue;
                StringBuilder ops = new StringBuilder();
                for (int i = 0; i < ins.getNumOperands(); i++) ops.append(i == 0 ? "" : ",").append(ins.getDefaultOperandRepresentation(i));
                w.write(String.format("%08X\t%s\t%s\n", word, ins.getMnemonicString(), ops));
            }
        }
        println(String.format("instructions=%d distinct words=%d", n, seen.size()));
    }
}
