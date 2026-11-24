// synthetisches MIPS-Programm, von Hand gebaut -> Recompiler, dazu test_defs.h; läuft in exec_tests.c
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "recomp.h"

namespace {

enum { ZERO = 0, V0 = 2, V1 = 3, A0 = 4, A1 = 5, T0 = 8, SP = 29, RA = 31 };

struct Asm {
    u32 base;
    std::vector<u32> w;
    std::map<std::string, u32> labels;
    struct Fix { size_t index; std::string label; bool jump; };
    std::vector<Fix> fixes;

    u32 here() const { return base + (u32)w.size() * 4; }
    void label(const std::string& n) { labels[n] = here(); }
    void emit(u32 v) { w.push_back(v); }

    void r(u32 rs, u32 rt, u32 rd, u32 sa, u32 funct) { emit((rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct); }
    void i(u32 op, u32 rs, u32 rt, s32 imm) { emit((op << 26) | (rs << 21) | (rt << 16) | ((u32)imm & 0xFFFF)); }
    void br(u32 op, u32 rs, u32 rt, const std::string& l) { fixes.push_back({w.size(), l, false}); i(op, rs, rt, 0); }
    void jmp(u32 op, const std::string& l) { fixes.push_back({w.size(), l, true}); emit(op << 26); }

    void nop() { emit(0); }
    void addiu(u32 rt, u32 rs, s32 imm) { i(9, rs, rt, imm); }
    void move(u32 rd, u32 rs) { r(rs, 0, rd, 0, 37); }
    void addu(u32 rd, u32 rs, u32 rt) { r(rs, rt, rd, 0, 33); }
    void sll(u32 rd, u32 rt, u32 sa) { r(0, rt, rd, sa, 0); }
    void jr(u32 rs) { r(rs, 0, 0, 0, 8); }
    void jalr(u32 rs) { r(rs, 0, RA, 0, 9); }
    void lw(u32 rt, s32 off, u32 rs) { i(35, rs, rt, off); }
    void sw(u32 rt, s32 off, u32 rs) { i(43, rs, rt, off); }
    void beq(u32 rs, u32 rt, const std::string& l) { br(4, rs, rt, l); }
    void bne(u32 rs, u32 rt, const std::string& l) { br(5, rs, rt, l); }
    void beql(u32 rs, u32 rt, const std::string& l) { br(20, rs, rt, l); }
    void bnel(u32 rs, u32 rt, const std::string& l) { br(21, rs, rt, l); }
    void jal(const std::string& l) { jmp(3, l); }
    void j(const std::string& l) { jmp(2, l); }
    void ret() { jr(RA); }

    void resolve() {
        for (const Fix& f : fixes) {
            auto it = labels.find(f.label);
            if (it == labels.end()) throw std::runtime_error("Marke fehlt: " + f.label);
            u32 at = base + (u32)f.index * 4;
            if (f.jump) w[f.index] |= (it->second >> 2) & 0x03FFFFFFu;
            else w[f.index] |= (u32)(((s32)(it->second - (at + 4))) >> 2) & 0xFFFF;
        }
    }
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "test_gen <ausgabeordner> <runtime-include>\n");
        return 2;
    }
    try {
        const u32 BASE = 0x00010000, TABLE = 0x00014000, IMAGE_SIZE = 0x8000;
        Asm a{BASE, {}, {}, {}};

        // T1: bedingter Sprung, Delay Slot ändert das Vergleichsregister (Bedingung zählt davor)
        a.label("t_nonlikely");
        a.beq(A0, ZERO, "t1_taken");
        a.addiu(A0, A0, 1);
        a.addiu(V0, ZERO, 10);
        a.ret(); a.nop();
        a.label("t1_taken");
        a.addiu(V0, A0, 100);
        a.ret(); a.nop();

        // T2: Likely-Branch, Delay Slot nur bei genommenem Sprung
        a.label("t_likely");
        a.move(V1, A1);
        a.beql(A0, ZERO, "t2_taken");
        a.addiu(V1, ZERO, 7);
        a.move(V0, V1);
        a.ret(); a.nop();
        a.label("t2_taken");
        a.move(V0, V1);
        a.ret(); a.nop();

        // Helfer: v0 = a0 * 2, Rechnung im Delay Slot von jr ra
        a.label("g_double");
        a.ret();
        a.sll(V0, A0, 1);

        // T3: jal, Delay Slot setzt das Argument; Rückkehr + Stack
        a.label("t_call");
        a.addiu(SP, SP, -8);
        a.sw(RA, 0, SP);
        a.jal("g_double");
        a.addiu(A0, A0, 5);
        a.lw(RA, 0, SP);
        a.addiu(V0, V0, 1);
        a.ret();
        a.addiu(SP, SP, 8);

        // T4: Schleife mit Rückwärtssprung (Safepoint)
        a.label("t_loop");
        a.move(V0, ZERO);
        a.label("t4_loop");
        a.addu(V0, V0, A0);
        a.addiu(A0, A0, -1);
        a.bne(A0, ZERO, "t4_loop");
        a.nop();
        a.ret(); a.nop();

        // T5: jalr über Register
        a.label("t_jalr");
        a.addiu(SP, SP, -8);
        a.sw(RA, 0, SP);
        a.jalr(A1);
        a.addiu(A0, A0, 5);
        a.lw(RA, 0, SP);
        a.addiu(V0, V0, 1);
        a.ret();
        a.addiu(SP, SP, 8);

        // T5b: jr <reg> als Tail-Call
        a.label("t_tail_jr");
        a.jr(A1);
        a.addiu(A0, A0, 1);

        // T6: Sprungtabelle
        a.label("t_switch");
        a.i(11, A0, V0, 3);                    // sltiu v0, a0, 3
        a.beq(V0, ZERO, "t6_default");
        a.nop();
        a.sll(V0, A0, 2);
        a.i(15, 0, V1, (s32)(TABLE >> 16));    // lui v1, HI
        a.addiu(V1, V1, (s32)(TABLE & 0xFFFF));
        a.addu(V0, V0, V1);
        a.lw(V0, 0, V0);
        a.jr(V0);
        a.nop();
        a.label("t6_c0"); a.ret(); a.addiu(V0, ZERO, 100);
        a.label("t6_c1"); a.ret(); a.addiu(V0, ZERO, 200);
        a.label("t6_c2"); a.ret(); a.addiu(V0, ZERO, 300);
        a.label("t6_default"); a.ret(); a.addiu(V0, ZERO, -1);

        // T7: Sprungziel ist der Delay Slot eines anderen Sprungs
        a.label("t_delay_target");
        a.beq(A0, ZERO, "t7_slot");
        a.addiu(V0, ZERO, 0);
        a.bne(A1, ZERO, "t7_end");
        a.label("t7_slot");
        a.addiu(V0, V0, 1);
        a.addiu(V0, V0, 10);
        a.label("t7_end");
        a.ret(); a.nop();

        // T8: Division inkl. Sonderfälle (lo -> v0, hi -> v1)
        a.label("t_div");
        a.r(A0, A1, 0, 0, 26);                 // div a0, a1
        a.r(0, 0, V0, 0, 18);                  // mflo v0
        a.ret();
        a.r(0, 0, V1, 0, 16);                  // mfhi v1

        // T9: ext / ins
        a.label("t_ext");
        a.ret();
        a.emit((31u << 26) | (A0 << 21) | (V0 << 16) | (7u << 11) | (4u << 6) | 0);    // ext v0, a0, 4, 8
        a.label("t_ins");
        a.move(V0, A1);
        a.ret();
        a.emit((31u << 26) | (A0 << 21) | (V0 << 16) | (11u << 11) | (8u << 6) | 4);   // ins v0, a0, 8, 4

        // T10: unausgerichtetes Laden und Speichern
        a.label("t_unaligned_load");
        a.i(34, A0, V0, 3);                    // lwl v0, 3(a0)
        a.ret();
        a.i(38, A0, V0, 0);                    // lwr v0, 0(a0)
        a.label("t_unaligned_store");
        a.i(42, A0, A1, 3);                    // swl a1, 3(a0)
        a.ret();
        a.i(46, A0, A1, 0);                    // swr a1, 0(a0)

        // T11: sltiu mit vorzeichenerweitertem Wert
        a.label("t_sltiu");
        a.ret();
        a.i(11, A0, V0, -1);                   // sltiu v0, a0, -1

        // T12: Aufruf eines Import-Stubs
        a.label("t_hle");
        a.addiu(SP, SP, -8);
        a.sw(RA, 0, SP);
        a.jal("stub");
        a.nop();
        a.lw(RA, 0, SP);
        a.ret();
        a.addiu(SP, SP, 8);

        // T13: j in eine andere Funktion (Tail-Call)
        a.label("t_tail_j");
        a.j("g_double");
        a.addiu(A0, A0, 3);

        // T14: Likely-Branch rückwärts
        a.label("t_likely_loop");
        a.move(V0, ZERO);
        a.label("t14_loop");
        a.addiu(A0, A0, -1);
        a.bnel(A0, ZERO, "t14_loop");
        a.addiu(V0, V0, 1);
        a.ret(); a.nop();

        // T15: ra nach jal korrekt
        a.label("g_get_ra");
        a.ret();
        a.move(V0, RA);
        a.label("t_ra");
        a.addiu(SP, SP, -8);
        a.sw(RA, 0, SP);
        a.label("t_ra_jal");
        a.jal("g_get_ra");
        a.nop();
        a.lw(RA, 0, SP);
        a.ret();
        a.addiu(SP, SP, 8);

        // ---------------- FPU (T12): a0/a1 -> f12/f13, Ergebnis f0 -> v0
        auto cop1 = [&](u32 fmt, u32 ft, u32 fs, u32 fd, u32 fn) { a.emit((17u << 26) | (fmt << 21) | (ft << 16) | (fs << 11) | (fd << 6) | fn); };
        auto fpu_fn = [&](const std::string& name, u32 fmt, u32 fn, bool binary) {
            a.label(name);
            cop1(4, A0, 12, 0, 0);                 // mtc1 a0, f12
            cop1(4, A1, 13, 0, 0);                 // mtc1 a1, f13
            cop1(fmt, binary ? 13 : 0, 12, 0, fn); // f0 = op(f12[, f13])
            a.ret();
            cop1(0, V0, 0, 0, 0);                  // mfc1 v0, f0
        };
        fpu_fn("t_f_add", 16, 0, true);
        fpu_fn("t_f_sub", 16, 1, true);
        fpu_fn("t_f_mul", 16, 2, true);
        fpu_fn("t_f_div", 16, 3, true);
        fpu_fn("t_f_sqrt", 16, 4, false);
        fpu_fn("t_f_abs", 16, 5, false);
        fpu_fn("t_f_neg", 16, 7, false);
        fpu_fn("t_f_round", 16, 12, false);
        fpu_fn("t_f_trunc", 16, 13, false);
        fpu_fn("t_f_ceil", 16, 14, false);
        fpu_fn("t_f_floor", 16, 15, false);
        fpu_fn("t_f_cvtw", 16, 36, false);
        fpu_fn("t_f_cvts", 20, 32, false);
        // c.cond + bc1t: v0 = 1 wenn Bedingung
        auto fcmp_fn = [&](const std::string& name, u32 cond) {
            a.label(name);
            cop1(4, A0, 12, 0, 0);
            cop1(4, A1, 13, 0, 0);
            cop1(16, 13, 12, 0, 48 + cond);        // c.cond.s f12, f13
            a.fixes.push_back({a.w.size(), name + "_yes", false});
            a.emit((17u << 26) | (8u << 21) | (1u << 16));   // bc1t
            a.addiu(V0, ZERO, 0);
            a.ret(); a.nop();
            a.label(name + "_yes");
            a.ret();
            a.addiu(V0, ZERO, 1);
        };
        fcmp_fn("t_f_ceq", 2);
        fcmp_fn("t_f_clt", 12);
        fcmp_fn("t_f_cle", 14);
        fcmp_fn("t_f_cnge", 13);
        fcmp_fn("t_f_cngt", 15);

        // ---------------- VFPU: a0 -> Puffer, 16 Byte aligned
        const u32 QUAD = (1u << 15) | (1u << 7);
        auto lvq = [&](u32 vt, s32 off) { a.emit((54u << 26) | (A0 << 21) | ((vt & 31) << 16) | ((u32)off & 0xFFFC) | ((vt >> 5) & 1)); };
        auto svq = [&](u32 vt, s32 off) { a.emit((62u << 26) | (A0 << 21) | ((vt & 31) << 16) | ((u32)off & 0xFFFC) | ((vt >> 5) & 1)); };
        auto lvs = [&](u32 vt, s32 off) { a.emit((50u << 26) | (A0 << 21) | ((vt & 31) << 16) | ((u32)off & 0xFFFC) | ((vt >> 5) & 3)); };
        auto svs = [&](u32 vt, s32 off) { a.emit((58u << 26) | (A0 << 21) | ((vt & 31) << 16) | ((u32)off & 0xFFFC) | ((vt >> 5) & 3)); };
        auto v3 = [&](u32 opc, u32 sub, u32 size_bits, u32 vd, u32 vs, u32 vt) { a.emit((opc << 26) | (sub << 23) | (vt << 16) | (vs << 8) | size_bits | vd); };
        auto v4 = [&](u32 rs, u32 sub, u32 size_bits, u32 vd, u32 vs) { a.emit((52u << 26) | (rs << 21) | (sub << 16) | (vs << 8) | size_bits | vd); };
        // Register: C000 = 0, C010 = 1, C020 = 2, S030 = 3 (Spalte 3, Zeile 0), Matrizen M100 = 4.., M200 = 8..

        a.label("t_v_add");                 // [32..] = [0..] + [16..]
        lvq(0, 0); lvq(1, 16);
        v3(24, 0, QUAD, 2, 0, 1);           // vadd.q C020, C000, C010
        a.ret(); svq(2, 32);

        a.label("t_v_sub_min_max");         // [32..] = a - b, [48..] = min, [64..] = max
        lvq(0, 0); lvq(1, 16);
        v3(24, 1, QUAD, 2, 0, 1); svq(2, 32);
        v3(27, 2, QUAD, 2, 0, 1); svq(2, 48);
        v3(27, 3, QUAD, 2, 0, 1);
        a.ret(); svq(2, 64);

        a.label("t_v_dot");                 // [32] = dot([0..], [16..])
        lvq(0, 0); lvq(1, 16);
        v3(25, 1, QUAD, 3, 0, 1);           // vdot.q S030, C000, C010
        a.ret(); svs(3, 32);

        a.label("t_v_scl");                 // [32..] = [0..] * [16]
        lvq(0, 0); lvs(3, 16);
        v3(25, 2, QUAD, 2, 0, 3);           // vscl.q C020, C000, S030
        a.ret(); svq(2, 32);

        a.label("t_v_pfx_neg");             // [32..] = -[0..] über vpfxs, danach ohne Präfix [48..] = [0..]
        lvq(0, 0);
        a.emit((55u << 26) | 0xE4u | (0xFu << 16));   // vpfxs [-x,-y,-z,-w]
        v4(0, 0, QUAD, 2, 0);               // vmov.q C020, C000
        svq(2, 32);
        v4(0, 0, QUAD, 2, 0);
        a.ret(); svq(2, 48);

        a.label("t_v_neg_zero_one");        // [32..] = -[0..] (vneg), [48..] = 0, [64..] = 1
        lvq(0, 0);
        v4(0, 2, QUAD, 2, 0); svq(2, 32);
        v4(0, 6, QUAD, 2, 0); svq(2, 48);
        v4(0, 7, QUAD, 2, 0);
        a.ret(); svq(2, 64);

        a.label("t_v_color");               // [16..] = vc2i([0]), [32] = vi2c([16..])
        lvs(0, 0);
        v4(1, 25, 0, 1, 0);                 // vc2i.s C010, S000
        svq(1, 16);
        v4(1, 29, QUAD, 3, 1);              // vi2c.q S030, C010
        a.ret(); svs(3, 32);

        a.label("t_v_cmp");                 // v0 = [0] < [4] (vcmp.s LT, bvt 0)
        lvs(0, 0); lvs(1, 4);
        a.emit((27u << 26) | (1u << 16) | (0u << 8) | 2u);   // vcmp.s LT, S000, S010
        a.fixes.push_back({a.w.size(), "t_v_cmp_yes", false});
        a.emit((18u << 26) | (8u << 21) | (1u << 16));       // bvt 0
        a.addiu(V0, ZERO, 0);
        a.ret(); a.nop();
        a.label("t_v_cmp_yes");
        a.ret();
        a.addiu(V0, ZERO, 1);

        a.label("t_v_mmul");                // M200 = vmmul(M000, M100); Spalten bei [0..63], [64..127] -> [128..191]
        for (u32 c = 0; c < 4; c++) { lvq(c, (s32)(c * 16)); lvq(4 + c, (s32)(64 + c * 16)); }
        a.emit((60u << 26) | (4u << 16) | (0u << 8) | QUAD | 8u);   // vmmul.q M200, M000, M100
        svq(8, 128); svq(9, 144); svq(10, 160);
        a.ret(); svq(11, 176);

        a.label("t_v_mtv");                 // mtv a1 -> S030, mfv v0 <- S030
        a.emit((18u << 26) | (7u << 21) | (A1 << 16) | 3u);
        a.ret();
        a.emit((18u << 26) | (3u << 21) | (V0 << 16) | 3u);

        // T16: Hook, Originalcode darf nicht laufen
        a.label("t_hooked");
        a.ret();
        a.addiu(V0, ZERO, 1);

        const u32 text_size = (u32)a.w.size() * 4;
        a.label("stub");
        a.ret(); a.nop();
        a.resolve();

        Module m;
        m.name = "TestModule";
        m.sha1 = "test";
        m.load_addr = m.image_addr = m.code_addr = BASE;
        m.entry = BASE;
        m.text_size = text_size;
        m.text_ranges.push_back({BASE, BASE + text_size});
        m.stub_addr = BASE + text_size;
        m.stub_size = 8;
        m.code_size = text_size + 8;
        m.image.assign(IMAGE_SIZE, 0);
        for (size_t k = 0; k < a.w.size(); k++) m.write32(BASE + (u32)k * 4, a.w[k]);
        m.write32(TABLE + 0, a.labels["t6_c0"]);
        m.write32(TABLE + 4, a.labels["t6_c1"]);
        m.write32(TABLE + 8, a.labels["t6_c2"]);
        m.imports.push_back({"TestLib", 0x11223344u, m.stub_addr});

        Config cfg;
        cfg.id = cfg.exe_name = "recomp_test";
        cfg.functions_per_file = 100000;
        cfg.safepoints = true;
        cfg.hooks.push_back({a.labels["t_hooked"], "test_hook"});

        Analysis an = analyze(m, cfg, {});
        if (an.jump_tables.size() != 1 || an.jump_tables.begin()->second.size() != 3)
            throw std::runtime_error("Sprungtabelle im Testprogramm nicht erkannt");
        EmitStats st = emit_code(m, cfg, an, argv[1], argv[2]);
        if (st.unimplemented) throw std::runtime_error("Testprogramm enthält nicht übersetzte Befehle");
        if (!an.suspicious_ptrs.empty()) throw std::runtime_error("Zeigerprüfung meldet verdächtige Zeiger im Testprogramm");

        std::ofstream h(std::filesystem::path(argv[1]) / "test_defs.h", std::ios::binary);
        h << "// Erzeugt von test_gen.\n";
        h << strf("#define TEST_IMAGE_ADDR 0x%08Xu\n#define TEST_IMAGE_SIZE 0x%Xu\n", BASE, IMAGE_SIZE);
        for (const auto& l : a.labels)
            if ((l.first.rfind("t_", 0) == 0 && l.first.find("_yes") == std::string::npos) || l.first.rfind("g_", 0) == 0 || l.first == "stub")
                h << strf("#define ADDR_%s 0x%08Xu\n", l.first.c_str(), l.second);
        h << "static const unsigned char test_image[TEST_IMAGE_SIZE] = {";
        size_t last = m.image.size();
        while (last > 0 && m.image[last - 1] == 0) last--;
        for (size_t k = 0; k < last; k++) h << (k % 24 == 0 ? "\n" : "") << (int)m.image[k] << ",";
        h << "\n};\n";
        fprintf(stderr, "test_gen: %u Funktionen, %u Befehle\n", st.functions, st.instructions);
        return 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "test_gen: %s\n", e.what());
        return 1;
    }
}
