/*
 * Schnittstelle generierter Code <-> Runtime (T04).
 * Reines C (C11), der generierte Code bindet nur diese Datei ein.
 */
#ifndef DIVA_PSP_CONTEXT_H
#define DIVA_PSP_CONTEXT_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSP_INTERFACE_VERSION 1

/* ------------------------------------------------------------------ Register */

enum {
    PSP_ZERO = 0, PSP_AT, PSP_V0, PSP_V1, PSP_A0, PSP_A1, PSP_A2, PSP_A3,
    PSP_T0, PSP_T1, PSP_T2, PSP_T3, PSP_T4, PSP_T5, PSP_T6, PSP_T7,
    PSP_S0, PSP_S1, PSP_S2, PSP_S3, PSP_S4, PSP_S5, PSP_S6, PSP_S7,
    PSP_T8, PSP_T9, PSP_K0, PSP_K1, PSP_GP, PSP_SP, PSP_FP, PSP_RA
};

typedef union PspFpr {
    float    f;
    uint32_t u;
    int32_t  i;
} PspFpr;

/* VFPU-Steuerregister (Index in vfpu_ctrl) */
enum {
    PSP_VFPU_CTRL_SPREFIX = 0, PSP_VFPU_CTRL_TPREFIX, PSP_VFPU_CTRL_DPREFIX, PSP_VFPU_CTRL_CC,
    PSP_VFPU_CTRL_INF4, PSP_VFPU_CTRL_RSV5, PSP_VFPU_CTRL_RSV6, PSP_VFPU_CTRL_REV,
    PSP_VFPU_CTRL_RCX0, PSP_VFPU_CTRL_RCX1, PSP_VFPU_CTRL_RCX2, PSP_VFPU_CTRL_RCX3,
    PSP_VFPU_CTRL_RCX4, PSP_VFPU_CTRL_RCX5, PSP_VFPU_CTRL_RCX6, PSP_VFPU_CTRL_RCX7,
    PSP_VFPU_CTRL_COUNT
};

/*
 * CPU-Kontext eines PSP-Threads. Jeder PSP-Thread hat genau einen Kontext.
 * Der generierte Code greift nur auf r, hi, lo, f, fcr31, fcc und budget direkt zu.
 * VFPU-Zustand wird ausschließlich über Hilfsfunktionen der Runtime verändert (psp_vfpu_*).
 */
typedef struct PspContext {
    uint32_t r[32];            /* r[0] ist immer 0; der Recompiler erzeugt nie Schreibzugriffe darauf */
    uint32_t hi, lo;
    PspFpr   f[32];
    uint32_t fcr31;            /* FPU-Steuerregister (Rundungsmodus; Bedingungsbit steht in fcc) */
    uint32_t fcc;              /* FPU-Bedingungsbit: 0 oder 1 */

    /* VFPU */
#if defined(_MSC_VER)
    __declspec(align(16))
#else
    __attribute__((aligned(16)))
#endif
    float    v[128];           /* Layout innerhalb des Feldes legt runtime/vfpu fest (T12) */
    uint32_t vfpu_ctrl[PSP_VFPU_CTRL_COUNT];
    uint8_t  vfpu_pfx_active;  /* Bit 0: S-Präfix, Bit 1: T-Präfix, Bit 2: D-Präfix gilt für den nächsten Befehl */
    uint8_t  reserved0[3];

    /* Runtime */
    int32_t  budget;           /* Zähler für PSP_SAFEPOINT; läuft er unter 0, ruft der Code psp_safepoint() */
    uint32_t pc;               /* nur zur Diagnose: Adresse der letzten Aufrufstelle / des laufenden Systemaufrufs */
    void*    thread;           /* Thread-Objekt der Runtime, für generierten Code undurchsichtig */
} PspContext;

/* ------------------------------------------------------------------ Funktionen */

/* Signatur jeder recompilierten Funktion und jedes HLE-Handlers. */
typedef void (*PspFunc)(PspContext* ctx, uint8_t* mem);

/* Name einer recompilierten Funktion: fn_<Adresse, 8 Hex-Ziffern, Großbuchstaben> */
#define PSP_FUNC(addr_hex)  void fn_##addr_hex(PspContext* ctx, uint8_t* mem)

/* Name eines HLE-Handlers: hle_<Bibliothek>_<NID>, z.B. hle_ThreadManForUser_0x446D8DE6 */
#define PSP_HLE_NAME(lib, nid)  hle_##lib##_##nid
#define PSP_HLE(lib, nid)       void PSP_HLE_NAME(lib, nid)(PspContext* ctx, uint8_t* mem)

