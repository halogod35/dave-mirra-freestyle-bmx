// DM-048 (optional RE tool): give the CRT's __ftol 0x004A4A80 its real x87 signature,
// long long __ftol(float10 x) with x in ST0 and the result in EDX:EAX (custom storage). Without it the
// decompiler drops every float->int conversion's argument ("__ftol()"); with it the expression shows.
// Usage (headless, writes the project): -postScript SetFtolSignature.java
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.data.*;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.lang.Register;
import java.util.*;
public class SetFtolSignature extends GhidraScript {
    public void run() throws Exception {
        Function f = getFunctionAt(toAddr(0x004a4a80L));
        Register st0 = currentProgram.getRegister("ST0");
        Register eax = currentProgram.getRegister("EAX"), edx = currentProgram.getRegister("EDX");
        ParameterImpl p = new ParameterImpl("x", Float10DataType.dataType, new VariableStorage(currentProgram, st0), currentProgram);
        ReturnParameterImpl r = new ReturnParameterImpl(LongLongDataType.dataType, new VariableStorage(currentProgram, edx, eax), currentProgram);
        f.updateFunction("__cdecl", r, Arrays.asList((Variable) p), Function.FunctionUpdateType.CUSTOM_STORAGE, true, SourceType.USER_DEFINED);
        println("SetFtolSignature: " + f.getPrototypeString(true, true));
    }
}
