// Import the recovered structs from decomp/include into the program (DM-040).
//
// Reads <includeDir>/ghidra_types.txt: header names (parsed in that order, as one translation
// unit) and "size <Type> <bytes>" checks. Preprocessor lines and the text between
// "GHIDRA-SKIP-BEGIN" and "GHIDRA-SKIP-END" are dropped. The headers are parsed with Ghidra's
// CParser into a scratch data-type manager (base types such as uint8_t resolve from the program),
// then every declared type is added to the program under /dmbmx with the REPLACE conflict
// handler, so re-running updates the types in place and keeps their uses. Fails (printerr +
// exception) on a parse error or a size mismatch.
//
// Usage (headless): -postScript ImportTypes.java <path\to\decomp\include>
//@category DMBMX

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.app.util.cparser.C.CParser;
import ghidra.program.model.data.*;

public class ImportTypes extends GhidraScript {

    static final CategoryPath CAT = new CategoryPath("/dmbmx");

    static String strip(String text) {
        StringBuilder sb = new StringBuilder();
        boolean skip = false;
        for (String l : text.split("\r?\n", -1)) {
            if (l.contains("GHIDRA-SKIP-BEGIN")) { skip = true; continue; }
            if (l.contains("GHIDRA-SKIP-END")) { skip = false; continue; }
            if (skip || l.trim().startsWith("#")) { sb.append('\n'); continue; }
            sb.append(l).append('\n');
        }
        return sb.toString();
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) { printerr("usage: ImportTypes.java <decomp/include dir>"); return; }
        File dir = new File(args[0]);
        List<String> headers = new ArrayList<>();
        Map<String, Integer> sizes = new LinkedHashMap<>();
        for (String l : Files.readAllLines(new File(dir, "ghidra_types.txt").toPath(), StandardCharsets.UTF_8)) {
            l = l.trim();
            if (l.isEmpty() || l.startsWith("#")) continue;
            if (l.startsWith("size ")) {
                String[] p = l.split("\\s+");
                sizes.put(p[1], Integer.decode(p[2]));
            } else headers.add(l);
        }
        StringBuilder src = new StringBuilder();
        for (String h : headers)
            src.append(strip(new String(Files.readAllBytes(new File(dir, h).toPath()), StandardCharsets.UTF_8)));

        DataTypeManager prog = currentProgram.getDataTypeManager();
        StandAloneDataTypeManager scratch = new StandAloneDataTypeManager("dmbmx-import");
        int tx = scratch.startTransaction("parse");
        Map<String, DataType> declared = new LinkedHashMap<>();
        try {
            CParser parser = new CParser(scratch, true, new DataTypeManager[] { prog });
            parser.parse(src.toString());
            if (!parser.didParseSucceed()) throw new Exception("parse failed: " + parser.getParseMessages());
            declared.putAll(parser.getComposites());
            declared.putAll(parser.getTypes());
        } finally {
            scratch.endTransaction(tx, true);
        }
        int n = 0;
        for (Map.Entry<String, DataType> e : declared.entrySet()) {
            DataType dt = e.getValue();
            if (dt.getDataTypeManager() != scratch) continue;   // resolved from the program (uint8_t ...)
            DataType copy = dt.copy(prog);
            copy.setCategoryPath(CAT);
            prog.addDataType(copy, DataTypeConflictHandler.REPLACE_HANDLER);
            n++;
        }
        scratch.close();
        int bad = 0;
        for (Map.Entry<String, Integer> e : sizes.entrySet()) {
            DataType dt = prog.getDataType(CAT, e.getKey());
            int len = dt == null ? -1 : dt.getLength();
            if (len != e.getValue()) {
                printerr(String.format("ImportTypes: size of %s is %s, expected 0x%x", e.getKey(),
                    len < 0 ? "missing" : String.format("0x%x", len), e.getValue()));
                bad++;
            }
        }
        println(String.format("ImportTypes: %d types from %d headers into %s, %d size checks, %d mismatches",
            n, headers.size(), CAT, sizes.size(), bad));
        if (bad > 0) throw new Exception("ImportTypes: size mismatch");
    }
}
