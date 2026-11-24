// Runtime-Kern ohne Spieldateien: handgeschriebenes "Spiel" startet Threads, wartet per sceKernelDelayThread.
// Geprüft: Reihenfolge, Zeiten, Prioritäten, Speicher.
#include <cstdio>
#include <cstring>
#include <string>

#include "rt.h"

extern "C" {
PSP_HLE(ThreadManForUser, 0x446D8DE6);   // sceKernelCreateThread
PSP_HLE(ThreadManForUser, 0xF475845D);   // sceKernelStartThread
PSP_HLE(ThreadManForUser, 0xCEADEB47);   // sceKernelDelayThread
PSP_HLE(ThreadManForUser, 0x278C0DF5);   // sceKernelWaitThreadEnd
PSP_HLE(ThreadManForUser, 0x293B45B8);   // sceKernelGetThreadId
PSP_HLE(Kernel_Library, 0x092968F4);     // sceKernelCpuSuspendIntr
PSP_HLE(Kernel_Library, 0x5F10D406);     // sceKernelCpuResumeIntr
}

namespace {

const uint32_t CODE = 0x08804000, NAMES = 0x08900000;
std::string g_trace;
int g_failed = 0, g_checks = 0;
uint64_t g_end_time = 0;
int g_b_arg_ok = 0;

void check(const char* name, bool ok) {
    g_checks++;
    if (!ok) {
        printf("FEHLER: %s\n", name);
        g_failed++;
    }
}

uint32_t create(PspContext* ctx, uint8_t* mem, const char* name, uint32_t entry, int prio) {
    static uint32_t next = NAMES;
    const uint32_t addr = next;
    next += 32;
    strcpy((char*)mem + addr, name);
    ctx->r[PSP_A0] = addr;
    ctx->r[PSP_A1] = entry;
    ctx->r[PSP_A2] = (uint32_t)prio;
    ctx->r[PSP_A3] = 0x4000;
    ctx->r[PSP_T0] = 0;
    ctx->r[PSP_T1] = 0;
    hle_ThreadManForUser_0x446D8DE6(ctx, mem);
    return ctx->r[PSP_V0];
}

void start(PspContext* ctx, uint8_t* mem, uint32_t id, uint32_t arg_len, uint32_t arg_ptr) {
    ctx->r[PSP_A0] = id;
    ctx->r[PSP_A1] = arg_len;
    ctx->r[PSP_A2] = arg_ptr;
    hle_ThreadManForUser_0xF475845D(ctx, mem);
}

void delay(PspContext* ctx, uint8_t* mem, uint32_t usec) {
    ctx->r[PSP_A0] = usec;
    hle_ThreadManForUser_0xCEADEB47(ctx, mem);
}

void wait_end(PspContext* ctx, uint8_t* mem, uint32_t id) {
    ctx->r[PSP_A0] = id;
    ctx->r[PSP_A1] = 0;
    hle_ThreadManForUser_0x278C0DF5(ctx, mem);
}

// ---- das "Spiel"
void fn_root(PspContext* ctx, uint8_t* mem) {
    strcpy((char*)mem + NAMES + 0x1000, "hallo");
    const uint32_t b = create(ctx, mem, "B", CODE + 4, 32);
    start(ctx, mem, b, 6, NAMES + 0x1000);            // gleiche Priorität: root läuft weiter
    check("B laeuft noch nicht", g_trace.empty());
    const uint32_t c = create(ctx, mem, "C", CODE + 8, 16);
    start(ctx, mem, c, 0, 0);                          // höhere Priorität: C läuft sofort
    check("C hat root verdraengt", g_trace == "C");
    for (int i = 0; i < 3; i++) {
        g_trace += 'A';
        delay(ctx, mem, 20000);
    }
    wait_end(ctx, mem, b);
    wait_end(ctx, mem, c);                             // C ist längst fertig: kehrt sofort zurück
    g_trace += 'E';
    g_end_time = rt::time_us();
    ctx->r[PSP_V0] = 7;
}

void fn_b(PspContext* ctx, uint8_t* mem) {
    g_b_arg_ok = ctx->r[PSP_A0] == 6 && strcmp((const char*)mem + ctx->r[PSP_A1], "hallo") == 0;
    for (int i = 0; i < 3; i++) {
        g_trace += 'B';
        delay(ctx, mem, 35000);
    }
}

void fn_c(PspContext* ctx, uint8_t* mem) {
    g_trace += 'C';
    delay(ctx, mem, 50000);
    g_trace += 'c';
    // kritischer Abschnitt: alter Zustand kommt zurück, Wiederherstellen funktioniert
    hle_Kernel_Library_0x092968F4(ctx, mem);
    const uint32_t flags = ctx->r[PSP_V0];
    ctx->r[PSP_A0] = flags;
    hle_Kernel_Library_0x5F10D406(ctx, mem);
    check("CpuSuspendIntr liefert 1", flags == 1);
}

const PspFunc g_table[4] = {fn_root, fn_b, fn_c, nullptr};
const PspModuleDesc g_module = {PSP_INTERFACE_VERSION, "ThreadTest", "-", CODE, CODE, 0, 0, CODE, sizeof g_table / sizeof g_table[0] * 4, g_table};

} // namespace

