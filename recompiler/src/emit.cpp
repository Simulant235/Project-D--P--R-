#include <algorithm>
#include <filesystem>
#include <fstream>

#include "recomp.h"

namespace {

std::string R(int n) { return n ? strf("ctx->r[%d]", n) : std::string("0u"); }
std::string U(u32 v) { return strf("0x%Xu", v); }
std::string F(int n) { return strf("ctx->f[%d]", n); }

// rs + imm als u32-Ausdruck
std::string EA(const Insn& in) {
    if (in.imm == 0) return R(in.rs);
    if (in.rs == 0) return U((u32)in.imm);
    return R(in.rs) + " + " + U((u32)in.imm);
}

struct Emitter {
    const Module& m;
    const Config& cfg;
    const Analysis& an;
    EmitStats stats;
    std::map<u32, const Import*> stub_at;
    std::map<u32, std::string> hook_at;
    std::vector<u32> starts;   // sortiert

    std::string unimpl(const Insn& in, const std::string& name) {
        stats.unimplemented++;
        stats.unimplemented_by_name[name]++;
        return strf("UNIMPLEMENTED(0x%08Xu, \"%s\");", in.addr, name.c_str());
    }

    static std::string W(int reg, const std::string& expr) {
        return reg ? strf("ctx->r[%d] = %s;", reg, expr.c_str()) : std::string();
    }

