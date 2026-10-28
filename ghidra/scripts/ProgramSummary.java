// Prints language, memory blocks, function/import counts and the address range of
// unresolved instructions. Run headless with -process <prog> -noanalysis -readOnly.
// @category MH3U
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.*;

public class ProgramSummary extends GhidraScript {
    @Override
    public void run() throws Exception {
        println("language=" + currentProgram.getLanguageID() + " cspec=" + currentProgram.getCompilerSpec().getCompilerSpecID());
        println("image_base=" + currentProgram.getImageBase());
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            println(String.format("block %-12s %s-%s %s%s%s %s", b.getName(), b.getStart(), b.getEnd(),
                b.isRead() ? "r" : "-", b.isWrite() ? "w" : "-", b.isExecute() ? "x" : "-",
                b.isExternalBlock() ? "external" : ""));
        }
        FunctionManager fm = currentProgram.getFunctionManager();
        int ext = 0;
        for (Function f : fm.getExternalFunctions()) ext++;
        println("functions=" + fm.getFunctionCount() + " external_functions=" + ext);
        ExternalManager em = currentProgram.getExternalManager();
        for (String lib : em.getExternalLibraryNames()) {
            int n = 0;
            ExternalLocationIterator it = em.getExternalLocations(lib);
            while (it.hasNext()) { it.next(); n++; }
            println("import " + lib + " " + n);
        }
        long paired = 0;
        InstructionIterator ii = currentProgram.getListing().getInstructions(true);
        while (ii.hasNext()) {
            String m = ii.next().getMnemonicString();
            if (m.startsWith("ps_") || m.startsWith("psq_")) paired++;
        }
        println("paired_single_instructions=" + paired);
    }
}
