// Writes the (relocated) bytes of .text, .rodata and .data as <dir>/<name>.bin, e.g. to compare the recompiler's
// loader output with Ghidra's memory image (recomp/tests/loader_check.cpp).
// Args: <dir>
// @category MH3U
import java.io.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.MemoryBlock;

public class ExportBlocks extends GhidraScript {
    @Override
    public void run() throws Exception {
        File dir = new File(getScriptArgs()[0]);
        dir.mkdirs();
        for (String name : new String[] { ".text", ".rodata", ".data" }) {
            MemoryBlock b = currentProgram.getMemory().getBlock(name);
            byte[] bytes = new byte[(int) b.getSize()];
            b.getBytes(b.getStart(), bytes);
            try (OutputStream o = new FileOutputStream(new File(dir, name.substring(1) + ".bin"))) { o.write(bytes); }
            println(String.format("%s %s size=0x%X", name, b.getStart(), bytes.length));
        }
    }
}
