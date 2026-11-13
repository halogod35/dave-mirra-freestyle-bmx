// Export every function of the current program to a CSV (decomp/symbols/functions.csv).
//
// Columns: va,end,size,name,thunk_of,library,source_file,confidence,n_callers,n_callees,callers,callees,strings_referenced,signature
//   va/end      entry VA and last byte of the body (0x%08x)
//   size        number of bytes in the function body
//   thunk_of    for a jmp thunk (e.g. the /INCREMENTAL ILT at 0x401000): the target's entry VA, else empty
//               (Ghidra shows a thunk under its target's name, so the name alone does not tell)
//   library     LIB_<x> function tag -> x ; Ghidra Function-ID match -> fid:<name> ; else empty
//   source_file/confidence  parsed from the "[symbols.csv]" plate comment written by ApplySymbols
//   callers/callees         ';'-separated entry VAs, resolved through ILT/jmp thunks (thunk callers
//                           are included; a callee that is a thunk is replaced by its target)
//   strings_referenced      ' | '-separated 0xVA=text (text escaped: \n \r \t, '|' -> '/', max 100 chars)
//   signature               Ghidra prototype incl. calling convention (DM-040 stubs use it)
// Rows are sorted by VA. Output is deterministic (re-running on an unchanged program gives the same file).
//
// Usage (headless): -postScript ExportFunctions.java <out.csv>
//@category DMBMX

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.*;

public class ExportFunctions extends GhidraScript {

    static String csv(String s) {
        if (s.indexOf(',') >= 0 || s.indexOf('"') >= 0 || s.indexOf('\n') >= 0)
            return "\"" + s.replace("\"", "\"\"") + "\"";
        return s;
    }

    static String esc(String s) {
        StringBuilder sb = new StringBuilder();
        for (char c : s.toCharArray()) {
            if (c == '\n') sb.append("\\n");
            else if (c == '\r') sb.append("\\r");
            else if (c == '\t') sb.append("\\t");
            else if (c == '|') sb.append('/');
            else if (c < 0x20 || c > 0x7e) sb.append('?');
            else sb.append(c);
        }
        String r = sb.toString();
        return r.length() > 100 ? r.substring(0, 100) + "..." : r;
    }

