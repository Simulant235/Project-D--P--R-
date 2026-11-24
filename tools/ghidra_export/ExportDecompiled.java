// Pseudocode in Paketen (100 Funktionen je Datei)
// py tools/ghidra_export/run_export.py game/decrypted/EBOOT.BIN --reuse --script ExportDecompiled.java [paketgröße]
// Ausgabe <ausgabeordner>/decompiled/pack_0001.c ..., nicht einchecken
//@category PSP

import java.io.File;
import java.io.FileWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class ExportDecompiled extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File outDir = new File(args.length > 0 ? args[0] : "generated/analysis", "decompiled");
		int packSize = args.length > 1 ? Integer.parseInt(args[1]) : 100;
		outDir.mkdirs();

		DecompInterface dec = new DecompInterface();
		dec.openProgram(currentProgram);
		int count = 0, pack = 0, failed = 0;
		Writer w = null;
		try {
			for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
				if (monitor.isCancelled()) {
					break;
				}
				if (f.isThunk() || f.isExternal()) {
					continue;
				}
				String block = currentProgram.getMemory().getBlock(f.getEntryPoint()).getName();
				if (block.equals(".sceStub.text")) {
					continue; // Import-Stubs
				}
				if (count % packSize == 0) {
					if (w != null) {
						w.close();
					}
					pack++;
					w = new FileWriter(new File(outDir, String.format("pack_%04d.c", pack)), StandardCharsets.UTF_8);
				}
				count++;
				w.write(String.format("// ===== %s  addr=0x%08X  size=%d =====\n", f.getName(),
					f.getEntryPoint().getOffset(), f.getBody().getNumAddresses()));
				DecompileResults res = dec.decompileFunction(f, 60, monitor);
				if (res != null && res.decompileCompleted() && res.getDecompiledFunction() != null) {
					w.write(res.getDecompiledFunction().getC());
				}
				else {
					failed++;
					w.write("// DEKOMPILIERUNG FEHLGESCHLAGEN\n");
				}
				w.write("\n");
			}
		}
		finally {
			if (w != null) {
				w.close();
			}
			dec.dispose();
		}
		println(String.format("Dekompiliert: %d Funktionen in %d Paketen, %d fehlgeschlagen -> %s", count, pack, failed,
			outDir.getAbsolutePath()));
	}
}