    // Befehl ohne Kontrollfluss; leerer String = keine Wirkung
    std::string simple(const Insn& in) {
        const std::string rs = R(in.rs), rt = R(in.rt);
        const int fs = in.rd, ft = in.rt, fd = in.sa;
        switch (in.op) {
        case Op::NOP: case Op::CACHE: case Op::SYNC: return "";
        case Op::SLL: return W(in.rd, strf("%s << %d", rt.c_str(), in.sa));
        case Op::SRL: return W(in.rd, strf("%s >> %d", rt.c_str(), in.sa));
        case Op::SRA: return W(in.rd, strf("(uint32_t)((int32_t)%s >> %d)", rt.c_str(), in.sa));
        case Op::ROTR: return W(in.rd, strf("psp_rotr(%s, %d)", rt.c_str(), in.sa));
        case Op::SLLV: return W(in.rd, strf("%s << (%s & 31)", rt.c_str(), rs.c_str()));
        case Op::SRLV: return W(in.rd, strf("%s >> (%s & 31)", rt.c_str(), rs.c_str()));
        case Op::SRAV: return W(in.rd, strf("(uint32_t)((int32_t)%s >> (%s & 31))", rt.c_str(), rs.c_str()));
        case Op::ROTRV: return W(in.rd, strf("psp_rotr(%s, %s & 31)", rt.c_str(), rs.c_str()));
        case Op::MOVZ: return in.rd ? strf("if (%s == 0) ctx->r[%d] = %s;", rt.c_str(), in.rd, rs.c_str()) : "";
        case Op::MOVN: return in.rd ? strf("if (%s != 0) ctx->r[%d] = %s;", rt.c_str(), in.rd, rs.c_str()) : "";
        case Op::BREAK: return strf("psp_break(ctx, 0x%08Xu, %uu);", in.addr, (in.raw >> 6) & 0xFFFFF);
        case Op::MFHI: return W(in.rd, "ctx->hi");
        case Op::MFLO: return W(in.rd, "ctx->lo");
        case Op::MTHI: return strf("ctx->hi = %s;", rs.c_str());
        case Op::MTLO: return strf("ctx->lo = %s;", rs.c_str());
        case Op::CLZ: return W(in.rd, strf("psp_clz(%s)", rs.c_str()));
        case Op::CLO: return W(in.rd, strf("psp_clz(~%s)", rs.c_str()));
        case Op::MULT: return strf("psp_mult(ctx, %s, %s);", rs.c_str(), rt.c_str());
        case Op::MULTU: return strf("psp_multu(ctx, %s, %s);", rs.c_str(), rt.c_str());
        case Op::DIV: return strf("psp_div(ctx, %s, %s);", rs.c_str(), rt.c_str());
        case Op::DIVU: return strf("psp_divu(ctx, %s, %s);", rs.c_str(), rt.c_str());
        case Op::MADD: return strf("psp_madd(ctx, %s, %s);", rs.c_str(), rt.c_str());
        case Op::MADDU: return strf("psp_maddu(ctx, %s, %s);", rs.c_str(), rt.c_str());
        case Op::MSUB: return strf("psp_msub(ctx, %s, %s);", rs.c_str(), rt.c_str());
        case Op::MSUBU: return strf("psp_msubu(ctx, %s, %s);", rs.c_str(), rt.c_str());
        case Op::ADD: case Op::ADDU: return W(in.rd, in.rt == 0 ? rs : in.rs == 0 ? rt : rs + " + " + rt);
        case Op::SUB: case Op::SUBU: return W(in.rd, rs + " - " + rt);
        case Op::AND: return W(in.rd, rs + " & " + rt);
        case Op::OR: return W(in.rd, in.rt == 0 ? rs : in.rs == 0 ? rt : rs + " | " + rt);
        case Op::XOR: return W(in.rd, rs + " ^ " + rt);
        case Op::NOR: return W(in.rd, "~(" + rs + " | " + rt + ")");
        case Op::SLT: return W(in.rd, strf("(uint32_t)((int32_t)%s < (int32_t)%s)", rs.c_str(), rt.c_str()));
        case Op::SLTU: return W(in.rd, strf("(uint32_t)(%s < %s)", rs.c_str(), rt.c_str()));
        case Op::MAX: return W(in.rd, strf("psp_max(%s, %s)", rs.c_str(), rt.c_str()));
        case Op::MIN: return W(in.rd, strf("psp_min(%s, %s)", rs.c_str(), rt.c_str()));
        case Op::ADDI: case Op::ADDIU: return W(in.rt, EA(in));
        case Op::SLTI: return W(in.rt, strf("(uint32_t)((int32_t)%s < (int32_t)%s)", rs.c_str(), U((u32)in.imm).c_str()));
        case Op::SLTIU: return W(in.rt, strf("(uint32_t)(%s < %s)", rs.c_str(), U((u32)in.imm).c_str()));
        case Op::ANDI: return W(in.rt, rs + " & " + U(in.uimm));
        case Op::ORI: return W(in.rt, in.rs == 0 ? U(in.uimm) : rs + " | " + U(in.uimm));
        case Op::XORI: return W(in.rt, rs + " ^ " + U(in.uimm));
        case Op::LUI: return W(in.rt, U(in.uimm << 16));
        case Op::EXT: {
            const u32 size = (u32)in.rd + 1, mask = size >= 32 ? 0xFFFFFFFFu : ((1u << size) - 1);
            return W(in.rt, strf("(%s >> %d) & %s", rs.c_str(), in.sa, U(mask).c_str()));
        }
        case Op::INS: {
            const int size = (int)in.rd - (int)in.sa + 1;
            if (size <= 0) return unimpl(in, "ins?");
            const u32 mask = (size >= 32 ? 0xFFFFFFFFu : ((1u << size) - 1)) << in.sa;
            return W(in.rt, strf("(%s & %s) | ((%s << %d) & %s)", rt.c_str(), U(~mask).c_str(), rs.c_str(), in.sa, U(mask).c_str()));
        }
        case Op::SEB: return W(in.rd, strf("(uint32_t)(int32_t)(int8_t)%s", rt.c_str()));
        case Op::SEH: return W(in.rd, strf("(uint32_t)(int32_t)(int16_t)%s", rt.c_str()));
        case Op::WSBH: return W(in.rd, strf("psp_wsbh(%s)", rt.c_str()));
        case Op::WSBW: return W(in.rd, strf("psp_wsbw(%s)", rt.c_str()));
        case Op::BITREV: return W(in.rd, strf("psp_bitrev(%s)", rt.c_str()));
        case Op::LB: return W(in.rt, strf("(uint32_t)(int32_t)(int8_t)psp_r8(mem, %s)", EA(in).c_str()));
        case Op::LBU: return W(in.rt, strf("psp_r8(mem, %s)", EA(in).c_str()));
        case Op::LH: return W(in.rt, strf("(uint32_t)(int32_t)(int16_t)psp_r16(mem, %s)", EA(in).c_str()));
        case Op::LHU: return W(in.rt, strf("psp_r16(mem, %s)", EA(in).c_str()));
        case Op::LW: return W(in.rt, strf("psp_r32(mem, %s)", EA(in).c_str()));
        case Op::LWL: return W(in.rt, strf("psp_lwl(mem, %s, %s)", EA(in).c_str(), rt.c_str()));
        case Op::LWR: return W(in.rt, strf("psp_lwr(mem, %s, %s)", EA(in).c_str(), rt.c_str()));
        case Op::SB: return strf("psp_w8(mem, %s, (uint8_t)%s);", EA(in).c_str(), rt.c_str());
        case Op::SH: return strf("psp_w16(mem, %s, (uint16_t)%s);", EA(in).c_str(), rt.c_str());
        case Op::SW: return strf("psp_w32(mem, %s, %s);", EA(in).c_str(), rt.c_str());
        case Op::SWL: return strf("psp_swl(mem, %s, %s);", EA(in).c_str(), rt.c_str());
        case Op::SWR: return strf("psp_swr(mem, %s, %s);", EA(in).c_str(), rt.c_str());
        case Op::LWC1: return strf("%s.u = psp_r32(mem, %s);", F(ft).c_str(), EA(in).c_str());
        case Op::SWC1: return strf("psp_w32(mem, %s, %s.u);", EA(in).c_str(), F(ft).c_str());
        case Op::MFC1: return W(in.rt, F(fs) + ".u");
        case Op::MTC1: return strf("%s.u = %s;", F(fs).c_str(), rt.c_str());
        case Op::CFC1:
            if (fs == 31) return W(in.rt, "(ctx->fcr31 & ~0x00800000u) | (ctx->fcc << 23)");
            return W(in.rt, fs == 0 ? "0x00003351u" : "0u");
        case Op::CTC1:
            if (fs == 31) return strf("ctx->fcr31 = %s & 0x0181FFFFu; ctx->fcc = (%s >> 23) & 1u;", rt.c_str(), rt.c_str());
            return "";
        case Op::MOV_S: return strf("%s.u = %s.u;", F(fd).c_str(), F(fs).c_str());
        case Op::FPU_OTHER: return fpu(in);
        case Op::VFPU_OTHER: return vfpu(in);
        case Op::SYSCALL: return unimpl(in, "syscall");
        case Op::COP0_OTHER: return unimpl(in, "cop0");
        default: return unimpl(in, strf("invalid:%08X", in.raw));
        }
    }


