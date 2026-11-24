// EBOOT laden, gegen den recompilierten Code prüfen, Hauptthread anlegen.
// Entschlüsselt nichts, ~PSP wird abgelehnt.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "rt.h"

namespace rt {

std::string sha1_hex(const uint8_t* data, size_t size) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::vector<uint8_t> msg(data, data + size);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    const uint64_t bits = (uint64_t)size * 8;
    for (int i = 7; i >= 0; i--) msg.push_back((uint8_t)(bits >> (i * 8)));
    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)msg[off + i * 4] << 24) | ((uint32_t)msg[off + i * 4 + 1] << 16) |
                   ((uint32_t)msg[off + i * 4 + 2] << 8) | msg[off + i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    char buf[41];
    snprintf(buf, sizeof buf, "%08x%08x%08x%08x%08x", h[0], h[1], h[2], h[3], h[4]);
    return buf;
}

namespace {

uint32_t r32(const std::vector<uint8_t>& d, size_t o) {
    if (o + 4 > d.size()) fatal(EXIT_WRONG_HASH, "BOOT", "EBOOT.BIN ist abgeschnitten");
    return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) | ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24);
}
uint16_t r16(const std::vector<uint8_t>& d, size_t o) { return (uint16_t)(r32(d, o) & 0xFFFF); }

std::vector<uint8_t> read_eboot() {
    std::vector<uint8_t> data;
    if (!g_opt.eboot.empty()) {
        FILE* f = fopen(g_opt.eboot.c_str(), "rb");
        if (!f) fatal(EXIT_NOT_FOUND, "BOOT", "EBOOT nicht gefunden: %s", g_opt.eboot.c_str());
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        data.resize((size_t)n);
        if (fread(data.data(), 1, data.size(), f) != data.size()) fatal(EXIT_NOT_FOUND, "BOOT", "EBOOT nicht lesbar");
        fclose(f);
        return data;
    }
    const IsoEntry e = g_iso.find("PSP_GAME/SYSDIR/EBOOT.BIN");
    if (!e.found) fatal(EXIT_NOT_FOUND, "BOOT", "PSP_GAME/SYSDIR/EBOOT.BIN fehlt in der ISO");
    data.resize(e.size);
    if (!g_iso.read(e, 0, data.data(), e.size)) fatal(EXIT_NOT_FOUND, "BOOT", "EBOOT.BIN in der ISO nicht lesbar");
    return data;
}

// ELF-Länge ohne Padding, wie beim Recompiler (Prüfsumme)
size_t elf_length(const std::vector<uint8_t>& d) {
    const uint32_t phoff = r32(d, 28), shoff = r32(d, 32);
    const uint32_t phnum = r16(d, 44), shnum = r16(d, 48);
    size_t end = std::max<size_t>(52, std::max<size_t>(phoff + phnum * 32, shoff + shnum * 40));
    for (uint32_t i = 0; i < phnum; i++) end = std::max<size_t>(end, (size_t)r32(d, phoff + i * 32 + 4) + r32(d, phoff + i * 32 + 16));
    for (uint32_t i = 0; i < shnum; i++) {
        const size_t o = shoff + i * 40;
        if (r32(d, o + 4) != 8 /* SHT_NOBITS */) end = std::max<size_t>(end, (size_t)r32(d, o + 16) + r32(d, o + 20));
    }
    return std::min(end, d.size());
}

} // namespace

