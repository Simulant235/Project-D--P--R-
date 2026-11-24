/*
 * VFPU-Helfer. Registeradressierung, Präfixe und Verhalten folgen dem PPSSPP-Interpreter
 * (Core/MIPS/InterpreterVFPU.cpp, Core/MIPS/MIPSVFPUUtils.cpp, Copyright (c) 2012- PPSSPP Project,
 * GPL 2.0 oder später, https://github.com/hrydgard/ppsspp). Neu geschrieben, aber daran ausgerichtet;
 * Projekt steht ebenfalls unter GPL.
 *
 * Vereinfachungen ggü. PPSSPP:
 * - vdot/vmmul mit normalem float, nicht bitgenau wie die Hardware
 * - vrsq/vrcp/vsqrt über Host-Funktionen
 * - ungültiger Swizzle (nicht vorhandenes Element) liefert 0
 * - vmmul: vom D-Präfix nur Sättigung der letzten Zeile, keine Schreibmaske
 */
#include <math.h>

#include "psp_vfpu.h"

/* Speicherlayout von ctx->v: Matrix * 16 + Spalte * 4 + Zeile */
static int single_index(int reg) { return ((reg >> 2) & 7) * 16 + (reg & 3) * 4 + ((reg >> 5) & 3); }

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* Elementindizes eines Vektorregisters, Rückgabe = Anzahl */
static int vec_indices(int idx[4], int size, int reg) {
    int n = size + 1, i, row;
    const int mtx = (reg << 2) & 0x70, col = reg & 3, transpose = (reg >> 5) & 1;
    if (size == 0) { idx[0] = single_index(reg); return 1; }
    row = size == 2 ? (reg >> 6) & 1 : (reg >> 5) & 2;
    for (i = 0; i < n; i++)
        idx[i] = transpose ? mtx + col + ((row + i) & 3) * 4 : mtx + col * 4 + ((row + i) & 3);
    return n;
}

static int read_vec(const PspContext* ctx, float out[4], int size, int reg) {
    int idx[4], n = vec_indices(idx, size, reg), i;
    for (i = 0; i < n; i++) out[i] = ctx->v[idx[i]];
    for (; i < 4; i++) out[i] = 0.0f;
    return n;
}

/* Schreiben mit D-Präfix-Maske (Bits 8..11) */
static void write_vec(PspContext* ctx, const float in[4], int size, int reg) {
    int idx[4], n = vec_indices(idx, size, reg), i;
    const uint32_t mask = (ctx->vfpu_ctrl[PSP_VFPU_CTRL_DPREFIX] >> 8) & 0xF;
    for (i = 0; i < n; i++)
        if (!((mask >> i) & 1)) ctx->v[idx[i]] = in[i];
}

static void eat_prefixes(PspContext* ctx) {
    ctx->vfpu_ctrl[PSP_VFPU_CTRL_SPREFIX] = 0xE4;
    ctx->vfpu_ctrl[PSP_VFPU_CTRL_TPREFIX] = 0xE4;
    ctx->vfpu_ctrl[PSP_VFPU_CTRL_DPREFIX] = 0;
}

/* S-/T-Präfix: Swizzle (Bits 0..7), Betrag (8..11), Konstante (12..15), Negation (16..19) */
static void apply_st(float r[4], uint32_t data, int n) {
    static const float constants[8] = {0.0f, 1.0f, 2.0f, 0.5f, 3.0f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f};
    float orig[4];
    int i;
    if (data == 0xE4) return;
    for (i = 0; i < 4; i++) orig[i] = i < n ? r[i] : 0.0f;
    for (i = 0; i < n; i++) {
        const int sw = (data >> (i * 2)) & 3, ab = (data >> (8 + i)) & 1;
        uint32_t u;
        if ((data >> (12 + i)) & 1) {
            u = f2u(constants[sw + (ab << 2)]);
        } else {
            u = f2u(orig[sw]);
            if (ab) u &= 0x7FFFFFFFu;
        }
        if ((data >> (16 + i)) & 1) u ^= 0x80000000u;
        r[i] = u2f(u);
    }
}

/* Ordnung für min/max/clamp wie Hardware: Denormale = 0, NaN/Inf nach Vorzeichen+Betrag */
static int32_t order_key(float f) {
    uint32_t u = f2u(f), mag = u & 0x7FFFFFFFu;
    if (mag < 0x00800000u) mag = 0;
    return (u & 0x80000000u) ? -(int32_t)mag : (int32_t)mag;
}
static float vmin(float a, float b) { return order_key(a) < order_key(b) ? a : b; }
static float vmax(float a, float b) { return order_key(a) > order_key(b) ? a : b; }