    // COP1, Format S bzw. W. fs = rd, ft = rt, fd = sa
    std::string fpu(const Insn& in) {
        const std::string fs = F(in.rd), ft = F(in.rt), fd = F(in.sa);
        const u32 fn = in.raw & 63;
        if (in.rs == 20) {   // Format W
            if (fn == 32) return strf("%s.f = (float)%s.i;", fd.c_str(), fs.c_str());   // cvt.s.w
            return unimpl(in, mnemonic(in));
        }
        switch (fn) {
        case 0: return strf("%s.f = %s.f + %s.f;", fd.c_str(), fs.c_str(), ft.c_str());
        case 1: return strf("%s.f = %s.f - %s.f;", fd.c_str(), fs.c_str(), ft.c_str());
        case 2: return strf("%s.f = psp_fmul(%s.f, %s.f);", fd.c_str(), fs.c_str(), ft.c_str());
        case 3: return strf("%s.f = psp_fdiv(%s.f, %s.f);", fd.c_str(), fs.c_str(), ft.c_str());
        case 4: return strf("%s.f = psp_fsqrt(%s.f);", fd.c_str(), fs.c_str());
        case 5: return strf("%s.u = %s.u & 0x7FFFFFFFu;", fd.c_str(), fs.c_str());
        case 7: return strf("%s.u = %s.u ^ 0x80000000u;", fd.c_str(), fs.c_str());
        case 12: return strf("%s.u = psp_f2i_round(%s.f);", fd.c_str(), fs.c_str());
        case 13: return strf("%s.u = psp_f2i_trunc(%s.f);", fd.c_str(), fs.c_str());
        case 14: return strf("%s.u = psp_f2i_ceil(%s.f);", fd.c_str(), fs.c_str());
        case 15: return strf("%s.u = psp_f2i_floor(%s.f);", fd.c_str(), fs.c_str());
        case 36: return strf("%s.u = psp_f2i_cvt(ctx, %s.f);", fd.c_str(), fs.c_str());
        default:
            if (fn >= 48) return strf("ctx->fcc = psp_fcmp(%s.f, %s.f, %u);", fs.c_str(), ft.c_str(), fn & 15);
            return unimpl(in, mnemonic(in));
        }
    }

