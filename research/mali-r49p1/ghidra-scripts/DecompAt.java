// args: "name=0xVADDR,..." outDir depth : decompile functions at ELF vaddrs (+image base) and callees to depth.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;
public class DecompAt extends GhidraScript {
  DecompInterface di; Set<Function> done;
  public void run() throws Exception {
    String[] a = getScriptArgs(); String outDir = a[1]; int depth = Integer.parseInt(a[2]);
    di = new DecompInterface(); di.openProgram(currentProgram);
    long base = currentProgram.getImageBase().getOffset();
    for (String kv : a[0].split(",")) {
      String[] p = kv.split("="); Address ad = toAddr(Long.decode(p[1]) + base);
      disassemble(ad); Function f = getFunctionAt(ad); if (f == null) f = createFunction(ad, null);
      PrintWriter pw = new PrintWriter(new FileWriter(outDir + "/" + p[0] + ".c")); done = new HashSet<>();
      if (f != null) walk(f, pw, depth, p[0]);
      pw.close();
    }
  }
  void walk(Function f, PrintWriter pw, int depth, String why) throws Exception {
    if (!done.add(f) || done.size() > 40) return;
    DecompileResults r = di.decompileFunction(f, 90, monitor);
    pw.println("// ==== " + why + " " + f.getName() + " @ " + f.getEntryPoint());
    if (r.decompileCompleted()) pw.println(r.getDecompiledFunction().getC());
    if (depth > 0) {
      // -noanalysis: create callee functions from the call flows of this body.
      for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
        if (!ins.getFlowType().isCall()) continue;
        for (Address t : ins.getFlows()) {
          if (getFunctionAt(t) == null) { disassemble(t); createFunction(t, null); }
        }
      }
    }
    if (depth > 0) for (Function c : f.getCalledFunctions(monitor)) {
      if (c.isExternal() || c.isThunk()) { pw.println("// calls " + c.getName()); continue; }
      walk(c, pw, depth - 1, "callee of " + f.getName());
    }
  }
}
