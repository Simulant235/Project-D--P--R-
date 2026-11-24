// gemeinsame Typen; Gegenstück zum generierten Code: runtime/include/psp_context.h
#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s16 = int16_t;
using s32 = int32_t;

enum ExitCode {
    EXIT_OK = 0, EXIT_INTERNAL = 1, EXIT_USAGE = 2, EXIT_NOT_FOUND = 10, EXIT_WRONG_GAME = 11,
    EXIT_WRONG_HASH = 12, EXIT_ENCRYPTED = 13, EXIT_ANALYSIS = 20, EXIT_CODEGEN = 30
};

struct RecompError : std::runtime_error {
    int code;
    RecompError(int c, const std::string& msg) : std::runtime_error(msg), code(c) {}
};

std::string strf(const char* fmt, ...);

// ---------------------------------------------------------------- Konfiguration (games/*.toml)

struct JumpTableCfg { u32 jr_addr = 0, table_addr = 0, count = 0; };
struct HookCfg { u32 addr = 0; std::string symbol; };
struct PatchCfg { u32 addr = 0; std::vector<u32> words; bool enabled = true; };

struct Config {
    std::string id, title, disc_id, disc_version;
    std::string exe_path, exe_format, module_name, elf_sha1;
    u32 load_addr = 0, elf_size = 0;
    bool safepoints = true;
    u32 functions_per_file = 500;
    std::string exe_name;
    std::vector<u32> functions;
    std::vector<JumpTableCfg> jump_tables;
    std::vector<HookCfg> hooks;
    std::vector<PatchCfg> patches;
    std::vector<std::pair<u32, u32>> code_ranges;   // zusätzlicher Code außerhalb von .text: [start, end)
};

Config load_config(const std::string& path);

// ---------------------------------------------------------------- Modul

struct Import { std::string lib; u32 nid = 0, stub_addr = 0; };

struct Module {
    std::string name, sha1;
    u32 load_addr = 0, entry = 0, gp = 0, module_info_addr = 0;
    u32 code_addr = 0, code_size = 0;   // Spanne der Funktionstabelle: .text bis Ende des letzten Codebereichs
    std::vector<std::pair<u32, u32>> text_ranges;   // übersetzter Code: .text und Bereiche aus [[code_range]], sortiert
    u32 text_size = 0;                  // nur .text (Bytes ab code_addr)
    u32 stub_addr = 0, stub_size = 0;
    u32 image_addr = 0;
    std::vector<u8> image;              // geladenes Speicherabbild ab image_addr (inkl. BSS als Nullen)
    std::vector<Import> imports;
    std::vector<u32> export_funcs;

    bool contains(u32 addr, u32 n = 4) const {
        return addr >= image_addr && (u64)addr + n <= (u64)image_addr + image.size();
    }
    u32 read32(u32 addr) const {
        const u8* p = &image[addr - image_addr];
        return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
    }
    void write32(u32 addr, u32 v) {
        u8* p = &image[addr - image_addr];
        p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
    }
    bool in_code(u32 addr) const { return addr >= code_addr && addr < code_addr + code_size; }
    bool in_text(u32 addr) const {
        for (const auto& r : text_ranges)
            if (addr >= r.first && addr < r.second) return true;
        return false;
    }
    u32 range_end(u32 addr) const {   // Ende des Codebereichs, in dem addr liegt
        for (const auto& r : text_ranges)
            if (addr >= r.first && addr < r.second) return r.second;
        return addr;
    }
    bool is_code(u32 addr) const { return in_text(addr) || in_stubs(addr); }   // anders als in_code: nur echter Code
    bool in_stubs(u32 addr) const { return stub_size && addr >= stub_addr && addr < stub_addr + stub_size; }
};

std::vector<u8> read_file(const std::string& path);
std::string sha1_hex(const u8* data, size_t size);
// ELF (ET_EXEC) oder PRX, entschlüsselt. prx_base = Relozierungsadresse beim PRX
Module load_module(const std::vector<u8>& file, u32 prx_base, u32 elf_size_hint);

// ---------------------------------------------------------------- Befehle