    // VFPU -> Helfer aus psp_vfpu.h. Kodierung nach PPSSPP (Core/MIPS/MIPSTables.cpp), Rest bleibt UNIMPLEMENTED
    std::string vfpu(const Insn& in) {
        static const char* const SUF[4] = {".s", ".p", ".t", ".q"};
        const u32 w = in.raw, opc = w >> 26;
        const int vd = w & 0x7F, vs = (w >> 8) & 0x7F, vt = (w >> 16) & 0x7F;
        const int sz = (int)(((w >> 7) & 1) | ((w >> 14) & 2));
        auto named = [&](const std::string& name, const std::string& code) {
            stats.vfpu_by_name[name]++;
            return code;
        };
        auto op3 = [&](const char* name, const char* id) {
            return named(std::string(name) + SUF[sz], strf("psp_vfpu_op3(ctx, %s, %d, %d, %d, %d);", id, sz, vd, vs, vt));
        };
        auto op2 = [&](const char* name, const char* id) {
            return named(std::string(name) + SUF[sz], strf("psp_vfpu_op2(ctx, %s, %d, %d, %d);", id, sz, vd, vs));
        };
        auto op1 = [&](const char* name, const char* id) {
            return named(std::string(name) + SUF[sz], strf("psp_vfpu_op1(ctx, %s, %d, %d);", id, sz, vd));
        };
        // lv/sv: Offset ohne untere 2 Bit
        const u32 off = (u32)(s32)(s16)(w & 0xFFFC);
        const std::string ea = off == 0 ? R(in.rs) : in.rs == 0 ? U(off) : R(in.rs) + " + " + U(off);
        const int vts = (int)(((w >> 16) & 0x1F) | ((w & 3) << 5)), vtq = (int)(((w >> 16) & 0x1F) | ((w & 1) << 5));

        switch (opc) {
        case 18: {   // COP2: mfv/mfvc, mtv/mtvc
            const int imm = w & 0xFF;
            if (in.rs == 3) {
                if (imm < 128) return named("mfv", W(in.rt, strf("psp_vfpu_ri(ctx, %d)", imm)));
                return named("mfvc", W(in.rt, strf("psp_vfpu_mfvc(ctx, %d)", imm - 128)));
            }
            if (in.rs == 7) {
                if (imm < 128) return named("mtv", strf("psp_vfpu_wi(ctx, %d, %s);", imm, R(in.rt).c_str()));
                return named("mtvc", strf("psp_vfpu_mtvc(ctx, %d, %s);", imm - 128, R(in.rt).c_str()));
            }
            break;
        }
        case 24:
            switch ((w >> 23) & 7) {
            case 0: return op3("vadd", "PSP_VOP3_ADD");
            case 1: return op3("vsub", "PSP_VOP3_SUB");
            case 7: return op3("vdiv", "PSP_VOP3_DIV");
            }
            break;
        case 25:
            switch ((w >> 23) & 7) {
            case 0: return op3("vmul", "PSP_VOP3_MUL");
            case 1: return op3("vdot", "PSP_VOP3_DOT");
            case 2: return op3("vscl", "PSP_VOP3_SCL");
            }
            break;
        case 27:
            switch ((w >> 23) & 7) {
            case 0: return named(std::string("vcmp") + SUF[sz], strf("psp_vfpu_vcmp(ctx, %u, %d, %d, %d);", w & 15, sz, vs, vt));
            case 2: return op3("vmin", "PSP_VOP3_MIN");
            case 3: return op3("vmax", "PSP_VOP3_MAX");
            }
            break;
        case 50: return named("lv.s", strf("psp_vfpu_wi(ctx, %d, psp_r32(mem, %s));", vts, ea.c_str()));
        case 58: return named("sv.s", strf("psp_w32(mem, %s, psp_vfpu_ri(ctx, %d));", ea.c_str(), vts));
        case 54: return named("lv.q", strf("psp_vfpu_lvq(ctx, mem, %s, %d);", ea.c_str(), vtq));
        case 62: return named("sv.q", strf("psp_vfpu_svq(ctx, mem, %s, %d);", ea.c_str(), vtq));
        case 52: {
            const u32 sub = (w >> 16) & 31;
            if (in.rs == 0) {
                switch (sub) {
                case 0: return op2("vmov", "PSP_VOP2_MOV");
                case 1: return op2("vabs", "PSP_VOP2_ABS");
                case 2: return op2("vneg", "PSP_VOP2_NEG");
                case 6: return op1("vzero", "PSP_VOP1_ZERO");
                case 7: return op1("vone", "PSP_VOP1_ONE");
                case 16: return op2("vrcp", "PSP_VOP2_RCP");
                case 17: return op2("vrsq", "PSP_VOP2_RSQ");
                case 22: return op2("vsqrt", "PSP_VOP2_SQRT");
                }
            } else if (in.rs == 1) {
                if (sub == 25) return op2("vc2i", "PSP_VOP2_C2I");
                if (sub == 29) return op2("vi2c", "PSP_VOP2_I2C");
            } else if (in.rs == 21) {
                const int tf = (w >> 19) & 1, imm3 = (w >> 16) & 7;
                return named(std::string(tf ? "vcmovf" : "vcmovt") + SUF[sz],
                             strf("psp_vfpu_vcmov(ctx, %d, %d, %d, %d, %d);", sz, vd, vs, imm3, tf));
            }
            break;
        }
        case 55: {
            const u32 sub = (w >> 23) & 7;
            static const char* const PFX[3] = {"vpfxs", "vpfxt", "vpfxd"};
            if (sub < 6) return named(PFX[sub >> 1], strf("psp_vfpu_pfx(ctx, %u, 0x%Xu);", sub >> 1, w & 0xFFFFF));
            break;
        }
        case 60:
            if (in.rs < 4 && sz != 0)
                return named(std::string("vmmul") + SUF[sz], strf("psp_vfpu_vmmul(ctx, %d, %d, %d, %d);", sz, vd, vs, vt));
            break;
        }
        return unimpl(in, strf("vfpu:%08X", w));
    }

    static std::string cond(const Insn& in) {
        const std::string rs = R(in.rs), rt = R(in.rt);
        switch (in.op) {
        case Op::BEQ: case Op::BEQL: return rs + " == " + rt;
        case Op::BNE: case Op::BNEL: return rs + " != " + rt;
        case Op::BLEZ: case Op::BLEZL: return "(int32_t)" + rs + " <= 0";
        case Op::BGTZ: case Op::BGTZL: return "(int32_t)" + rs + " > 0";
        case Op::BLTZ: case Op::BLTZL: case Op::BLTZAL: case Op::BLTZALL: return "(int32_t)" + rs + " < 0";
        case Op::BGEZ: case Op::BGEZL: case Op::BGEZAL: case Op::BGEZALL: return "(int32_t)" + rs + " >= 0";
        case Op::BC1F: case Op::BC1FL: return "ctx->fcc == 0";
        case Op::BC1T: case Op::BC1TL: return "ctx->fcc != 0";
        case Op::BVF: case Op::BVFL: return strf("((ctx->vfpu_ctrl[PSP_VFPU_CTRL_CC] >> %u) & 1u) == 0", (in.raw >> 18) & 7);
        case Op::BVT: case Op::BVTL: return strf("((ctx->vfpu_ctrl[PSP_VFPU_CTRL_CC] >> %u) & 1u) != 0", (in.raw >> 18) & 7);
        default: return "0";
        }
    }

