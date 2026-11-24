// ELF/PRX laden, Relocations, Import-/Export-Tabellen. ~PSP-Header wird abgelehnt.
#include <cstring>
#include <fstream>

#include "recomp.h"

std::vector<u8> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw RecompError(EXIT_NOT_FOUND, "Datei nicht gefunden: " + path);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------- SHA-1

std::string sha1_hex(const u8* data, size_t size) {
    u32 h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::vector<u8> msg(data, data + size);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    u64 bits = (u64)size * 8;
    for (int i = 7; i >= 0; i--) msg.push_back((u8)(bits >> (i * 8)));
    auto rol = [](u32 v, int n) { return (v << n) | (v >> (32 - n)); };
    for (size_t off = 0; off < msg.size(); off += 64) {
        u32 w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((u32)msg[off + i * 4] << 24) | ((u32)msg[off + i * 4 + 1] << 16) | ((u32)msg[off + i * 4 + 2] << 8) |
                   msg[off + i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            u32 f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            u32 t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    return strf("%08x%08x%08x%08x%08x", h[0], h[1], h[2], h[3], h[4]);
}

// ---------------------------------------------------------------- ELF

namespace {

struct Reader {
    const std::vector<u8>& d;
    void need(size_t off, size_t n) const {
        if (off + n > d.size()) throw RecompError(EXIT_ANALYSIS, "ELF abgeschnitten");
    }
    u16 r16(size_t o) const { need(o, 2); return (u16)(d[o] | (d[o + 1] << 8)); }
    u32 r32(size_t o) const { need(o, 4); return (u32)d[o] | ((u32)d[o + 1] << 8) | ((u32)d[o + 2] << 16) | ((u32)d[o + 3] << 24); }
};

struct Phdr { u32 type, offset, vaddr, paddr, filesz, memsz, flags; };
struct Shdr { std::string name; u32 type, flags, addr, offset, size; };

const u32 ET_EXEC = 2, ET_PRX = 0xFFA0, PT_LOAD = 1, SHT_PRXRELOC = 0x700000A0, PT_PRXRELOC2 = 0x700000A1;

std::string cstr(const Module& m, u32 addr) {
    std::string s;
    while (m.contains(addr, 1) && m.image[addr - m.image_addr] && s.size() < 255) s += (char)m.image[addr++ - m.image_addr];
    return s;
}

} // namespace

Module load_module(const std::vector<u8>& file, u32 prx_base, u32 elf_size_hint) {
    Reader r{file};
    if (file.size() >= 4 && memcmp(file.data(), "~PSP", 4) == 0)
        throw RecompError(EXIT_ENCRYPTED, "EBOOT.BIN ist verschlüsselt (~PSP). Dieses Projekt entschlüsselt nicht; "
                                          "entschlüsselte Datei mit --eboot angeben.");
    if (file.size() < 52 || memcmp(file.data(), "\x7f" "ELF", 4) != 0) throw RecompError(EXIT_ANALYSIS, "kein ELF");
    if (file[4] != 1 || file[5] != 1 || r.r16(18) != 8) throw RecompError(EXIT_ANALYSIS, "kein 32-Bit-LE-MIPS-ELF");

    const u32 e_type = r.r16(16), e_entry = r.r32(24), phoff = r.r32(28), shoff = r.r32(32);
    const u32 phnum = r.r16(44), shnum = r.r16(48), shstrndx = r.r16(50);
    if (e_type != ET_EXEC && e_type != ET_PRX) throw RecompError(EXIT_ANALYSIS, strf("unbekannter ELF-Typ 0x%X", e_type));
    const bool prx = e_type == ET_PRX;
    const u32 base = prx ? prx_base : 0;

    std::vector<Phdr> ph;
    for (u32 i = 0; i < phnum; i++) {
        size_t o = phoff + i * 32;
        ph.push_back({r.r32(o), r.r32(o + 4), r.r32(o + 8), r.r32(o + 12), r.r32(o + 16), r.r32(o + 20), r.r32(o + 24)});
    }
    std::vector<Shdr> sh;
    if (shoff && shnum) {
        std::vector<u32> name_off;
        for (u32 i = 0; i < shnum; i++) {
            size_t o = shoff + i * 40;
            name_off.push_back(r.r32(o));
            sh.push_back({"", r.r32(o + 4), r.r32(o + 8), r.r32(o + 12), r.r32(o + 16), r.r32(o + 20)});
        }
        if (shstrndx < shnum) {
            u32 so = sh[shstrndx].offset;
            for (u32 i = 0; i < shnum; i++)
                for (size_t p = so + name_off[i]; p < file.size() && file[p]; p++) sh[i].name += (char)file[p];
        }
    }

    Module m;
    // Abbild aus PT_LOAD
    u32 lo = 0xFFFFFFFFu, hi = 0;
    std::vector<const Phdr*> loads;
    for (const Phdr& p : ph)
        if (p.type == PT_LOAD && p.memsz) {
            loads.push_back(&p);
            lo = std::min(lo, p.vaddr + base);
            hi = std::max(hi, p.vaddr + base + p.memsz);
        }
    if (loads.empty()) throw RecompError(EXIT_ANALYSIS, "ELF ohne ladbares Segment");
    m.image_addr = lo;
    m.image.assign(hi - lo, 0);
    for (const Phdr* p : loads) {
        r.need(p->offset, p->filesz);
        memcpy(&m.image[p->vaddr + base - lo], &file[p->offset], p->filesz);
    }
    m.load_addr = loads[0]->vaddr + base;

    // SHA-1 nur übers ELF, ohne Dateipadding
    size_t elf_len = elf_size_hint && elf_size_hint <= file.size() ? elf_size_hint : file.size();
    m.sha1 = sha1_hex(file.data(), elf_len);

    // PRX-Relocations (Typ A, 0x700000A0), Adressen segmentbezogen
    if (prx) {
        u32 applied = 0;
        bool type_b = false;
        for (const Phdr& p : ph) type_b |= p.type == PT_PRXRELOC2;
        for (const Shdr& s : sh) {
            if (s.type != SHT_PRXRELOC) continue;
            std::vector<u32> pending_hi;   // Adressen offener HI16
            for (u32 o = 0; o + 8 <= s.size; o += 8) {
                u32 r_offset = r.r32(s.offset + o), r_info = r.r32(s.offset + o + 4);
                u32 type = r_info & 0xFF, ofs_seg = (r_info >> 8) & 0xFF, addr_seg = (r_info >> 16) & 0xFF;
                if (ofs_seg >= ph.size() || addr_seg >= ph.size())
                    throw RecompError(EXIT_ANALYSIS, "PRX-Relocation: Segmentindex ungültig");
                u32 where = ph[ofs_seg].vaddr + base + r_offset;
                u32 A = ph[addr_seg].vaddr + base;
                if (!m.contains(where)) throw RecompError(EXIT_ANALYSIS, strf("PRX-Relocation außerhalb des Abbilds: 0x%08X", where));
                u32 w = m.read32(where);
                switch (type) {
                case 0: break;                                            // R_MIPS_NONE
                case 2: m.write32(where, w + A); break;                   // R_MIPS_32
                case 4: {                                                 // R_MIPS_26
                    u32 t = ((w & 0x03FFFFFFu) << 2) + A;
                    m.write32(where, (w & 0xFC000000u) | ((t >> 2) & 0x03FFFFFFu));
                    break;
                }
                case 5: pending_hi.push_back(where); break;               // R_MIPS_HI16, wartet aufs LO16
                case 6: {                                                 // R_MIPS_LO16
                    s32 lo16 = (s16)(w & 0xFFFF);
                    for (u32 h : pending_hi) {
                        u32 hw = m.read32(h);
                        u32 full = ((hw & 0xFFFF) << 16) + (u32)lo16 + A;
                        u32 new_hi = (full - (u32)(s32)(s16)(full & 0xFFFF)) >> 16;
                        m.write32(h, (hw & 0xFFFF0000u) | (new_hi & 0xFFFF));
                    }
                    pending_hi.clear();
                    m.write32(where, (w & 0xFFFF0000u) | (((u32)lo16 + A) & 0xFFFF));
                    break;
                }
                case 7: break;                                            // R_MIPS_GPREL16: bleibt
                default: throw RecompError(EXIT_ANALYSIS, strf("PRX-Relocation: unbekannter Typ %u", type));
                }
                applied++;
            }
        }
        if (applied == 0 && type_b)
            throw RecompError(EXIT_ANALYSIS, "PRX nutzt Relocations vom Typ B (Segment 0x700000A1); das kommt in T60");
        if (applied == 0) throw RecompError(EXIT_ANALYSIS, "PRX ohne Relocations");
    }

    // Sektionen
    auto find = [&](const char* name) -> const Shdr* {
        for (const Shdr& s : sh)
            if (s.name == name) return &s;
        return nullptr;
    };
    const Shdr* text = find(".text");
    const Shdr* stub = find(".sceStub.text");
    const Shdr* minfo = find(".rodata.sceModuleInfo");
    if (!text || !minfo) throw RecompError(EXIT_ANALYSIS, "ELF ohne Sektionen .text/.rodata.sceModuleInfo (gestrippt?)");
    m.code_addr = text->addr + base;
    m.text_size = text->size & ~3u;
    m.code_size = m.text_size;
    if (stub && stub->size) {
        m.stub_addr = stub->addr + base;
        m.stub_size = stub->size & ~3u;
        if (m.stub_addr < m.code_addr + m.text_size) throw RecompError(EXIT_ANALYSIS, ".sceStub.text liegt vor dem Ende von .text");
        m.code_size = m.stub_addr + m.stub_size - m.code_addr;
    }
    if (!m.contains(m.code_addr, m.code_size)) throw RecompError(EXIT_ANALYSIS, "Codebereich liegt außerhalb des Abbilds");
    m.text_ranges.push_back({m.code_addr, m.code_addr + m.text_size});

    // sceModuleInfo: attr(2) ver(2) name(28) gp ent_top ent_end stub_top stub_end
    u32 mi = minfo->addr + base;
    m.module_info_addr = mi;
    if (!m.contains(mi, 52)) throw RecompError(EXIT_ANALYSIS, "sceModuleInfo außerhalb des Abbilds");
    m.name = cstr(m, mi + 4).substr(0, 27);
    m.gp = m.read32(mi + 32);
    u32 ent_top = m.read32(mi + 36), ent_end = m.read32(mi + 40), stub_top = m.read32(mi + 44), stub_end = m.read32(mi + 48);

    for (u32 p = stub_top; p + 20 <= stub_end && m.contains(p, 20);) {
        u32 name_ptr = m.read32(p), w2 = m.read32(p + 8);
        u32 ent_len = w2 & 0xFF, func_count = w2 >> 16;
        u32 nid_ptr = m.read32(p + 12), stub_ptr = m.read32(p + 16);
        if (ent_len == 0) break;
        std::string lib = cstr(m, name_ptr);
        for (u32 i = 0; i < func_count; i++) {
            if (!m.contains(nid_ptr + i * 4)) throw RecompError(EXIT_ANALYSIS, "Import-Tabelle fehlerhaft");
            m.imports.push_back({lib, m.read32(nid_ptr + i * 4), stub_ptr + i * 8});
        }
        p += ent_len * 4;
    }
    for (u32 p = ent_top; p + 16 <= ent_end && m.contains(p, 16);) {
        u32 w2 = m.read32(p + 8);
        u32 ent_len = w2 & 0xFF, var_count = (w2 >> 8) & 0xFF, func_count = w2 >> 16;
        u32 table = m.read32(p + 12);
        if (ent_len == 0) break;
        u32 total = func_count + var_count;
        for (u32 i = 0; i < func_count; i++) {
            u32 nid = m.read32(table + i * 4), addr = m.read32(table + (total + i) * 4);
            m.export_funcs.push_back(addr);
            if (nid == 0xD632ACDBu) m.entry = addr;   // module_start
        }
        p += ent_len * 4;
    }
    if (!m.entry) m.entry = e_entry + base;
    return m;
}
