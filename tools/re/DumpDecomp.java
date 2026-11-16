// Ghidra headless script: decompile every function and write one C file per
// namespace (C++ class). Usage: -postScript DumpDecomp.java <outdir> [skip-regex]
// Functions whose namespace matches skip-regex are left out (library code).
import java.io.File;
import java.io.FileWriter;
import java.util.HashMap;
import java.util.Map;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class DumpDecomp extends GhidraScript {
	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		File out = new File(args[0]);
		String skip = args.length > 1 ? args[1] : null;
		out.mkdirs();
		DecompInterface d = new DecompInterface();
		d.openProgram(currentProgram);
		Map<String, FileWriter> files = new HashMap<>();
		int n = 0, failed = 0;

		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			if (f.isThunk() || f.isExternal())
				continue;
			String full = f.getParentNamespace().getName(true);
			if (skip != null && full.matches(skip))
				continue;
			String ns = full.replaceAll("[^A-Za-z0-9_]", "_");
			if (ns.length() > 80)
				ns = ns.substring(0, 80);
			FileWriter w = files.get(ns);
			if (w == null) {
				w = new FileWriter(new File(out, ns + ".c"));
				files.put(ns, w);
			}
			DecompileResults r = d.decompileFunction(f, 120, monitor);
			w.write("// ==== " + f.getEntryPoint() + " " + f.getName(true) + "\n");
			if (r.decompileCompleted()) {
				w.write(r.getDecompiledFunction().getC());
			} else {
				w.write("/* decompile failed: " + r.getErrorMessage() + " */\n");
				failed++;
			}
			w.write("\n");
			if (++n % 500 == 0)
				println("decompiled " + n);
		}
		for (FileWriter w : files.values())
			w.close();
		println("done: " + n + " functions, " + failed + " failed");
	}
}