    // Call auf feste Adresse
    std::string call(u32 t) {
        auto st = stub_at.find(t);
        if (st != stub_at.end()) return strf("hle_%s_0x%08X(ctx, mem);", st->second->lib.c_str(), st->second->nid);
        if (!(t & 3) && m.in_text(t) && an.starts.count(t)) return strf("fn_%08X(ctx, mem);", t);
        return strf("psp_call_indirect(ctx, mem, 0x%08Xu);", t);
    }

    void emit_function(std::string& out, u32 s, u32 e) {
        out += strf("PSP_FUNC(%08X) {\n", s);
        auto hk = hook_at.find(s);
        if (hk != hook_at.end()) {
            out += strf("    %s(ctx, mem);\n}\n\n", hk->second.c_str());
            return;
        }
        // Labels für alle Ziele in der Funktion
        std::set<u32> labels;
        for (u32 a = s; a < e; a += 4) {
            const Insn& in = an.at(m, a);
            if ((is_cond_branch(in.op) || in.op == Op::J) && in.target >= s && in.target < e && !(in.target & 3)) labels.insert(in.target);
            if (in.op == Op::JR) {
                auto jt = an.jump_tables.find(a);
                if (jt != an.jump_tables.end())
                    for (u32 t : jt->second)
                        if (t >= s && t < e) labels.insert(t);
            }
        }
        const std::string I = "    ";
        auto jump = [&](u32 from, u32 t) -> std::string {
            if (t >= s && t < e && !(t & 3)) {
                std::string sp = (cfg.safepoints && t <= from) ? "PSP_SAFEPOINT(); " : "";
                return sp + strf("goto L_%08X;", t);
            }
            return call(t) + " return;";
        };
        const std::string fallthrough = e < m.code_addr + m.code_size ? call(e) + " return;" : std::string("return;");

        bool falls_out = true;
        for (u32 a = s; a < e;) {
            const Insn& in = an.at(m, a);
            stats.instructions++;
            if (labels.count(a)) out += strf("L_%08X:;\n", a);
            if (!has_delay_slot(in.op)) {
                std::string c = simple(in);
                if (!c.empty()) out += I + c + "\n";
                a += 4;
                falls_out = true;
                continue;
            }
            // mit Delay Slot
            std::string D;
            const u32 da = a + 4;
            if (m.in_code(da)) {
                const Insn& ds = an.at(m, da);
                if (da < e) stats.instructions++;
                D = has_delay_slot(ds.op) ? unimpl(ds, "branch-in-delay-slot") : simple(ds);
            }
            const std::string Ds = D.empty() ? "" : D + " ";
            bool can_fall = true;
            if (is_cond_branch(in.op) && !is_unconditional(in)) {
                if (is_likely(in.op)) out += I + strf("if (%s) { %s%s }\n", cond(in).c_str(), Ds.c_str(), jump(a, in.target).c_str());
                else if (D.empty()) out += I + strf("if (%s) { %s }\n", cond(in).c_str(), jump(a, in.target).c_str());
                else out += I + strf("{ int c = (%s); %sif (c) { %s } }\n", cond(in).c_str(), Ds.c_str(), jump(a, in.target).c_str());
            } else if (is_cond_branch(in.op) || in.op == Op::J) {   // unbedingt
                out += I + Ds + jump(a, in.target) + "\n";
                can_fall = false;
            } else if (in.op == Op::JAL) {
                out += I + strf("ctx->r[31] = 0x%08Xu; %s%s\n", a + 8, Ds.c_str(), call(in.target).c_str());
            } else if (is_link_branch(in.op)) {
                std::string body = is_likely(in.op) ? Ds + call(in.target) : call(in.target);
                std::string pre = is_likely(in.op) ? "" : Ds;
                out += I + strf("{ int c = (%s); ctx->r[31] = 0x%08Xu; %sif (c) { %s } }\n", cond(in).c_str(), a + 8, pre.c_str(), body.c_str());
            } else if (in.op == Op::JALR) {
                std::string link = in.rd ? strf("ctx->r[%d] = 0x%08Xu; ", in.rd, a + 8) : "";
                out += I + strf("{ uint32_t t = %s; %s%spsp_call_indirect(ctx, mem, t); }\n", R(in.rs).c_str(), link.c_str(), Ds.c_str());
            } else if (in.op == Op::JR && in.rs == 31) {
                out += I + Ds + "return;\n";
                can_fall = false;
            } else {   // jr <reg>
                auto jt = an.jump_tables.find(a);
                if (jt == an.jump_tables.end()) {
                    out += I + strf("{ uint32_t t = %s; %spsp_call_indirect(ctx, mem, t); return; }\n", R(in.rs).c_str(), Ds.c_str());
                } else {
                    out += I + strf("{ uint32_t t = %s; %s\n", R(in.rs).c_str(), Ds.c_str());
                    out += I + "switch (t) {\n";
                    std::set<u32> uniq(jt->second.begin(), jt->second.end());
                    for (u32 t : uniq) out += I + strf("case 0x%08Xu: %s\n", t, jump(a, t).c_str());
                    out += I + "default: psp_call_indirect(ctx, mem, t); return;\n" + I + "} }\n";
                }
                can_fall = false;
            }
            // Delay Slot als Sprungziel: eigene Kopie hinter dem Sprung
            if (da < e && labels.count(da)) {
                if (can_fall) {
                    if (a + 8 < e) {
                        labels.insert(a + 8);
                        out += I + strf("goto L_%08X;\n", a + 8);
                    } else {
                        out += I + fallthrough + "\n";
                    }
                }
                out += strf("L_%08X:;\n", da);
                if (!D.empty()) out += I + D + "\n";
                can_fall = true;
            }
            falls_out = can_fall;
            a += 8;
        }
        if (falls_out) out += I + fallthrough + "\n";
        out += "}\n\n";
    }
};

const char* const OPS_H = R"(// recomp_ops.h: Hilfsfunktionen für generierten Code. Vom Recompiler erzeugt, nicht bearbeiten.
#ifndef DIVA_RECOMP_OPS_H
#define DIVA_RECOMP_OPS_H
#include <math.h>
#include "psp_context.h"
#include "psp_vfpu.h"

static inline uint32_t psp_rotr(uint32_t v, uint32_t n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
static inline uint32_t psp_clz(uint32_t v) { uint32_t n = 0; if (!v) return 32; while (!(v & 0x80000000u)) { v <<= 1; n++; } return n; }
static inline uint32_t psp_max(uint32_t a, uint32_t b) { return (int32_t)a > (int32_t)b ? a : b; }
static inline uint32_t psp_min(uint32_t a, uint32_t b) { return (int32_t)a < (int32_t)b ? a : b; }
static inline uint32_t psp_wsbh(uint32_t v) { return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); }
static inline uint32_t psp_wsbw(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
static inline uint32_t psp_bitrev(uint32_t v) { uint32_t r = 0; int i; for (i = 0; i < 32; i++) { r = (r << 1) | (v & 1); v >>= 1; } return r; }

static inline void psp_mult(PspContext* ctx, uint32_t a, uint32_t b) {
    int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b; ctx->lo = (uint32_t)p; ctx->hi = (uint32_t)((uint64_t)p >> 32);
}
static inline void psp_multu(PspContext* ctx, uint32_t a, uint32_t b) {
    uint64_t p = (uint64_t)a * (uint64_t)b; ctx->lo = (uint32_t)p; ctx->hi = (uint32_t)(p >> 32);
}
static inline void psp_madd(PspContext* ctx, uint32_t a, uint32_t b) {
    uint64_t acc = ((uint64_t)ctx->hi << 32) | ctx->lo; acc += (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b);
    ctx->lo = (uint32_t)acc; ctx->hi = (uint32_t)(acc >> 32);
}
static inline void psp_maddu(PspContext* ctx, uint32_t a, uint32_t b) {
    uint64_t acc = ((uint64_t)ctx->hi << 32) | ctx->lo; acc += (uint64_t)a * (uint64_t)b;
    ctx->lo = (uint32_t)acc; ctx->hi = (uint32_t)(acc >> 32);
}
static inline void psp_msub(PspContext* ctx, uint32_t a, uint32_t b) {
    uint64_t acc = ((uint64_t)ctx->hi << 32) | ctx->lo; acc -= (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b);
    ctx->lo = (uint32_t)acc; ctx->hi = (uint32_t)(acc >> 32);
}
static inline void psp_msubu(PspContext* ctx, uint32_t a, uint32_t b) {
    uint64_t acc = ((uint64_t)ctx->hi << 32) | ctx->lo; acc -= (uint64_t)a * (uint64_t)b;
    ctx->lo = (uint32_t)acc; ctx->hi = (uint32_t)(acc >> 32);
}
/* Division: Sonderfälle wie auf der PSP (kein Trap; Werte wie im PPSSPP-Interpreter). */
static inline void psp_div(PspContext* ctx, uint32_t ua, uint32_t ub) {
    int32_t a = (int32_t)ua, b = (int32_t)ub;
    if (ua == 0x80000000u && b == -1) { ctx->lo = 0x80000000u; ctx->hi = 0xFFFFFFFFu; }
    else if (b != 0) { ctx->lo = (uint32_t)(a / b); ctx->hi = (uint32_t)(a % b); }
    else { ctx->lo = a < 0 ? 1u : 0xFFFFFFFFu; ctx->hi = ua; }
}
static inline void psp_divu(PspContext* ctx, uint32_t a, uint32_t b) {
    if (b != 0) { ctx->lo = a / b; ctx->hi = a % b; }
    else { ctx->lo = a <= 0xFFFFu ? 0xFFFFu : 0xFFFFFFFFu; ctx->hi = a; }
}
/* Unausgerichtete Zugriffe (little-endian) */
static inline uint32_t psp_lwl(const uint8_t* mem, uint32_t a, uint32_t rt) {
    uint32_t sh = (a & 3) * 8, w = psp_r32(mem, a & ~3u); return (rt & (0x00FFFFFFu >> sh)) | (w << (24 - sh));
}
static inline uint32_t psp_lwr(const uint8_t* mem, uint32_t a, uint32_t rt) {
    uint32_t sh = (a & 3) * 8, w = psp_r32(mem, a & ~3u); return (rt & (0xFFFFFF00u << (24 - sh))) | (w >> sh);
}
static inline void psp_swl(uint8_t* mem, uint32_t a, uint32_t rt) {
    uint32_t sh = (a & 3) * 8, w = psp_r32(mem, a & ~3u); psp_w32(mem, a & ~3u, (rt >> (24 - sh)) | (w & (0xFFFFFF00u << sh)));
}
static inline void psp_swr(uint8_t* mem, uint32_t a, uint32_t rt) {
    uint32_t sh = (a & 3) * 8, w = psp_r32(mem, a & ~3u); psp_w32(mem, a & ~3u, (rt << sh) | (w & (0x00FFFFFFu >> (24 - sh))));
}

/* FPU. Verhalten der Sonderfälle wie im PPSSPP-Interpreter (Core/MIPS/Interpreter.cpp). */
static inline float psp_fmul(float a, float b) {   /* inf * 0 ergibt das positive NaN der PSP */
    float r = a * b;
    if (r != r && a == a && b == b) { uint32_t u = 0x7FC00000u; memcpy(&r, &u, 4); }
    return r;
}
static inline float psp_fdiv(float a, float b) { return a / b; }
static inline float psp_fsqrt(float a) {            /* negativ: positives NaN */
    float r = sqrtf(a);
    if (a < 0.0f) { uint32_t u = 0x7FC00000u; memcpy(&r, &u, 4); }
    return r;
}
static inline uint32_t psp_f2i_sat(float v) {        /* v ist bereits gerundet */
    if (v != v) return 0x7FFFFFFFu;
    if (v >= 2147483648.0f) return 0x7FFFFFFFu;
    if (v < -2147483648.0f) return 0x80000000u;
    return (uint32_t)(int32_t)v;
}
static inline uint32_t psp_f2i_trunc(float v) { return psp_f2i_sat(truncf(v)); }
static inline uint32_t psp_f2i_ceil(float v) { return psp_f2i_sat(ceilf(v)); }
static inline uint32_t psp_f2i_floor(float v) { return psp_f2i_sat(floorf(v)); }
static inline uint32_t psp_f2i_round(float v) {      /* zur nächsten, bei Gleichstand zur geraden Zahl */
    float f = floorf(v), d = v - f;
    if (d > 0.5f || (d == 0.5f && fmodf(f, 2.0f) != 0.0f)) f += 1.0f;
    return psp_f2i_sat(f);
}
static inline uint32_t psp_f2i_cvt(const PspContext* ctx, float v) {   /* Rundungsmodus aus fcr31 */
    switch (ctx->fcr31 & 3) {
    case 1: return psp_f2i_trunc(v);
    case 2: return psp_f2i_ceil(v);
    case 3: return psp_f2i_floor(v);
    default: return psp_f2i_round(v);
    }
}
/* c.cond.s: Bit 0 = wahr bei NaN, Bit 1 = gleich, Bit 2 = kleiner (Bit 3 nur Signalisierung) */
static inline uint32_t psp_fcmp(float a, float b, uint32_t cond) {
    if (a != a || b != b) return cond & 1;
    return (uint32_t)(((cond & 2) && a == b) || ((cond & 4) && a < b));
}
#endif
)";

void write_file(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw RecompError(EXIT_CODEGEN, "kann nicht schreiben: " + path);
    f.write(text.data(), (std::streamsize)text.size());
    if (!f) throw RecompError(EXIT_CODEGEN, "Schreibfehler: " + path);
}

} // namespace

