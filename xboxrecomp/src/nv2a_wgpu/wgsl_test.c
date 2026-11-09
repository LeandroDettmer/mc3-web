/*
 * wgsl_test.c -- host test for the WebGPU back end's WGSL emitters
 * (docs/webgpu-plan.md M2). Builds natively, with no browser:
 *
 *   cc -std=c11 -I. wgsl_test.c wgsl_psh.c wgsl_vsh.c -o wgsl_test
 *   ./wgsl_test <outdir>        writes <outdir>/<case>.wgsl, one per case
 *
 * then validate every file with naga or tint (tools/wgsl_check.sh). The
 * fragment cases are here; the vertex ones are wgsl_vsh.c's, in
 * wgsl_vsh_test.inc (its header says how it is called).
 */
#include "wgpu_int.h"
#include "wgsl_psh.h"

#include <stdio.h>
#include <string.h>

static char        g_buf[1 << 17];
static const char *g_dir;
static int         g_cases, g_fail;

static void emit_case(const char *name, const char *src, int n)
{
    char path[512];
    FILE *f;

    g_cases++;
    if (n < 0) {
        fprintf(stderr, "FAIL %s: emitter returned %d\n", name, n);
        g_fail++;
        return;
    }
    snprintf(path, sizeof path, "%s/%s.wgsl", g_dir, name);
    f = fopen(path, "wb");
    if (!f || fwrite(src, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "FAIL %s: cannot write %s\n", name, path);
        g_fail++;
    }
    if (f)
        fclose(f);
}

static int vsh_module(const char *name, const char *wgsl)
{
    int f = g_fail;

    emit_case(name, wgsl, (int)strlen(wgsl));
    return g_fail != f;
}

#include "wgsl_vsh_test.inc"

/* ---- fragment cases ----------------------------------------------------- */

/* XDK input byte: register | ALPHA 0x10 | mapping << 5 */
#define IN(reg, alpha, map) ((uint32_t)(reg) | ((alpha) ? 0x10u : 0) | ((uint32_t)(map) << 5))
#define ICW(a, b, c, d)     ((a) << 24 | (b) << 16 | (c) << 8 | (d))
/* OCW: CD dst, AB dst, SUM dst, flags */
#define OCW(cd, ab, sum)    ((uint32_t)(cd) | (uint32_t)(ab) << 4 | (uint32_t)(sum) << 8)
#define MODES(m0, m1, m2, m3) ((uint32_t)(m0) | (uint32_t)(m1) << 5 | (uint32_t)(m2) << 10 | (uint32_t)(m3) << 15)

enum { R_ZERO = 0, R_C0 = 1, R_C1 = 2, R_FOG = 3, R_V0 = 4, R_V1 = 5,
       R_T0 = 8, R_T1 = 9, R_T2 = 10, R_T3 = 11, R_R0 = 12, R_R1 = 13,
       R_V1R0 = 14, R_EF = 15 };

static void psh_case(const char *name, const Nv2aPshKey *k, const uint8_t dim[4])
{
    emit_case(name, g_buf, wgsl_psh(k, dim, g_buf, sizeof g_buf));
}