/* D-Präfix: Sättigung je Element (Bits 0..7): 1 = [0,1], 3 = [-1,1] */
static void apply_d(const PspContext* ctx, float v[4], int n) {
    const uint32_t data = ctx->vfpu_ctrl[PSP_VFPU_CTRL_DPREFIX];
    int i;
    if (!data) return;
    for (i = 0; i < n; i++) {
        const int sat = (data >> (i * 2)) & 3;
        if (sat == 1) v[i] = vmin(vmax(v[i], 0.0f), 1.0f);
        else if (sat == 3) v[i] = vmin(vmax(v[i], -1.0f), 1.0f);
    }
}

static float fix_dot(float f) {   /* Sonderwerte wie Hardware */
    uint32_t u = f2u(f);
    if (f != f) return u2f(0x7F800001u);
    if ((u & 0x7F800000u) == 0) return u2f(u & 0xFF800000u);
    return f;
}

/* ------------------------------------------------------------------ öffentliche Funktionen */

uint32_t psp_vfpu_ri(const PspContext* ctx, int reg) { return f2u(ctx->v[single_index(reg)]); }
void psp_vfpu_wi(PspContext* ctx, int reg, uint32_t value) { ctx->v[single_index(reg)] = u2f(value); }

uint32_t psp_vfpu_mfvc(const PspContext* ctx, int n) { return n >= 0 && n < PSP_VFPU_CTRL_COUNT ? ctx->vfpu_ctrl[n] : 0; }
void psp_vfpu_mtvc(PspContext* ctx, int n, uint32_t value) {
    if (n < 0 || n >= PSP_VFPU_CTRL_COUNT) return;
    if (n == PSP_VFPU_CTRL_CC) value &= 0x3F;
    ctx->vfpu_ctrl[n] = value;
}

void psp_vfpu_lvq(PspContext* ctx, const uint8_t* mem, uint32_t addr, int vt) {
    int idx[4], i;
    vec_indices(idx, 3, vt);
    for (i = 0; i < 4; i++) ctx->v[idx[i]] = psp_rf(mem, addr + 4u * (uint32_t)i);
}

void psp_vfpu_svq(const PspContext* ctx, uint8_t* mem, uint32_t addr, int vt) {
    int idx[4], i;
    vec_indices(idx, 3, vt);
    for (i = 0; i < 4; i++) psp_wf(mem, addr + 4u * (uint32_t)i, ctx->v[idx[i]]);
}

void psp_vfpu_pfx(PspContext* ctx, int which, uint32_t data) {
    if (which >= 0 && which <= 2) ctx->vfpu_ctrl[PSP_VFPU_CTRL_SPREFIX + which] = data & 0xFFFFFu;
}

void psp_vfpu_reset(PspContext* ctx) {
    memset(ctx->v, 0, sizeof ctx->v);
    memset(ctx->vfpu_ctrl, 0, sizeof ctx->vfpu_ctrl);
    eat_prefixes(ctx);
    ctx->vfpu_ctrl[PSP_VFPU_CTRL_REV] = 0x7772CEABu;
    ctx->vfpu_ctrl[PSP_VFPU_CTRL_RCX0] = 0x3F800001u;
    ctx->vfpu_ctrl[PSP_VFPU_CTRL_RCX1] = 0x3F800002u;
    ctx->vfpu_ctrl[PSP_VFPU_CTRL_RCX2] = 0x3F800004u;
    ctx->vfpu_ctrl[PSP_VFPU_CTRL_RCX3] = 0x3F800008u;
    ctx->vfpu_ctrl[PSP_VFPU_CTRL_RCX4] = 0x3F800000u;
    ctx->vfpu_ctrl[PSP_VFPU_CTRL_RCX5] = 0x3F800000u;
    ctx->vfpu_ctrl[PSP_VFPU_CTRL_RCX6] = 0x3F800000u;
    ctx->vfpu_ctrl[PSP_VFPU_CTRL_RCX7] = 0x3F800000u;
}

void psp_vfpu_op1(PspContext* ctx, int op, int size, int vd) {
    const float c = op == PSP_VOP1_ONE ? 1.0f : 0.0f;
    float d[4] = {c, c, c, c};
    apply_d(ctx, d, size + 1);
    write_vec(ctx, d, size, vd);
    eat_prefixes(ctx);
}

