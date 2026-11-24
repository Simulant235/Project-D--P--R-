/* führt das recompilierte Testprogramm aus (test_gen.cpp), mit Mini-Runtime */
#include <stdio.h>
#include <stdlib.h>

#include "psp_context.h"
#include "psp_vfpu.h"
#include "test_defs.h"

static uint8_t* g_mem;
static int g_safepoints, g_failed, g_checks;

void psp_call_indirect(PspContext* ctx, uint8_t* mem, uint32_t target) {
    int i;
    for (i = 0; g_psp_modules[i]; i++) {
        const PspModuleDesc* m = g_psp_modules[i];
        if (!(target & 3) && target >= m->code_addr && target < m->code_addr + m->code_size) {
            PspFunc f = m->func_table[(target - m->code_addr) >> 2];
            if (f) { f(ctx, mem); return; }
        }
    }
    printf("FEHLER: unbekanntes Sprungziel 0x%08X\n", target);
    exit(73);
}

void psp_safepoint(PspContext* ctx, uint8_t* mem) { (void)mem; g_safepoints++; ctx->budget = 3; }
void psp_unimplemented(PspContext* ctx, uint32_t addr, const char* mnemonic) {
    (void)ctx;
    printf("FEHLER: nicht uebersetzter Befehl 0x%08X %s\n", addr, mnemonic);
    exit(70);
}
void psp_break(PspContext* ctx, uint32_t addr, uint32_t code) { (void)ctx; (void)addr; (void)code; }

/* einziger Import des Testmoduls */
void hle_TestLib_0x11223344(PspContext* ctx, uint8_t* mem) { (void)mem; ctx->r[PSP_V0] = ctx->r[PSP_A0] + 1000; }

/* Ersatz für die per [[hook]] ersetzte Funktion */
void test_hook(PspContext* ctx, uint8_t* mem) { (void)mem; ctx->r[PSP_V0] = 4242; }

