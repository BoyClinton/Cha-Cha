// Applies config/symbols.txt to the program. C++-style names are split into namespaces at top-level "::"
// (not inside template brackets): "rTexture::vf7" -> function vf7 in namespace rTexture,
// "MtTypedArray<nID::cPadIcon>::__vtbl" -> label __vtbl in namespace "MtTypedArray<nID::cPadIcon>".
// func lines rename (creating the function if needed); data/vtable lines become primary labels.
// Uses SourceType.IMPORTED so analysis never overwrites them. Re-running is harmless.
// Args: <symbols.txt>
// @category MH3U
import java.io.*;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.*;

public class ApplySymbols extends GhidraScript {
    private final Map<String, Namespace> nsCache = new HashMap<>();

    @Override
    public void run() throws Exception {
        int funcs = 0, labels = 0, createdFn = 0, failed = 0;
        SymbolTable st = currentProgram.getSymbolTable();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(getScriptArgs()[0]), "UTF-8"))) {
            for (String line; (line = r.readLine()) != null; ) {
                if (line.startsWith("#") || line.isBlank()) continue;
                String[] f = line.split("\\s+", 4);
                Address a = toAddr(Long.decode(f[0]));
                List<String> parts = split(f[1]);
                String leaf = parts.remove(parts.size() - 1);
                try {
                    Namespace ns = namespace(parts);
                    if (f[2].equals("func")) {
                        Function fn = getFunctionAt(a);
                        if (fn == null) {
                            if (getInstructionAt(a) == null) disassemble(a);
                            fn = createFunction(a, null);
                            if (fn != null) createdFn++;
                        }
                        if (fn == null) { failed++; continue; }
                        fn.setParentNamespace(ns);
                        fn.setName(leaf, SourceType.IMPORTED);
                        funcs++;
                    } else {
                        Symbol s = st.createLabel(a, leaf, ns, SourceType.IMPORTED);
                        s.setPrimary();
                        labels++;
                    }
                } catch (Exception e) {
                    if (failed++ < 10) println("failed " + line + " : " + e.getMessage());
                }
            }
        }
        println(String.format("functions named=%d (created %d), labels=%d, failed=%d, namespaces=%d",
            funcs, createdFn, labels, failed, nsCache.size()));
    }

    private Namespace namespace(List<String> parts) throws Exception {
        Namespace ns = currentProgram.getGlobalNamespace();
        StringBuilder key = new StringBuilder();
        for (String p : parts) {
            key.append("::").append(p);
            Namespace cached = nsCache.get(key.toString());
            if (cached == null) {
                Namespace existing = currentProgram.getSymbolTable().getNamespace(p, ns);
                cached = existing != null ? existing
                    : currentProgram.getSymbolTable().createNameSpace(ns, p, SourceType.IMPORTED);
                nsCache.put(key.toString(), cached);
            }
            ns = cached;
        }
        return ns;
    }

    // Split at "::" outside <...>
    private static List<String> split(String name) {
        List<String> out = new ArrayList<>();
        int depth = 0, start = 0;
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if (c == '<') depth++;
            else if (c == '>') depth--;
            else if (depth == 0 && c == ':' && i + 1 < name.length() && name.charAt(i + 1) == ':') {
                out.add(name.substring(start, i));
                start = i + 2;
                i++;
            }
        }
        out.add(name.substring(start));
        return out;
    }
}