void psp_vfpu_op2(PspContext* ctx, int op, int size, int vd, int vs) {
    float s[4], d[4] = {0, 0, 0, 0};
    int n = read_vec(ctx, s, size, vs), i, out_size = size;
    apply_st(s, ctx->vfpu_ctrl[PSP_VFPU_CTRL_SPREFIX], op == PSP_VOP2_I2C ? 4 : n);
    switch (op) {
    case PSP_VOP2_MOV: for (i = 0; i < n; i++) d[i] = s[i]; break;
    case PSP_VOP2_ABS: for (i = 0; i < n; i++) d[i] = u2f(f2u(s[i]) & 0x7FFFFFFFu); break;
    case PSP_VOP2_NEG: for (i = 0; i < n; i++) d[i] = u2f(f2u(s[i]) ^ 0x80000000u); break;
    case PSP_VOP2_SQRT: for (i = 0; i < n; i++) d[i] = sqrtf(u2f(f2u(s[i]) & 0x7FFFFFFFu)); break;
    case PSP_VOP2_RSQ: for (i = 0; i < n; i++) d[i] = 1.0f / sqrtf(s[i]); break;
    case PSP_VOP2_RCP: for (i = 0; i < n; i++) d[i] = 1.0f / s[i]; break;
    case PSP_VOP2_C2I: {
        const uint32_t v = f2u(s[0]);
        d[0] = u2f((v & 0xFFu) << 24);
        d[1] = u2f((v & 0xFF00u) << 16);
        d[2] = u2f((v & 0xFF0000u) << 8);
        d[3] = u2f(v & 0xFF000000u);
        out_size = 3;
        break;
    }
    case PSP_VOP2_I2C: {
        uint32_t v = 0;
        for (i = 0; i < 4; i++) v |= (f2u(s[i]) >> 24) << (i * 8);
        d[0] = u2f(v);
        out_size = 0;
        break;
    }
    default: break;
    }
    if (op != PSP_VOP2_C2I && op != PSP_VOP2_I2C) apply_d(ctx, d, out_size + 1);
    write_vec(ctx, d, out_size, vd);
    eat_prefixes(ctx);
}

void psp_vfpu_op3(PspContext* ctx, int op, int size, int vd, int vs, int vt) {
    float s[4], t[4], d[4] = {0, 0, 0, 0};
    int n = read_vec(ctx, s, size, vs), i, out_size = size;
    const uint32_t sp = ctx->vfpu_ctrl[PSP_VFPU_CTRL_SPREFIX], tp = ctx->vfpu_ctrl[PSP_VFPU_CTRL_TPREFIX];
    if (op == PSP_VOP3_SCL) {
        const float scale = ctx->v[single_index(vt)];
        apply_st(s, sp, n);
        for (i = 0; i < 4; i++) t[i] = scale;
        if (tp != 0xE4) {   /* T-Präfix auf den Skalar: abs/neg/Konstante je Element */
            float one[4] = {scale, scale, scale, scale};
            apply_st(one, (tp & ~0xFFu) | 0xE4u, 4);
            for (i = 0; i < 4; i++) t[i] = one[i];
        }
        for (i = 0; i < n; i++) d[i] = s[i] * t[i];
    } else if (op == PSP_VOP3_DOT) {
        float sum = 0.0f;
        read_vec(ctx, t, size, vt);
        apply_st(s, sp, 4);
        apply_st(t, tp, 4);
        for (i = 0; i < 4; i++) sum += s[i] * t[i];
        d[0] = fix_dot(sum);
        out_size = 0;
    } else {
        read_vec(ctx, t, size, vt);
        apply_st(s, sp, n);
        apply_st(t, tp, n);
        for (i = 0; i < n; i++) {
            switch (op) {
            case PSP_VOP3_ADD: d[i] = s[i] + t[i]; break;
            case PSP_VOP3_SUB: d[i] = s[i] - t[i]; break;
            case PSP_VOP3_MUL: d[i] = s[i] * t[i]; break;
            case PSP_VOP3_DIV: d[i] = s[i] / t[i]; break;
            case PSP_VOP3_MIN: d[i] = vmin(s[i], t[i]); break;
            case PSP_VOP3_MAX: d[i] = vmax(s[i], t[i]); break;
            default: break;
            }
        }
    }
    apply_d(ctx, d, out_size + 1);
    write_vec(ctx, d, out_size, vd);
    eat_prefixes(ctx);
}

