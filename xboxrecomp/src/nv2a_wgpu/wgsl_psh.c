/*
 * wgsl_psh.c -- NV2A texture shader + register combiners as a WGSL fragment
 * module (WebGPU renderer, ADR 0011, docs/webgpu-plan.md M2).
 *
 * A function-by-function port of nv2a_gl/gl_psh.c: the same Nv2aPshKey, the
 * same register encoding (see gl_psh.c's header), the same arithmetic. What
 * differs is WebGPU's:
 *   - constants come from the arena: K[D[rec].pk_off + WG_PK_*] (nv2a_gl's
 *     u_c0/u_c1/u_fc0/u_fc1/u_fogcolor), tscale, stage_info and the alpha
 *     test (afunc, aref) from D[rec];
 *   - group 1 is typed by tex_dim: binding i texture_2d or texture_cube,
 *     binding 4 + i its sampler; a stage with tex_dim NONE declares nothing
 *     and reads (0, 0, 0, 1), as a stage without WG_SI_BOUND does;
 *   - every texture is sampled first, in uniform control flow, and
 *     discards (clip planes, alpha test) come at the end;
 *   - WGSL has no multi-component swizzle stores and no scalar/vector
 *     overloads of max/clamp: writes rebuild the vec4, limits are typed.
 * Cube maps are sampled (tex_dim CUBE), as nv2a_vk does; GL reads them black.
 * Mip bias: WebGPU samplers have none, so a texture with mips is sampled
 * with textureSampleBias(RECOMP_GL_LOD_BIAS, default -0.25), as nv2a_gl
 * sets GL_TEXTURE_LOD_BIAS on mipmapped textures only. EF_PROD read by E
 * or F is 0 (gl_psh's GLSL does not compile there).
 *
 * shortcut: the bump-map and dot-product modes (6-14, 17, 18) sample as
 * plain 2D, as gl_psh does (2026-11-08, M4 parity check: it implements no
 * more); add them with gl_psh when a title needs them.
 */
#include "wgpu_int.h"
#include "wgsl_psh.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char  *p;
    size_t left;
    int    overflow;
} Out;

static void emit(Out *o, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (o->overflow)
        return;
    va_start(ap, fmt);
    n = vsnprintf(o->p, o->left, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->left) {
        o->overflow = 1;
        return;
    }
    o->p += n;
    o->left -= (size_t)n;
}

void wgsl_psh_key(const uint32_t *r, Nv2aPshKey *k)
{
    int i;

    memset(k, 0, sizeof *k);
    for (i = 0; i < 8; i++) {
        k->color_icw[i] = r[0xAC0 / 4 + i];
        k->color_ocw[i] = r[0x1E40 / 4 + i];
        k->alpha_icw[i] = r[0x260 / 4 + i];
        k->alpha_ocw[i] = r[0xAA0 / 4 + i];
    }
    k->control = r[0x1E60 / 4];
    k->final0 = r[0x288 / 4];
    k->final1 = r[0x28C / 4];
    k->shader_program = r[0x1E70 / 4];
    k->other_input = r[0x1E78 / 4];
}

static const char *reg_name(uint32_t reg, int stage, const Nv2aPshKey *k,
                            char *buf, size_t n)
{
    switch (reg) {
    case 1:
        snprintf(buf, n, "K[pk + %du]", WG_PK_C0 + ((k->control & (1u << 12)) ? stage : 0));
        return buf;
    case 2:
        snprintf(buf, n, "K[pk + %du]", WG_PK_C1 + ((k->control & (1u << 16)) ? stage : 0));
        return buf;
    case 3:  return "FOG";
    case 4:  return "V0";
    case 5:  return "V1";
    case 8:  return "T0";
    case 9:  return "T1";
    case 10: return "T2";
    case 11: return "T3";
    case 12: return "R0";
    case 13: return "R1";
    case 14: return "V1R0";
    case 15: return "EF";
    default: return "ZERO";
    }
}

/* One general-combiner input, mapped. `alpha_portion` selects the
 * float-valued (alpha) path, where the RGB channel bit means blue. */
