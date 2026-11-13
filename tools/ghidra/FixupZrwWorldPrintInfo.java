// One-off fix-up, run once before ApplySymbols on a Ghidra project made before zrwWorld.c was decompiled:
// zrwWorldPrintInfo 0x00495F60 (slot +0x18 of g_zrwWorldApi 0x005620E0, only reached through that
// table) was disassembled from the wrong offset during auto-analysis (a stray instruction stream
// at 0x00495F70 overlaps it), so ApplySymbols cannot create the function there. This clears the
// code units in [0x00495F60, 0x00496120) and disassembles from the real entry point.
//
// Usage (headless): -postScript FixupZrwWorldPrintInfo.java      (idempotent)
//@category DMBMX

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;

public class FixupZrwWorldPrintInfo extends GhidraScript {
    @Override
    public void run() throws Exception {
        Address start = toAddr(0x00495F60L), end = toAddr(0x0049611FL);
        Function f = getFunctionAt(start);
        if (f != null) { println("FixupZrwWorldPrintInfo: function already present at " + start); return; }
        FunctionIterator it = currentProgram.getListing().getFunctions(new AddressSet(start, end), true);
        while (it.hasNext()) {
            Function g = it.next();
            println("FixupZrwWorldPrintInfo: removing overlapping function " + g.getName() + " at " + g.getEntryPoint());
            removeFunction(g);
        }
        clearListing(start, end);
        disassemble(start);
        f = createFunction(start, "zrwWorldPrintInfo");
        println("FixupZrwWorldPrintInfo: " + (f == null ? "FAILED to create" : "created " + f.getName() + " size " + f.getBody().getNumAddresses()));
    }
}
