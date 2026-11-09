/*
 * wgsl_vsh_test.inc -- wgsl_vsh.c's cases for the host test wgsl_test.c,
 * which includes this file (after wgpu_int.h, <stdio.h>, <string.h>) and
 * links wgsl_vsh.c.
 *
 *   int wgsl_vsh_tests(int (*module)(const char *name, const char *wgsl))
 *
 * runs the emitter checks and hands every vertex module it makes to
 * `module` (wgsl_test.c writes it out for tools/wgsl_check.sh, naga or
 * tint); `module` returns nonzero for a module it rejects. Returns the
 * number of failures, each printed as "wgsl_vsh: FAIL ...".
 */
#include "wgsl_vsh.h"

static int wvt_fail(const char *what)
{
    printf("wgsl_vsh: FAIL %s\n", what);
    return 1;
}

/* Braces and parentheses close in order: a cheap text check that catches a
 * broken string join before any WGSL tool sees it. */
static int wvt_balanced(const char *s)
{
    char st[64];
    int n = 0;
    for (; *s; s++) {
        if (*s == '{' || *s == '(' || *s == '[') {
            if (n == (int)sizeof st)
                return 0;
            st[n++] = *s;
        } else if (*s == '}' || *s == ')' || *s == ']') {
            char o = *s == '}' ? '{' : *s == ')' ? '(' : '[';
            if (!n || st[--n] != o)
                return 0;
        }
    }
    return n == 0;
}

/* One microcode slot from its fields, in the 128-bit layout gl_vsh.c and
 * the executor's vp_run decode (xemu vsh.c). A source is mux << 4 | reg
 * (WV_R/WV_V/WV_C), WV_NEG negates, WV_SW(s) gives an 8-bit swizzle (x in
 * bits 7-6 .. w in bits 1-0; xyzw otherwise). out: the address, WV_OUT for
 * an output register (else c[]), WV_REL a0-relative constants (reads and
 * the c[] write), WV_ILU writes the ILU result (else the MAC's). */
#define WV_R(n)   (0x10u | (uint32_t)(n))
#define WV_V      0x20u
#define WV_C      0x30u
#define WV_NEG    0x100u
#define WV_SW(s)  (0x200u | (uint32_t)(s) << 10)
#define WV_OUT    0x100u
#define WV_REL    0x200u
#define WV_ILU    0x400u
enum { WV_NOP, WV_MOV, WV_MUL, WV_ADD, WV_MAD, WV_DP3, WV_DPH, WV_DP4, WV_DST,
       WV_MIN, WV_MAX, WV_SLT, WV_SGE, WV_ARL };
enum { WV_I_NOP, WV_I_MOV, WV_I_RCP, WV_I_RCC, WV_I_RSQ, WV_I_EXP, WV_I_LOG, WV_I_LIT };
enum { WV_OPOS, WV_OD0 = 3, WV_OD1, WV_OFOG, WV_OPTS, WV_OB0, WV_OB1, WV_OT0 };

static uint32_t wvt_sw(uint32_t s) { return s & 0x200u ? (s >> 10) & 0xFFu : 0x1Bu; }

static void wvt_slot(uint32_t *t, uint32_t mac, uint32_t ilu,
                     uint32_t a, uint32_t b, uint32_t c, uint32_t vin, uint32_t ci,
                     uint32_t rd, uint32_t mmask, uint32_t imask,
                     uint32_t out, uint32_t omask, int final)
{
    uint32_t creg = c & 0xFu;
    t[0] = 0;
    t[1] = wvt_sw(a) | (a >> 8 & 1u) << 8 | vin << 9 | ci << 13 | mac << 21 | ilu << 25;
    t[2] = (creg >> 2) | wvt_sw(c) << 2 | (c >> 8 & 1u) << 10
         | (b >> 4 & 3u) << 11 | (b & 0xFu) << 13 | wvt_sw(b) << 17 | (b >> 8 & 1u) << 25
         | (a >> 4 & 3u) << 26 | (a & 0xFu) << 28;
    t[3] = (uint32_t)final | (out >> 9 & 1u) << 1 | (out >> 10 & 1u) << 2
         | (out & 0xFFu) << 3 | (out >> 8 & 1u) << 11 | omask << 12 | imask << 16
         | rd << 20 | mmask << 24 | (c >> 4 & 3u) << 28 | (creg & 3u) << 30;
}