EmitStats emit_code(const Module& m, const Config& cfg, const Analysis& an, const std::string& out_dir,
                    const std::string& runtime_include) {
    namespace fs = std::filesystem;
    Emitter em{m, cfg, an, {}, {}, {}, {}};
    for (const Import& im : m.imports) em.stub_at[im.stub_addr] = &im;
    em.starts.assign(an.starts.begin(), an.starts.end());
    for (const HookCfg& h : cfg.hooks) {
        if (!an.starts.count(h.addr))
            throw RecompError(EXIT_USAGE, strf("[[hook]] 0x%08X ist kein Funktionsanfang", h.addr));
        em.hook_at[h.addr] = h.symbol;
    }

    const fs::path code_dir = fs::path(out_dir) / "code";
    std::error_code ec;
    fs::remove_all(code_dir, ec);
    fs::create_directories(code_dir, ec);
    if (ec) throw RecompError(EXIT_CODEGEN, "kann Ordner nicht anlegen: " + code_dir.string());

    // funcs.h
    std::string h = "// Vom Recompiler erzeugt. Stammt aus dem Spiel: nicht einchecken, nicht weitergeben.\n"
                    "#ifndef DIVA_RECOMP_FUNCS_H\n#define DIVA_RECOMP_FUNCS_H\n#include \"recomp_ops.h\"\n\n";
    for (const Import& im : m.imports) h += strf("PSP_HLE(%s, 0x%08X);\n", im.lib.c_str(), im.nid);
    for (const auto& hk : em.hook_at) h += strf("void %s(PspContext* ctx, uint8_t* mem);\n", hk.second.c_str());
    h += "\n";
    for (u32 s : em.starts) h += strf("PSP_FUNC(%08X);\n", s);
    for (const Import& im : m.imports) h += strf("PSP_FUNC(%08X);\n", im.stub_addr);
    h += "\n#endif\n";
    write_file((code_dir / "funcs.h").string(), h);
    write_file((code_dir / "recomp_ops.h").string(), OPS_H);

    // Funktionen auf Dateien verteilt
    const char* head = "// Vom Recompiler erzeugt. Stammt aus dem Spiel: nicht einchecken, nicht weitergeben.\n#include \"funcs.h\"\n\n";
    std::string out = head;
    u32 in_file = 0;
    auto flush = [&]() {
        if (in_file == 0) return;
        write_file((code_dir / strf("fn_%03u.c", em.stats.files)).string(), out);
        em.stats.files++;
        out = head;
        in_file = 0;
    };
    for (size_t i = 0; i < em.starts.size(); i++) {
        // bis zum nächsten harten Anfang, weiche Anfänge = nur Labels
        size_t k = i + 1;
        while (k < em.starts.size() && an.soft.count(em.starts[k])) k++;
        const u32 s = em.starts[i];
        const u32 e = std::min(k < em.starts.size() ? em.starts[k] : m.range_end(s), m.range_end(s));
        em.emit_function(out, s, e);
        em.stats.functions++;
        if (++in_file >= cfg.functions_per_file) flush();
    }
    flush();

    // module.c
    std::string mod = head;
    for (const Import& im : m.imports)
        mod += strf("PSP_FUNC(%08X) { hle_%s_0x%08X(ctx, mem); }\n", im.stub_addr, im.lib.c_str(), im.nid);
    mod += strf("\nstatic const PspFunc s_func_table[%u] = {\n", m.code_size / 4);
    for (u32 s : em.starts) mod += strf("    [%u] = fn_%08X,\n", (s - m.code_addr) / 4, s);
    for (const Import& im : m.imports)
        if (m.in_code(im.stub_addr)) mod += strf("    [%u] = fn_%08X,\n", (im.stub_addr - m.code_addr) / 4, im.stub_addr);
    mod += "};\n\n";
    mod += strf("static const PspModuleDesc s_module = {\n    PSP_INTERFACE_VERSION, \"%s\", \"%s\",\n"
                "    0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu, s_func_table\n};\n\n",
                m.name.c_str(), m.sha1.c_str(), m.load_addr, m.entry, m.gp, m.module_info_addr, m.code_addr, m.code_size);
    mod += "const PspModuleDesc* const g_psp_modules[] = { &s_module, 0 };\n";
    write_file((code_dir / "module.c").string(), mod);

    // CMakeLists.txt des generierten Codes
    std::string inc = runtime_include;
    std::replace(inc.begin(), inc.end(), '\\', '/');
    std::string cm = strf("# Vom Recompiler erzeugt.\ncmake_minimum_required(VERSION 3.25)\nproject(%s_code C)\n"
                          "set(DIVA_RUNTIME_INCLUDE \"%s\" CACHE PATH \"Ordner mit psp_context.h\")\n"
                          "file(GLOB DIVA_CODE_SOURCES CONFIGURE_DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/code/*.c\")\n"
                          "add_library(%s_code STATIC ${DIVA_CODE_SOURCES})\n"
                          "set_target_properties(%s_code PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)\n"
                          "target_include_directories(%s_code PUBLIC \"${DIVA_RUNTIME_INCLUDE}\" \"${CMAKE_CURRENT_SOURCE_DIR}/code\")\n"
                          "if(MSVC)\n    target_compile_options(%s_code PRIVATE /MP /W3 /wd4102 /wd4702 /bigobj)\nendif()\n",
                          cfg.exe_name.c_str(), inc.c_str(), cfg.exe_name.c_str(), cfg.exe_name.c_str(), cfg.exe_name.c_str(),
                          cfg.exe_name.c_str());
    write_file((fs::path(out_dir) / "CMakeLists.txt").string(), cm);
    return em.stats;
}
