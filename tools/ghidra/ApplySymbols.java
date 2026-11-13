// Apply decomp/symbols/symbols.csv to the current program (AD-6). Idempotent.
//
// CSV columns: address,kind,name,type,source_file,confidence,card
//   address     0x%08x VA (lowercase)
//   kind        func | data | label | struct | nofunc
//   name        symbol name ("" = keep the current name, only update the comment/tags)
//   type        func: optional C prototype, parameters separated by ',' or ';' (';' avoids CSV quoting);
//               data: optional data type ("int", "char[16]", "uint8_t[0x1680]", ...). A type that does not
//               parse (e.g. a struct not yet in the program) is reported as a warning and skipped.
//   source_file original source path relative to the source root (e.g. Bmx/Src/com/action/memcard.c),
//               or lib:<library> (lib/<library> also accepted) for statically linked library code
//   confidence  V | I
//   card        DM-xxx that added the row
//
// Duplicate addresses: the LAST row for an address wins (rows from any card are accepted).
// struct rows are keyed by their name instead (their address column is not used; write 0x00000000).
// For each row the script sets the name (USER_DEFINED), creates the function if needed, writes a
// one-line "[symbols.csv] ..." plate comment (replacing an earlier one, other plate text is kept)
// and, for lib:* source files, a function tag LIB_<library>. struct rows (name = type, type = header)
// check that ImportTypes.java imported the type. A data row inside data that is already typed
// (e.g. a field of a struct global) only gets its label.
// nofunc rows (DM-064) remove a bogus function that Ghidra's analysis made inside another one (e.g. a
// switch case reached by a jump table): the function at the address is deleted, the function that
// contains the address before it gets its body recomputed (so it takes the code back), and the name
// stays as a code label. A nofunc row for an address with no function is a no-op (idempotent).
// Functions that carry a "[symbols.csv]" note but have no row any more are pruned (note and LIB_
// tags removed, a generated "<file>_<va>" name reset to the default), so Ghidra follows the CSV.
//
// Usage (headless): -postScript ApplySymbols.java <path\to\symbols.csv>
//@category DMBMX

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.app.util.cparser.C.CParser;
import ghidra.app.util.cparser.C.CParserUtils;
import ghidra.app.services.DataTypeManagerService;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.util.data.DataTypeParser;

public class ApplySymbols extends GhidraScript {

    static final String HEADER = "address,kind,name,type,source_file,confidence,card";
    static final String TAG_PREFIX = "[symbols.csv] ";

    static List<String> parseCsvLine(String line) {
        List<String> out = new ArrayList<>();
        StringBuilder cur = new StringBuilder();
        boolean q = false;
        for (int i = 0; i < line.length(); i++) {
            char c = line.charAt(i);
            if (q) {
                if (c == '"') {
                    if (i + 1 < line.length() && line.charAt(i + 1) == '"') { cur.append('"'); i++; }
                    else q = false;
                } else cur.append(c);
            } else if (c == '"') q = true;
            else if (c == ',') { out.add(cur.toString()); cur.setLength(0); }
            else cur.append(c);
        }
        out.add(cur.toString());
        return out;
    }

