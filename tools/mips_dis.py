#!/usr/bin/env python3
"""Mini-Disassembler MIPS Allegrex (PSP)

    py tools/mips_dis.py game/decrypted/EBOOT.BIN 0x088069A0 24
    py tools/mips_dis.py game/decrypted/EBOOT.BIN 0x088069A0 0x088069F0

unverschlüsseltes ELF; Ausgabe stammt aus dem Spiel, nicht einchecken. FPU benannt, VFPU nur Rohwert
"""
import struct
import sys

REG = ("zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 "
       "s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra").split()
SPECIAL = {0: "sll", 2: "srl", 3: "sra", 4: "sllv", 6: "srlv", 7: "srav", 10: "movz", 11: "movn", 22: "clz", 23: "clo",
           32: "add", 33: "addu", 34: "sub", 35: "subu", 36: "and", 37: "or", 38: "xor", 39: "nor", 42: "slt",
           43: "sltu", 44: "max", 45: "min"}
MULDIV = {24: "mult", 25: "multu", 26: "div", 27: "divu", 28: "madd", 29: "maddu", 46: "msub", 47: "msubu"}
IMM = {8: "addi", 9: "addiu", 10: "slti", 11: "sltiu", 12: "andi", 13: "ori", 14: "xori"}
MEM = {32: "lb", 33: "lh", 34: "lwl", 35: "lw", 36: "lbu", 37: "lhu", 38: "lwr", 40: "sb", 41: "sh", 42: "swl",
       43: "sw", 46: "swr", 49: "lwc1", 57: "swc1"}
BR2 = {4: "beq", 5: "bne", 20: "beql", 21: "bnel"}
BR1 = {6: "blez", 7: "bgtz", 22: "blezl", 23: "bgtzl"}
REGIMM = {0: "bltz", 1: "bgez", 2: "bltzl", 3: "bgezl", 16: "bltzal", 17: "bgezal"}
FPU = {0: "add.s", 1: "sub.s", 2: "mul.s", 3: "div.s", 4: "sqrt.s", 5: "abs.s", 6: "mov.s", 7: "neg.s",
       12: "round.w.s", 13: "trunc.w.s", 14: "ceil.w.s", 15: "floor.w.s", 36: "cvt.w.s"}
FCOND = "f un eq ueq olt ult ole ule sf ngle seq ngl lt nge le ngt".split()


