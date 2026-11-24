// Allegrex-Dekoder; FPU-Rechen- und VFPU-Befehle nur benannt (T12)
#include "recomp.h"

Insn decode(u32 addr, u32 raw) {
    Insn in;
    in.addr = addr;
    in.raw = raw;
    in.rs = (raw >> 21) & 31;
    in.rt = (raw >> 16) & 31;
    in.rd = (raw >> 11) & 31;
    in.sa = (raw >> 6) & 31;
    in.uimm = raw & 0xFFFF;
    in.imm = (s16)(raw & 0xFFFF);
    const u32 opc = raw >> 26, funct = raw & 63;
    const u32 btarget = addr + 4 + ((u32)in.imm << 2);
    auto branch = [&](Op op) { in.op = op; in.target = btarget; };

    switch (opc) {
    case 0:   // SPECIAL
        switch (funct) {
        case 0: in.op = raw == 0 ? Op::NOP : Op::SLL; break;
        case 2: in.op = (in.rs & 1) ? Op::ROTR : Op::SRL; break;
        case 3: in.op = Op::SRA; break;
        case 4: in.op = Op::SLLV; break;
        case 6: in.op = (in.sa & 1) ? Op::ROTRV : Op::SRLV; break;
        case 7: in.op = Op::SRAV; break;
        case 8: in.op = Op::JR; break;
        case 9: in.op = Op::JALR; break;
        case 10: in.op = Op::MOVZ; break;
        case 11: in.op = Op::MOVN; break;
        case 12: in.op = Op::SYSCALL; break;
        case 13: in.op = Op::BREAK; break;
        case 15: in.op = Op::SYNC; break;
        case 16: in.op = Op::MFHI; break;
        case 17: in.op = Op::MTHI; break;
        case 18: in.op = Op::MFLO; break;
        case 19: in.op = Op::MTLO; break;
        case 22: in.op = Op::CLZ; break;
        case 23: in.op = Op::CLO; break;
        case 24: in.op = Op::MULT; break;
        case 25: in.op = Op::MULTU; break;
        case 26: in.op = Op::DIV; break;
        case 27: in.op = Op::DIVU; break;
        case 28: in.op = Op::MADD; break;
        case 29: in.op = Op::MADDU; break;
        case 32: in.op = Op::ADD; break;
        case 33: in.op = Op::ADDU; break;
        case 34: in.op = Op::SUB; break;
        case 35: in.op = Op::SUBU; break;
        case 36: in.op = Op::AND; break;
        case 37: in.op = Op::OR; break;
        case 38: in.op = Op::XOR; break;
        case 39: in.op = Op::NOR; break;
        case 42: in.op = Op::SLT; break;
        case 43: in.op = Op::SLTU; break;
        case 44: in.op = Op::MAX; break;
        case 45: in.op = Op::MIN; break;
        case 46: in.op = Op::MSUB; break;
        case 47: in.op = Op::MSUBU; break;
        default: break;
        }
        break;
    case 1:   // REGIMM
        switch (in.rt) {
        case 0: branch(Op::BLTZ); break;
        case 1: branch(Op::BGEZ); break;
        case 2: branch(Op::BLTZL); break;
        case 3: branch(Op::BGEZL); break;
        case 16: branch(Op::BLTZAL); break;
        case 17: branch(Op::BGEZAL); break;
        case 18: branch(Op::BLTZALL); break;
        case 19: branch(Op::BGEZALL); break;
        default: break;
        }
        break;
    case 2: in.op = Op::J; in.target = ((addr + 4) & 0xF0000000u) | ((raw & 0x03FFFFFFu) << 2); break;
    case 3: in.op = Op::JAL; in.target = ((addr + 4) & 0xF0000000u) | ((raw & 0x03FFFFFFu) << 2); break;
    case 4: branch(Op::BEQ); break;
    case 5: branch(Op::BNE); break;
    case 6: branch(Op::BLEZ); break;
    case 7: branch(Op::BGTZ); break;
    case 8: in.op = Op::ADDI; break;
    case 9: in.op = Op::ADDIU; break;
    case 10: in.op = Op::SLTI; break;
    case 11: in.op = Op::SLTIU; break;
    case 12: in.op = Op::ANDI; break;
    case 13: in.op = Op::ORI; break;
    case 14: in.op = Op::XORI; break;
    case 15: in.op = Op::LUI; break;
    case 16: in.op = Op::COP0_OTHER; break;
    case 17:  // COP1
        switch (in.rs) {
        case 0: in.op = Op::MFC1; break;
        case 2: in.op = Op::CFC1; break;
        case 4: in.op = Op::MTC1; break;
        case 6: in.op = Op::CTC1; break;
        case 8:
            switch (in.rt & 3) {
            case 0: branch(Op::BC1F); break;
            case 1: branch(Op::BC1T); break;
            case 2: branch(Op::BC1FL); break;
            case 3: branch(Op::BC1TL); break;
            }
            break;
        case 16: in.op = funct == 6 ? Op::MOV_S : Op::FPU_OTHER; break;
        case 20: in.op = Op::FPU_OTHER; break;
        default: break;
        }
        break;
    case 18:  // COP2 (VFPU)
        if (in.rs == 8) {
            switch (in.rt & 3) {
            case 0: branch(Op::BVF); break;
            case 1: branch(Op::BVT); break;
            case 2: branch(Op::BVFL); break;
            case 3: branch(Op::BVTL); break;
            }
        } else {
            in.op = Op::VFPU_OTHER;
        }
        break;
    case 20: branch(Op::BEQL); break;
    case 21: branch(Op::BNEL); break;
    case 22: branch(Op::BLEZL); break;
    case 23: branch(Op::BGTZL); break;
    case 24: case 25: case 27: in.op = Op::VFPU_OTHER; break;
    case 31:  // SPECIAL3
        if (funct == 0) in.op = Op::EXT;
        else if (funct == 4) in.op = Op::INS;
        else if (funct == 32) {
            switch (in.sa) {
            case 2: in.op = Op::WSBH; break;
            case 3: in.op = Op::WSBW; break;
            case 16: in.op = Op::SEB; break;
            case 20: in.op = Op::BITREV; break;
            case 24: in.op = Op::SEH; break;
            default: break;
            }
        }
        break;
    case 32: in.op = Op::LB; break;
    case 33: in.op = Op::LH; break;
    case 34: in.op = Op::LWL; break;
    case 35: in.op = Op::LW; break;
    case 36: in.op = Op::LBU; break;
    case 37: in.op = Op::LHU; break;
    case 38: in.op = Op::LWR; break;
    case 40: in.op = Op::SB; break;
    case 41: in.op = Op::SH; break;
    case 42: in.op = Op::SWL; break;
    case 43: in.op = Op::SW; break;
    case 46: in.op = Op::SWR; break;
    case 47: in.op = Op::CACHE; break;
    case 49: in.op = Op::LWC1; break;
    case 57: in.op = Op::SWC1; break;
    case 50: case 52: case 53: case 54: case 55: case 58: case 60: case 61: case 62: case 63:
        in.op = Op::VFPU_OTHER;
        break;
    default: break;
    }
    return in;
}