/* Beschreibung eines recompilierten Moduls; der Recompiler erzeugt genau eine Instanz je Modul. */
typedef struct PspModuleDesc {
    uint32_t       interface_version;   /* PSP_INTERFACE_VERSION */
    const char*    name;                /* z.B. "PdvApp" */
    const char*    elf_sha1;            /* SHA-1 des ELF, aus dem der Code erzeugt wurde */
    uint32_t       load_addr;           /* Adresse des ersten Segments */
    uint32_t       entry;               /* module_start */
    uint32_t       gp;
    uint32_t       module_info_addr;
    uint32_t       code_addr;           /* Beginn des Codebereichs (.text) */
    uint32_t       code_size;           /* Bytes, einschließlich .sceStub.text */
    const PspFunc* func_table;          /* code_size / 4 Einträge; NULL = kein Funktionsanfang */
} PspModuleDesc;

/* Vom Recompiler erzeugt: Liste aller Module, mit NULL abgeschlossen. */
extern const PspModuleDesc* const g_psp_modules[];

/* ------------------------------------------------------------------ Von der Runtime bereitgestellt */

/* Indirekter Aufruf (jalr, jr <reg> als Tail-Call, Callbacks). Kehrt zurück, wenn das Ziel zurückkehrt. */
void psp_call_indirect(PspContext* ctx, uint8_t* mem, uint32_t target);

/* Sicherer Punkt: Interrupts/Callbacks zustellen, ggf. Thread wechseln. Setzt ctx->budget neu. */
void psp_safepoint(PspContext* ctx, uint8_t* mem);

/* Nicht übersetzter Befehl. Loggt und beendet den Prozess (Exit-Code 70). */
void psp_unimplemented(PspContext* ctx, uint32_t addr, const char* mnemonic);

/* break-Befehl (z.B. Division durch null). Loggt. */
void psp_break(PspContext* ctx, uint32_t addr, uint32_t code);

#define UNIMPLEMENTED(addr, mnemonic)  psp_unimplemented(ctx, (addr), (mnemonic))
#define PSP_SAFEPOINT()                do { if (--ctx->budget < 0) psp_safepoint(ctx, mem); } while (0)

/* ------------------------------------------------------------------ Speicherzugriff */
/*
 * mem zeigt auf den Anfang einer 4-GiB-Reservierung. PSP-Adresse a liegt bei mem + a.
 * Spiegel (0x4xxxxxxx, 0x8xxxxxxx ...) sind als Sichten desselben Speichers eingeblendet,
 * der generierte Code maskiert deshalb nicht. PSP und Host sind little-endian.
 * memcpy statt Zeiger-Cast: erlaubt unausgerichtete Zugriffe ohne undefiniertes Verhalten.
 */
static inline uint8_t  psp_r8 (const uint8_t* mem, uint32_t a) { return mem[a]; }
static inline uint16_t psp_r16(const uint8_t* mem, uint32_t a) { uint16_t v; memcpy(&v, mem + a, 2); return v; }
static inline uint32_t psp_r32(const uint8_t* mem, uint32_t a) { uint32_t v; memcpy(&v, mem + a, 4); return v; }
static inline float    psp_rf (const uint8_t* mem, uint32_t a) { float v;    memcpy(&v, mem + a, 4); return v; }
static inline void psp_w8 (uint8_t* mem, uint32_t a, uint8_t v)  { mem[a] = v; }
static inline void psp_w16(uint8_t* mem, uint32_t a, uint16_t v) { memcpy(mem + a, &v, 2); }
static inline void psp_w32(uint8_t* mem, uint32_t a, uint32_t v) { memcpy(mem + a, &v, 4); }
static inline void psp_wf (uint8_t* mem, uint32_t a, float v)    { memcpy(mem + a, &v, 4); }

/* Adressraum */
#define PSP_SCRATCHPAD_ADDR  0x00010000u
#define PSP_SCRATCHPAD_SIZE  0x00004000u
#define PSP_VRAM_ADDR        0x04000000u
#define PSP_VRAM_SIZE        0x00200000u
#define PSP_RAM_ADDR         0x08000000u   /* Kernel-Bereich bis 0x087FFFFF, User-RAM ab 0x08800000 */
#define PSP_USER_RAM_ADDR    0x08800000u
#define PSP_RAM_SIZE_MAX     0x04000000u   /* 64 MiB eingeblendet; nutzbar laut games/<spiel>.toml */

#ifdef __cplusplus
}
#endif

#endif /* DIVA_PSP_CONTEXT_H */