void psp_vfpu_vcmp(PspContext* ctx, int cond, int size, int vs, int vt) {
    float s[4], t[4];
    int n = read_vec(ctx, s, size, vs), i;
    uint32_t cc = 0, any = 0, all = 1, affected = (1u << 4) | (1u << 5);
    read_vec(ctx, t, size, vt);
    apply_st(s, ctx->vfpu_ctrl[PSP_VFPU_CTRL_SPREFIX], n);
    apply_st(t, ctx->vfpu_ctrl[PSP_VFPU_CTRL_TPREFIX], n);
    for (i = 0; i < n; i++) {
        const float a = s[i], b = t[i];
        const int is_nan = a != a, is_inf = !is_nan && (f2u(a) & 0x7FFFFFFFu) == 0x7F800000u;
        uint32_t c = 0;
        switch (cond & 0xF) {
        case 0: c = 0; break;                     /* FL */
        case 1: c = a == b; break;                /* EQ */
        case 2: c = a < b; break;                 /* LT */
        case 3: c = a <= b; break;                /* LE */
        case 4: c = 1; break;                     /* TR */
        case 5: c = a != b; break;                /* NE */
        case 6: c = a >= b; break;                /* GE */
        case 7: c = a > b; break;                 /* GT */
        case 8: c = a == 0.0f; break;             /* EZ */
        case 9: c = is_nan; break;                /* EN */
        case 10: c = is_inf; break;               /* EI */
        case 11: c = is_nan || is_inf; break;     /* ES */
        case 12: c = a != 0.0f; break;            /* NZ */
        case 13: c = !is_nan; break;              /* NN */
        case 14: c = !is_inf; break;              /* NI */
        case 15: c = !(is_nan || is_inf); break;  /* NS */
        }
        cc |= c << i;
        any |= c;
        all &= c;
        affected |= 1u << i;
    }
    ctx->vfpu_ctrl[PSP_VFPU_CTRL_CC] =
        (ctx->vfpu_ctrl[PSP_VFPU_CTRL_CC] & ~affected) | ((cc | (any << 4) | (all << 5)) & affected);
    eat_prefixes(ctx);
}

void psp_vfpu_vcmov(PspContext* ctx, int size, int vd, int vs, int imm3, int tf) {
    float s[4], d[4];
    int n = read_vec(ctx, s, size, vs), i;
    const uint32_t cc = ctx->vfpu_ctrl[PSP_VFPU_CTRL_CC];
    apply_st(s, ctx->vfpu_ctrl[PSP_VFPU_CTRL_SPREFIX], n);
    read_vec(ctx, d, size, vd);
    apply_st(d, ctx->vfpu_ctrl[PSP_VFPU_CTRL_TPREFIX], n);
    if (imm3 == 6) {
        for (i = 0; i < n; i++)
            if (((cc >> i) & 1) == (uint32_t)!tf) d[i] = s[i];
    } else if (((cc >> imm3) & 1) == (uint32_t)!tf) {
        for (i = 0; i < n; i++) d[i] = s[i];
    }
    apply_d(ctx, d, n);
    write_vec(ctx, d, size, vd);
    eat_prefixes(ctx);
}

/* Matrixelement (Spalte j, Zeile i) einer Matrix-Registerangabe */
static int mtx_index(int size, int reg, int j, int i) {
    const int mtx = ((reg >> 2) & 7) * 16, col = reg & 3, transpose = (reg >> 5) & 1;
    const int row = size == 2 ? (reg >> 6) & 1 : (reg >> 5) & 2;
    return transpose ? mtx + ((row + i) & 3) * 4 + ((col + j) & 3) : mtx + ((col + j) & 3) * 4 + ((row + i) & 3);
}

void psp_vfpu_vmmul(PspContext* ctx, int size, int vd, int vs, int vt) {
    const int n = size == 1 ? 2 : size == 2 ? 3 : 4;
    float s[16] = {0}, t[16] = {0}, d[16] = {0};
    int a, b, c;
    for (a = 0; a < n; a++)
        for (b = 0; b < n; b++) {
            s[a * 4 + b] = ctx->v[mtx_index(size, vs, a, b)];
            t[a * 4 + b] = ctx->v[mtx_index(size, vt, a, b)];
        }
    for (a = 0; a < n; a++)
        for (b = 0; b < n; b++) {
            float sum = 0.0f;
            if (a == n - 1 && b == n - 1) {   /* Präfixe nur aufs letzte Element */
                apply_st(&s[b * 4], ctx->vfpu_ctrl[PSP_VFPU_CTRL_SPREFIX], 4);
                apply_st(&t[a * 4], ctx->vfpu_ctrl[PSP_VFPU_CTRL_TPREFIX], 4);
            }
            for (c = 0; c < 4; c++) sum += s[b * 4 + c] * t[a * 4 + c];
            d[a * 4 + b] = fix_dot(sum);
        }
    {
        const int sat = ctx->vfpu_ctrl[PSP_VFPU_CTRL_DPREFIX] & 3;
        float* last = &d[(n - 1) * 4 + (n - 1)];
        if (sat == 1) *last = vmin(vmax(*last, 0.0f), 1.0f);
        else if (sat == 3) *last = vmin(vmax(*last, -1.0f), 1.0f);
    }
    for (a = 0; a < n; a++)
        for (b = 0; b < n; b++) ctx->v[mtx_index(size, vd, a, b)] = d[a * 4 + b];
    eat_prefixes(ctx);
}
