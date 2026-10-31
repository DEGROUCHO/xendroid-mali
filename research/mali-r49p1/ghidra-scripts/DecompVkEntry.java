// Decompile the Mali driver functions registered under given Vulkan entry point names.
// For each name: find the string, every data reference to it (name/function-pointer tables), read the
// pointer stored next to it, decompile that function and its direct callees (depth 1) to decomp/<name>.c
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class DecompVkEntry extends GhidraScript {
  public void run() throws Exception {
    String[] names = getScriptArgs()[0].split(",");
    String outDir = getScriptArgs()[1];
    DecompInterface di = new DecompInterface(); di.openProgram(currentProgram);
    Memory mem = currentProgram.getMemory(); Listing lst = currentProgram.getListing();
    for (String name : names) {
      PrintWriter pw = new PrintWriter(new FileWriter(outDir + "/" + name + ".c"));
      Set<Function> done = new HashSet<>();
      Address s = find(null, (name + "\0").getBytes("US-ASCII"));
      while (s != null) {
        // No full analysis: find 8-byte little-endian pointers to the string (name/pointer tables).
        long sv = s.getOffset(); byte[] pb = new byte[8];
        for (int i = 0; i < 8; i++) pb[i] = (byte) (sv >>> (8 * i));
        List<Address> refs = new ArrayList<>();
        Address q = find(null, pb);
        while (q != null && refs.size() < 16) { refs.add(q); q = find(q.add(1), pb); }
        for (Address from : refs) {
          for (int off : new int[]{8, -8, 16}) {
            try {
              long p = mem.getLong(from.add(off));
              Function f = getFunctionAt(toAddr(p));
              if (f == null) { createFunction(toAddr(p), null); f = getFunctionAt(toAddr(p)); }
              if (f != null && done.add(f)) dump(di, f, pw, name + " (ref " + from + " off " + off + ")", done);
            } catch (Exception e) {}
          }
          pw.println("// pointer to name at " + from);
        }
        s = find(s.add(1), (name + "\0").getBytes("US-ASCII"));
      }
      pw.close();
    }
  }
  void dump(DecompInterface di, Function f, PrintWriter pw, String why, Set<Function> done) {
    DecompileResults r = di.decompileFunction(f, 60, monitor);
    pw.println("// ==== " + why + " -> " + f.getName() + " @ " + f.getEntryPoint());
    if (r.decompileCompleted()) pw.println(r.getDecompiledFunction().getC());
    for (Function c : f.getCalledFunctions(monitor)) {
      if (done.add(c)) {
        DecompileResults rc = di.decompileFunction(c, 60, monitor);
        pw.println("// ---- callee " + c.getName() + " @ " + c.getEntryPoint());
        if (rc.decompileCompleted()) pw.println(rc.getDecompiledFunction().getC());
      }
    }
  }
}