bool has_delay_slot(Op op) {
    switch (op) {
    case Op::JR: case Op::JALR: case Op::J: case Op::JAL:
        return true;
    default:
        return is_cond_branch(op) || is_link_branch(op);
    }
}

bool is_cond_branch(Op op) {
    switch (op) {
    case Op::BLTZ: case Op::BGEZ: case Op::BLTZL: case Op::BGEZL:
    case Op::BEQ: case Op::BNE: case Op::BLEZ: case Op::BGTZ: case Op::BEQL: case Op::BNEL: case Op::BLEZL: case Op::BGTZL:
    case Op::BC1F: case Op::BC1T: case Op::BC1FL: case Op::BC1TL:
    case Op::BVF: case Op::BVT: case Op::BVFL: case Op::BVTL:
        return true;
    default:
        return false;
    }
}

bool is_link_branch(Op op) {
    return op == Op::BLTZAL || op == Op::BGEZAL || op == Op::BLTZALL || op == Op::BGEZALL;
}

bool is_likely(Op op) {
    switch (op) {
    case Op::BLTZL: case Op::BGEZL: case Op::BEQL: case Op::BNEL: case Op::BLEZL: case Op::BGTZL:
    case Op::BC1FL: case Op::BC1TL: case Op::BVFL: case Op::BVTL: case Op::BLTZALL: case Op::BGEZALL:
        return true;
    default:
        return false;
    }
}