static void psh_cases(void)
{
    Nv2aPshKey k;
    uint32_t regs[0x2000 / 4];
    uint8_t d2[4] = { WG_TEXDIM_2D, WG_TEXDIM_2D, WG_TEXDIM_2D, WG_TEXDIM_2D };
    uint8_t none[4] = { 0, 0, 0, 0 };
    char small[64];
    uint32_t seed = 12345;
    int i, j;

    /* A movie / 2D quad: R0 = T0 * V0, out = R0 (through the method shadow). */
    memset(regs, 0, sizeof regs);
    regs[0xAC0 / 4] = ICW(IN(R_T0, 0, 0), IN(R_V0, 0, 0), 0, 0);
    regs[0x1E40 / 4] = OCW(0, R_R0, 0);
    regs[0x260 / 4] = ICW(IN(R_T0, 1, 0), IN(R_V0, 1, 0), 0, 0);
    regs[0xAA0 / 4] = OCW(0, R_R0, 0);
    regs[0x1E60 / 4] = 1;
    regs[0x288 / 4] = ICW(0, 0, 0, IN(R_R0, 0, 0));
    regs[0x28C / 4] = IN(R_R0, 1, 0) << 8;
    regs[0x1E70 / 4] = MODES(1, 0, 0, 0);
    wgsl_psh_key(regs, &k);
    if (k.color_icw[0] != regs[0xAC0 / 4] || k.control != 1 || k.shader_program != 1) {
        fprintf(stderr, "FAIL wgsl_psh_key\n");
        g_fail++;
    }
    psh_case("psh_movie", &k, d2);
    psh_case("psh_movie_unbound", &k, none);

    /* D3D fixed-function fog: lerp(FOG.rgb, V1R0, fog), clamped sum. */
    k.final0 = ICW(IN(R_FOG, 1, 0), IN(R_V1R0, 0, 0), IN(R_FOG, 0, 0), 0);
    k.final1 = 0x80 | IN(R_R0, 1, 0) << 8;
    psh_case("psh_fog", &k, d2);

    /* Two stages: dot products, mux (MSB and LSB), scale/bias, blue-to-alpha,
     * unique C0/C1, every mapping, final E*F, inverted inputs. */
    memset(&k, 0, sizeof k);
    k.control = 2 | 1u << 12 | 1u << 16;
    k.color_icw[0] = ICW(IN(R_T0, 0, 2), IN(R_T1, 0, 2), IN(R_C0, 0, 1), IN(R_V0, 1, 3));
    k.color_ocw[0] = OCW(R_R1, R_R0, 0) | 1u << 13 | 1u << 12 | 1u << 18 | 1u << 19 | 3u << 15;
    k.alpha_icw[0] = ICW(IN(R_C1, 1, 4), IN(R_T0, 0, 5), IN(R_ZERO, 0, 1), IN(R_V1, 1, 6));
    k.alpha_ocw[0] = OCW(R_R1, R_R0, R_T2) | 1u << 14 | 6u << 15;
    k.color_icw[1] = ICW(IN(R_R0, 0, 6), IN(R_C0, 0, 7), IN(R_R1, 0, 0), IN(R_C1, 1, 0));
    k.color_ocw[1] = OCW(0, 0, R_R0) | 1u << 14 | 2u << 15;
    k.alpha_icw[1] = ICW(IN(R_ZERO, 0, 0), IN(R_ZERO, 0, 2), IN(R_ZERO, 0, 4), IN(R_ZERO, 0, 5));
    k.alpha_ocw[1] = OCW(0, 0, R_R0) | 4u << 15;
    k.final0 = ICW(IN(R_C0, 0, 1), IN(R_EF, 0, 0), IN(R_V1R0, 1, 0), IN(R_C1, 0, 1));
    k.final1 = (uint32_t)IN(R_T0, 0, 1) << 24 | (uint32_t)IN(R_EF, 0, 0) << 16 | IN(R_R1, 1, 1) << 8 | 0x60;
    k.shader_program = MODES(1, 1, 0, 0);
    psh_case("psh_dot_mux", &k, d2);
    k.control |= 1u << 8;
    psh_case("psh_dot_mux_msb", &k, d2);

    /* Texture modes: dependent AR/GB (stage 2 reading stage 0), cube,
     * pass-through, clip plane, bump (sampled as 2D), project 3D. */
    k.shader_program = MODES(1, 15, 16, 2);
    k.other_input = 0u << 16 | 1u << 20;
    psh_case("psh_dependent", &k, d2);
    {
        uint8_t dc[4] = { WG_TEXDIM_CUBE, WG_TEXDIM_2D, WG_TEXDIM_2D, WG_TEXDIM_2D };
        k.shader_program = MODES(3, 4, 5, 6);
        psh_case("psh_cube_clip", &k, dc);
        k.shader_program = MODES(3, 15, 3, 16);     /* cube read on 2D bindings: black */
        psh_case("psh_cube_on_2d", &k, d2);
        k.shader_program = MODES(15, 5, 5, 5);      /* AR on stage 0: zero */
        psh_case("psh_clip3", &k, none);
        /* A clip plane before later stages' (biased) samples: they must stay
         * in uniform control flow. Cube on stage 3, project 2D on stage 2. */
        {
            uint8_t dk[4] = { WG_TEXDIM_2D, WG_TEXDIM_2D, WG_TEXDIM_2D, WG_TEXDIM_CUBE };
            k.shader_program = MODES(5, 2, 1, 3);
            psh_case("psh_clip_then_sample", &k, dk);
            k.shader_program = MODES(1, 5, 16, 3);  /* GB on stage 2 after a clip */
            k.other_input = 0u << 16;
            psh_case("psh_clip_dependent", &k, dk);
        }
    }

    /* Bump-map and dot-product modes (6-14, 17, 18): plain 2D, as gl_psh;
     * on a cube binding (0, 0, 0, 1). Final E*F reading EF itself: 0. */
    {
        static const uint32_t bump[] = { 6, 7, 8, 9, 10, 11, 12, 13, 14, 17, 18 };
        uint8_t dc[4] = { WG_TEXDIM_2D, WG_TEXDIM_2D, WG_TEXDIM_CUBE, WG_TEXDIM_CUBE };
        char name[32];

        for (i = 0; i < (int)(sizeof bump / sizeof bump[0]); i++) {
            uint32_t m = bump[i];
            k.shader_program = MODES(1, m, m, m);
            snprintf(name, sizeof name, "psh_mode%02u", (unsigned)m);
            psh_case(name, &k, i & 1 ? dc : d2);
        }
        k.final1 = (uint32_t)IN(R_EF, 0, 0) << 24 | (uint32_t)IN(R_EF, 1, 1) << 16 | IN(R_EF, 0, 0) << 8;
        k.final0 = ICW(IN(R_EF, 0, 0), IN(R_V1R0, 0, 0), IN(R_EF, 1, 1), IN(R_EF, 0, 0));
        psh_case("psh_ef_self", &k, d2);
    }

    /* Pseudo-random keys: every register, mapping, destination and mode
     * mix, 8 stages, random binding types. */
    for (i = 0; i < 64; i++) {
        uint32_t *w = (uint32_t *)&k;
        uint8_t dim[4];
        char name[32];

        for (j = 0; j < (int)(sizeof k / 4); j++) {
            seed = seed * 1664525u + 1013904223u;
            w[j] = seed ^ (seed >> 13) * 0x9E3779B9u;
        }
        k.control = (k.control & ~0xFFu) | (uint32_t)(1 + i % 8);
        for (j = 0; j < 4; j++) {
            seed = seed * 1664525u + 1013904223u;
            dim[j] = (uint8_t)((seed >> 24) % 3);
            if (j == 0 && i % 4 == 0)                /* a cube stage with a cube binding */
                k.shader_program = (k.shader_program & ~0x1Fu) | 3u, dim[0] = WG_TEXDIM_CUBE;
        }
        snprintf(name, sizeof name, "psh_fuzz%02d", i);
        psh_case(name, &k, dim);
    }

    /* Overflow reports -1, not a cut module. */
    g_cases++;
    if (wgsl_psh(&k, d2, small, sizeof small) != -1) {
        fprintf(stderr, "FAIL psh overflow: no -1\n");
        g_fail++;
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <outdir>\n", argv[0]);
        return 2;
    }
    g_dir = argv[1];
    psh_cases();
    g_fail += wgsl_vsh_tests(vsh_module);
    printf("wgsl_test: %d cases, %d failed\n", g_cases, g_fail);
    return g_fail != 0;
}
