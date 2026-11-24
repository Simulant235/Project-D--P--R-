// Test für Semaphoren, Event-Flags, Mutexe, ohne Spieldateien (Aufbau wie thread_test.cpp)
#include <cstdio>
#include <cstring>
#include <string>

#include "rt.h"

extern "C" {
PSP_HLE(ThreadManForUser, 0x446D8DE6);   // sceKernelCreateThread
PSP_HLE(ThreadManForUser, 0xF475845D);   // sceKernelStartThread
PSP_HLE(ThreadManForUser, 0xCEADEB47);   // sceKernelDelayThread
PSP_HLE(ThreadManForUser, 0x278C0DF5);   // sceKernelWaitThreadEnd
PSP_HLE(ThreadManForUser, 0xD6DA4BA1);   // sceKernelCreateSema
PSP_HLE(ThreadManForUser, 0x4E3A1105);   // sceKernelWaitSema
PSP_HLE(ThreadManForUser, 0x3F53E640);   // sceKernelSignalSema
PSP_HLE(ThreadManForUser, 0x58B1F937);   // sceKernelPollSema
PSP_HLE(ThreadManForUser, 0x55C20A00);   // sceKernelCreateEventFlag
PSP_HLE(ThreadManForUser, 0x1FB15A32);   // sceKernelSetEventFlag
PSP_HLE(ThreadManForUser, 0x402FCF22);   // sceKernelWaitEventFlag
PSP_HLE(ThreadManForUser, 0xB7D098C6);   // sceKernelCreateMutex
PSP_HLE(ThreadManForUser, 0xB011B11F);   // sceKernelLockMutex
PSP_HLE(ThreadManForUser, 0x6B30100F);   // sceKernelUnlockMutex
}

