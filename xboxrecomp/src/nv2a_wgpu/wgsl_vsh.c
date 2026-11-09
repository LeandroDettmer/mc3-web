/*
 * wgsl_vsh.c -- the NV2A vertex side in WGSL (WebGPU back end, ADR 0011).
 *
 * Follows nv2a_gl's gl_vsh.c function by function, so a picture that differs
 * between WebGL and WebGPU points at the back end, not at the transform:
 *   gl_prelude            -> s_prelude (helpers, clip, snap, fog equation)
 *   NV2A_FFP_FOG_GLSL     -> nv2a_ffp_fog
 *   nv2a_gl_vsh_fixed     -> wgsl_vsh_fixed
 *   nv2a_gl_vsh_program   -> wgsl_vsh_program (+ NV2A_VSH_MAIN's epilogue)
 * What changes is where the inputs come from. GL took one vertex attribute
 * array per input and uniforms per draw; here every draw's values are its
 * WgDrawRec, D[instance_index] (firstInstance = record index), and the
 * vertices are pulled from the frame arena by nv2a_attr through the draw's
 * WgVFetch descriptor, with the executor's conversions (nv2a_pb_exec.c
 * fetch_attr) for every NV097 attribute type.
 *
 * Clip space differs from GL in two ways. WebGPU's depth range is 0..w, as
 * Vulkan's: z is taken from nv2a_vk's prelude (clamped in front of the eye
 * only). And WebGPU's framebuffer row 0 is at NDC y = +1, so guest row y
 * (top-first, as surfaces are stored) maps to y_ndc = 1 - 2y/H: the GL
 * expression's y is negated. Rows stay top-first in the attachment, so a
 * surface sampled later as a texture needs no flip.
 *
 * The arena loads (ld32/ld16, any alignment) and the correctly rounded
 * divisions (cdiv: reciprocal and one FMA residual step) are nfsu2-web's
 * runtime/web/vsemu_wgsl.inc W_PULL, by th0mk (MIT). The /1023 and /511
 * reciprocals for CMP were checked the same way: every input of the 11- and
 * 10-bit fields gives the C division's float exactly.
 */
#include "wgsl_vsh.h"

#include <stdarg.h>
#include <stdio.h>