bool is_unconditional(const Insn& in) {
    switch (in.op) {
    case Op::J: case Op::JR:
        return true;
    case Op::BEQ: case Op::BEQL:
        return in.rs == in.rt;
    case Op::BGEZ: case Op::BGEZL: case Op::BLEZ: case Op::BLEZL:
        return in.rs == 0;
    default:
        return false;
    }
}

const char* mnemonic(const Insn& in) {
    static const char* const fpu_s[64] = {
        "add.s", "sub.s", "mul.s", "div.s", "sqrt.s", "abs.s", "mov.s", "neg.s", 0, 0, 0, 0,
        "round.w.s", "trunc.w.s", "ceil.w.s", "floor.w.s", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, "cvt.w.s", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        "c.f.s", "c.un.s", "c.eq.s", "c.ueq.s", "c.olt.s", "c.ult.s", "c.ole.s", "c.ule.s",
        "c.sf.s", "c.ngle.s", "c.seq.s", "c.ngl.s", "c.lt.s", "c.nge.s", "c.le.s", "c.ngt.s"};
    switch (in.op) {
    case Op::FPU_OTHER: {
        if (in.rs == 20) return (in.raw & 63) == 32 ? "cvt.s.w" : "fpu.w?";
        const char* n = fpu_s[in.raw & 63];
        return n ? n : "fpu.s?";
    }
    case Op::VFPU_OTHER: return "vfpu";
    case Op::COP0_OTHER: return "cop0";
    case Op::SYSCALL: return "syscall";
    case Op::INVALID: return "invalid";
    default: return "?";
    }
}

int dest_reg(const Insn& in) {
    switch (in.op) {
    case Op::SLL: case Op::SRL: case Op::SRA: case Op::ROTR: case Op::SLLV: case Op::SRLV: case Op::SRAV: case Op::ROTRV:
    case Op::MOVZ: case Op::MOVN: case Op::MFHI: case Op::MFLO: case Op::CLZ: case Op::CLO:
    case Op::ADD: case Op::ADDU: case Op::SUB: case Op::SUBU: case Op::AND: case Op::OR: case Op::XOR: case Op::NOR:
    case Op::SLT: case Op::SLTU: case Op::MAX: case Op::MIN: case Op::SEB: case Op::SEH: case Op::WSBH: case Op::WSBW:
    case Op::BITREV: case Op::JALR:
        return in.rd;
    case Op::ADDI: case Op::ADDIU: case Op::SLTI: case Op::SLTIU: case Op::ANDI: case Op::ORI: case Op::XORI: case Op::LUI:
    case Op::EXT: case Op::INS: case Op::LB: case Op::LH: case Op::LWL: case Op::LW: case Op::LBU: case Op::LHU: case Op::LWR:
    case Op::MFC1: case Op::CFC1:
        return in.rt;
    case Op::JAL: case Op::BLTZAL: case Op::BGEZAL: case Op::BLTZALL: case Op::BGEZALL:
        return 31;
    default:
        return -1;
    }
}