static void input(Out *o, uint32_t b, int stage, int alpha_portion,
                  const Nv2aPshKey *k)
{
    char nb[16];
    /* V1R0_SUM and EF_PROD exist only in the final combiner. */
    const char *r = (b & 0xF) >= 14 ? "ZERO"
                                    : reg_name(b & 0xF, stage, k, nb, sizeof nb);
    const char *z = alpha_portion ? "0.0" : "vec3f(0.0)";
    char val[40];
    int use_alpha = (b >> 4) & 1;
    uint32_t map = (b >> 5) & 7;

    if (!strcmp(r, "ZERO")) {
        /* The zero register through each mapping, as a literal. */
        static const char *const v[8] = { "0.0", "1.0", "-1.0", "1.0", "-0.5", "0.5", "0.0", "0.0" };
        if (alpha_portion) emit(o, "%s", v[map]);
        else               emit(o, "vec3f(%s)", v[map]);
        return;
    }
    if (alpha_portion)
        snprintf(val, sizeof val, "%s.%s", r, use_alpha ? "a" : "b");
    else if (use_alpha)
        snprintf(val, sizeof val, "vec3f(%s.a)", r);
    else
        snprintf(val, sizeof val, "%s.rgb", r);

    switch (map) {
    case 0: emit(o, "max(%s, %s)", val, z); break;                    /* unsigned */
    case 1: emit(o, "(1.0 - saturate(%s))", val); break;              /* unsigned invert */
    case 2: emit(o, "(2.0 * max(%s, %s) - 1.0)", val, z); break;      /* expand normal */
    case 3: emit(o, "(1.0 - 2.0 * max(%s, %s))", val, z); break;      /* expand negate */
    case 4: emit(o, "(max(%s, %s) - 0.5)", val, z); break;            /* half-bias normal */
    case 5: emit(o, "(0.5 - max(%s, %s))", val, z); break;            /* half-bias negate */
    case 6: emit(o, "(%s)", val); break;                              /* signed identity */
    default: emit(o, "(-(%s))", val); break;                          /* signed negate */
    }
}

static const char *dst_name(uint32_t reg)
{
    switch (reg) {
    case 4:  return "V0";
    case 5:  return "V1";
    case 8:  return "T0";
    case 9:  return "T1";
    case 10: return "T2";
    case 11: return "T3";
    case 12: return "R0";
    case 13: return "R1";
    default: return NULL;           /* 0 discards; others are not writable */
    }
}

static const char *scale_bias(uint32_t op)
{
    switch (op) {
    case 1:  return "(%s - 0.5)";
    case 2:  return "(%s * 2.0)";
    case 3:  return "((%s - 0.5) * 2.0)";
    case 4:  return "(%s * 4.0)";
    case 6:  return "(%s * 0.5)";
    default: return "(%s)";
    }
}

/* name = clamp(scale_bias(name), -1, 1), typed. */
static void limit(Out *o, uint32_t ocw, const char *name, int alpha)
{
    char e[64];

    snprintf(e, sizeof e, scale_bias((ocw >> 15) & 7), name);
    if (alpha) emit(o, "        %s = clamp(%s, -1.0, 1.0);\n", name, e);
    else       emit(o, "        %s = clamp(%s, vec3f(-1.0), vec3f(1.0));\n", name, e);
}