static const char s_prelude[] =
    WGSL_COMMON
    /* Little-endian loads from the arena at any byte address (a misaligned
     * u32 combines two words; shorts may sit at odd strides). */
    "fn ld32(a: u32) -> u32 {\n"
    "  let w = a >> 2u;\n"
    "  let s = (a & 3u) << 3u;\n"
    "  if (s == 0u) { return A[w]; }\n"
    "  return (A[w] >> s) | (A[w + 1u] << (32u - s));\n"
    "}\n"
    "fn ld16(a: u32) -> u32 {\n"
    "  let s = (a & 3u) << 3u;\n"
    "  if (s <= 16u) { return (A[a >> 2u] >> s) & 0xFFFFu; }\n"
    "  return ld32(a) & 0xFFFFu;\n"
    "}\n"
    "fn lds(a: u32) -> f32 { return f32(bitcast<i32>(ld16(a) << 16u) >> 16u); }\n"
    "fn lds4(a: u32) -> vec4f { return vec4f(lds(a), lds(a + 2u), lds(a + 4u), lds(a + 6u)); }\n"
    "fn ldf4(a: u32) -> vec4f {\n"
    "  return bitcast<vec4f>(vec4u(ld32(a), ld32(a + 4u), ld32(a + 8u), ld32(a + 12u)));\n"
    "}\n"
    "fn ldub4(a: u32) -> vec4f {\n"
    "  let u = ld32(a);\n"
    "  return vec4f(f32(u & 0xFFu), f32((u >> 8u) & 0xFFu), f32((u >> 16u) & 0xFFu), f32(u >> 24u));\n"
    "}\n"
    /* x / y rounded as the executor's C division: a plain WGSL division is
     * not correctly rounded on every GPU. r = f32(1 / y). */
    "fn cdiv(x: vec4f, y: f32, r: f32) -> vec4f {\n"
    "  let q = x * r;\n"
    "  return fma(fma(-q, vec4f(y), x), vec4f(r), q);\n"
    "}\n"
    "fn n8(x: vec4f) -> vec4f { return cdiv(x, 255.0, 0x1.010102p-8); }\n"
    "fn s16n(x: vec4f) -> vec4f { return cdiv(x, 32767.0, 0x1.0002p-15); }\n"
    /* Attribute a (0..15) of vertex v for draw r: WgVFetch at A[vf_off],
     * {off, NV097 format word} per attribute. size 0: no array, the last
     * SET_VERTEX_DATA value at K[vdef_off + a]. Components past size read
     * 0, 0, 0, 1; a type the executor does not convert reads that too. */
    "fn nv2a_attr(r: u32, a: u32, v: u32) -> vec4f {\n"
    "  let fo = D[r].vf_off + a * 2u;\n"
    "  let fmt = A[fo + 1u];\n"
    "  let sz = (fmt >> 4u) & 0xFu;\n"
    "  if (sz == 0u) { return K[D[r].vdef_off + a]; }\n"
    "  let ad = D[r].vtx_base + A[fo] + v * (fmt >> 8u);\n"
    "  let keep = vec4u(0u, 1u, 2u, 3u) < vec4u(sz);\n"
    "  let dflt = vec4f(0.0, 0.0, 0.0, 1.0);\n"
    "  var o = dflt;\n"
    "  switch (fmt & 0xFu) {\n"
    "    case 0u: { o = n8(ldub4(ad).zyxw); }\n"                          /* UB_D3D: B,G,R,A bytes, any size */
    "    case 1u: { o = select(dflt, max(s16n(lds4(ad)), vec4f(-1.0)), keep); }\n" /* S1 */
    "    case 2u: { o = select(dflt, ldf4(ad), keep); }\n"                /* F */
    "    case 4u: { o = select(dflt, n8(ldub4(ad)), keep); }\n"           /* UB_OGL */
    "    case 5u: { o = select(dflt, lds4(ad), keep); }\n"                /* S32K */
    "    case 6u: {\n"                                                    /* CMP: 11:11:10 signed */
    "      let u = ld32(ad);\n"
    "      let xy = cdiv(vec4f(f32(bitcast<i32>(u << 21u) >> 21u),\n"
    "                          f32(bitcast<i32>(u << 10u) >> 21u), 0.0, 0.0), 1023.0, 0x1.00401p-10);\n"
    "      let z = cdiv(vec4f(f32(bitcast<i32>(u) >> 22u)), 511.0, 0x1.00804p-9);\n"
    "      o = vec4f(xy.x, xy.y, z.x, 1.0);\n"
    "    }\n"
    "    default: { }\n"
    "  }\n"
    "  return o;\n"
    "}\n"
    /* gl_vsh NV2A_SNAP_GLSL_OFF: screen positions to the rasterizer's 1/16
     * pixel, truncated; off = D.surf.w, D.aa.z (u_surf.w, u_snapy). */
    "fn nv2a_snap(sw: vec2f, w: f32, off: vec2f) -> vec2f {\n"
    "  if (w > 0.0) { return (trunc(sw / w * 16.0) / 16.0 - off) * w; }\n"
    "  return sw;\n"
    "}\n"
    /* screen*w (real pixels, rows top-first) and clip w to WebGPU clip space:
     * y negated (row 0 at NDC +1), z in 0..w as nv2a_vk. A widened target
     * (Hor+, D.aa.w = 1/k, wgpu_surf.c RECOMP_ASPECT): x squeezed into the
     * middle 1/k, but a 2D vertex (pre-transformed, or w = 1) within a pixel
     * of the frame's left or right edge, or beyond it, stays where it is.
     * D.aa.w < 0: squeeze every vertex (a clip narrower than the frame, or
     * a pre-transformed draw that does not span the frame: wgpu_draw.c
     * wide_full_2d). */
    "fn nv2a_clip(r: u32, s: vec3f, w: f32) -> vec4f {\n"
    "  let surf = D[r].surf;\n"
    "  let z = s.z * surf.z;\n"
    "  let xy = nv2a_snap(s.xy, w, vec2f(surf.w, D[r].aa.z));\n"
    "  var x = xy.x * surf.x - w;\n"
    "  let ik = D[r].aa.w;\n"
    "  if (ik < 0.0) {\n"
    "    x = x * -ik;\n"
    "  } else if (ik > 0.0 && !((D[r].xform == 0u || w == 1.0) && abs(x) >= (1.0 - surf.x) * abs(w))) {\n"
    "    x = x * ik;\n"
    "  }\n"
    "  return vec4f(x, w - xy.y * surf.y,\n"
    "               select(z, clamp(z, 0.0, w), w > 0.0), w);\n"
    "}\n"
    /* FOG_MODE on a fog distance (gl_vsh nv2a_fog_eq). */
    "fn nv2a_fog_eq(r: u32, dist: f32) -> f32 {\n"
    "  let f0 = D[r].fog0;\n"
    "  let f1 = D[r].fog1;\n"
    "  var d = dist;\n"
    "  if (f0.w != 0.0) { d = abs(d); }\n"
    "  let e = i32(f0.z);\n"
    "  if (e == 0) { return f1.x + d * f1.y - 1.0; }\n"
    "  if (e == 1) { return f1.x + exp2(d * f1.y * 16.0) - 1.5; }\n"
    "  return f1.x + exp2(-d * d * f1.y * f1.y * 32.0) - 1.5;\n"
    "}\n";

