// PSP-Adressraum: 4 GiB reserviert, Adresse a = g_mem + a.
// Eingeblendet: Scratchpad, VRAM (4 Spiegel), RAM, jeweils auch unter | 0x40000000/0x80000000/0xC0000000.
// Rest reserviert, nicht zugreifbar.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <vector>

#include "rt.h"

namespace rt {

uint8_t* g_mem = nullptr;

namespace {

struct View { uint64_t addr; uint32_t size, offset; };
const uint32_t SEC_SCRATCH = 0, SEC_VRAM = 0x10000, SEC_RAM = 0x210000, SEC_TOTAL = 0x4210000;

std::vector<View> build_views() {
    std::vector<View> v;
    for (uint64_t m = 0; m < 4; m++) {
        const uint64_t base = m << 30;
        v.push_back({base + PSP_SCRATCHPAD_ADDR, 0x10000, SEC_SCRATCH});
        for (uint32_t k = 0; k < 4; k++) v.push_back({base + PSP_VRAM_ADDR + k * PSP_VRAM_SIZE, PSP_VRAM_SIZE, SEC_VRAM});
        v.push_back({base + PSP_RAM_ADDR, PSP_RAM_SIZE_MAX, SEC_RAM});
    }
    std::sort(v.begin(), v.end(), [](const View& a, const View& b) { return a.addr < b.addr; });
    return v;
}

std::vector<View> g_views;

bool map_all(HANDLE section, uint8_t* base, std::vector<void*>& mapped, std::vector<void*>& reserved) {
    uint64_t cursor = 0;
    auto reserve_gap = [&](uint64_t end) {
        if (end <= cursor) return true;
        void* p = VirtualAlloc(base + cursor, (SIZE_T)(end - cursor), MEM_RESERVE, PAGE_NOACCESS);
        if (!p) return false;
        reserved.push_back(p);
        return true;
    };
    for (const View& v : g_views) {
        if (!reserve_gap(v.addr)) return false;
        void* p = MapViewOfFileEx(section, FILE_MAP_ALL_ACCESS, 0, v.offset, v.size, base + v.addr);
        if (!p) return false;
        mapped.push_back(p);
        cursor = v.addr + v.size;
    }
    return reserve_gap(0x100000000ull);
}

LONG CALLBACK access_violation_handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2 || !g_mem) return EXCEPTION_CONTINUE_SEARCH;
    const uint8_t* addr = (const uint8_t*)rec->ExceptionInformation[1];
    if (addr < g_mem || addr >= g_mem + 0x100000000ull) return EXCEPTION_CONTINUE_SEARCH;
    const Thread* t = cur();
    fatal(EXIT_MEMORY, "MEM", "invalid access 0x%08X %s pc=0x%08X ra=0x%08X", (uint32_t)(addr - g_mem),
          rec->ExceptionInformation[0] == 1 ? "write" : "read", t ? t->ctx.pc : 0, t ? t->ctx.r[PSP_RA] : 0);
}

// simple Blockverwaltung User-Speicher (Stacks, später SysMemUserForUser)
struct Block { uint32_t addr, size; bool used; std::string name; };
std::vector<Block> g_blocks;

void merge_free() {
    for (size_t i = 0; i + 1 < g_blocks.size();) {
        if (!g_blocks[i].used && !g_blocks[i + 1].used) {
            g_blocks[i].size += g_blocks[i + 1].size;
            g_blocks.erase(g_blocks.begin() + (ptrdiff_t)i + 1);
        } else {
            i++;
        }
    }
}

} // namespace

void mem_init() {
    g_views = build_views();
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, SEC_TOTAL, nullptr);
    if (!section) fatal(EXIT_FATAL, "MEM", "CreateFileMapping fehlgeschlagen (%lu)", GetLastError());
    for (int attempt = 0; attempt < 32; attempt++) {
        // freien 4-GiB-Bereich suchen, freigeben, stückweise belegen
        uint8_t* base = (uint8_t*)VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS);
        if (!base) break;
        VirtualFree(base, 0, MEM_RELEASE);
        std::vector<void*> mapped, reserved;
        if (map_all(section, base, mapped, reserved)) {
            g_mem = base;
            AddVectoredExceptionHandler(1, access_violation_handler);
            psp_log(PSP_LOG_INFO, "MEM", "address space at %p, %zu views", (void*)base, mapped.size());
            return;
        }
        for (void* p : mapped) UnmapViewOfFile(p);
        for (void* p : reserved) VirtualFree(p, 0, MEM_RELEASE);
    }
    fatal(EXIT_FATAL, "MEM", "4-GiB-Adressraum konnte nicht eingerichtet werden");
}

bool mem_valid(uint32_t addr, uint32_t size) {
    const uint64_t a = addr, e = a + size;
    for (const View& v : g_views)
        if (a >= v.addr && e <= v.addr + v.size) return true;
    return false;
}

void mem_set_user_range(uint32_t lo, uint32_t hi) {
    g_blocks.clear();
    lo = (lo + 0xFF) & ~0xFFu;
    hi &= ~0xFFu;
    g_blocks.push_back({lo, hi - lo, false, ""});
}

uint32_t mem_alloc(uint32_t size, bool high, const char* name) {
    size = (size + 0xFF) & ~0xFFu;
    if (size == 0) size = 0x100;
    int found = -1;
    if (high) {
        for (int i = (int)g_blocks.size() - 1; i >= 0; i--)
            if (!g_blocks[(size_t)i].used && g_blocks[(size_t)i].size >= size) { found = i; break; }
    } else {
        for (int i = 0; i < (int)g_blocks.size(); i++)
            if (!g_blocks[(size_t)i].used && g_blocks[(size_t)i].size >= size) { found = i; break; }
    }
    if (found < 0) return 0;
    Block b = g_blocks[(size_t)found];
    const uint32_t rest = b.size - size;
    Block used{high ? b.addr + rest : b.addr, size, true, name ? name : ""};
    g_blocks.erase(g_blocks.begin() + found);
    if (rest) {
        Block free_part{high ? b.addr : b.addr + size, rest, false, ""};
        if (high) {
            g_blocks.insert(g_blocks.begin() + found, used);
            g_blocks.insert(g_blocks.begin() + found, free_part);
        } else {
            g_blocks.insert(g_blocks.begin() + found, free_part);
            g_blocks.insert(g_blocks.begin() + found, used);
        }
    } else {
        g_blocks.insert(g_blocks.begin() + found, used);
    }
    return used.addr;
}

bool mem_free(uint32_t addr) {
    for (Block& b : g_blocks)
        if (b.used && b.addr == addr) {
            b.used = false;
            b.name.clear();
            merge_free();
            return true;
        }
    return false;
}

uint32_t mem_free_total() {
    uint32_t n = 0;
    for (const Block& b : g_blocks)
        if (!b.used) n += b.size;
    return n;
}

uint32_t mem_free_max() {
    uint32_t n = 0;
    for (const Block& b : g_blocks)
        if (!b.used) n = std::max(n, b.size);
    return n;
}

} // namespace rt