def dis(addr, w):
    op, rs, rt, rd, sa, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    imm = w & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    bt = (addr + 4 + (simm << 2)) & 0xFFFFFFFF
    if w == 0:
        return "nop"
    if op == 0:
        if fn in (0, 2, 3):
            name = "rotr" if fn == 2 and rs & 1 else SPECIAL[fn]
            return f"{name} {REG[rd]},{REG[rt]},{sa}"
        if fn in (4, 6, 7):
            return f"{SPECIAL[fn]} {REG[rd]},{REG[rt]},{REG[rs]}"
        if fn == 8:
            return f"jr {REG[rs]}"
        if fn == 9:
            return f"jalr {REG[rs]}" + ("" if rd == 31 else f" (link {REG[rd]})")
        if fn in (22, 23):
            return f"{SPECIAL[fn]} {REG[rd]},{REG[rs]}"
        if fn in MULDIV:
            return f"{MULDIV[fn]} {REG[rs]},{REG[rt]}"
        if fn in (16, 18):
            return f"{'mfhi' if fn == 16 else 'mflo'} {REG[rd]}"
        if fn in (17, 19):
            return f"{'mthi' if fn == 17 else 'mtlo'} {REG[rs]}"
        if fn == 13:
            return f"break {(w >> 6) & 0xFFFFF:#x}"
        if fn == 12:
            return "syscall"
        if fn == 15:
            return "sync"
        if fn in SPECIAL:
            if fn in (33, 37) and rt == 0:
                return f"move {REG[rd]},{REG[rs]}"
            return f"{SPECIAL[fn]} {REG[rd]},{REG[rs]},{REG[rt]}"
    if op == 1 and rt in REGIMM:
        return f"{REGIMM[rt]} {REG[rs]},{bt:#010x}"
    if op in (2, 3):
        return f"{'j' if op == 2 else 'jal'} {((addr + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2):#010x}"
    if op in BR2:
        if op == 4 and rs == rt:
            return f"b {bt:#010x}"
        return f"{BR2[op]} {REG[rs]},{REG[rt]},{bt:#010x}"
    if op in BR1:
        return f"{BR1[op]} {REG[rs]},{bt:#010x}"
    if op in IMM:
        if op == 9 and rs == 0:
            return f"li {REG[rt]},{simm}"
        v = simm if op in (8, 9, 10, 11) else imm
        return f"{IMM[op]} {REG[rt]},{REG[rs]},{v:#x}" if op >= 12 else f"{IMM[op]} {REG[rt]},{REG[rs]},{v}"
    if op == 15:
        return f"lui {REG[rt]},{imm:#x}"
    if op in MEM:
        r = f"f{rt}" if op in (49, 57) else REG[rt]
        return f"{MEM[op]} {r},{simm}({REG[rs]})"
    if op == 47:
        return "cache"
    if op == 17:
        if rs == 0:
            return f"mfc1 {REG[rt]},f{rd}"
        if rs == 4:
            return f"mtc1 {REG[rt]},f{rd}"
        if rs == 2:
            return f"cfc1 {REG[rt]},{rd}"
        if rs == 6:
            return f"ctc1 {REG[rt]},{rd}"
        if rs == 8:
            return f"{('bc1f', 'bc1t', 'bc1fl', 'bc1tl')[rt & 3]} {bt:#010x}"
        if rs == 16:
            if fn >= 48:
                return f"c.{FCOND[fn - 48]}.s f{rd},f{rt}"
            if fn in FPU:
                return f"{FPU[fn]} f{sa},f{rd}" + (f",f{rt}" if fn < 4 else "")
        if rs == 20 and fn == 32:
            return f"cvt.s.w f{sa},f{rd}"
    if op == 31:
        if fn == 0:
            return f"ext {REG[rt]},{REG[rs]},{sa},{rd + 1}"
        if fn == 4:
            return f"ins {REG[rt]},{REG[rs]},{sa},{rd - sa + 1}"
        if fn == 32:
            n = {2: "wsbh", 3: "wsbw", 16: "seb", 20: "bitrev", 24: "seh"}.get(sa)
            if n:
                return f"{n} {REG[rd]},{REG[rt]}"
    if op == 18 and rs == 8:
        return f"{('bvf', 'bvt', 'bvfl', 'bvtl')[rt & 3]} {(w >> 18) & 7},{bt:#010x}"
    if op in (18, 24, 25, 27, 50, 52, 53, 54, 55, 58, 60, 61, 62, 63):
        return f"vfpu {w:08x}"
    return f".word {w:08x}"


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    data = open(sys.argv[1], "rb").read()
    if data[:4] != b"\x7fELF":
        print("kein ELF", file=sys.stderr)
        return 1
    start = int(sys.argv[2], 0)
    arg = int(sys.argv[3], 0) if len(sys.argv) > 3 else 16
    count = (arg - start) // 4 if arg > 0x10000 else arg
    phoff, phnum = struct.unpack_from("<I", data, 28)[0], struct.unpack_from("<H", data, 44)[0]
    for i in range(phnum):
        _t, off, vaddr, _p, filesz = struct.unpack_from("<5I", data, phoff + i * 32)
        if vaddr <= start < vaddr + filesz:
            for k in range(count):
                a = start + k * 4
                if a + 4 > vaddr + filesz:
                    break
                w = struct.unpack_from("<I", data, off + a - vaddr)[0]
                print(f"{a:08X}  {w:08x}  {dis(a, w)}")
            return 0
    print("Adresse liegt in keinem Segment", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