/* gl_vsh NV2A_FFP_FOG_GLSL and nv2a_gl_vsh_fixed. xform 1: the composite
 * matrix, the viewport offset, logical -> real pixels; 0 (pre-transformed):
 * the title's screen position divided by its w. Inputs as the GL shader:
 * v0 position, v3/v4 diffuse/specular, v5 fog coordinate, v9..v12 texture
 * coordinates. No lighting, texgen or texture matrix: gl_vsh has none, the
 * title's fixed-function draws (2D, movies) carry their own colours and
 * coordinates. */
static const char s_fixed[] =
    "fn nv2a_ffp_fog(r: u32, v: u32, v0: vec4f, spec: vec4f) -> f32 {\n"
    "  let f0 = D[r].fog0;\n"
    "  if (f0.x == 0.0) { return 1.0; }\n"
    "  let gm = i32(f0.y);\n"
    "  if (gm == 0) { return spec.a; }\n"                                /* SPEC_ALPHA */
    "  let eye = vec3f(dot(D[r].mv[0], v0), dot(D[r].mv[1], v0), dot(D[r].mv[2], v0));\n"
    "  var d: f32;\n"
    "  if (gm == 1) { d = length(eye); }\n"                              /* RADIAL */
    "  else if (gm == 6) { d = nv2a_attr(r, 5u, v).x; }\n"               /* FOG_X */
    "  else { d = dot(D[r].fogplane.xyz, eye) + D[r].fogplane.w; }\n"    /* PLANAR */
    "  if (gm == 3) { d = abs(d); }\n"
    "  return nv2a_fog_eq(r, d);\n"
    "}\n"
    "@vertex fn vs_main(@builtin(vertex_index) v: u32,\n"
    "                   @builtin(instance_index) rec: u32) -> VsOut {\n"
    "  var o: VsOut;\n"
    "  let v0 = nv2a_attr(rec, 0u, v);\n"
    "  if (D[rec].xform == 1u) {\n"
    "    let m = D[rec].m;\n"
    "    let clip = vec4f(dot(m[0], v0), dot(m[1], v0), dot(m[2], v0), dot(m[3], v0));\n"
    "    let w = select(clip.w, 1e-6, abs(clip.w) < 1e-6);\n"
    "    let sw = clip.xyz + D[rec].vpoff.xyz * w;\n"
    "    o.pos = nv2a_clip(rec, vec3f(sw.xy * D[rec].aa.xy, sw.z), w);\n"
    "  } else {\n"
    "    let w = select(1.0, 1.0 / v0.w, v0.w > 0.0);\n"
    "    o.pos = nv2a_clip(rec, v0.xyz * w, w);\n"
    "  }\n"
    "  let spec = nv2a_attr(rec, 4u, v);\n"
    "  o.d0 = saturate(nv2a_attr(rec, 3u, v));\n"
    "  o.d1 = saturate(spec);\n"
    "  o.t0 = nv2a_attr(rec, 9u, v);\n"
    "  o.t1 = nv2a_attr(rec, 10u, v);\n"
    "  o.t2 = nv2a_attr(rec, 11u, v);\n"
    "  o.t3 = nv2a_attr(rec, 12u, v);\n"
    "  o.fog = nv2a_ffp_fog(rec, v, v0, spec);\n"
    "  o.rec = rec;\n"
    "  return o;\n"
    "}\n";

