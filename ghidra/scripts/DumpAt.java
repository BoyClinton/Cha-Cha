// Dumps instructions of the function at each address, a window of instructions, or words of data.
// Args: f:<hexaddr>[:maxInstr] | i:<hexaddr>:<before>:<after> | w:<hexaddr>:<count>
//   e.g. f:02d63178  i:020df378:24:4  w:10095288:16
// @category MH3U
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class DumpAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String arg : getScriptArgs()) {
            String[] p = arg.split(":");
            Address a = toAddr(Long.parseLong(p[1], 16));
            if (p[0].equals("i")) {
                int before = Integer.parseInt(p[2]), after = Integer.parseInt(p[3]);
                println("== window @ " + a);
                Instruction ins = getInstructionContaining(a);
                if (ins == null) {  // code only reachable via vtables/tables was never disassembled
                    disassemble(a);
                    ins = getInstructionContaining(a);
                    println("   (disassembled on demand)");
                }
                if (ins == null) { println("   (no instruction)"); continue; }
                for (int i = 0; i < before && ins.getPrevious() != null; i++) ins = ins.getPrevious();
                for (int i = 0; i < before + after + 1 && ins != null; i++, ins = ins.getNext()) {
                    StringBuilder r = new StringBuilder();
                    for (Reference rr : ins.getReferencesFrom()) r.append(" ->").append(rr.getToAddress());
                    println("   " + ins.getAddress() + "  " + ins + r);
                }
            } else if (p[0].equals("f")) {
                int max = p.length > 2 ? Integer.parseInt(p[2]) : 80;
                Function f = getFunctionContaining(a);
                println("== function " + (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()));
                Instruction ins = getInstructionAt(f == null ? a : f.getEntryPoint());
                for (int i = 0; i < max && ins != null; i++, ins = ins.getNext()) {
                    if (f != null && !f.getBody().contains(ins.getAddress())) break;
                    StringBuilder r = new StringBuilder();
                    for (Reference rr : ins.getReferencesFrom()) r.append(" ->").append(rr.getToAddress());
                    println("   " + ins.getAddress() + "  " + ins + r);
                }
            } else {
                int n = Integer.parseInt(p[2]);
                println("== words @ " + a);
                for (int i = 0; i < n; i++) {
                    Address x = a.add(4L * i);
                    int v = getInt(x);
                    Address t = toAddr(v & 0xFFFFFFFFL);
                    Function f = getFunctionAt(t);
                    Symbol s = getSymbolAt(t);
                    println(String.format("   %s  %08x  %s", x, v, f != null ? "func " + f.getName() : (s != null ? s.getName() : "")));
                }
            }
        }
    }
}
