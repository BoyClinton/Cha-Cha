// Lists instructions whose text (as Ghidra shows it, e.g. "stfs f1,0xa4(r31)") matches a regex, with the containing
// function. Args: <regex> [start hex] [end hex]   e.g. "stfs f\d+,0xa4\(r\d+\)" 02a00000 02b20000
// @category MH3U
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

public class FindInsn extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Pattern p = Pattern.compile(args[0]);
        Address start = args.length > 1 ? toAddr(Long.parseLong(args[1], 16)) : currentProgram.getMinAddress();
        Address end = args.length > 2 ? toAddr(Long.parseLong(args[2], 16)) : currentProgram.getMaxAddress();
        InstructionIterator it = currentProgram.getListing().getInstructions(start, true);
        int n = 0;
        while (it.hasNext() && n < 2000) {
            Instruction i = it.next();
            if (i.getAddress().compareTo(end) > 0) break;
            String text = i.toString();
            if (!p.matcher(text).find()) continue;
            Function f = getFunctionContaining(i.getAddress());
            println(i.getAddress() + " " + (f != null ? f.getName() : "?") + "  " + text);
            ++n;
        }
    }
}