const char *wgsl_vsh_prelude(void)
{
    return s_prelude;
}

int wgsl_vsh_fixed(char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "%s%s", s_prelude, s_fixed);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

/* ---- vertex programs (gl_vsh nv2a_gl_vsh_program + NV2A_VSH_MAIN) ---- */

/* The ILU helpers, with the interpreter's constants (gl_prelude). */
static const char s_ilu[] =
    "fn nv2a_rcp(x: f32) -> f32 {\n"
    "  if (x != 0.0) { return 1.0 / x; }\n"
    "  return 1.884467e+19;\n"
    "}\n"
    "fn nv2a_rcc(x: f32) -> f32 {\n"
    "  let y = nv2a_rcp(x);\n"
    "  let a = clamp(abs(y), 5.42101e-20, 1.884467e+19);\n"
    "  return select(a, -a, y < 0.0);\n"
    "}\n"
    "fn nv2a_exp(x: f32) -> vec4f {\n"
    "  let f = floor(x);\n"
    "  return vec4f(exp2(f), x - f, exp2(x), 1.0);\n"
    "}\n"
    "fn nv2a_log(x: f32) -> vec4f {\n"
    "  let a = abs(x);\n"
    "  if (a == 0.0) { return vec4f(-1.884467e+19, 1.0, -1.884467e+19, 1.0); }\n"
    "  let e = floor(log2(a));\n"
    "  return vec4f(e, a / exp2(e), log2(a), 1.0);\n"
    "}\n"
    "fn nv2a_lit(s: vec4f) -> vec4f {\n"
    "  let lx = max(s.x, 0.0);\n"
    "  let ly = max(s.y, 0.0);\n"
    "  let w = clamp(s.w, -127.996, 127.996);\n"
    "  var p = 0.0;\n"
    "  if (lx > 0.0 && ly > 0.0) { p = exp2(w * log2(ly)); }\n"
    "  return vec4f(1.0, lx, p, 1.0);\n"
    "}\n";

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

static uint32_t f(const uint32_t *t, int dw, int pos, int bits)
{
    return (t[dw] >> pos) & ((1u << bits) - 1u);
}

static const char *const s_swz = "xyzw";

/* Source A (0), B (1) or C (2): mux, register, negate, swizzle (gl_vsh src). */
static void src_fields(const uint32_t *t, int which, uint32_t *mux, uint32_t *reg,
                       uint32_t *neg, uint32_t *sw)
{
    switch (which) {
    case 0:  *mux = f(t, 2, 26, 2); *reg = f(t, 2, 28, 4);
             *neg = f(t, 1, 8, 1);  *sw = f(t, 1, 0, 8);  break;
    case 1:  *mux = f(t, 2, 11, 2); *reg = f(t, 2, 13, 4);
             *neg = f(t, 2, 25, 1); *sw = f(t, 2, 17, 8); break;
    default: *mux = f(t, 3, 28, 2);
             *reg = (f(t, 2, 0, 2) << 2) | f(t, 3, 30, 2);
             *neg = f(t, 2, 10, 1); *sw = f(t, 2, 2, 8);  break;
    }
}

