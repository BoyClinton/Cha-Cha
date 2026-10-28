// Lists references to each address: from-address, ref type, containing function. "entry" lists the
// program's external entry points instead.
// Args: <hexaddr>[:max] ... | entry      e.g. 02005404 103cea44:20 entry
// @category MH3U
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class XRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String arg : getScriptArgs()) {
            if (arg.equals("entry")) {
                for (Address a : currentProgram.getSymbolTable().getExternalEntryPointIterator()) {
                    Function f = getFunctionAt(a);
                    println("entry " + a + " " + (f == null ? "" : f.getName(true)));
                }
                continue;
            }
            String[] p = arg.split(":");
            int max = p.length > 1 ? Integer.parseInt(p[1]) : 50;
            Address a = toAddr(Long.parseLong(p[0], 16));
            Reference[] refs = getReferencesTo(a);
            Function self = getFunctionAt(a);
            println(String.format("== %s %s refs=%d", a, self == null ? "" : self.getName(true), refs.length));
            for (int i = 0; i < refs.length && i < max; i++) {
                Function f = getFunctionContaining(refs[i].getFromAddress());
                println(String.format("   %s %-14s %s", refs[i].getFromAddress(), refs[i].getReferenceType(),
                        f == null ? "(no function)" : f.getName(true) + " @" + f.getEntryPoint()));
            }
        }
    }
}