    int nFunc, nData, nLabel, nStruct, nCreated, nRenamed, nUnchanged, nErr, nTypeWarn, nNested, nRemoved;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("usage: ApplySymbols.java <symbols.csv>");
            return;
        }
        File csv = new File(args[0]);
        List<String> lines = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(csv), StandardCharsets.UTF_8))) {
            String l;
            while ((l = r.readLine()) != null) lines.add(l);
        }
        if (lines.isEmpty() || !lines.get(0).replace("﻿", "").trim().equals(HEADER)) {
            printerr("bad header in " + csv + " (expected: " + HEADER + ")");
            return;
        }
        // last row per address wins
        LinkedHashMap<Long, List<String>> rows = new LinkedHashMap<>();
        LinkedHashMap<String, List<String>> structs = new LinkedHashMap<>();
        int dup = 0;
        for (int i = 1; i < lines.size(); i++) {
            String l = lines.get(i);
            if (l.trim().isEmpty() || l.startsWith("#")) continue;
            List<String> f = parseCsvLine(l);
            while (f.size() < 7) f.add("");
            long a;
            try { a = Long.decode(f.get(0).trim()); }
            catch (NumberFormatException e) { printerr("line " + (i + 1) + ": bad address " + f.get(0)); nErr++; continue; }
            if (f.get(1).trim().equals("struct")) {   // struct rows are keyed by type name, not address
                if (structs.remove(f.get(2).trim()) != null) dup++;
                structs.put(f.get(2).trim(), f);
                continue;
            }
            if (rows.remove(a) != null) dup++;
            rows.put(a, f);
        }
        println("ApplySymbols: " + rows.size() + " addresses, " + structs.size() + " structs (" + dup + " duplicate rows superseded)");
        for (List<String> f : structs.values()) { applyStruct(null, f.get(2).trim(), f.get(3).trim()); nStruct++; }

        for (Map.Entry<Long, List<String>> e : rows.entrySet()) {
            monitor.checkCancelled();
            List<String> f = e.getValue();
            Address addr = toAddr(e.getKey());
            String kind = f.get(1).trim(), name = f.get(2).trim(), type = f.get(3).trim();
            String src = f.get(4).trim(), conf = f.get(5).trim(), card = f.get(6).trim();
            try {
                switch (kind) {
                    case "func": applyFunc(addr, name, type, src, conf, card); nFunc++; break;
                    case "data": applyData(addr, name, type, src, conf, card); nData++; break;
                    case "label": applyLabel(addr, name, src, conf, card, CommentType.EOL); nLabel++; break;
                    case "nofunc": removeFunc(addr, name, src, conf, card); break;
                    default: printerr(addr + ": unknown kind '" + kind + "'"); nErr++;
                }
            } catch (Exception ex) {
                printerr(addr + " " + name + ": " + ex);
                nErr++;
            }
        }
        // Prune: functions that carry our note but no longer have a row (e.g. an inference that
        // was withdrawn). Remove the note and LIB_ tags; reset a generated "<file>_<va>" name.
        int nPruned = 0;
        for (Function fn : currentProgram.getFunctionManager().getFunctions(true)) {
            String c = fn.getComment();
            if (c == null || !c.contains(TAG_PREFIX)) continue;
            if (rows.containsKey(fn.getEntryPoint().getOffset())) continue;
            StringBuilder keep = new StringBuilder();
            for (String l : c.split("\n", -1)) {
                if (l.startsWith(TAG_PREFIX)) continue;
                if (keep.length() > 0) keep.append('\n');
                keep.append(l);
            }
            fn.setComment(keep.toString().trim().isEmpty() ? null : keep.toString());
            for (FunctionTag t : new ArrayList<>(fn.getTags()))
                if (t.getName().startsWith("LIB_")) fn.removeTag(t.getName());
            String auto = String.format("_%08x", fn.getEntryPoint().getOffset());
            if (fn.getSymbol().getSource() == SourceType.USER_DEFINED && fn.getName().endsWith(auto)
                    && fn.getName().matches("[A-Za-z0-9]+_[0-9a-f]{8}")) {
                fn.getSymbol().setName(null, SourceType.DEFAULT);
            }
            nPruned++;
        }
        println(String.format("ApplySymbols: func=%d data=%d label=%d struct=%d nested=%d created=%d removed=%d changed=%d unchanged=%d pruned=%d type-warnings=%d errors=%d",
            nFunc, nData, nLabel, nStruct, nNested, nCreated, nRemoved, nRenamed, nUnchanged, nPruned, nTypeWarn, nErr));
    }

    /** nofunc row: delete the function at addr and give its code back to the function before it. */
    void removeFunc(Address addr, String name, String src, String conf, String card) throws Exception {
        Function fn = getFunctionAt(addr);
        if (fn == null) { nUnchanged++; return; }
        Function outer = getFunctionContaining(addr.subtract(1));
        removeFunction(fn);
        if (outer != null && !CreateFunctionCmd.fixupFunctionBody(currentProgram, outer, monitor))
            println("warning: " + addr + ": could not recompute the body of " + outer.getName());
        if (getFunctionAt(addr) != null) { printerr(addr + ": function came back after removal"); nErr++; return; }
        applyLabel(addr, name, src, conf, card, CommentType.PRE);
        nRemoved++;
        println("ApplySymbols: removed function " + addr + (outer != null ? " (body returned to " + outer.getName() + ")" : ""));
    }

    String note(String src, String conf, String card) {
        StringBuilder sb = new StringBuilder(TAG_PREFIX);
        sb.append("source_file: ").append(src.isEmpty() ? "?" : src);
        sb.append(" | confidence: ").append(conf.isEmpty() ? "?" : conf);
        sb.append(" | card: ").append(card.isEmpty() ? "?" : card);
        return sb.toString();
    }

    /** Replace our one-line note inside an existing comment, keeping other text. */
    static String mergeComment(String old, String line) {
        if (old == null || old.isEmpty()) return line;
        StringBuilder sb = new StringBuilder(line);
        for (String l : old.split("\n", -1)) {
            if (l.startsWith(TAG_PREFIX)) continue;
            sb.append('\n').append(l);
        }
        return sb.toString().replaceAll("\n+$", "");
    }

    boolean rename(Symbol sym, String name) throws Exception {
        if (name.isEmpty() || sym == null) return false;
        if (sym.getName().equals(name) && sym.getSource() == SourceType.USER_DEFINED) return false;
        try {
            sym.setName(name, SourceType.USER_DEFINED);
        } catch (ghidra.util.exception.DuplicateNameException d) {
            sym.setName(name + "_" + sym.getAddress().toString(), SourceType.USER_DEFINED);
        }
        return true;
    }

    void applyFunc(Address addr, String name, String type, String src, String conf, String card) throws Exception {
        Function fn = getFunctionAt(addr);
        if (fn == null) {
            Function containing = getFunctionContaining(addr);
            if (containing != null)
                println("note: " + addr + " is inside " + containing.getName() + "; creating a function there anyway");
            if (getInstructionAt(addr) == null) disassemble(addr);
            fn = createFunction(addr, name.isEmpty() ? null : name);
            if (fn == null) { printerr(addr + ": could not create function " + name); nErr++; return; }
            nCreated++;
        }
        boolean changed = rename(fn.getSymbol(), name);
        if (!type.isEmpty()) {
            FunctionDefinitionDataType sig = null;
            try {
                sig = CParserUtils.parseSignature((DataTypeManagerService) null, currentProgram, type.replace(';', ','), false);
            } catch (Exception ex) {
                sig = null;
            }
            if (sig == null) { println("warning: " + addr + ": prototype not applied (does not parse): " + type); nTypeWarn++; }
            else {
                sig.setName(fn.getName());
                new ApplyFunctionSignatureCmd(addr, sig, SourceType.USER_DEFINED).applyTo(currentProgram, monitor);
            }
        }
        String c = mergeComment(fn.getComment(), note(src, conf, card));
        if (!c.equals(fn.getComment())) { fn.setComment(c); changed = true; }
        // library tag
        String lib = null;
        if (src.startsWith("lib:") || src.startsWith("lib/")) {
            String l = src.substring(4).split("[/:]")[0];
            if (!l.isEmpty()) lib = "LIB_" + l;
        }
        for (FunctionTag t : new ArrayList<>(fn.getTags()))
            if (t.getName().startsWith("LIB_") && !t.getName().equals(lib)) { fn.removeTag(t.getName()); changed = true; }
        boolean hasLib = false;
        for (FunctionTag t : fn.getTags()) if (t.getName().equals(lib)) hasLib = true;
        if (lib != null && !hasLib) { fn.addTag(lib); changed = true; }
        if (changed) nRenamed++; else nUnchanged++;
    }

    void applyLabel(Address addr, String name, String src, String conf, String card, CommentType commentType) throws Exception {
        boolean changed = false;
        if (!name.isEmpty()) {
            Symbol prim = currentProgram.getSymbolTable().getPrimarySymbol(addr);
            if (prim == null || prim.getSource() == SourceType.DEFAULT) {
                Symbol s = currentProgram.getSymbolTable().createLabel(addr, name, SourceType.USER_DEFINED);
                s.setPrimary();
                changed = true;
            } else if (!prim.getName().equals(name)) {
                Symbol existing = currentProgram.getSymbolTable().getSymbol(name, addr, null);
                if (existing == null) existing = currentProgram.getSymbolTable().createLabel(addr, name, SourceType.USER_DEFINED);
                existing.setPrimary();
                changed = true;
            }
        }
        Listing listing = currentProgram.getListing();
        String old = listing.getComment(commentType, addr);
        String c = mergeComment(old, note(src, conf, card));
        if (!c.equals(old)) { listing.setComment(addr, commentType, c); changed = true; }
        if (changed) nRenamed++; else nUnchanged++;
    }

    /** Parse a data-type string: the plain parser first ("int", "ZassetLib *", "uint8_t[0x10]"),
     *  then CParser for anything C can declare ("uint8_t[12*3][0x150]", "const char *[4]",
     *  "struct { ... }[8]", "struct zfileDevice"). Returns null if neither works. */
    DataType parseType(String type) {
        DataTypeManager dtm = currentProgram.getDataTypeManager();
        try {
            DataType dt = new DataTypeParser(dtm, dtm, null, DataTypeParser.AllowedDataTypes.ALL).parse(type);
            if (dt != null) return dt;
        } catch (Exception ex) {
            // fall through to CParser
        }
        // split "<base>[dims]" at the first '[' outside braces: typedef <base> __t[dims];
        int depth = 0, cut = -1;
        for (int i = 0; i < type.length(); i++) {
            char c = type.charAt(i);
            if (c == '{') depth++;
            else if (c == '}') depth--;
            else if (c == '[' && depth == 0) { cut = i; break; }
        }
        String decl = cut < 0 ? "typedef " + type + " __dmbmx_t;"
                              : "typedef " + type.substring(0, cut) + " __dmbmx_t" + type.substring(cut) + ";";
        decl = decl.replace("struct zfileDevice", "zfileDevice");
        StandAloneDataTypeManager scratch = new StandAloneDataTypeManager("dmbmx-type");
        int tx = scratch.startTransaction("t");
        try {
            CParser p = new CParser(scratch, true, new DataTypeManager[] { dtm });
            p.parse(decl);
            DataType t = p.getTypes().get("__dmbmx_t");
            if (t instanceof TypeDef) t = ((TypeDef) t).getBaseDataType();
            return t == null ? null : dtm.resolve(t, DataTypeConflictHandler.DEFAULT_HANDLER);
        } catch (Throwable ex) {
            return null;
        } finally {
            scratch.endTransaction(tx, false);
            scratch.close();
        }
    }

    static boolean isStringType(DataType t) {
        return t instanceof AbstractStringDataType || t instanceof StringDataType
            || (t instanceof Array && ((Array) t).getDataType() instanceof CharDataType);
    }

    void applyData(Address addr, String name, String type, String src, String conf, String card) throws Exception {
        if (!type.isEmpty()) {
            Data inside = currentProgram.getListing().getDataContaining(addr);
            Data d = getDataAt(addr);
            if (inside != null && d == null && inside.isDefined() && !inside.getAddress().equals(addr)
                    && (inside.getDataType() instanceof Composite || inside.getDataType() instanceof Array)) {
                // nested in data already typed (e.g. g_riderRecords inside g_saveBlockA): label only
                nNested++;
            } else if (type.equals("char[]") || type.equals("char *[]")) {
                // a C string: keep Ghidra's string data, define one if there is none
                if (d == null || !isStringType(d.getDataType())) {
                    try {
                        DataUtilities.createData(currentProgram, addr, new TerminatedStringDataType(), -1,
                            DataUtilities.ClearDataMode.CLEAR_ALL_UNDEFINED_CONFLICT_DATA);
                    } catch (Exception ex) {
                        println("warning: " + addr + ": string not applied: " + ex.getMessage()); nTypeWarn++;
                    }
                }
            } else {
                DataType dt = parseType(type);
                if (dt == null) {
                    println("warning: " + addr + ": type not applied (does not parse): '" + type + "'"); nTypeWarn++;
                } else if (d == null || !(d.getDataType().isEquivalent(dt)
                        || (d.getDataType() instanceof Pointer && dt instanceof Pointer && d.getLength() == dt.getLength()))) {
                    try {
                        // clears conflicting *data* (never instructions) inside the new item
                        DataUtilities.createData(currentProgram, addr, dt, -1,
                            DataUtilities.ClearDataMode.CLEAR_ALL_CONFLICT_DATA);
                    } catch (Exception ex) {
                        println("warning: " + addr + ": type '" + type + "' not applied: " + ex.getMessage()); nTypeWarn++;
                    }
                }
            }
        }
        applyLabel(addr, name, src, conf, card, CommentType.EOL);
    }

    /** struct rows: name = type name, type = header it is declared in. Types come from
     *  ImportTypes.java; the row only checks that the type exists. */
    void applyStruct(Address addr, String name, String type) {
        if (name.isEmpty()) return;
        List<DataType> found = new ArrayList<>();
        currentProgram.getDataTypeManager().findDataTypes(name, found);
        if (found.isEmpty()) { println("warning: struct " + name + " (" + type + ") not in the program; run ImportTypes"); nTypeWarn++; }
    }
}