static void stage(Out *o, int i, const Nv2aPshKey *k)
{
    uint32_t cicw = k->color_icw[i], cocw = k->color_ocw[i];
    uint32_t aicw = k->alpha_icw[i], aocw = k->alpha_ocw[i];
    /* Only what this stage's outputs use, in gl_psh's order: products, sum,
     * then scale/bias and clamp, then writes. */
    const char *dAB = dst_name((cocw >> 4) & 0xF), *dCD = dst_name(cocw & 0xF);
    const char *dS = dst_name((cocw >> 8) & 0xF);
    const char *daAB = dst_name((aocw >> 4) & 0xF), *daCD = dst_name(aocw & 0xF);
    const char *daS = dst_name((aocw >> 8) & 0xF);
    int b2a_ab = (cocw & (1u << 19)) && dAB, b2a_cd = (cocw & (1u << 18)) && dCD;
    int nAB = dAB || dS || b2a_ab, nCD = dCD || dS || b2a_cd;
    int naAB = daAB || daS, naCD = daCD || daS;
    const char *mux = (k->control & (1u << 8)) ? "(R0.a >= 0.5)"
                                               : "((i32(R0.a * 255.0 + 0.5) & 1) != 0)";

    emit(o, "    { // stage %d\n", i);
    if (nAB) {
        emit(o, "        let cA = "); input(o, cicw >> 24, i, 0, k); emit(o, ";\n");
        emit(o, "        let cB = "); input(o, cicw >> 16, i, 0, k); emit(o, ";\n");
        emit(o, "        var cAB = %s;\n", (cocw & (1u << 13)) ? "vec3f(dot(cA, cB))" : "cA * cB");
    }
    if (nCD) {
        emit(o, "        let cC = "); input(o, cicw >> 8, i, 0, k); emit(o, ";\n");
        emit(o, "        let cD = "); input(o, cicw, i, 0, k);      emit(o, ";\n");
        emit(o, "        var cCD = %s;\n", (cocw & (1u << 12)) ? "vec3f(dot(cC, cD))" : "cC * cD");
    }
    if (dS) {
        if (cocw & (1u << 14)) emit(o, "        var cS = select(cAB, cCD, %s);\n", mux);
        else                   emit(o, "        var cS = cAB + cCD;\n");
    }
    if (naAB) {
        emit(o, "        let aA = "); input(o, aicw >> 24, i, 1, k); emit(o, ";\n");
        emit(o, "        let aB = "); input(o, aicw >> 16, i, 1, k); emit(o, ";\n");
        emit(o, "        var aAB = aA * aB;\n");
    }
    if (naCD) {
        emit(o, "        let aC = "); input(o, aicw >> 8, i, 1, k); emit(o, ";\n");
        emit(o, "        let aD = "); input(o, aicw, i, 1, k);      emit(o, ";\n");
        emit(o, "        var aCD = aC * aD;\n");
    }
    if (daS) {
        if (aocw & (1u << 14)) emit(o, "        var aS = select(aAB, aCD, %s);\n", mux);
        else                   emit(o, "        var aS = aAB + aCD;\n");
    }

    /* Scale/bias, clamp to the signed range: what is written. */
    if (dAB || b2a_ab) limit(o, cocw, "cAB", 0);
    if (dCD || b2a_cd) limit(o, cocw, "cCD", 0);
    if (dS)            limit(o, cocw, "cS", 0);
    if (daAB)          limit(o, aocw, "aAB", 1);
    if (daCD)          limit(o, aocw, "aCD", 1);
    if (daS)           limit(o, aocw, "aS", 1);

    if (dAB)  emit(o, "        %s = vec4f(cAB, %s.a);\n", dAB, dAB);
    if (dCD)  emit(o, "        %s = vec4f(cCD, %s.a);\n", dCD, dCD);
    if (dS)   emit(o, "        %s = vec4f(cS, %s.a);\n", dS, dS);
    if (daAB) emit(o, "        %s.a = aAB;\n", daAB);
    if (daCD) emit(o, "        %s.a = aCD;\n", daCD);
    if (daS)  emit(o, "        %s.a = aS;\n", daS);
    /* Blue-to-alpha replaces the alpha written to the AB / CD destination. */
    if (b2a_ab) emit(o, "        %s.a = cAB.b;\n", dAB);
    if (b2a_cd) emit(o, "        %s.a = cCD.b;\n", dCD);
    emit(o, "    }\n");
}

/* A final-combiner input: clamped to [0,1], optionally inverted. Its C0 and
 * C1 are its own (SPECULAR_FOG_FACTOR0/1), not a general stage's. EF_PROD
 * read by E or F themselves (no_ef) is zero: it does not exist yet. */
static void final_input(Out *o, uint32_t b, int alpha_only, int no_ef,
                        const Nv2aPshKey *k)
{
    char nb[16];
    const char *r = (b & 0xF) == 1 ? "K[pk + 16u]"            /* WG_PK_FC0 */
                  : (b & 0xF) == 2 ? "K[pk + 17u]"            /* WG_PK_FC1 */
                  : (b & 0xF) == 15 && no_ef ? "ZERO"
                  : reg_name(b & 0xF, 0, k, nb, sizeof nb);
    int use_alpha = (b >> 4) & 1;
    int invert = (b >> 5) & 1;
    char val[40];

    if (alpha_only)
        snprintf(val, sizeof val, "%s.%s", r, use_alpha ? "a" : "b");
    else if (use_alpha)
        snprintf(val, sizeof val, "vec3f(%s.a)", r);
    else
        snprintf(val, sizeof val, "%s.rgb", r);
    if (invert)
        emit(o, "(1.0 - saturate(%s))", val);
    else
        emit(o, "saturate(%s)", val);
}

/* The earlier stage a dependent mode reads (SHADER_OTHER_STAGE_INPUT):
 * stage 1 always reads T0; stage 2 selects with bits [19:16], stage 3 with
 * [23:20]. */
_Static_assert(WG_PK_FC0 == 16 && WG_PK_FC1 == 17, "final_input's literals");

static int input_stage(const Nv2aPshKey *k, int t)
{
    int s = t <= 1 ? 0 : (int)((k->other_input >> (t == 2 ? 16 : 20)) & 0xF);
    return s < t ? s : t - 1;
}