namespace {

const uint32_t CODE = 0x08804000, SCRATCH = 0x08900000;
std::string g_trace;
int g_failed = 0, g_checks = 0;
uint32_t g_sema = 0, g_flag = 0, g_mutex = 0;

void check(const char* name, bool ok) {
    g_checks++;
    if (!ok) {
        printf("FEHLER: %s\n", name);
        g_failed++;
    }
}

uint32_t call(PspFunc fn, PspContext* ctx, uint8_t* mem, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t t0 = 0) {
    ctx->r[PSP_A0] = a0;
    ctx->r[PSP_A1] = a1;
    ctx->r[PSP_A2] = a2;
    ctx->r[PSP_A3] = a3;
    ctx->r[PSP_T0] = t0;
    fn(ctx, mem);
    return ctx->r[PSP_V0];
}

uint32_t name(uint8_t* mem, const char* s) {
    static uint32_t next = SCRATCH + 0x1000;
    const uint32_t a = next;
    next += 32;
    strcpy((char*)mem + a, s);
    return a;
}

uint32_t spawn(PspContext* ctx, uint8_t* mem, const char* n, uint32_t entry) {
    const uint32_t id = call(hle_ThreadManForUser_0x446D8DE6, ctx, mem, name(mem, n), entry, 32, 0x4000);
    call(hle_ThreadManForUser_0xF475845D, ctx, mem, id);
    return id;
}

const uint32_t ERR_WAIT_TIMEOUT = 0x800201A8u, ERR_SEMA_ZERO = 0x800201ADu, ERR_MUTEX_NOT_OWNED = 0x800201C5u,
               ERR_MUTEX_RECURSIVE = 0x800201C8u, ERR_UNKNOWN_SEMID = 0x80020199u;

// ---- Thread B: signalisiert die Semaphore
void fn_b(PspContext* ctx, uint8_t* mem) {
    g_trace += 'b';
    call(hle_ThreadManForUser_0x3F53E640, ctx, mem, g_sema, 1);   // weckt root, B läuft weiter (gleiche Priorität)
    g_trace += 'B';
}

// ---- Thread C: setzt Event-Flag-Bits in zwei Schritten
void fn_c(PspContext* ctx, uint8_t* mem) {
    call(hle_ThreadManForUser_0x1FB15A32, ctx, mem, g_flag, 0x1);
    g_trace += 'c';
    call(hle_ThreadManForUser_0xCEADEB47, ctx, mem, 10000);
    call(hle_ThreadManForUser_0x1FB15A32, ctx, mem, g_flag, 0x2);
}

// ---- Thread D: hält den Mutex, bis root ihn braucht
void fn_d(PspContext* ctx, uint8_t* mem) {
    call(hle_ThreadManForUser_0xB011B11F, ctx, mem, g_mutex, 1, 0);
    g_trace += 'd';
    call(hle_ThreadManForUser_0xCEADEB47, ctx, mem, 20000);
    call(hle_ThreadManForUser_0x6B30100F, ctx, mem, g_mutex, 1);
    g_trace += 'D';
}

void fn_root(PspContext* ctx, uint8_t* mem) {
    // Semaphore: root wartet, B signalisiert
    g_sema = call(hle_ThreadManForUser_0xD6DA4BA1, ctx, mem, name(mem, "sema"), 0, 0, 2);
    check("CreateSema", (int32_t)g_sema > 0);
    check("CreateSema mit init > max schlägt fehl", call(hle_ThreadManForUser_0xD6DA4BA1, ctx, mem, name(mem, "x"), 0, 3, 2) == 0x800201BDu);
    const uint32_t b = spawn(ctx, mem, "B", CODE + 4);
    g_trace += 'r';
    check("WaitSema liefert 0", call(hle_ThreadManForUser_0x4E3A1105, ctx, mem, g_sema, 1, 0) == 0);
    g_trace += 'R';
    call(hle_ThreadManForUser_0x278C0DF5, ctx, mem, b, 0);

    // Timeout
    const uint64_t t0 = rt::time_us();
    psp_w32(mem, SCRATCH, 30000);
    check("WaitSema Timeout", call(hle_ThreadManForUser_0x4E3A1105, ctx, mem, g_sema, 1, SCRATCH) == ERR_WAIT_TIMEOUT);
    check("Timeout dauert etwa 30 ms", rt::time_us() - t0 >= 25000 && rt::time_us() - t0 < 300000);
    check("PollSema leer", call(hle_ThreadManForUser_0x58B1F937, ctx, mem, g_sema, 1) == ERR_SEMA_ZERO);
    call(hle_ThreadManForUser_0x3F53E640, ctx, mem, g_sema, 2);
    check("PollSema nach Signal", call(hle_ThreadManForUser_0x58B1F937, ctx, mem, g_sema, 2) == 0);
    check("SignalSema über max", call(hle_ThreadManForUser_0x3F53E640, ctx, mem, g_sema, 3) == 0x800201AEu);
    check("unbekannte Semaphore", call(hle_ThreadManForUser_0x4E3A1105, ctx, mem, 0x1234, 1, 0) == ERR_UNKNOWN_SEMID);

    // Event-Flag: UND-Verknüpfung über zwei Schritte
    g_flag = call(hle_ThreadManForUser_0x55C20A00, ctx, mem, name(mem, "flag"), 0, 0);
    const uint32_t c = spawn(ctx, mem, "C", CODE + 8);
    psp_w32(mem, SCRATCH + 8, 0);
    check("WaitEventFlag (AND 0x3)", call(hle_ThreadManForUser_0x402FCF22, ctx, mem, g_flag, 0x3, 0x00, SCRATCH + 8, 0) == 0);
    check("Muster beim Aufwachen = 3", psp_r32(mem, SCRATCH + 8) == 3);
    check("Thread C hat zwischendurch gelaufen", g_trace.find('c') != std::string::npos);
    call(hle_ThreadManForUser_0x278C0DF5, ctx, mem, c, 0);
    // ODER mit Löschen der Bits (0x20): zweites Warten findet das Muster nicht mehr
    check("WaitEventFlag (OR, clear)", call(hle_ThreadManForUser_0x402FCF22, ctx, mem, g_flag, 0x1, 0x21, SCRATCH + 8, 0) == 0);
    psp_w32(mem, SCRATCH, 15000);
    check("Bits wurden gelöscht -> Timeout", call(hle_ThreadManForUser_0x402FCF22, ctx, mem, g_flag, 0x1, 0x01, SCRATCH + 8, SCRATCH) == ERR_WAIT_TIMEOUT);

    // Mutex
    g_mutex = call(hle_ThreadManForUser_0xB7D098C6, ctx, mem, name(mem, "mutex"), 0, 0);
    check("LockMutex frei", call(hle_ThreadManForUser_0xB011B11F, ctx, mem, g_mutex, 1, 0) == 0);
    check("LockMutex rekursiv ohne Attribut", call(hle_ThreadManForUser_0xB011B11F, ctx, mem, g_mutex, 1, 0) == ERR_MUTEX_RECURSIVE);
    check("UnlockMutex", call(hle_ThreadManForUser_0x6B30100F, ctx, mem, g_mutex, 1) == 0);
    check("UnlockMutex ohne Besitz", call(hle_ThreadManForUser_0x6B30100F, ctx, mem, g_mutex, 1) == ERR_MUTEX_NOT_OWNED);
    const uint32_t d = spawn(ctx, mem, "D", CODE + 12);
    call(hle_ThreadManForUser_0xCEADEB47, ctx, mem, 1000);   // D bekommt den Mutex
    const uint64_t t1 = rt::time_us();
    check("LockMutex wartet auf D", call(hle_ThreadManForUser_0xB011B11F, ctx, mem, g_mutex, 1, 0) == 0);
    check("Wartezeit etwa 20 ms", rt::time_us() - t1 >= 12000);
    g_trace += 'M';
    call(hle_ThreadManForUser_0x278C0DF5, ctx, mem, d, 0);
    ctx->r[PSP_V0] = 0;
}

void fn_nop(PspContext*, uint8_t*) {}

const PspFunc g_table[6] = {fn_root, fn_b, fn_c, fn_d, fn_nop, nullptr};
const PspModuleDesc g_module = {PSP_INTERFACE_VERSION, "SyncTest", "-", CODE, CODE, 0, 0, CODE, sizeof g_table / sizeof g_table[0] * 4, g_table};

} // namespace

extern "C" const PspModuleDesc* const g_psp_modules[] = {&g_module, nullptr};

int main() {
    using namespace rt;
    log_init("", PSP_LOG_WARN);
    mem_init();
    mem_set_user_range(0x08A00000, 0x0A000000);
    kernel_init(0);
    const int id = thread_create("root", CODE, 32, 0x8000, 0);
    thread_start(id, 0, 0);
    scheduler_run();
    printf("Ablauf: %s\n", g_trace.c_str());
    check("Reihenfolge rbBRc dM D", g_trace.find("rbBR") == 0);
    check("Mutex-Übergabe: d vor M vor D", g_trace.find('d') < g_trace.find('M') && g_trace.find('M') != std::string::npos);
    printf("%d Pruefungen, %d Fehler\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