extern "C" const PspModuleDesc* const g_psp_modules[] = {&g_module, nullptr};

int main() {
    using namespace rt;
    log_init("", PSP_LOG_WARN);
    mem_init();

    // Speicher: Spiegel zeigen auf denselben Inhalt
    psp_w32(g_mem, 0x08900100, 0x12345678u);
    check("RAM-Spiegel 0x4...", psp_r32(g_mem, 0x48900100u) == 0x12345678u);
    check("RAM-Spiegel 0x8...", psp_r32(g_mem, 0x88900100u) == 0x12345678u);
    psp_w32(g_mem, 0x04000010, 0xCAFEBABEu);
    check("VRAM-Spiegel", psp_r32(g_mem, 0x04200010u) == 0xCAFEBABEu && psp_r32(g_mem, 0x44000010u) == 0xCAFEBABEu);
    psp_w32(g_mem, 0x00010000, 1);
    check("Scratchpad", psp_r32(g_mem, 0x00010000) == 1);
    check("mem_valid", mem_valid(0x08800000, 0x1000) && !mem_valid(0x00000000, 4) && !mem_valid(0x0C000000, 4));

    // Speicherverwaltung
    mem_set_user_range(0x08A00000, 0x0A000000);
    const uint32_t total = mem_free_total();
    const uint32_t lo = mem_alloc(0x1000, false, "lo"), hi = mem_alloc(0x1000, true, "hi");
    check("mem_alloc unten/oben", lo == 0x08A00000 && hi == 0x0A000000 - 0x1000);
    check("mem_free", mem_free(lo) && mem_free(hi) && mem_free_total() == total && mem_free_max() == total);

    // Threads
    kernel_init(0);
    const uint64_t t0 = time_us();
    const int id = thread_create("root", CODE, 32, 0x8000, 0);
    check("thread_create", id > 0);
    check("thread_start", thread_start(id, 0, 0) == 0);
    scheduler_run();
    const uint64_t elapsed = g_end_time - t0;

    printf("Ablauf: %s, Dauer %llu ms\n", g_trace.c_str(), (unsigned long long)(elapsed / 1000));
    check("Reihenfolge CABABAcBE", g_trace == "CABABAcBE");
    check("Dauer mindestens 105 ms", elapsed >= 105000);
    check("Dauer hoechstens 400 ms", elapsed <= 400000);
    check("Argumente an Thread B", g_b_arg_ok == 1);
    Thread* root = thread_by_id(id);
    check("Exit-Status aus v0", root && root->state == ThreadState::Dead && root->exit_status == 7);
    check("Stacks freigegeben nach Loeschen", thread_delete(id) == 0);

    printf("%d Pruefungen, %d Fehler\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