/* RECOMP_GL_LOD_BIAS (nv2a_gl's tex_lod_bias), in WGSL's range. */
static float lod_bias(void)
{
    static int set;
    static float b;

    if (!set) {
        const char *e = getenv("RECOMP_GL_LOD_BIAS");
        b = e ? (float)atof(e) : -0.25f;
        if (!(b >= -16.0f)) b = b != b ? 0.0f : -16.0f;   /* NaN -> 0 */
        if (b > 15.99f) b = 15.99f;
        set = 1;
    }
    return b;
}

/* textureSample of stage t at coords; with a bias, textureSampleBias on
 * textures that have mips (bias 0 on the rest, as nv2a_gl). Called only
 * from fs_main's top, in uniform control flow, as both require. */
static void sample(char *s, size_t n, int t, const char *coords)
{
    float b = lod_bias();

    if (b == 0.0f)
        snprintf(s, n, "textureSample(tex%d, smp%d, %s)", t, t, coords);
    else
        snprintf(s, n, "textureSampleBias(tex%d, smp%d, %s, select(0.0, %.4f, textureNumLevels(tex%d) > 1u))",
                 t, t, coords, b, t);
}

/* T<t> from stage t's mode and its group 1 binding type. A clip plane sets
 * kill: its discard comes at the end. */
static void tex_fetch(Out *o, int t, uint32_t mode, uint32_t dim, const Nv2aPshKey *k)
{
    char smp[192], c[48];
    const char *ts = t == 0 ? "ts[0].xy" : t == 1 ? "ts[0].zw" : t == 2 ? "ts[1].xy" : "ts[1].zw";

    smp[0] = 0;
    switch (mode) {
    case 15:                                         /* DEPENDENT_AR */
    case 16:                                         /* DEPENDENT_GB */
        /* A lookup indexed by an earlier stage's colour (NFSU2's colour
         * grading); the coordinates are already normalised. */
        if (t != 0 && dim == WG_TEXDIM_2D) {
            snprintf(c, sizeof c, "T%d.%s", input_stage(k, t), mode == 15 ? "ar" : "gb");
            sample(smp, sizeof smp, t, c);
        } else if (t == 0) {
            emit(o, "    var T%d = vec4f(0.0);\n", t);
            return;
        }
        break;
    case 0:                                          /* NONE */
        emit(o, "    var T%d = vec4f(0.0);\n", t);
        return;
    case 4:                                          /* PASS_THROUGH */
        emit(o, "    var T%d = saturate(in.t%d);\n", t, t);
        return;
    case 5:                                          /* CLIP_PLANE */
        /* `|`, not `||`: a short circuit is a branch, and later stages'
         * samples must stay in uniform control flow (Tint checks it). */
        emit(o, "    kill = kill | any(in.t%d < vec4f(0.0));\n"
                "    var T%d = vec4f(0.0);\n", t, t);
        return;
    case 3:                                          /* CUBE_MAP */
        /* Car reflections: (s, t, r), not divided by q (as xemu). A 2D
         * binding reads black, as on GL. */
        if (dim == WG_TEXDIM_CUBE) {
            snprintf(c, sizeof c, "in.t%d.xyz", t);
            sample(smp, sizeof smp, t, c);
        } else if (dim == WG_TEXDIM_2D) {
            emit(o, "    var T%d = vec4f(0.0);\n", t);
            return;
        }
        break;
    default:                                         /* 2D (+ the rest, for now) */
        if (dim != WG_TEXDIM_2D)
            break;
        if (mode == 1)                               /* PROJECT2D */
            emit(o, "    let tc%d = select(in.t%d.xy, in.t%d.xy / in.t%d.w, abs(in.t%d.w) > 1e-8);\n",
                 t, t, t, t, t);
        else
            emit(o, "    let tc%d = in.t%d.xy;\n", t, t);
        snprintf(c, sizeof c, "tc%d * %s", t, ts);
        sample(smp, sizeof smp, t, c);
        break;
    }
    if (!smp[0])                                     /* no binding of the right type */
        emit(o, "    var T%d = vec4f(0.0, 0.0, 0.0, 1.0);\n", t);
    else
        emit(o, "    var T%d = select(vec4f(0.0, 0.0, 0.0, 1.0), %s, (si & 0x%xu) != 0u);\n",
             t, smp, WG_SI_BOUND << (8 * t));
}

