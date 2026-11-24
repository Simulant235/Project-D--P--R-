/*
 * Hilfen für HLE-Handler (nachgebildete PSP-Syscalls).
 */
#ifndef DIVA_PSP_HLE_H
#define DIVA_PSP_HLE_H

#include "psp_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Argumente nach MIPS-EABI der PSP: a0-a3, dann t0-t3. Weitere Argumente gibt es bei Systemaufrufen nicht. */
#define HLE_ARG(n)      (ctx->r[PSP_A0 + (n)])            /* n = 0..7 */
#define HLE_ARG64(n)    ((uint64_t)ctx->r[PSP_A0 + (n)] | ((uint64_t)ctx->r[PSP_A0 + (n) + 1] << 32)) /* n gerade */
#define HLE_RET(v)      do { ctx->r[PSP_V0] = (uint32_t)(v); } while (0)
#define HLE_RET64(v)    do { uint64_t hle_v_ = (uint64_t)(v); ctx->r[PSP_V0] = (uint32_t)hle_v_; \
                             ctx->r[PSP_V1] = (uint32_t)(hle_v_ >> 32); } while (0)
#define HLE_PTR(a)      ((a) ? (void*)(mem + (uint32_t)(a)) : (void*)0)
#define HLE_STR(a)      ((const char*)HLE_PTR(a))

/* Logging */
typedef enum PspLogLevel { PSP_LOG_ERROR = 0, PSP_LOG_WARN, PSP_LOG_INFO, PSP_LOG_DEBUG, PSP_LOG_VERBOSE } PspLogLevel;
void psp_log(PspLogLevel level, const char* category, const char* fmt, ...);

/*
 * Für Stubs: loggt "UNIMPL <lib> <nid> <name> a0=.. a1=.. a2=.. a3=.. ra=.." (die ersten 3 Aufrufe je Funktion
 * als WARN, danach nur noch zählen) und setzt v0 = ret.
 */
void psp_hle_unimplemented(PspContext* ctx, const char* lib, uint32_t nid, const char* name, uint32_t ret);

/* Ein Stub. Wird die Funktion implementiert, die Zeile aus runtime/hle/stubs/<lib>.cpp löschen. */
#define PSP_HLE_STUB(lib, nid, name, ret) \
    PSP_HLE(lib, nid) { (void)mem; psp_hle_unimplemented(ctx, #lib, nid, name, (uint32_t)(ret)); }

#ifdef __cplusplus
}
#endif

#endif /* DIVA_PSP_HLE_H */
