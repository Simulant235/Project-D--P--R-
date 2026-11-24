// Analyse als JSON exportieren (Recompiler/Runtime), Aufruf über run_export.py; Ausgabe nach generated/analysis/, nicht ins Git
//@category PSP

import java.io.File;
import java.io.FileWriter;
import java.io.Writer;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.reloc.Relocation;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class ExportAnalysis extends GhidraScript {

	private Memory mem;
	private Listing listing;
	private File outDir;
	private final Gson gson = new GsonBuilder().setPrettyPrinting().serializeNulls().disableHtmlEscaping().create();

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		outDir = new File(args.length > 0 ? args[0] : "generated/analysis");
		outDir.mkdirs();
		mem = currentProgram.getMemory();
		listing = currentProgram.getListing();

		write("program.json", programInfo());
		write("functions.json", functions());
		write("imports.json", imports());
		write("exports.json", exports());
		write("relocations.json", relocations());
		write("instr_histogram.json", histogram());
		write("jumptables.json", jumpTables());
		println("Export fertig: " + outDir.getAbsolutePath());
	}

	// ------------------------------------------------------------ Hilfen

	private void write(String name, Object data) throws Exception {
		try (Writer w = new FileWriter(new File(outDir, name), java.nio.charset.StandardCharsets.UTF_8)) {
			gson.toJson(data, w);
		}
		println("  geschrieben: " + name);
	}

	private static String hex(long v) {
		return String.format("0x%08X", v & 0xFFFFFFFFL);
	}

	private Address addr(long v) {
		return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(v & 0xFFFFFFFFL);
	}

	private long u32(long a) throws Exception {
		return mem.getInt(addr(a)) & 0xFFFFFFFFL;
	}

	private int u16(long a) throws Exception {
		return mem.getShort(addr(a)) & 0xFFFF;
	}

	private int u8(long a) throws Exception {
		return mem.getByte(addr(a)) & 0xFF;
	}

	private String cstr(long a) throws Exception {
		StringBuilder sb = new StringBuilder();
		for (int i = 0; i < 256; i++) {
			int c = u8(a + i);
			if (c == 0) {
				break;
			}
			sb.append((char) c);
		}
		return sb.toString();
	}

	/** nur echte Code-Sektionen (Ghidra disassembliert teils Daten in .data) */
	private boolean isCodeBlock(MemoryBlock b) {
		String n = b.getName();
		return n.startsWith(".text") || n.equals(".sceStub.text") || n.equals(".init") || n.equals(".fini");
	}

	private AddressSet codeSet() {
		AddressSet set = new AddressSet();
		for (MemoryBlock b : mem.getBlocks()) {
			if (isCodeBlock(b)) {
				set.add(b.getStart(), b.getEnd());
			}
		}
		return set;
	}

	private static String mnemonic(Instruction in) {
		String m = in.getMnemonicString().toLowerCase();
		return m.startsWith("_") ? m.substring(1) : m;
	}

	// ------------------------------------------------------------ Programm

	private Map<String, Object> programInfo() {
		Map<String, Object> p = new LinkedHashMap<>();
		p.put("name", currentProgram.getName());
		p.put("language", currentProgram.getLanguageID().getIdAsString());
		p.put("compiler_spec", currentProgram.getCompilerSpec().getCompilerSpecID().getIdAsString());
		p.put("format", currentProgram.getExecutableFormat());
		p.put("image_base", hex(currentProgram.getImageBase().getOffset()));
		p.put("sha256", currentProgram.getExecutableSHA256());
		List<Map<String, Object>> blocks = new ArrayList<>();
		for (MemoryBlock b : mem.getBlocks()) {
			Map<String, Object> m = new LinkedHashMap<>();
			m.put("name", b.getName());
			m.put("start", hex(b.getStart().getOffset()));
			m.put("size", b.getSize());
			m.put("perm", (b.isRead() ? "r" : "-") + (b.isWrite() ? "w" : "-") + (b.isExecute() ? "x" : "-"));
			m.put("initialized", b.isInitialized());
			m.put("code", isCodeBlock(b));
			if (b.isExecute()) {
				long instrBytes = 0, instrCount = 0;
				InstructionIterator it = listing.getInstructions(new AddressSet(b.getStart(), b.getEnd()), true);
				while (it.hasNext()) {
					instrBytes += it.next().getLength();
					instrCount++;
				}
				m.put("instructions", instrCount);
				m.put("instruction_bytes", instrBytes);
				m.put("not_disassembled_bytes", b.getSize() - instrBytes);
			}
			if (isCodeBlock(b)) {
				m.put("gaps", gaps(b));
			}
			blocks.add(m);
		}
		p.put("blocks", blocks);
		return p;
	}

	/** nicht disassemblierte Bereiche einer Code-Sektion */
	private List<Map<String, Object>> gaps(MemoryBlock b) {
		List<Map<String, Object>> out = new ArrayList<>();
		AddressSet rest = new AddressSet(b.getStart(), b.getEnd());
		InstructionIterator it = listing.getInstructions(new AddressSet(b.getStart(), b.getEnd()), true);
		AddressSet covered = new AddressSet();
		while (it.hasNext()) {
			Instruction in = it.next();
			covered.add(in.getMinAddress(), in.getMaxAddress());
		}
		rest.delete(covered);
		for (ghidra.program.model.address.AddressRange r : rest) {
			Map<String, Object> m = new LinkedHashMap<>();
			m.put("start", hex(r.getMinAddress().getOffset()));
			m.put("size", r.getLength());
			boolean zero = true;
			try {
				byte[] buf = new byte[(int) Math.min(r.getLength(), 1 << 20)];
				mem.getBytes(r.getMinAddress(), buf);
				for (byte x : buf) {
					if (x != 0) {
						zero = false;
						break;
					}
				}
			}
			catch (Exception e) {
				zero = false;
			}
			m.put("all_zero", zero);
			m.put("defined_data", listing.getDefinedDataContaining(r.getMinAddress()) != null);
			out.add(m);
		}
		return out;
	}

	// ------------------------------------------------------------ Funktionen

	private List<Map<String, Object>> functions() {
		List<Map<String, Object>> out = new ArrayList<>();
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			Map<String, Object> m = new LinkedHashMap<>();
			m.put("addr", hex(f.getEntryPoint().getOffset()));
			m.put("block", mem.getBlock(f.getEntryPoint()).getName());
			m.put("size", f.getBody().getNumAddresses());
			m.put("name", f.getName());
			m.put("ranges", f.getBody().getNumAddressRanges());
			m.put("thunk", f.isThunk());
			m.put("noreturn", f.hasNoReturn());
			int callers = 0, dataRefs = 0;
			ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
			while (refs.hasNext()) {
				Reference r = refs.next();
				if (r.getReferenceType().isCall()) {
					callers++;
				}
				else if (r.getReferenceType().isData()) {
					dataRefs++;
				}
			}
			m.put("call_refs", callers);
			m.put("data_refs", dataRefs); // > 0: Adresse wird als Funktionszeiger verwendet
			out.add(m);
		}
		return out;
	}

	// ------------------------------------------------------------ Importe / Exporte
	// direkt aus .lib.stub / .lib.ent (sceModuleInfo), unabhängig von den Stub-Namen des Plugins

	private long[] moduleInfo() throws Exception {
		MemoryBlock b = mem.getBlock(".rodata.sceModuleInfo");
		if (b == null) {
			throw new IllegalStateException(".rodata.sceModuleInfo nicht gefunden");
		}
		long a = b.getStart().getOffset();
		// attr(2) ver(2) name(28) gp ent_top ent_end stub_top stub_end
		return new long[] { a, u32(a + 32), u32(a + 36), u32(a + 40), u32(a + 44), u32(a + 48) };
	}

	private List<Map<String, Object>> imports() throws Exception {
		long[] mi = moduleInfo();
		List<Map<String, Object>> libs = new ArrayList<>();
		long p = mi[4];
		while (p < mi[5]) {
			int entLen = u8(p + 8); // in 32-Bit-Worten
			if (entLen == 0) {
				break;
			}
			int varCount = u8(p + 9);
			int funcCount = u16(p + 10);
			long nidPtr = u32(p + 12), stubPtr = u32(p + 16);
			Map<String, Object> lib = new LinkedHashMap<>();
			lib.put("library", cstr(u32(p)));
			lib.put("version", hex(u16(p + 4)));
			lib.put("attr", hex(u16(p + 6)));
			lib.put("var_count", varCount);
			List<Map<String, Object>> funcs = new ArrayList<>();
			for (int i = 0; i < funcCount; i++) {
				Address stub = addr(stubPtr + 8L * i);
				Map<String, Object> f = new LinkedHashMap<>();
				f.put("nid", hex(u32(nidPtr + 4L * i)));
				f.put("stub_addr", hex(stub.getOffset()));
				Function gf = getFunctionAt(stub);
				f.put("name", gf != null ? gf.getName() : null);
				int calls = 0, other = 0;
				Set<String> callers = new LinkedHashSet<>();
				ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(stub);
				while (refs.hasNext()) {
					Reference r = refs.next();
					if (r.getReferenceType().isCall() || r.getReferenceType().isJump()) {
						calls++;
						Function cf = getFunctionContaining(r.getFromAddress());
						callers.add(cf != null ? hex(cf.getEntryPoint().getOffset()) : hex(r.getFromAddress().getOffset()));
					}
					else {
						other++;
					}
				}
				f.put("call_sites", calls);
				f.put("other_refs", other);
				f.put("callers", callers);
				funcs.add(f);
			}
			lib.put("functions", funcs);
			libs.add(lib);
			p += entLen * 4L;
		}
		return libs;
	}

	private List<Map<String, Object>> exports() throws Exception {
		long[] mi = moduleInfo();
		List<Map<String, Object>> libs = new ArrayList<>();
		long p = mi[2];
		while (p < mi[3]) {
			int entLen = u8(p + 8);
			if (entLen == 0) {
				break;
			}
			int varCount = u8(p + 9);
			int funcCount = u16(p + 10);
			long table = u32(p + 12);
			long namePtr = u32(p);
			Map<String, Object> lib = new LinkedHashMap<>();
			lib.put("library", namePtr == 0 ? null : cstr(namePtr));
			lib.put("attr", hex(u16(p + 6)));
			List<Map<String, Object>> entries = new ArrayList<>();
			int total = funcCount + varCount;
			for (int i = 0; i < total; i++) {
				Map<String, Object> e = new LinkedHashMap<>();
				e.put("kind", i < funcCount ? "function" : "variable");
				e.put("nid", hex(u32(table + 4L * i)));
				e.put("addr", hex(u32(table + 4L * (total + i))));
				entries.add(e);
			}
			lib.put("entries", entries);
			libs.add(lib);
			p += entLen * 4L;
		}
		return libs;
	}

	// ------------------------------------------------------------ Relocations

	private Map<String, Object> relocations() {
		Map<String, Object> out = new LinkedHashMap<>();
		Map<String, Integer> byType = new TreeMap<>();
		List<Map<String, Object>> list = new ArrayList<>();
		java.util.Iterator<Relocation> it = currentProgram.getRelocationTable().getRelocations();
		while (it.hasNext()) {
			Relocation r = it.next();
			byType.merge(String.valueOf(r.getType()), 1, Integer::sum);
			Map<String, Object> m = new LinkedHashMap<>();
			m.put("addr", hex(r.getAddress().getOffset()));
			m.put("type", r.getType());
			m.put("status", String.valueOf(r.getStatus()));
			list.add(m);
		}
		out.put("count", list.size());
		out.put("by_type", byType);
		out.put("relocations", list);
		return out;
	}

	// ------------------------------------------------------------ Befehlsstatistik

	private static final Set<String> FPU_PLAIN = Set.of("mfc1", "mtc1", "cfc1", "ctc1", "lwc1", "swc1",
		"bc1f", "bc1t", "bc1fl", "bc1tl");
	private static final Set<String> VFPU_PLAIN = Set.of("mfv", "mtv", "mfvc", "mtvc", "bvf", "bvt", "bvfl", "bvtl");

	private static String category(Instruction in, String m) {
		if (m.startsWith("v") || m.startsWith("lv") || m.startsWith("sv") || VFPU_PLAIN.contains(m)) {
			return "vfpu";
		}
		if (FPU_PLAIN.contains(m) || m.endsWith(".s") || m.endsWith(".w") || m.endsWith(".d")) {
			return "fpu";
		}
		FlowType ft = in.getFlowType();
		if (ft.isJump() || ft.isCall() || ft.isTerminal()) {
			return "sprung";
		}
		return "integer";
	}

	private Map<String, Object> histogram() {
		Map<String, Map<String, Integer>> cats = new TreeMap<>();
		Map<String, Integer> catTotals = new TreeMap<>();
		long total = 0, inDelaySlot = 0, branchInDelaySlot = 0, likely = 0;
		long jrRa = 0, jrOther = 0, jalr = 0, jal = 0, syscall = 0;
		int funcsWithVfpu = 0, funcsWithFpu = 0;
		Set<Function> vf = new java.util.HashSet<>(), ff = new java.util.HashSet<>();
		List<String> syscallAddrs = new ArrayList<>();
		InstructionIterator it = listing.getInstructions(codeSet(), true);
		while (it.hasNext()) {
			Instruction in = it.next();
			String m = mnemonic(in);
			String c = category(in, m);
			cats.computeIfAbsent(c, k -> new TreeMap<>()).merge(m, 1, Integer::sum);
			catTotals.merge(c, 1, Integer::sum);
			total++;
			if (in.isInDelaySlot()) {
				inDelaySlot++;
				FlowType ft = in.getFlowType();
				if (ft.isJump() || ft.isCall() || ft.isTerminal()) {
					branchInDelaySlot++;
				}
			}
			if (c.equals("sprung") || c.equals("fpu") || c.equals("vfpu")) {
				if (m.startsWith("b") && m.endsWith("l") && !m.equals("bal") && !m.equals("bgezal") && !m.equals("bltzal")) {
					likely++;
				}
			}
			if (m.equals("jr")) {
				String op = in.getDefaultOperandRepresentation(0);
				if (op.equals("ra")) {
					jrRa++;
				}
				else {
					jrOther++;
				}
			}
			else if (m.equals("jalr")) {
				jalr++;
			}
			else if (m.equals("jal")) {
				jal++;
			}
			else if (m.equals("syscall")) {
				syscall++;
				syscallAddrs.add(hex(in.getAddress().getOffset()));
			}
			if (c.equals("vfpu") || c.equals("fpu")) {
				Function f = getFunctionContaining(in.getAddress());
				if (f != null) {
					(c.equals("vfpu") ? vf : ff).add(f);
				}
			}
		}
		funcsWithVfpu = vf.size();
		funcsWithFpu = ff.size();
		Map<String, Object> out = new LinkedHashMap<>();
		out.put("total_instructions", total);
		out.put("category_totals", catTotals);
		Map<String, Object> special = new LinkedHashMap<>();
		special.put("in_delay_slot", inDelaySlot);
		special.put("branch_in_delay_slot", branchInDelaySlot);
		special.put("likely_branches", likely);
		special.put("jal", jal);
		special.put("jalr", jalr);
		special.put("jr_ra", jrRa);
		special.put("jr_other", jrOther);
		special.put("syscall", syscall);
		special.put("functions_with_fpu", funcsWithFpu);
		special.put("functions_with_vfpu", funcsWithVfpu);
		out.put("special", special);
		out.put("syscall_addrs", syscallAddrs);
		List<String> vfpuFuncs = new ArrayList<>();
		for (Function f : vf) {
			vfpuFuncs.add(hex(f.getEntryPoint().getOffset()));
		}
		java.util.Collections.sort(vfpuFuncs);
		out.put("vfpu_functions", vfpuFuncs);
		out.put("mnemonics", cats);
		return out;
	}

	// ------------------------------------------------------------ Sprungtabellen-Kandidaten
	// jedes jr <reg> außer jr ra, mit Zielen von Ghidra + Befehlen davor

	private List<Map<String, Object>> jumpTables() {
		List<Map<String, Object>> out = new ArrayList<>();
		InstructionIterator it = listing.getInstructions(codeSet(), true);
		while (it.hasNext()) {
			Instruction in = it.next();
			if (!mnemonic(in).equals("jr")) {
				continue;
			}
			String reg = in.getDefaultOperandRepresentation(0);
			if (reg.equals("ra")) {
				continue;
			}
			Map<String, Object> m = new LinkedHashMap<>();
			m.put("addr", hex(in.getAddress().getOffset()));
			Function f = getFunctionContaining(in.getAddress());
			m.put("function", f != null ? hex(f.getEntryPoint().getOffset()) : null);
			m.put("reg", reg);
			List<String> targets = new ArrayList<>();
			for (Reference r : in.getReferencesFrom()) {
				if (r.getReferenceType().isFlow()) {
					targets.add(hex(r.getToAddress().getOffset()));
				}
			}
			m.put("resolved_targets", targets.size());
			m.put("targets", targets);
			List<String> ctx = new ArrayList<>();
			Instruction p = in;
			for (int i = 0; i < 12 && p != null; i++) {
				ctx.add(0, hex(p.getAddress().getOffset()) + "  " + p.toString());
				p = p.getPrevious();
			}
			Instruction ds = in.getNext();
			if (ds != null) {
				ctx.add(hex(ds.getAddress().getOffset()) + "  " + ds.toString() + "   ; Delay Slot");
			}
			m.put("context", ctx);
			out.add(m);
		}
		return out;
	}
}
