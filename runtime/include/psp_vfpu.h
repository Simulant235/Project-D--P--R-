/*
 * VFPU-Helfer für generierten Code (T12): Aufrufe mit schon dekodierten Operanden statt C-Ausdrücken.
 * Nur was das Spiel braucht (plus Verwandtes), Rest = UNIMPLEMENTED. Impl.: runtime/vfpu/psp_vfpu.c
 */
#ifndef DIVA_PSP_VFPU_H
#define DIVA_PSP_VFPU_H

#include "psp_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Größe: 0 = Single (.s), 1 = Pair (.p), 2 = Triple (.t), 3 = Quad (.q); bei Matrizen 1 = 2x2, 2 = 3x3, 3 = 4x4 */

enum {  /* drei Operanden: vd = vs <op> vt */
    PSP_VOP3_ADD, PSP_VOP3_SUB, PSP_VOP3_MUL, PSP_VOP3_DIV, PSP_VOP3_MIN, PSP_VOP3_MAX,
    PSP_VOP3_SCL,   /* vt ist ein Einzelregister */
    PSP_VOP3_DOT    /* vd ist ein Einzelregister */
};
enum {  /* zwei Operanden: vd = <op> vs */
    PSP_VOP2_MOV, PSP_VOP2_ABS, PSP_VOP2_NEG, PSP_VOP2_SQRT, PSP_VOP2_RSQ, PSP_VOP2_RCP,
    PSP_VOP2_C2I,   /* Einzelregister (Farbe) -> Quad */
    PSP_VOP2_I2C    /* Quad -> Einzelregister (Farbe) */
};
enum { PSP_VOP1_ZERO, PSP_VOP1_ONE };

/* Rohzugriff auf ein Einzelregister (lv.s, sv.s, mtv, mfv) */
uint32_t psp_vfpu_ri(const PspContext* ctx, int reg);
void     psp_vfpu_wi(PspContext* ctx, int reg, uint32_t value);
/* Steuerregister (mtvc/mfvc), n = 0..15 */
uint32_t psp_vfpu_mfvc(const PspContext* ctx, int n);
void     psp_vfpu_mtvc(PspContext* ctx, int n, uint32_t value);

void psp_vfpu_lvq(PspContext* ctx, const uint8_t* mem, uint32_t addr, int vt);
void psp_vfpu_svq(const PspContext* ctx, uint8_t* mem, uint32_t addr, int vt);

/* which: 0 = vpfxs, 1 = vpfxt, 2 = vpfxd */
void psp_vfpu_pfx(PspContext* ctx, int which, uint32_t data);

void psp_vfpu_op1(PspContext* ctx, int op, int size, int vd);
void psp_vfpu_op2(PspContext* ctx, int op, int size, int vd, int vs);
void psp_vfpu_op3(PspContext* ctx, int op, int size, int vd, int vs, int vt);
void psp_vfpu_vcmp(PspContext* ctx, int cond, int size, int vs, int vt);
void psp_vfpu_vcmov(PspContext* ctx, int size, int vd, int vs, int imm3, int tf);
void psp_vfpu_vmmul(PspContext* ctx, int size, int vd, int vs, int vt);

/* VFPU-Startzustand je Thread, Präfixe neutral */
void psp_vfpu_reset(PspContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* DIVA_PSP_VFPU_H */