static uint32_t F(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static PspContext run(uint32_t fn, uint32_t a0, uint32_t a1) {
    PspContext c;
    memset(&c, 0, sizeof c);
    c.r[PSP_A0] = a0;
    c.r[PSP_A1] = a1;
    c.r[PSP_SP] = 0x00080000u;
    c.r[PSP_RA] = 0xDEAD0000u;
    c.budget = 3;
    c.fcr31 = 0x00000E00u;
    psp_vfpu_reset(&c);
    psp_call_indirect(&c, g_mem, fn);
    if (c.r[PSP_SP] != 0x00080000u) { printf("FEHLER: sp nach 0x%08X nicht wiederhergestellt\n", fn); g_failed++; }
    if (c.r[0] != 0) { printf("FEHLER: r0 veraendert in 0x%08X\n", fn); g_failed++; }
    return c;
}

static void check(const char* name, uint32_t got, uint32_t want) {
    g_checks++;
    if (got != want) { printf("FEHLER %-28s erhalten 0x%08X, erwartet 0x%08X\n", name, got, want); g_failed++; }
}

int main(void) {
    PspContext c;
    g_mem = (uint8_t*)calloc(1, 0x100000);
    if (!g_mem) return 1;
    memcpy(g_mem + TEST_IMAGE_ADDR, test_image, sizeof test_image);

    check("nonlikely genommen", run(ADDR_t_nonlikely, 0, 0).r[PSP_V0], 101);
    check("nonlikely nicht genommen", run(ADDR_t_nonlikely, 5, 0).r[PSP_V0], 10);
    check("likely genommen", run(ADDR_t_likely, 0, 3).r[PSP_V0], 7);
    check("likely nicht genommen", run(ADDR_t_likely, 1, 3).r[PSP_V0], 3);
    check("jal + delay slot", run(ADDR_t_call, 10, 0).r[PSP_V0], 31);

    g_safepoints = 0;
    check("schleife", run(ADDR_t_loop, 100, 0).r[PSP_V0], 5050);
    check("safepoints in schleife", g_safepoints > 10, 1);

    check("jalr", run(ADDR_t_jalr, 10, ADDR_g_double).r[PSP_V0], 31);
    check("jalr auf import-stub", run(ADDR_t_jalr, 10, ADDR_stub).r[PSP_V0], 1016);
    check("jr reg als tail-call", run(ADDR_t_tail_jr, 4, ADDR_g_double).r[PSP_V0], 10);

    check("switch 0", run(ADDR_t_switch, 0, 0).r[PSP_V0], 100);
    check("switch 1", run(ADDR_t_switch, 1, 0).r[PSP_V0], 200);
    check("switch 2", run(ADDR_t_switch, 2, 0).r[PSP_V0], 300);
    check("switch default", run(ADDR_t_switch, 5, 0).r[PSP_V0], 0xFFFFFFFFu);

    check("ziel im delay slot (a)", run(ADDR_t_delay_target, 0, 0).r[PSP_V0], 11);
    check("ziel im delay slot (b)", run(ADDR_t_delay_target, 1, 1).r[PSP_V0], 1);
    check("ziel im delay slot (c)", run(ADDR_t_delay_target, 1, 0).r[PSP_V0], 11);

    c = run(ADDR_t_div, 7, 2);
    check("div lo", c.r[PSP_V0], 3); check("div hi", c.r[PSP_V1], 1);
    c = run(ADDR_t_div, (uint32_t)-7, 2);
    check("div negativ lo", c.r[PSP_V0], (uint32_t)-3); check("div negativ hi", c.r[PSP_V1], (uint32_t)-1);
    c = run(ADDR_t_div, 7, 0);
    check("div durch 0 lo", c.r[PSP_V0], 0xFFFFFFFFu); check("div durch 0 hi", c.r[PSP_V1], 7);
    c = run(ADDR_t_div, (uint32_t)-7, 0);
    check("div durch 0 negativ lo", c.r[PSP_V0], 1);
    c = run(ADDR_t_div, 0x80000000u, 0xFFFFFFFFu);
    check("div INT_MIN/-1 lo", c.r[PSP_V0], 0x80000000u);

    check("ext", run(ADDR_t_ext, 0x12345678u, 0).r[PSP_V0], 0x67);
    check("ins", run(ADDR_t_ins, 0xAu, 0xFFFFFFFFu).r[PSP_V0], 0xFFFFFAFFu);

    g_mem[0x20001] = 0x11; g_mem[0x20002] = 0x22; g_mem[0x20003] = 0x33; g_mem[0x20004] = 0x44;
    check("lwl/lwr unausgerichtet", run(ADDR_t_unaligned_load, 0x20001, 0).r[PSP_V0], 0x44332211u);
    check("lwl/lwr ausgerichtet", run(ADDR_t_unaligned_load, 0x20000, 0).r[PSP_V0], 0x33221100u);
    run(ADDR_t_unaligned_store, 0x20012, 0xAABBCCDDu);
    check("swl/swr unausgerichtet", psp_r32(g_mem, 0x20012), 0xAABBCCDDu);
    check("swl/swr nachbar davor", psp_r16(g_mem, 0x20010), 0);
    check("swl/swr nachbar danach", psp_r16(g_mem, 0x20016), 0);

    check("sltiu kleiner", run(ADDR_t_sltiu, 5, 0).r[PSP_V0], 1);
    check("sltiu gleich", run(ADDR_t_sltiu, 0xFFFFFFFFu, 0).r[PSP_V0], 0);

    check("import-stub per jal", run(ADDR_t_hle, 5, 0).r[PSP_V0], 1005);
    check("j als tail-call", run(ADDR_t_tail_j, 2, 0).r[PSP_V0], 10);
    check("likely rueckwaerts", run(ADDR_t_likely_loop, 5, 0).r[PSP_V0], 4);
    check("ra nach jal", run(ADDR_t_ra, 0, 0).r[PSP_V0], ADDR_t_ra_jal + 8);

    /* ---------------- FPU */
    check("add.s", run(ADDR_t_f_add, F(1.5f), F(2.25f)).r[PSP_V0], F(3.75f));
    check("sub.s", run(ADDR_t_f_sub, F(1.5f), F(2.25f)).r[PSP_V0], F(-0.75f));
    check("mul.s", run(ADDR_t_f_mul, F(1.5f), F(2.0f)).r[PSP_V0], F(3.0f));
    check("mul.s inf*0", run(ADDR_t_f_mul, 0x7F800000u, 0).r[PSP_V0], 0x7FC00000u);
    check("div.s", run(ADDR_t_f_div, F(1.0f), F(4.0f)).r[PSP_V0], F(0.25f));
    check("div.s durch 0", run(ADDR_t_f_div, F(1.0f), 0).r[PSP_V0], 0x7F800000u);
    check("sqrt.s", run(ADDR_t_f_sqrt, F(9.0f), 0).r[PSP_V0], F(3.0f));
    check("sqrt.s negativ", run(ADDR_t_f_sqrt, F(-1.0f), 0).r[PSP_V0], 0x7FC00000u);
    check("abs.s", run(ADDR_t_f_abs, F(-2.0f), 0).r[PSP_V0], F(2.0f));
    check("neg.s", run(ADDR_t_f_neg, F(2.0f), 0).r[PSP_V0], F(-2.0f));
    check("trunc.w.s", run(ADDR_t_f_trunc, F(2.7f), 0).r[PSP_V0], 2);
    check("trunc.w.s negativ", run(ADDR_t_f_trunc, F(-2.7f), 0).r[PSP_V0], (uint32_t)-2);
    check("trunc.w.s zu gross", run(ADDR_t_f_trunc, F(3.0e9f), 0).r[PSP_V0], 0x7FFFFFFFu);
    check("trunc.w.s zu klein", run(ADDR_t_f_trunc, F(-3.0e9f), 0).r[PSP_V0], 0x80000000u);
    check("trunc.w.s NaN", run(ADDR_t_f_trunc, 0x7FC00000u, 0).r[PSP_V0], 0x7FFFFFFFu);
    check("round.w.s 2.5", run(ADDR_t_f_round, F(2.5f), 0).r[PSP_V0], 2);
    check("round.w.s 3.5", run(ADDR_t_f_round, F(3.5f), 0).r[PSP_V0], 4);
    check("round.w.s -2.5", run(ADDR_t_f_round, F(-2.5f), 0).r[PSP_V0], (uint32_t)-2);
    check("ceil.w.s", run(ADDR_t_f_ceil, F(2.1f), 0).r[PSP_V0], 3);
    check("floor.w.s", run(ADDR_t_f_floor, F(-2.1f), 0).r[PSP_V0], (uint32_t)-3);
    check("cvt.w.s", run(ADDR_t_f_cvtw, F(2.5f), 0).r[PSP_V0], 2);
    check("cvt.s.w", run(ADDR_t_f_cvts, 7, 0).r[PSP_V0], F(7.0f));
    check("cvt.s.w negativ", run(ADDR_t_f_cvts, (uint32_t)-3, 0).r[PSP_V0], F(-3.0f));
    check("c.eq.s gleich", run(ADDR_t_f_ceq, F(1.0f), F(1.0f)).r[PSP_V0], 1);
    check("c.eq.s NaN", run(ADDR_t_f_ceq, 0x7FC00000u, 0x7FC00000u).r[PSP_V0], 0);
    check("c.lt.s kleiner", run(ADDR_t_f_clt, F(1.0f), F(2.0f)).r[PSP_V0], 1);
    check("c.lt.s groesser", run(ADDR_t_f_clt, F(2.0f), F(1.0f)).r[PSP_V0], 0);
    check("c.lt.s NaN", run(ADDR_t_f_clt, 0x7FC00000u, F(1.0f)).r[PSP_V0], 0);
    check("c.le.s gleich", run(ADDR_t_f_cle, F(1.0f), F(1.0f)).r[PSP_V0], 1);
    check("c.nge.s kleiner", run(ADDR_t_f_cnge, F(1.0f), F(2.0f)).r[PSP_V0], 1);
    check("c.nge.s NaN", run(ADDR_t_f_cnge, 0x7FC00000u, F(1.0f)).r[PSP_V0], 1);
    check("c.ngt.s groesser", run(ADDR_t_f_cngt, F(2.0f), F(1.0f)).r[PSP_V0], 0);
    check("c.ngt.s NaN", run(ADDR_t_f_cngt, 0x7FC00000u, F(1.0f)).r[PSP_V0], 1);

    /* ---------------- VFPU: Puffer bei 0x30000 */
    {
        const uint32_t B = 0x30000;
        const float va[4] = {1.0f, 2.0f, 3.0f, 4.0f}, vb[4] = {10.0f, -20.0f, 30.0f, 0.5f};
        int i, j, k;
        for (i = 0; i < 4; i++) { psp_wf(g_mem, B + 4 * i, va[i]); psp_wf(g_mem, B + 16 + 4 * i, vb[i]); }
        run(ADDR_t_v_add, B, 0);
        for (i = 0; i < 4; i++) check("vadd.q", psp_r32(g_mem, B + 32 + 4 * i), F(va[i] + vb[i]));
        run(ADDR_t_v_sub_min_max, B, 0);
        for (i = 0; i < 4; i++) {
            check("vsub.q", psp_r32(g_mem, B + 32 + 4 * i), F(va[i] - vb[i]));
            check("vmin.q", psp_r32(g_mem, B + 48 + 4 * i), F(va[i] < vb[i] ? va[i] : vb[i]));
            check("vmax.q", psp_r32(g_mem, B + 64 + 4 * i), F(va[i] > vb[i] ? va[i] : vb[i]));
        }
        run(ADDR_t_v_dot, B, 0);
        check("vdot.q", psp_r32(g_mem, B + 32), F(1.0f * 10.0f + 2.0f * -20.0f + 3.0f * 30.0f + 4.0f * 0.5f));
        run(ADDR_t_v_scl, B, 0);
        for (i = 0; i < 4; i++) check("vscl.q", psp_r32(g_mem, B + 32 + 4 * i), F(va[i] * 10.0f));
        run(ADDR_t_v_pfx_neg, B, 0);
        for (i = 0; i < 4; i++) {
            check("vpfxs negiert", psp_r32(g_mem, B + 32 + 4 * i), F(-va[i]));
            check("praefix gilt nur einmal", psp_r32(g_mem, B + 48 + 4 * i), F(va[i]));
        }
        run(ADDR_t_v_neg_zero_one, B, 0);
        for (i = 0; i < 4; i++) {
            check("vneg.q", psp_r32(g_mem, B + 32 + 4 * i), F(-va[i]));
            check("vzero.q", psp_r32(g_mem, B + 48 + 4 * i), 0);
            check("vone.q", psp_r32(g_mem, B + 64 + 4 * i), F(1.0f));
        }
        psp_w32(g_mem, B, 0x80FF4020u);
        run(ADDR_t_v_color, B, 0);
        check("vc2i.s [0]", psp_r32(g_mem, B + 16), 0x20000000u);
        check("vc2i.s [1]", psp_r32(g_mem, B + 20), 0x40000000u);
        check("vc2i.s [2]", psp_r32(g_mem, B + 24), 0xFF000000u);
        check("vc2i.s [3]", psp_r32(g_mem, B + 28), 0x80000000u);
        check("vi2c.q", psp_r32(g_mem, B + 32), 0x80FF4020u);
        psp_wf(g_mem, B, 1.0f); psp_wf(g_mem, B + 4, 2.0f);
        check("vcmp.s LT wahr", run(ADDR_t_v_cmp, B, 0).r[PSP_V0], 1);
        psp_wf(g_mem, B, 3.0f);
        check("vcmp.s LT falsch", run(ADDR_t_v_cmp, B, 0).r[PSP_V0], 0);
        check("mtv/mfv", run(ADDR_t_v_mtv, 0, 0x12345678u).r[PSP_V0], 0x12345678u);
        {   /* vmmul: Ergebnis(Spalte a, Zeile b) = Summe ueber c von S(Spalte b, Zeile c) * T(Spalte a, Zeile c) */
            float S[4][4], T[4][4];
            for (j = 0; j < 4; j++)
                for (i = 0; i < 4; i++) {
                    S[j][i] = (float)(1 + j * 4 + i);
                    T[j][i] = (float)((j == i ? 2 : 0) + (j == 3 ? i + 1 : 0));
                    psp_wf(g_mem, B + j * 16 + i * 4, S[j][i]);
                    psp_wf(g_mem, B + 64 + j * 16 + i * 4, T[j][i]);
                }
            run(ADDR_t_v_mmul, B, 0);
            for (j = 0; j < 4; j++)
                for (i = 0; i < 4; i++) {
                    float sum = 0.0f;
                    for (k = 0; k < 4; k++) sum += S[i][k] * T[j][k];
                    check("vmmul.q", psp_r32(g_mem, B + 128 + j * 16 + i * 4), F(sum));
                }
        }
    }

    check("hook ersetzt funktion", run(ADDR_t_hooked, 0, 0).r[PSP_V0], 4242);

    printf("%d Pruefungen, %d Fehler\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
