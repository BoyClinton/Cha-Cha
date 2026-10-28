// Writes every RPL import (function stubs in .fimport_*, data slots in .dimport_*) to a CSV:
//   rpl,function,kind,address,refs,hle_status
// refs = number of references to the stub/slot from game code (rough "how much it's used").
// Arg: output path. Run headless with -process <prog> -noanalysis -readOnly.
// @category MH3U
import java.io.PrintWriter;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.*;

public class ExportImports extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) throw new IllegalArgumentException("usage: ExportImports.java <out.csv>");

        SymbolTable st = currentProgram.getSymbolTable();
        ReferenceManager rm = currentProgram.getReferenceManager();
        List<String> rows = new ArrayList<>();

        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            String name = b.getName();
            String kind;
            if (name.startsWith(".fimport_")) kind = "func";
            else if (name.startsWith(".dimport_")) kind = "data";
            else continue;
            String rpl = name.substring(".fimport_".length());
            if (!rpl.endsWith(".rpl")) rpl += ".rpl";

            SymbolIterator it = st.getPrimarySymbolIterator(b.getStart(), true);
            while (it.hasNext()) {
                Symbol s = it.next();
                Address a = s.getAddress();
                if (a.compareTo(b.getEnd()) > 0) break;
                int refs = rm.getReferenceCountTo(a);
                rows.add(String.format("%s,%s,%s,0x%08X,%d,todo", rpl, s.getName(), kind, a.getOffset(), refs));
            }
        }
        rows.sort(null);

        try (PrintWriter w = new PrintWriter(args[0], "UTF-8")) {
            w.println("rpl,function,kind,address,refs,hle_status");
            for (String r : rows) w.println(r);
        }
        println("wrote " + rows.size() + " imports to " + args[0]);
    }
}