/* Emit, check the shape every program module has, hand it to `module`. */
static int wvt_prog(int (*module)(const char *, const char *), const char *pre,
                    const char *name, const uint32_t (*p)[4], uint32_t slots,
                    uint32_t start, char *buf, size_t cap)
{
    char what[96];
    int n = wgsl_vsh_program(p, slots, start, buf, cap);

    if (n <= 0 || (size_t)n != strlen(buf)) {
        snprintf(what, sizeof what, "%s: no module", name);
        return wvt_fail(what);
    }
    if (strncmp(buf, pre, strlen(pre)) != 0 || !strstr(buf, "@vertex fn vs_main(")
        || !strstr(buf, "vo.rec = rec;") || !strstr(buf, "nv2a_clip(rec, ")
        || !wvt_balanced(buf)) {
        snprintf(what, sizeof what, "%s: module shape", name);
        return wvt_fail(what);
    }
    if (module && module(name, buf)) {
        snprintf(what, sizeof what, "%s: rejected by the WGSL check", name);
        return wvt_fail(what);
    }
    return 0;
}

static int wvt_programs(int (*module)(const char *, const char *), const char *pre)
{
    static char buf[1 << 17], small[1 << 17];
    static uint32_t p[136][4];
    uint32_t seed = 0x2468ACEu, len, j;
    int fails = 0, n, i;
    const uint32_t (*cp)[4] = (const uint32_t (*)[4])p;

    /* The XDK's transform program: DP4 oPos by c[0..3] (raw 96..99), the
     * viewport by c[-38]/c[-37] (raw 0x3A/0x3B, the executor's mirrors),
     * RCC of w, colour and texture coordinates through. */
    memset(p, 0, sizeof p);
    for (j = 0; j < 4; j++)
        wvt_slot(p[j], WV_DP4, 0, WV_V, WV_C, 0, 0, 96 + j, 0, 0, 0,
                 WV_OUT | WV_OPOS, 8u >> j, 0);
    wvt_slot(p[4], WV_MUL, 0, WV_R(12), WV_C, 0, 0, 0x3A, 12, 0xE, 0, 0, 0, 0);
    wvt_slot(p[5], 0, WV_I_RCC, 0, 0, WV_R(12) | WV_SW(0xFF), 0, 0, 1, 0, 8, 0, 0, 0);
    wvt_slot(p[6], WV_MAD, 0, WV_R(12), WV_R(1) | WV_SW(0x00), WV_C, 0, 0x3B, 12, 0xE, 0, 0, 0, 0);
    wvt_slot(p[7], WV_MOV, 0, WV_V, 0, 0, 3, 0, 0, 0, 0, WV_OUT | WV_OD0, 0xF, 0);
    wvt_slot(p[8], WV_MOV, 0, WV_V, 0, 0, 9, 0, 0, 0, 0, WV_OUT | WV_OT0, 0xC, 1);
    fails += wvt_prog(module, pre, "vsh_prog_xform", cp, 136, 0, buf, sizeof buf);
    if (!strstr(buf, "K[kb + 58u]") || !strstr(buf, "K[kb + 59u]") || !strstr(buf, "K[kb + 96u]"))
        fails += wvt_fail("prog_xform: constant reads");
    if (strstr(buf, "cl[") || !strstr(buf, "let v0 = nv2a_attr(rec, 0u, v);")
        || !strstr(buf, "let v9 = nv2a_attr(rec, 9u, v);") || strstr(buf, "let v1 ="))
        fails += wvt_fail("prog_xform: inputs / no local constants");
    if (!strstr(buf, "oPos.y = m.y;") || !strstr(buf, "R1.x = l.x;")
        || !strstr(buf, "oD0 = m;") || !strstr(buf, "let sb = R1.xxxx;"))
        fails += wvt_fail("prog_xform: masked writes / swizzle");
    /* Overflow: no room for the NUL is -1 (and an empty buffer); one more fits. */
    n = (int)strlen(buf);
    if (wgsl_vsh_program(cp, 136, 0, small, (size_t)n) != -1 || small[0])
        fails += wvt_fail("prog_xform: overflow not reported");
    if (wgsl_vsh_program(cp, 136, 0, small, (size_t)n + 1) != n || strcmp(small, buf) != 0)
        fails += wvt_fail("prog_xform: exact fit");

    /* Skinning-style: ARL from a weight index, a0-relative matrix rows,
     * MAD paired with RSQ (ILU to R1), every remaining MAC and ILU op,
     * negated sources, fog and point size, a relative c[] write (local
     * copy), back colours. Starts at slot 2 (slots 0-1 another program's). */
    memset(p, 0, sizeof p);
    wvt_slot(p[0], WV_MOV, 0, WV_V, 0, 0, 7, 0, 0, 0, 0, WV_OUT | WV_OD1, 0xF, 1);
    wvt_slot(p[1], WV_MOV, 0, WV_V, 0, 0, 7, 0, 0, 0, 0, WV_OUT | WV_OD1, 0xF, 1);
    wvt_slot(p[2], WV_ARL, 0, WV_V | WV_SW(0x55), 0, 0, 1, 0, 0, 0, 0, 0, 0, 0);
    wvt_slot(p[3], WV_DP4, 0, WV_V, WV_C, 0, 0, 100, 0, 8, 0, WV_REL, 0, 0);
    wvt_slot(p[4], WV_DPH, 0, WV_V, WV_C, 0, 0, 101, 0, 4, 0, WV_REL, 0, 0);
    wvt_slot(p[5], WV_MAD, WV_I_RSQ, WV_R(0), WV_C | WV_NEG, WV_V | WV_SW(0xAA), 2, 102,
             2, 0xF, 0x8, WV_REL, 0, 0);
    wvt_slot(p[6], WV_DP3, WV_I_LIT, WV_R(2), WV_R(1), WV_R(0), 0, 0, 3, 0x7, 0xF,
             WV_OUT | WV_OD0 | WV_ILU, 0xF, 0);
    wvt_slot(p[7], WV_DST, WV_I_EXP, WV_R(3), WV_R(2), WV_R(3) | WV_SW(0xFF), 0, 0, 4, 0xF, 0x6, 0, 0, 0);
    wvt_slot(p[8], WV_MIN, WV_I_LOG, WV_R(4), WV_R(1) | WV_NEG, WV_R(4), 0, 0, 5, 0xF, 0x3, 0, 0, 0);
    wvt_slot(p[9], WV_SLT, WV_I_RCP, WV_R(5), WV_R(4), WV_R(5), 0, 0, 6, 0xF, 0x1, 0, 0, 0);
    wvt_slot(p[10], WV_SGE, WV_I_MOV, WV_R(6), WV_R(5), WV_R(11), 0, 0, 7, 0x9, 0x4, 0, 0, 0);
    wvt_slot(p[11], WV_MAX, 0, WV_R(7), WV_R(6), 0, 0, 0, 8, 0xF, 0, WV_OUT | WV_OFOG, 0x8, 0);
    wvt_slot(p[12], WV_ADD, 0, WV_R(8), 0, WV_C, 0, 120, 9, 0xF, 0, WV_REL | 104, 0xF, 0);
    wvt_slot(p[13], WV_MUL, 0, WV_C, WV_V, 0, 15, 104, 0, 0, 0, WV_OUT | WV_OPTS, 0x8, 0);
    wvt_slot(p[14], WV_MOV, 0, WV_R(9), 0, 0, 0, 0, 0, 0, 0, WV_OUT | WV_OB0, 0xF, 0);
    wvt_slot(p[15], WV_MOV, 0, WV_R(0), 0, 0, 0, 0, 0, 0, 0, WV_OUT | WV_OPOS, 0xF, 1);
    wvt_slot(p[16], WV_MOV, 0, WV_V, 0, 0, 13, 0, 0, 0, 0, WV_OUT | WV_OT0 + 3, 0xF, 1);
    fails += wvt_prog(module, pre, "vsh_prog_skin", cp, 136, 2, buf, sizeof buf);
    if (!strstr(buf, "a0 = i32(floor(sa.x + 0.001));") || !strstr(buf, "cl[clamp(100 + a0, 0, 191)]")
        || !strstr(buf, "var cl: array<vec4f, 192>;") || !strstr(buf, "cl[clamp(104 + a0, 0, 191)] = m;"))
        fails += wvt_fail("prog_skin: a0 / local constants");
    if (!strstr(buf, "let l = vec4f(nv2a_rcp(sqrt(abs(sc.x))));") || !strstr(buf, "R1.x = l.x;")
        || !strstr(buf, "let l = nv2a_lit(sc);") || !strstr(buf, "oD0 = l;")
        || !strstr(buf, "let sb = -cl[clamp(102 + a0, 0, 191)].xyzw;") || !strstr(buf, "oFog.x = m.x;"))
        fails += wvt_fail("prog_skin: paired ILU / negation / outputs");
    if (strstr(buf, "let v7 =") || strstr(buf, "let v13 =") || !strstr(buf, "let v15 ="))
        fails += wvt_fail("prog_skin: start slot / FINAL");

    /* Random microcode (gl_es_test's fuzz): every field mix the decoder can
     * meet, valid WGSL whatever it means. */
    for (i = 0; i < 48; i++) {
        char name[32];
        memset(p, 0, sizeof p);
        seed = seed * 1664525u + 1013904223u;
        len = 1 + (seed >> 8) % 24;
        for (j = 0; j < len; j++) {
            int k;
            for (k = 0; k < 4; k++) {
                seed = seed * 1664525u + 1013904223u;
                p[j][k] = seed ^ (seed >> 13) * 0x9E3779B9u;
            }
            p[j][3] &= ~1u;
        }
        p[len - 1][3] |= 1;
        snprintf(name, sizeof name, "vsh_prog_fuzz%02d", i);
        fails += wvt_prog(module, pre, name, cp, 136, 0, buf, sizeof buf);
    }

    /* A whole 136-slot program without FINAL ends at the last slot. */
    for (j = 0; j < 136; j++)
        wvt_slot(p[j], WV_MAD, WV_I_RCP, WV_R(j % 12), WV_C, WV_V, j % 16, j % 192,
                 (j + 1) % 12, 0xF, 0x8, WV_OUT | WV_OT0 + j % 4, 0xF, 0);
    fails += wvt_prog(module, pre, "vsh_prog_full", cp, 136, 0, buf, sizeof buf);
    return fails;
}