    /** A printable NUL-terminated ASCII string of >= 4 chars at addr, or null. */
    String stringAt(Address a) {
        Data d = getDataAt(a);
        if (d != null && d.hasStringValue()) {
            Object v = d.getValue();
            return v == null ? null : v.toString();
        }
        if (d != null && d.isDefined() && !d.getDataType().getName().startsWith("undefined")) return null;
        Memory mem = currentProgram.getMemory();
        StringBuilder sb = new StringBuilder();
        try {
            for (int i = 0; i < 256; i++) {
                byte b = mem.getByte(a.add(i));
                if (b == 0) break;
                if ((b < 0x20 || b > 0x7e) && b != '\n' && b != '\r' && b != '\t') return null;
                sb.append((char) b);
            }
        } catch (Exception e) {
            return null;
        }
        return sb.length() >= 4 ? sb.toString() : null;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) { printerr("usage: ExportFunctions.java <out.csv>"); return; }
        Listing listing = currentProgram.getListing();
        ReferenceManager rm = currentProgram.getReferenceManager();
        AddressSetView exec = currentProgram.getMemory().getExecuteSet();
        int n = 0, nLib = 0, nSrc = 0;
        try (PrintWriter w = new PrintWriter(new OutputStreamWriter(new FileOutputStream(args[0]), StandardCharsets.UTF_8))) {
            w.print("va,end,size,name,thunk_of,library,source_file,confidence,n_callers,n_callees,callers,callees,strings_referenced,signature\n");
            FunctionIterator it = listing.getFunctions(true);
            while (it.hasNext()) {
                monitor.checkCancelled();
                Function f = it.next();
                if (f.isExternal()) continue;
                AddressSetView body = f.getBody();
                String lib = "";
                for (FunctionTag t : f.getTags())
                    if (t.getName().startsWith("LIB_")) lib = t.getName().substring(4);
                String plate = f.getComment();
                if (lib.isEmpty() && plate != null && plate.contains("Library Function")) lib = "fid";
                String src = "", conf = "";
                if (plate != null) {
                    for (String l : plate.split("\n")) {
                        if (!l.startsWith("[symbols.csv] ")) continue;
                        for (String part : l.substring(14).split(" \\| ")) {
                            if (part.startsWith("source_file: ")) src = part.substring(13).trim();
                            if (part.startsWith("confidence: ")) conf = part.substring(12).trim();
                        }
                    }
                }
                if ("?".equals(src)) src = "";
                if ("?".equals(conf)) conf = "";
                if (!lib.isEmpty()) nLib++;
                if (!src.isEmpty()) nSrc++;
                TreeSet<Long> callers = new TreeSet<>(), callees = new TreeSet<>();
                // The exe is linked /INCREMENTAL: nearly every call goes through an ILT jmp thunk.
                // Callers are collected through the thunks; callees are resolved to the thunk target.
                for (Function c : f.getCallingFunctions(monitor))
                    if (!(c.isThunk() && f.equals(c.getThunkedFunction(true)))) callers.add(c.getEntryPoint().getOffset());
                Address[] thunks = f.getFunctionThunkAddresses(true);
                if (thunks != null) for (Address ta : thunks) {
                    Function t = getFunctionAt(ta);
                    if (t != null) for (Function c : t.getCallingFunctions(monitor)) callers.add(c.getEntryPoint().getOffset());
                }
                for (Function c : f.getCalledFunctions(monitor)) {
                    Function d = c.isThunk() ? c.getThunkedFunction(true) : c;
                    if (d != null && !d.isExternal()) callees.add(d.getEntryPoint().getOffset());
                }
                TreeMap<Long, String> strs = new TreeMap<>();
                for (Address a : rm.getReferenceSourceIterator(body, true)) {
                    for (Reference r : rm.getReferencesFrom(a)) {
                        Address to = r.getToAddress();
                        if (!to.isMemoryAddress() || exec.contains(to)) continue;
                        if (strs.containsKey(to.getOffset())) continue;
                        String s = stringAt(to);
                        if (s != null) strs.put(to.getOffset(), s);
                    }
                }
                StringBuilder sb = new StringBuilder();
                sb.append(String.format("0x%08x,0x%08x,%d,", f.getEntryPoint().getOffset(),
                    body.getMaxAddress().getOffset(), body.getNumAddresses()));
                String thunkOf = "";
                if (f.isThunk()) {
                    Function t = f.getThunkedFunction(true);
                    if (t != null) thunkOf = t.isExternal() ? "ext:" + t.getName() : String.format("0x%08x", t.getEntryPoint().getOffset());
                }
                sb.append(csv(f.getName())).append(',').append(thunkOf).append(',').append(csv(lib)).append(',').append(csv(src)).append(',').append(conf).append(',');
                sb.append(callers.size()).append(',').append(callees.size()).append(',');
                sb.append(join(callers)).append(',').append(join(callees)).append(',');
                StringBuilder ss = new StringBuilder();
                for (Map.Entry<Long, String> e : strs.entrySet()) {
                    if (ss.length() > 0) ss.append(" | ");
                    ss.append(String.format("0x%08x=", e.getKey())).append(esc(e.getValue()));
                }
                sb.append(csv(ss.toString())).append(',').append(csv(f.getPrototypeString(false, true)));
                w.print(sb.append('\n'));
                n++;
            }
        }
        println(String.format("ExportFunctions: %d functions (%d library-tagged, %d with source_file) -> %s", n, nLib, nSrc, args[0]));
    }

    static String join(Set<Long> s) {
        StringBuilder sb = new StringBuilder();
        for (long v : s) { if (sb.length() > 0) sb.append(';'); sb.append(String.format("0x%08x", v)); }
        return sb.toString();
    }
}