/* Which sources slot t reads and which results it forms (gl_vsh: only what
 * the slot writes, so the GPU compiler gets no dead code). */
typedef struct {
    int a, b, c, m, l;
} Need;

static Need slot_needs(const uint32_t *t)
{
    uint32_t mac = f(t, 1, 21, 4), ilu = f(t, 1, 25, 3);
    uint32_t omask = f(t, 3, 12, 4), rd = f(t, 3, 20, 4);
    int out_l = omask && f(t, 3, 2, 1);
    int mac_op = mac >= 1 && mac <= 12;
    Need n;

    n.m = (mac && mac != 13 && rd < 13) || (omask && !out_l);
    n.l = (ilu && (mac ? 1u : rd) < 13) || out_l;
    n.a = (n.m && mac_op) || mac == 13;
    n.b = n.m && mac_op && mac != 1 && mac != 3;
    n.c = (n.m && (mac == 3 || mac == 4)) || (n.l && ilu);
    return n;
}

/* Source `which` as a WGSL vec4f expression. Constant i is K[kb + i], or
 * the local copy cl[i] when the program writes constants. */
static void src(Out *o, const uint32_t *t, int which, int consts_local)
{
    uint32_t mux, reg, neg, sw;
    char comp[5];

    src_fields(t, which, &mux, &reg, &neg, &sw);
    comp[0] = s_swz[(sw >> 6) & 3];
    comp[1] = s_swz[(sw >> 4) & 3];
    comp[2] = s_swz[(sw >> 2) & 3];
    comp[3] = s_swz[sw & 3];
    comp[4] = 0;

    emit(o, "%s", neg ? "-" : "");
    if (mux == 1 && reg < 12) {
        emit(o, "R%u.%s", reg, comp);
    } else if (mux == 1 && reg == 12) {
        emit(o, "oPos.%s", comp);
    } else if (mux == 2) {
        emit(o, "v%u.%s", f(t, 1, 9, 4), comp);
    } else if (mux == 3) {
        uint32_t ci = f(t, 1, 13, 8);
        if (f(t, 3, 1, 1)) {
            if (consts_local)
                emit(o, "cl[clamp(%u + a0, 0, 191)].%s", ci, comp);
            else
                emit(o, "K[kb + u32(clamp(%u + a0, 0, 191))].%s", ci, comp);
        } else {
            if (consts_local)
                emit(o, "cl[%u].%s", ci < 192 ? ci : 191, comp);
            else
                emit(o, "K[kb + %uu].%s", ci < 192 ? ci : 191, comp);
        }
    } else {
        emit(o, "vec4f(0.0)");
    }
}

/* dst.mask = val.mask: whole, or one component at a time (WGSL assigns no
 * multi-component swizzles). Mask bits 3 = x .. 0 = w. */
static void write_masked(Out *o, const char *dst, uint32_t mask, const char *val)
{
    int i;

    if (mask == 0xF) {
        emit(o, "    %s = %s;\n", dst, val);
        return;
    }
    for (i = 0; i < 4; i++)
        if (mask & (8u >> i))
            emit(o, "    %s.%c = %s.%c;\n", dst, s_swz[i], val, s_swz[i]);
}

static const char *const s_out_names[13] = {
    "oPos", "o1", "o2", "oD0", "oD1", "oFog", "oPts", "oB0", "oB1",
    "oT0", "oT1", "oT2", "oT3",
};

/* A vertex program: s_prelude, the ILU helpers, then vs_main with the body
 * translated from slots start..FINAL as nv2a_gl_vsh_program does (MAC and
 * ILU read every source before either writes; a paired ILU writes R1;
 * a0-relative constants clamped to 0..191; the local copy cl[] when the
 * program writes c[]). Constant i is K[D[rec].k_off + i] -- the executor
 * mirrors SET_VIEWPORT_SCALE/OFFSET into slots 0x3A/0x3B (c[-38]/c[-37]).
 * Inputs v0..v15 come from nv2a_attr, only those read. The end is
 * NV2A_VSH_MAIN's: oPos.xy * D.aa.xy, nv2a_clip, saturated colours, fog
 * factor 1 when D.fog0.x == 0 else nv2a_fog_eq(oFog.x). oB0/oB1 and oPts
 * are kept but not passed on, as in GL. */