static int wgsl_vsh_tests(int (*module)(const char *name, const char *wgsl))
{
    static char buf[32768];
    const char *pre = wgsl_vsh_prelude();
    int fails = 0, n, k;

    /* The fixed/pre-transformed module: whole, the shared prelude first,
     * one entry point, the record index passed on to the fragment stage. */
    n = wgsl_vsh_fixed(buf, sizeof buf);
    if (n <= 0 || (size_t)n != strlen(buf))
        return fails + wvt_fail("fixed: no module");
    if (strncmp(buf, pre, strlen(pre)) != 0)
        fails += wvt_fail("fixed: does not start with the prelude");
    if (!strstr(buf, "@vertex fn vs_main(") || strstr(buf, "@fragment"))
        fails += wvt_fail("fixed: entry points");
    if (!strstr(buf, "o.rec = rec;"))
        fails += wvt_fail("fixed: VsOut.rec not set");
    if (!strstr(buf, "struct WgDrawRec {") || !strstr(buf, "var<storage, read> D: array<WgDrawRec>"))
        fails += wvt_fail("fixed: WGSL_COMMON missing");
    if (!wvt_balanced(buf))
        fails += wvt_fail("fixed: unbalanced brackets");
    /* Every NV097 type the executor converts has its case. */
    for (k = 0; k <= 6; k++) {
        char c[16];
        snprintf(c, sizeof c, "case %du:", k);
        if (k != 3 && !strstr(buf, c)) {
            fails += wvt_fail("fixed: attribute type case missing");
            break;
        }
    }
    if (module && module("vsh_fixed", buf))
        fails += wvt_fail("fixed: rejected by the WGSL check");

    /* Overflow: exactly no room for the NUL is -1; one more byte fits. */
    {
        static char small[32768];
        if (wgsl_vsh_fixed(small, (size_t)n) != -1)
            fails += wvt_fail("fixed: overflow not reported");
        if (wgsl_vsh_fixed(small, (size_t)n + 1) != n || strcmp(small, buf) != 0)
            fails += wvt_fail("fixed: exact fit");
    }

    fails += wvt_programs(module, pre);

    if (!fails)
        printf("wgsl_vsh: ok (fixed module %d bytes, programs)\n", n);
    return fails;
}