int wgsl_psh(const Nv2aPshKey *k, const uint8_t tex_dim[4], char *buf, size_t cap)
{
    Out o = { buf, cap, 0 };
    uint32_t count = k->control & 0xFF;
    uint32_t i;

    if (count > 8)
        count = 8;
    emit(&o, "%s", WGSL_COMMON);
    for (i = 0; i < 4; i++) {
        if (tex_dim[i] == WG_TEXDIM_NONE)
            continue;
        emit(&o, "@group(1) @binding(%u) var tex%u: %s;\n"
                 "@group(1) @binding(%u) var smp%u: sampler;\n",
             i, i, tex_dim[i] == WG_TEXDIM_CUBE ? "texture_cube<f32>" : "texture_2d<f32>",
             i + 4, i);
    }
    emit(&o,
        "@fragment\n"
        "fn fs_main(in: VsOut, @builtin(front_facing) ff: bool) -> @location(0) vec4f {\n"
        "    let pk = D[in.rec].pk_off;\n"
        "    let si = D[in.rec].stage_info;\n"
        "    let ts = D[in.rec].tscale;\n"
        "    var kill = false;\n");
    /* Every sample before any branch: textureSample needs uniform control flow. */
    for (i = 0; i < 4; i++)
        tex_fetch(&o, (int)i, (k->shader_program >> (5 * i)) & 0x1F, tex_dim[i], k);
    emit(&o,
        "    let ZERO = vec4f(0.0);\n"
        "    var V0 = in.d0;\n"
        "    var V1 = in.d1;\n"
        "    var R0 = vec4f(0.0, 0.0, 0.0, T0.a);\n"
        "    var R1 = vec4f(0.0);\n"
        "    let FOG = vec4f(K[pk + %du].rgb, saturate(in.fog));\n", WG_PK_FOG);
    for (i = 0; i < count; i++)
        stage(&o, (int)i, k);

    /* Final combiner. */
    {
        uint32_t cw0 = k->final0, cw1 = k->final1;
        emit(&o, "    var V1R0 = vec4f(%s + %s, 0.0);\n",
             (cw1 & 0x40) ? "(1.0 - saturate(V1.rgb))" : "saturate(V1.rgb)",
             (cw1 & 0x20) ? "(1.0 - saturate(R0.rgb))" : "saturate(R0.rgb)");
        if (cw1 & 0x80)
            emit(&o, "    V1R0 = saturate(V1R0);\n");
        emit(&o, "    let EF = vec4f(");
        final_input(&o, cw1 >> 24, 0, 1, k);
        emit(&o, " * ");
        final_input(&o, cw1 >> 16, 0, 1, k);
        emit(&o, ", 0.0);\n");
        emit(&o, "    let fA = "); final_input(&o, cw0 >> 24, 0, 0, k); emit(&o, ";\n");
        emit(&o, "    let fB = "); final_input(&o, cw0 >> 16, 0, 0, k); emit(&o, ";\n");
        emit(&o, "    let fC = "); final_input(&o, cw0 >> 8, 0, 0, k);  emit(&o, ";\n");
        emit(&o, "    let fD = "); final_input(&o, cw0, 0, 0, k);       emit(&o, ";\n");
        emit(&o, "    let fG = "); final_input(&o, cw1 >> 8, 1, 0, k);  emit(&o, ";\n");
        emit(&o, "    var c = vec4f(fA * fB + (1.0 - fA) * fC + fD, fG);\n");
    }
    {
        /* RECOMP_GL_PROBE=1 (as nv2a_gl): every draw writes plain green. */
        static int probe = -1;
        if (probe < 0) {
            const char *e = getenv("RECOMP_GL_PROBE");
            probe = e && *e == '1';
        }
        if (probe)
            emit(&o, "    c = vec4f(0.0, 1.0, 0.0, 1.0);\n");
    }
    emit(&o,
        "    c = saturate(c);\n"
        "    let ar = c.a * 255.0;\n"
        "    let af = D[in.rec].afunc;\n"
        "    let aref = D[in.rec].aref;\n"
        "    var ok = true;\n"
        "    if (af == 0x200u) { ok = false; }\n"
        "    else if (af == 0x201u) { ok = ar <  aref; }\n"
        "    else if (af == 0x202u) { ok = ar == aref; }\n"
        "    else if (af == 0x203u) { ok = ar <= aref; }\n"
        "    else if (af == 0x204u) { ok = ar >  aref; }\n"
        "    else if (af == 0x205u) { ok = ar != aref; }\n"
        "    else if (af == 0x206u) { ok = ar >= aref; }\n"
        "    if (kill || !ok) { discard; }\n"
        "    return c;\n"
        "}\n");
    return o.overflow ? -1 : (int)(cap - o.left);
}
