// Resolves runtime imports made through OSDynLoad.
//   OSDynLoad_Acquire(const char *module /*r3*/, OSDynLoad_Module *out /*r4*/)
//   OSDynLoad_FindExport(OSDynLoad_Module m /*r3*/, BOOL isData /*r4*/, const char *name /*r5*/, void **out /*r6*/)
// MH3U calls FindExport only through tiny wrappers of the form
//   wrapper(void **out /*r3*/, const char *name /*r4*/) { return FindExport(*handleGlobal, isData, name, out); }
// so for each wrapper we read handleGlobal/isData from its body, then constant-propagate every caller of
// the wrapper to recover name (r4) and out (r3). The decompiler can't be used: the stub branch targets
// (0x04xxxxxx) don't map to memory, which breaks p-code for these functions.
// Args: [details.csv] [imports.csv]
//   details.csv  module_handle,module,export,kind,out_ptr,wrapper,call_site (overwritten)
//   imports.csv  appends rows in config/imports.csv format; address = function-pointer slot,
//                refs = references to that slot. Run after ExportImports.java, which rewrites the file.
// @category MH3U
import java.io.PrintWriter;
import java.math.BigInteger;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.*;
import ghidra.program.util.SymbolicPropogator;
import ghidra.app.plugin.core.analysis.ConstantPropagationContextEvaluator;

public class ResolveDynLoad extends GhidraScript {
    private final Map<Function, SymbolicPropogator> props = new HashMap<>();

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Address acquire = findStub("OSDynLoad_Acquire");
        Address findExport = findStub("OSDynLoad_FindExport");

        // 1. Module handles: Acquire(name, &handle)
        Map<Long, String> handleModule = new TreeMap<>();
        for (Reference r : getReferencesTo(acquire)) {
            Address call = r.getFromAddress();
            Long name = reg(call, "r3"), out = reg(call, "r4");
            String mod = name == null ? null : readString(name);
            println(String.format("acquire call=%s module=%s handle=%s", call, mod, hex(out)));
            if (out != null) handleModule.put(out, mod);
        }

        // 2. Wrappers around FindExport, then their callers.
        List<String> rows = new ArrayList<>();
        List<String> importRows = new ArrayList<>();
        ReferenceManager rm = currentProgram.getReferenceManager();
        int unresolved = 0;
        for (Reference r : getReferencesTo(findExport)) {
            Function w = getFunctionContaining(r.getFromAddress());
            Long handle = loadedGlobal(w, "r3");
            Long isData = reg(r.getFromAddress(), "r4");
            String mod = handle == null ? "?" : handleModule.getOrDefault(handle, "?");
            String kind = isData == null ? "dyn?" : (isData == 0 ? "dynfunc" : "dyndata");
            int callers = 0;
            for (Reference cr : getReferencesTo(w.getEntryPoint())) {
                if (!cr.getReferenceType().isCall()) continue;
                callers++;
                Address call = cr.getFromAddress();
                Long name = reg(call, "r4"), out = reg(call, "r3");
                String exp = name == null ? null : readString(name);
                if (exp == null) unresolved++;
                else importRows.add(String.format("%s,%s,%s,%s,%d,todo", mod, exp, kind, hex(out),
                    out == null ? 0 : rm.getReferenceCountTo(toAddr(out))));
                rows.add(String.format("%s,%s,%s,%s,%s,%s,%s", hex(handle), mod, exp == null ? "UNRESOLVED" : exp,
                    kind, hex(out), w.getEntryPoint(), call));
            }
            if (callers == 0) {
                unresolved++;
                rows.add(String.format("%s,%s,NO_CALLERS,%s,,%s,", hex(handle), mod, kind, w.getEntryPoint()));
            }
        }
        rows.sort(null);
        for (String row : rows) println(row);
        println("wrappers=" + getReferencesTo(findExport).length + " exports=" + rows.size() + " unresolved=" + unresolved);

        if (args.length > 0) {
            try (PrintWriter pw = new PrintWriter(args[0], "UTF-8")) {
                pw.println("module_handle,module,export,kind,out_ptr,wrapper,call_site");
                for (String row : rows) pw.println(row);
            }
        }
        if (args.length > 1) {
            importRows.sort(null);
            try (PrintWriter pw = new PrintWriter(new java.io.FileWriter(args[1], true))) {
                for (String row : importRows) pw.println(row);
            }
            println("appended " + importRows.size() + " runtime imports to " + args[1]);
        }
    }

    private Address findStub(String name) {
        for (Symbol s : currentProgram.getSymbolTable().getSymbols(name)) {
            MemoryBlock b = getMemoryBlock(s.getAddress());  // null for EXTERNAL: symbols
            if (b != null && b.getName().startsWith(".fimport_")) return s.getAddress();
        }
        throw new IllegalStateException("no import stub for " + name);
    }

    // Constant value of a register just before the instruction at `at` executes.
    private Long reg(Address at, String regName) throws Exception {
        Function f = getFunctionContaining(at);
        if (f == null) return null;
        SymbolicPropogator p = props.get(f);
        if (p == null) {
            p = new SymbolicPropogator(currentProgram);
            p.flowConstants(f.getEntryPoint(), f.getBody(), new ConstantPropagationContextEvaluator(monitor, true), true, monitor);
            props.put(f, p);
        }
        Register r = currentProgram.getRegister(regName);
        SymbolicPropogator.Value v = p.getRegisterValue(at, r);
        if (v == null || v.isRegisterRelativeValue()) return null;
        return v.getValue() & 0xFFFFFFFFL;
    }

    // Address of the global loaded into `regName` by an lwz inside the function (e.g. the module handle).
    private Long loadedGlobal(Function f, String regName) {
        for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
            if (!ins.getMnemonicString().equals("lwz")) continue;
            Object[] res = ins.getResultObjects();
            if (res.length == 0 || !regName.equals(res[0].toString())) continue;
            for (Reference r : ins.getReferencesFrom()) {
                if (r.getReferenceType().isRead()) return r.getToAddress().getOffset();
            }
        }
        return null;
    }

    private String readString(long addr) {
        try {
            Address a = toAddr(addr);
            if (getMemoryBlock(a) == null) return null;
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 128; i++) {
                byte b = getByte(a.add(i));
                if (b == 0) break;
                if (b < 0x20 || b > 0x7E) return null;
                sb.append((char) b);
            }
            return sb.length() > 0 ? sb.toString() : null;
        } catch (Exception e) {
            return null;
        }
    }

    private static String hex(Long v) {
        return v == null ? "?" : String.format("0x%08X", v);
    }
}