void load_game() {
    const PspModuleDesc* mod = g_psp_modules[0];
    if (!mod || mod->interface_version != PSP_INTERFACE_VERSION)
        fatal(EXIT_FATAL, "BOOT", "generierter Code passt nicht zur Runtime (Schnittstellenversion)");

    const std::vector<uint8_t> file = read_eboot();
    if (file.size() >= 4 && memcmp(file.data(), "~PSP", 4) == 0)
        fatal(EXIT_ENCRYPTED, "BOOT", "EBOOT.BIN ist verschlüsselt (~PSP). Entschlüsselte Datei mit --eboot angeben.");
    if (file.size() < 52 || memcmp(file.data(), "\x7f" "ELF", 4) != 0) fatal(EXIT_WRONG_HASH, "BOOT", "EBOOT.BIN ist kein ELF");

    const std::string sha = sha1_hex(file.data(), elf_length(file));
    if (sha != mod->elf_sha1 && sha1_hex(file.data(), file.size()) != mod->elf_sha1)
        fatal(EXIT_WRONG_HASH, "BOOT", "EBOOT.BIN passt nicht zum recompilierten Code: SHA-1 %s, erwartet %s", sha.c_str(), mod->elf_sha1);

    // Segmente laden (ET_EXEC), PRX kommt mit T60
    if (r16(file, 16) != 2) fatal(EXIT_FATAL, "BOOT", "nur ET_EXEC wird unterstützt (PRX: T60)");
    const uint32_t phoff = r32(file, 28), phnum = r16(file, 44);
    uint32_t image_end = 0;
    for (uint32_t i = 0; i < phnum; i++) {
        const size_t o = phoff + i * 32;
        if (r32(file, o) != 1) continue;   // PT_LOAD
        const uint32_t offset = r32(file, o + 4), vaddr = r32(file, o + 8), filesz = r32(file, o + 16), memsz = r32(file, o + 20);
        if (!mem_valid(vaddr, memsz) || (size_t)offset + filesz > file.size()) fatal(EXIT_FATAL, "BOOT", "Segment außerhalb des Speichers");
        memcpy(g_mem + vaddr, file.data() + offset, filesz);
        memset(g_mem + vaddr + filesz, 0, memsz - filesz);
        image_end = std::max(image_end, vaddr + memsz);
        psp_log(PSP_LOG_INFO, "BOOT", "segment 0x%08X file=0x%X mem=0x%X", vaddr, filesz, memsz);
    }
    mem_set_user_range(image_end, PSP_USER_RAM_ADDR + g_opt.user_ram_size);

    // module_start_thread_parameter aus der Export-Tabelle: { 3, Priorität, Stackgröße, Attribut }
    uint32_t prio = 32, stack = 0x40000, attr = 0;
    const uint32_t mi = mod->module_info_addr;
    const uint32_t ent_top = psp_r32(g_mem, mi + 36), ent_end = psp_r32(g_mem, mi + 40);
    for (uint32_t p = ent_top; p + 16 <= ent_end;) {
        const uint32_t w2 = psp_r32(g_mem, p + 8), len = w2 & 0xFF, vars = (w2 >> 8) & 0xFF, funcs = w2 >> 16;
        const uint32_t table = psp_r32(g_mem, p + 12);
        if (!len) break;
        for (uint32_t i = funcs; i < funcs + vars; i++)
            if (psp_r32(g_mem, table + i * 4) == 0x0F7C276Cu) {
                const uint32_t a = psp_r32(g_mem, table + (funcs + vars + i) * 4);
                if (mem_valid(a, 16)) {
                    prio = psp_r32(g_mem, a + 4);
                    stack = psp_r32(g_mem, a + 8);
                    attr = psp_r32(g_mem, a + 12);
                }
            }
        p += len * 4;
    }
    if (prio < 0x08 || prio > 0x77) prio = 32;
    if (stack < 0x1000) stack = 0x40000;

    kernel_init(mod->gp);
    psp_log(PSP_LOG_INFO, "BOOT", "module %s entry=0x%08X gp=0x%08X main thread prio=%u stack=0x%X", mod->name, mod->entry, mod->gp, prio, stack);
    const int id = thread_create("root", mod->entry, (int)prio, stack, attr);
    if (id < 0) fatal(EXIT_FATAL, "BOOT", "Hauptthread konnte nicht angelegt werden (0x%08X)", (uint32_t)id);
    static const char arg[] = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    const uint32_t arg_addr = mem_alloc(sizeof arg, false, "bootarg");
    memcpy(g_mem + arg_addr, arg, sizeof arg);
    thread_start(id, sizeof arg, arg_addr);
    mem_free(arg_addr);
}

} // namespace rt