enum class Op : u8 {
    INVALID, NOP,
    SLL, SRL, SRA, ROTR, SLLV, SRLV, SRAV, ROTRV,
    JR, JALR, MOVZ, MOVN, SYSCALL, BREAK, SYNC,
    MFHI, MTHI, MFLO, MTLO, CLZ, CLO,
    MULT, MULTU, DIV, DIVU, MADD, MADDU, MSUB, MSUBU,
    ADD, ADDU, SUB, SUBU, AND, OR, XOR, NOR, SLT, SLTU, MAX, MIN,
    BLTZ, BGEZ, BLTZL, BGEZL, BLTZAL, BGEZAL, BLTZALL, BGEZALL,
    J, JAL, BEQ, BNE, BLEZ, BGTZ, BEQL, BNEL, BLEZL, BGTZL,
    ADDI, ADDIU, SLTI, SLTIU, ANDI, ORI, XORI, LUI,
    EXT, INS, SEB, SEH, WSBH, WSBW, BITREV,
    LB, LH, LWL, LW, LBU, LHU, LWR, SB, SH, SWL, SW, SWR, CACHE,
    LWC1, SWC1, MFC1, MTC1, CFC1, CTC1, BC1F, BC1T, BC1FL, BC1TL, MOV_S,
    FPU_OTHER,      // FPU-Rechenbefehle: T12
    BVF, BVT, BVFL, BVTL,
    VFPU_OTHER,     // VFPU: T12
    COP0_OTHER
};

struct Insn {
    u32 addr = 0, raw = 0;
    Op op = Op::INVALID;
    u8 rs = 0, rt = 0, rd = 0, sa = 0;
    s32 imm = 0;       // vorzeichenerweitert
    u32 uimm = 0;      // nullerweitert
    u32 target = 0;    // Sprungziel (Branches, J, JAL)
};

Insn decode(u32 addr, u32 raw);
const char* mnemonic(const Insn& in);     // für Log-Stubs und Statistik
bool has_delay_slot(Op op);
bool is_cond_branch(Op op);               // bedingte Sprünge inkl. Likely, FPU, VFPU (ohne Link)
bool is_likely(Op op);
bool is_link_branch(Op op);               // bltzal ...
bool is_unconditional(const Insn& in);    // j, jr, b (beq x,x / bgez zero / blez zero)
int dest_reg(const Insn& in);             // geschriebenes Allzweckregister oder -1

// ---------------------------------------------------------------- Analyse

struct Analysis {
    std::vector<Insn> insns;                       // Index (addr - code_addr) / 4
    std::set<u32> starts;                          // Funktionsanfänge im .text
    std::set<u32> soft;                            // Teilmenge von starts: beendet die Funktion davor nicht
    std::map<u32, std::vector<u32>> jump_tables;   // Adresse des jr -> Ziele
    u32 indirect_jr = 0;                           // jr <reg> ohne Tabelle
    u32 backward_cross = 0;                        // Rückwärtssprünge über Funktionsgrenzen (sollte klein sein)
    std::vector<u32> backward_cross_examples;
    // Zeigerprüfung: Konstanten in Daten/Code, die in den Code zeigen
    u32 data_ptrs = 0, code_ptrs = 0;
    std::vector<std::pair<u32, u32>> suspicious_ptrs;   // (Fundstelle, Ziel): weder Funktionsanfang noch Tabellenziel
    std::vector<std::pair<u32, u32>> code_like_ptrs;    // (Fundstelle, Ziel): Ziel außerhalb des Codes sieht wie ein Funktionsanfang aus
    const Insn& at(const Module& m, u32 addr) const { return insns[(addr - m.code_addr) / 4]; }
};

Analysis analyze(const Module& m, const Config& cfg, const std::vector<u32>& extra_starts);
std::vector<u32> load_ghidra_functions(const std::string& analysis_dir);

// ---------------------------------------------------------------- Codeerzeugung

struct EmitStats {
    u32 functions = 0, instructions = 0, files = 0, unimplemented = 0;
    std::map<std::string, u32> unimplemented_by_name;
    std::map<std::string, u32> vfpu_by_name;   // übersetzte VFPU-Befehle, zum Abgleich mit Ghidra
};

EmitStats emit_code(const Module& m, const Config& cfg, const Analysis& an, const std::string& out_dir,
                    const std::string& runtime_include);