int wgsl_vsh_program(const uint32_t (*prog)[4], uint32_t slots, uint32_t start,
                     char *buf, size_t cap)
{
    Out o = { buf, cap, 0 };
    uint32_t pc, inputs = 0;
    int writes_consts = 0, i;

    for (pc = start; pc < slots; pc++) {
        const uint32_t *t = prog[pc];
        Need n = slot_needs(t);
        int s;
        if (f(t, 3, 12, 4) && !f(t, 3, 11, 1))
            writes_consts = 1;
        for (s = 0; s < 3; s++) {
            uint32_t mux, reg, neg, sw;
            if (!(s == 0 ? n.a : s == 1 ? n.b : n.c))
                continue;
            src_fields(t, s, &mux, &reg, &neg, &sw);
            if (mux == 2)
                inputs |= 1u << f(t, 1, 9, 4);
        }
        if (f(t, 3, 0, 1))
            break;
    }

    emit(&o, "%s%s", s_prelude, s_ilu);
    emit(&o, "@vertex fn vs_main(@builtin(vertex_index) v: u32,\n"
             "                   @builtin(instance_index) rec: u32) -> VsOut {\n"
             "  let kb = D[rec].k_off;\n");
    for (i = 0; i < 16; i++)
        if (inputs & (1u << i))
            emit(&o, "  let v%d = nv2a_attr(rec, %du, v);\n", i, i);
    emit(&o, "  var a0: i32 = 0;\n");
    for (i = 0; i < 12; i++)
        emit(&o, "  var R%d = vec4f(0.0);\n", i);
    emit(&o, "  var oPos = vec4f(0.0);\n  var o1 = vec4f(0.0);\n  var o2 = vec4f(0.0);\n"
             "  var oFog = vec4f(0.0);\n  var oPts = vec4f(0.0);\n");
    for (i = 3; i < 13; i++)
        if (i != 5 && i != 6)
            emit(&o, "  var %s = vec4f(0.0, 0.0, 0.0, 1.0);\n", s_out_names[i]);
    if (writes_consts)
        emit(&o, "  var cl: array<vec4f, 192>;\n"
                 "  for (var i = 0u; i < 192u; i++) { cl[i] = K[kb + i]; }\n");

    for (pc = start; pc < slots; pc++) {
        const uint32_t *t = prog[pc];
        uint32_t mac = f(t, 1, 21, 4), ilu = f(t, 1, 25, 3);
        uint32_t omask = f(t, 3, 12, 4);
        uint32_t rd = f(t, 3, 20, 4);
        Need n = slot_needs(t);

        emit(&o, "  { // %u\n", pc);
        if (n.a) { emit(&o, "    let sa = "); src(&o, t, 0, writes_consts); emit(&o, ";\n"); }
        if (n.b) { emit(&o, "    let sb = "); src(&o, t, 1, writes_consts); emit(&o, ";\n"); }
        if (n.c) { emit(&o, "    let sc = "); src(&o, t, 2, writes_consts); emit(&o, ";\n"); }
        if (n.m) {
            switch (mac >= 1 && mac <= 12 ? mac : 0) {
            case 1:  emit(&o, "    let m = sa;\n"); break;
            case 2:  emit(&o, "    let m = sa * sb;\n"); break;
            case 3:  emit(&o, "    let m = sa + sc;\n"); break;
            case 4:  emit(&o, "    let m = sa * sb + sc;\n"); break;
            case 5:  emit(&o, "    let m = vec4f(dot(sa.xyz, sb.xyz));\n"); break;
            case 6:  emit(&o, "    let m = vec4f(dot(sa.xyz, sb.xyz) + sb.w);\n"); break;
            case 7:  emit(&o, "    let m = vec4f(dot(sa, sb));\n"); break;
            case 8:  emit(&o, "    let m = vec4f(1.0, sa.y * sb.y, sa.z, sb.w);\n"); break;
            case 9:  emit(&o, "    let m = min(sa, sb);\n"); break;
            case 10: emit(&o, "    let m = max(sa, sb);\n"); break;
            case 11: emit(&o, "    let m = select(vec4f(0.0), vec4f(1.0), sa < sb);\n"); break;
            case 12: emit(&o, "    let m = select(vec4f(0.0), vec4f(1.0), sa >= sb);\n"); break;
            default: emit(&o, "    let m = vec4f(0.0);\n"); break;
            }
        }
        if (n.l) {
            switch (ilu) {
            case 1:  emit(&o, "    let l = sc;\n"); break;
            case 2:  emit(&o, "    let l = vec4f(nv2a_rcp(sc.x));\n"); break;
            case 3:  emit(&o, "    let l = vec4f(nv2a_rcc(sc.x));\n"); break;
            case 4:  emit(&o, "    let l = vec4f(nv2a_rcp(sqrt(abs(sc.x))));\n"); break;
            case 5:  emit(&o, "    let l = nv2a_exp(sc.x);\n"); break;
            case 6:  emit(&o, "    let l = nv2a_log(sc.x);\n"); break;
            case 7:  emit(&o, "    let l = nv2a_lit(sc);\n"); break;
            default: emit(&o, "    let l = vec4f(0.0);\n"); break;
            }
        }

        if (mac == 13) {
            emit(&o, "    a0 = i32(floor(sa.x + 0.001));\n");
        } else if (mac && rd < 13) {
            char dst[8];
            if (rd == 12) snprintf(dst, sizeof dst, "oPos");
            else          snprintf(dst, sizeof dst, "R%u", rd);
            write_masked(&o, dst, f(t, 3, 24, 4), "m");
        }
        if (ilu) {
            uint32_t ld = mac ? 1 : rd;              /* paired ILU -> R1 */
            if (ld < 13) {
                char dst[8];
                if (ld == 12) snprintf(dst, sizeof dst, "oPos");
                else          snprintf(dst, sizeof dst, "R%u", ld);
                write_masked(&o, dst, f(t, 3, 16, 4), "l");
            }
        }
        if (omask) {
            const char *val = f(t, 3, 2, 1) ? "l" : "m";
            uint32_t addr = f(t, 3, 3, 8);
            if (f(t, 3, 11, 1)) {
                if (addr < 13)
                    write_masked(&o, s_out_names[addr], omask, val);
            } else {
                char dst[48];
                if (f(t, 3, 1, 1))
                    snprintf(dst, sizeof dst, "cl[clamp(%u + a0, 0, 191)]", addr);
                else
                    snprintf(dst, sizeof dst, "cl[%u]", addr < 192 ? addr : 191);
                write_masked(&o, dst, omask, val);
            }
        }
        emit(&o, "  }\n");
        if (f(t, 3, 0, 1))                          /* FINAL */
            break;
    }

    emit(&o, "  var vo: VsOut;\n"
             "  let w = select(oPos.w, 1e-6, abs(oPos.w) < 1e-6);\n"
             "  vo.pos = nv2a_clip(rec, vec3f(oPos.xy * D[rec].aa.xy, oPos.z) * w, w);\n"
             "  vo.d0 = saturate(oD0);\n"
             "  vo.d1 = saturate(oD1);\n"
             "  vo.t0 = oT0;\n  vo.t1 = oT1;\n  vo.t2 = oT2;\n  vo.t3 = oT3;\n"
             "  vo.fog = 1.0;\n"
             "  if (D[rec].fog0.x != 0.0) { vo.fog = nv2a_fog_eq(rec, oFog.x); }\n"
             "  vo.rec = rec;\n"
             "  return vo;\n"
             "}\n");
    if (o.overflow) {
        if (cap)
            buf[0] = 0;
        return -1;
    }
    return (int)(cap - o.left);
}
