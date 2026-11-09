/*
 * wgpu_draw.c -- one NV2A draw on the WebGPU back end (render pthread; ADR
 * 0011, docs/webgpu-plan.md, wgpu_int.h).
 *
 * As nv2a_gl's gl_draw_raw, but nothing is drawn here: the draw becomes
 * arena data the shaders read -- its vertices as the title stored them, the
 * vertex-fetch descriptor (WgVFetch), u32 list indices, constants and one
 * WgDrawRec (the GL uniforms) -- and command records: the pass state that
 * changed, SET_PIPE, SET_TEX and a DRAW whose firstInstance is the record.
 *
 * The frame arena is one wasm buffer handed to web/wgpu.js with the frame
 * (wg_arena_take); when it fills, the part so far is submitted
 * (wg_flush_partial) and the draw is laid out again in the empty arena.
 */
#include "wgpu_int.h"
#include "../nv2a_gl/gl_vsh.h"          /* nv2a_ffp_fog_uniforms */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Frame arena ──────────────────────────────────────────────────── */

static uint8_t *s_arena;
static uint32_t s_arena_len, s_arena_cap;
static uint32_t s_arena_gen = 1;        /* bumped at every take: offsets cached before are stale */

static int arena_init(void)
{
    const char *e = getenv("RECOMP_WGPU_ARENA_MB");
    uint64_t cap = (uint64_t)(e && atoi(e) > 0 ? atoi(e) : 32) << 20;
    void *p = NULL;

    if (wg_ctl.max_storage_binding && cap > wg_ctl.max_storage_binding)
        cap = wg_ctl.max_storage_binding;       /* group 0 binds the arena whole */
    cap &= ~(uint64_t)15;
    if (posix_memalign(&p, 16, (size_t)cap) != 0 || !p) {
        fprintf(stderr, "  [WGPU] frame arena of %llu MB: out of memory\n",
                (unsigned long long)(cap >> 20));
        return 0;
    }
    s_arena = (uint8_t *)p;
    s_arena_cap = (uint32_t)cap;
    fprintf(stderr, "  [WGPU] frame arena %u MB (RECOMP_WGPU_ARENA_MB)\n", s_arena_cap >> 20);
    return 1;
}

uint32_t wg_arena_alloc(uint32_t bytes, uint32_t align, void **out)
{
    uint64_t at;
    if (!s_arena && !arena_init())
        return UINT32_MAX;
    at = ((uint64_t)s_arena_len + align - 1) / align * align;
    if (at + bytes > s_arena_cap)
        return UINT32_MAX;
    s_arena_len = (uint32_t)(at + bytes);
    memset(s_arena + at, 0, bytes);
    *out = s_arena + at;
    return (uint32_t)at;
}

void wg_arena_take(uint32_t *ptr, uint32_t *bytes)
{
    *ptr = s_arena_len ? (uint32_t)(uintptr_t)s_arena : 0;
    *bytes = (s_arena_len + 15u) & ~15u;
    s_arena_len = 0;
    s_arena_gen++;
}

/* ── Helpers ──────────────────────────────────────────────────────── */

static float byte_f(uint32_t v, int shift) { return (float)((v >> shift) & 0xFF) / 255.0f; }

static void argb_vec4(uint32_t v, float out[4])
{
    out[0] = byte_f(v, 16); out[1] = byte_f(v, 8);
    out[2] = byte_f(v, 0);  out[3] = byte_f(v, 24);
}

static void once(int *told, const char *what)
{
    if (!*told) {
        *told = 1;
        fprintf(stderr, "  [WGPU] %s (logged once)\n", what);
    }
}

/* NV097 primitive -> a WebGPU list: topology, and the u32 indices in *out
 * (scratch, valid until the next call). Strips keep their winding (every
 * other triangle's first two swapped), fans and polygons fan from vertex 0,
 * quads split along 0-2, line loops close. Returns the index count. */
static uint32_t prim_list(const Nv2aRawBatch *b, uint8_t *topo, const uint32_t **out)
{
    static uint32_t *buf;
    static size_t cap;
    const uint32_t *ix = b->indices;
    uint32_t n = b->index_count, m = 0, t;
    size_t need;

    *out = ix;
    switch (b->prim) {
    case 1:  *topo = WG_TOPO_POINT_LIST; return n;
    case 2:  *topo = WG_TOPO_LINE_LIST; return n & ~1u;
    case 5:  *topo = WG_TOPO_TRIANGLE_LIST; return n - n % 3;
    case 3: case 4:                     /* line loop, line strip */
        *topo = WG_TOPO_LINE_LIST;
        need = (size_t)n * 2;
        break;
    case 6: case 7: case 9: case 10:    /* strip, fan, quad strip, polygon */
    case 8:                             /* quads */
        *topo = WG_TOPO_TRIANGLE_LIST;
        need = b->prim == 8 ? (size_t)n / 4 * 6 : n >= 3 ? (size_t)(n - 2) * 3 : 0;
        break;
    default:
        return 0;
    }
    if (need > cap) {
        free(buf);
        cap = need + 4096;
        buf = (uint32_t *)malloc(cap * 4);
        if (!buf) { cap = 0; return 0; }
    }
    switch (b->prim) {
    case 3: case 4:
        for (t = 0; t + 1 < n; t++) { buf[m++] = ix[t]; buf[m++] = ix[t + 1]; }
        if (b->prim == 3 && n > 2) { buf[m++] = ix[n - 1]; buf[m++] = ix[0]; }
        break;
    case 6: case 9:                     /* a quad strip draws as a triangle strip */
        for (t = 0; t + 2 < n; t++) {
            buf[m++] = ix[t + (t & 1)];
            buf[m++] = ix[t + 1 - (t & 1)];
            buf[m++] = ix[t + 2];
        }
        break;
    case 7: case 10:
        for (t = 0; t + 2 < n; t++) { buf[m++] = ix[0]; buf[m++] = ix[t + 1]; buf[m++] = ix[t + 2]; }
        break;
    case 8:
        for (t = 0; t + 3 < n; t += 4) {
            buf[m++] = ix[t];     buf[m++] = ix[t + 1]; buf[m++] = ix[t + 2];
            buf[m++] = ix[t];     buf[m++] = ix[t + 2]; buf[m++] = ix[t + 3];
        }
        break;
    }
    *out = buf;
    return m;
}

/* GL blend factor (NV2A's values) -> WG_BF_*. *cuse gets bit 0 when the
 * factor reads the constant's colour, bit 1 its alpha: WebGPU has no
 * CONSTANT_ALPHA, so the constant's rgb becomes its alpha (wgpu_proto.h),
 * which a draw reading both cannot have. */
static uint8_t blend_factor(uint32_t v, int *cuse)
{
    switch (v) {
    case 0x0000: return WG_BF_ZERO;
    case 0x0001: return WG_BF_ONE;
    case 0x0300: return WG_BF_SRC;
    case 0x0301: return WG_BF_ONE_MINUS_SRC;
    case 0x0302: return WG_BF_SRC_ALPHA;
    case 0x0303: return WG_BF_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return WG_BF_DST_ALPHA;
    case 0x0305: return WG_BF_ONE_MINUS_DST_ALPHA;
    case 0x0306: return WG_BF_DST;
    case 0x0307: return WG_BF_ONE_MINUS_DST;
    case 0x0308: return WG_BF_SRC_ALPHA_SATURATED;
    case 0x8001: *cuse |= 1; return WG_BF_CONSTANT;
    case 0x8002: *cuse |= 1; return WG_BF_ONE_MINUS_CONSTANT;
    case 0x8003: *cuse |= 2; return WG_BF_CONSTANT;
    case 0x8004: *cuse |= 2; return WG_BF_ONE_MINUS_CONSTANT;
    default:     return WG_BF_ONE;
    }
}

static uint8_t blend_op(uint32_t v)
{
    switch (v) {
    case 0x800A: return WG_BO_SUBTRACT;
    case 0x800B: return WG_BO_REVERSE_SUBTRACT;
    case 0x8007: return WG_BO_MIN;
    case 0x8008: return WG_BO_MAX;
    default:     return WG_BO_ADD;          /* also the signed variants, as nv2a_gl */
    }
}

/* GL compare function 0x200..0x207 -> WG_CMP_* (0: the default dflt). */
static uint8_t cmp_func(uint32_t v, uint8_t dflt)
{
    return v >= 0x200 && v <= 0x207 ? (uint8_t)(v - 0x200) : dflt;
}

/* GL stencil op -> WG_SO_*; 0 is KEEP, as nv2a_gl reads it. */
static uint8_t stencil_op(uint32_t v)
{
    switch (v) {
    case 0x1E01: return WG_SO_REPLACE;
    case 0x150A: return WG_SO_INVERT;
    case 0x1E02: return WG_SO_INCR_CLAMP;
    case 0x1E03: return WG_SO_DECR_CLAMP;
    case 0x8507: return WG_SO_INCR_WRAP;
    case 0x8508: return WG_SO_DECR_WRAP;
    default:     return WG_SO_KEEP;         /* GL_KEEP, 0 */
    }
}

/* The pipeline key from the method shadow, as nv2a_gl's apply_state and
 * nv2a_vk's apply_dynamic read it. Returns 0 when the draw shows nothing
 * (both faces culled).
 *
 * Not mapped, as in nv2a_gl and nv2a_vk: SET_FRONT/BACK_POLYGON_MODE (point
 * and line fill draw filled), point size (WebGPU points are one pixel, as
 * GL's without gl_PointSize; MC3's menus draw none), line width (one pixel). */
static int pipe_key(const uint32_t *r, uint8_t topo, const WsTarget *t, const uint8_t dim[4],
                    WgPipeKey *k, float bconst[4])
{
    static int told_cmix, told_nost;
    int has_depth = t->depth_fmt != WG_FMT_NONE, cuse = 0;
    uint32_t cm = r[0x358 / 4];

    memset(k, 0, sizeof *k);
    k->topology = topo;
    if (topo == WG_TOPO_TRIANGLE_LIST && r[0x308 / 4]) {
        uint32_t face = r[0x39C / 4] ? r[0x39C / 4] : 0x405;
        if (face == 0x408)                  /* GL_FRONT_AND_BACK */
            return 0;
        k->cull = face == 0x404 ? WG_CULL_FRONT : WG_CULL_BACK;
        /* wgsl_vsh puts the surface's row 0 at the top of the attachment
         * (y negated into NDC), so the title's winding holds as it is;
         * nv2a_gl mirrors it because GL's rows are bottom-first. */
        k->front_face = r[0x3A0 / 4] == 0x0901 ? WG_FACE_CCW : WG_FACE_CW;
    }
    k->unclipped_depth = (wg_ctl.features & WG_FEAT_DEPTH_CLIP_CONTROL) ? 1 : 0;
    k->color_fmt = (uint8_t)t->color_fmt;
    k->depth_fmt = (uint8_t)t->depth_fmt;
    if (t->color_fmt != WG_FMT_NONE)
        k->color_mask = (uint8_t)(((cm & 0x00010000u) ? WG_CM_R : 0) | ((cm & 0x00000100u) ? WG_CM_G : 0)
                                | ((cm & 0x00000001u) ? WG_CM_B : 0) | ((cm & 0x01000000u) ? WG_CM_A : 0));
    if (r[0x304 / 4] && t->color_fmt != WG_FMT_NONE) {
        k->blend_enable = 1;
        k->blend_src = blend_factor(r[0x344 / 4], &cuse);
        k->blend_dst = blend_factor(r[0x348 / 4], &cuse);
        k->blend_op = blend_op(r[0x350 / 4]);
        if (k->blend_op == WG_BO_MIN || k->blend_op == WG_BO_MAX) {
            k->blend_src = k->blend_dst = WG_BF_ONE;   /* WebGPU requires it; GL ignores them */
            cuse = 0;
        }
        argb_vec4(r[0x34C / 4], bconst);
        if (cuse == 3)                      /* colour and alpha: the colour wins */
            once(&told_cmix, "a blend reading both the constant's colour and its alpha:"
                 " the alpha factor reads the colour");
        else if (cuse == 2)
            bconst[0] = bconst[1] = bconst[2] = bconst[3];
    }
    k->depth_cmp = WG_CMP_ALWAYS;
    if (has_depth) {
        /* GL and Vulkan write depth only with the test on (glDepthMask
         * under a disabled GL_DEPTH_TEST writes nothing); WebGPU writes with
         * depthCompare 'always' too, so the write follows the test. */
        if (r[0x30C / 4]) {
            k->depth_cmp = cmp_func(r[0x354 / 4], WG_CMP_LESS_EQUAL);
            k->depth_write = r[0x35C / 4] ? 1 : 0;
        }
        if (r[0x32C / 4] && t->depth_fmt != WG_FMT_DEPTH24PLUS_STENCIL8)
            once(&told_nost, "a stencil test on a depth target without stencil: test off");
        if (r[0x32C / 4] && t->depth_fmt == WG_FMT_DEPTH24PLUS_STENCIL8) {
            /* Like GL's glStencilFunc/glStencilOp (and nv2a_vk's
             * FRONT_AND_BACK): one state for both faces; a method value of
             * 0 (never written) reads as ALWAYS / KEEP. */
            k->stencil_enable = 1;
            k->stencil_cmp = cmp_func(r[0x364 / 4], WG_CMP_ALWAYS);
            k->stencil_fail = stencil_op(r[0x370 / 4]);
            k->stencil_zfail = stencil_op(r[0x374 / 4]);
            k->stencil_pass = stencil_op(r[0x378 / 4]);
            k->stencil_read_mask = (uint8_t)(r[0x36C / 4] & 0xFF);
            k->stencil_write_mask = (uint8_t)(r[0x360 / 4] & 0xFF);
        }
    }
    memcpy(k->tex_dim, dim, 4);
    return 1;
}

/* ── Arena layout of one draw ─────────────────────────────────────── */

/* What the previous draws put in this arena, for reuse (valid while gen ==
 * s_arena_gen). */
static struct {
    uint32_t vf_gen, vf_off;  WgVFetch vf;
    uint32_t pk_gen, pk_off;  float pk[WG_PK_ROWS][4];
    uint32_t vd_gen, vd_off;  float vd[16][4];
    uint32_t k_gen, k_off, k_cgen;  float k[192][4];
} s_c;

/* Room for n rows of K: the cached rows (gen, off) reused when equal, else
 * written. Returns the K row, UINT32_MAX when full. */
static uint32_t k_rows(const float (*rows)[4], uint32_t n, float (*cache)[4],
                       uint32_t *gen, uint32_t *off)
{
    void *p;
    uint32_t at;
    if (*gen == s_arena_gen && !memcmp(cache, rows, (size_t)n * 16))
        return *off;
    at = wg_arena_alloc(n * 16, 16, &p);
    if (at == UINT32_MAX)
        return at;
    memcpy(p, rows, (size_t)n * 16);
    memcpy(cache, rows, (size_t)n * 16);
    *gen = s_arena_gen;
    *off = at / 16;
    return *off;
}

static uint32_t elem_bytes(uint32_t type, uint32_t size)
{
    switch (type) {
    case 0: case 6: return 4;           /* D3DCOLOR, packed normal */
    case 2: return 4 * size;
    case 4: return size;
    default: return 2 * size;           /* S1, S32K */
    }
}

typedef struct {
    const uint32_t *idx;
    uint32_t n;
    const uint8_t *vlo;                 /* the vertex bytes the draw reads */
    uint32_t vbytes;
    WgVFetch vf;                        /* offsets from vlo */
    float pk[WG_PK_ROWS][4];
    WgDrawRec rec;                      /* all but the arena offsets */
} WdDraw;

/* Everything into the arena; the record's offset / sizeof(WgDrawRec), or
 * UINT32_MAX when it is full. *first_index: DRAW.first_index. */
static uint32_t lay_out(const Nv2aRawBatch *b, WdDraw *d, uint32_t *first_index)
{
    WgDrawRec *rec;
    void *p;
    uint32_t at, vf_off;

    d->rec.vtx_base = 0;
    if (d->vbytes) {
        if ((at = wg_arena_alloc(d->vbytes, 16, &p)) == UINT32_MAX)
            return at;
        memcpy(p, d->vlo, d->vbytes);
        d->rec.vtx_base = at;
    }
    if ((at = wg_arena_alloc(d->n * 4, 16, &p)) == UINT32_MAX)
        return at;
    memcpy(p, d->idx, (size_t)d->n * 4);
    *first_index = at / 4;

    if (s_c.vf_gen == s_arena_gen && !memcmp(&s_c.vf, &d->vf, sizeof d->vf)) {
        vf_off = s_c.vf_off;
    } else {
        if ((at = wg_arena_alloc(sizeof d->vf, 16, &p)) == UINT32_MAX)
            return at;
        memcpy(p, &d->vf, sizeof d->vf);
        s_c.vf = d->vf;
        s_c.vf_gen = s_arena_gen;
        s_c.vf_off = vf_off = at / 4;
    }
    d->rec.vf_off = vf_off;
    if ((d->rec.vdef_off = k_rows(b->attr_const, 16, s_c.vd, &s_c.vd_gen, &s_c.vd_off)) == UINT32_MAX)
        return UINT32_MAX;
    if ((d->rec.pk_off = k_rows((const float (*)[4])d->pk, WG_PK_ROWS, s_c.pk, &s_c.pk_gen,
                                &s_c.pk_off)) == UINT32_MAX)
        return UINT32_MAX;
    d->rec.k_off = 0;
    if (b->xform == 2 && b->vp_consts) {
        /* Transform constants: compared only when the executor's version
         * moved (equal versions, equal values). Sharing the rows between
         * draws is safe with programs that write c[]: wgsl_vsh copies them
         * into a private array first, K stays read-only. */
        if (s_c.k_gen == s_arena_gen && s_c.k_cgen == b->vp_const_gen) {
            d->rec.k_off = s_c.k_off;
        } else {
            if ((d->rec.k_off = k_rows(b->vp_consts, 192, s_c.k, &s_c.k_gen, &s_c.k_off)) == UINT32_MAX)
                return UINT32_MAX;
            s_c.k_cgen = b->vp_const_gen;
        }
    }
    if ((at = wg_arena_alloc(sizeof *rec, sizeof *rec, &p)) == UINT32_MAX)
        return at;
    rec = (WgDrawRec *)p;
    *rec = d->rec;
    return at / (uint32_t)sizeof *rec;
}

/* ── Pass state ───────────────────────────────────────────────────── */

/* What the open pass has, as last sent (reset at every BEGIN_PASS). */
static struct {
    uint32_t seq, valid;
    float    vp[6];
    uint32_t sc[4];
    float    bc[4];
    uint32_t sref, pipe, tex[4], smp[4];
} s_ps;

static void ps_reset(const WsTarget *t)
{
    memset(&s_ps, 0, sizeof s_ps);
    s_ps.valid = 1;
    s_ps.seq = t->pass_seq;
    s_ps.vp[2] = (float)t->w;
    s_ps.vp[3] = (float)t->h;
    s_ps.vp[5] = 1.0f;
    s_ps.sc[2] = t->w;
    s_ps.sc[3] = t->h;
}

/* SET_SURFACE_CLIP as a scissor in attachment pixels (nv2a_gl's
 * surface_scissor): the whole target when it covers the surface. */
static void scissor_of(const uint32_t *r, const Nv2aRawBatch *b, const WsTarget *t, uint32_t sc[4])
{
    uint32_t ax = b->surface.aa_sx ? b->surface.aa_sx : 1, ay = b->surface.aa_sy ? b->surface.aa_sy : 1;
    uint32_t x0 = (r[0x200 / 4] & 0xFFFF) * ax, x1 = x0 + (r[0x200 / 4] >> 16) * ax;
    uint32_t y0 = (r[0x204 / 4] & 0xFFFF) * ay, y1 = y0 + (r[0x204 / 4] >> 16) * ay;
    uint32_t pw = t->pw ? t->pw : t->w, ph = t->ph ? t->ph : t->h;

    sc[0] = sc[1] = 0;
    sc[2] = t->w;
    sc[3] = t->h;
    if (x1 > pw) x1 = pw;
    if (y1 > ph) y1 = ph;
    if (x1 <= x0 || y1 <= y0 || (x0 == 0 && y0 == 0 && x1 == pw && y1 == ph))
        return;
    /* real pixels -> attachment pixels, rounded as nv2a_gl's to_stored */
#define WD_STORED(v, att, real) ((uint32_t)(((uint64_t)(v) * (att) + (real) / 2) / (real)))
    sc[0] = wsurf_x(x0, pw, t->w, t->wide);     /* (Hor+ widening too) */
    sc[1] = WD_STORED(y0, t->h, ph);
    sc[2] = wsurf_x(x1, pw, t->w, t->wide) - sc[0];
    sc[3] = WD_STORED(y1, t->h, ph) - sc[1];
#undef WD_STORED
}

/* ── The draw ─────────────────────────────────────────────────────── */

void wdraw_batch(const Nv2aRawBatch *b)
{
    static int told_big, told_full, told_tgt, told_noattr;
    static WdDraw d;
    const uint32_t *r = b->regs;
    const WsTarget *t;
    Nv2aPshKey pk;
    WtStage st[4];
    WgPipeKey key;
    uint8_t dim[4], topo = 0;
    float bconst[4] = { 0, 0, 0, 0 }, mv[16];
    uint32_t vs, fs, pipe, rec_ix, first = 0, i, a, sc[4], zmax;
    const uint8_t *lo = NULL, *hi = NULL;
    int tries;

    if (!b->index_count || !b->vertex_count)
        return;
    wsurf_draw(&b->surface, b->zeta_va);
    wsurf_pass_begin();
    if (!wsurf_target()) {
        once(&told_tgt, "a draw with no target pass: dropped");
        return;
    }

    /* Primitive and vertex layout first: nothing is emitted for a draw that
     * shows nothing. */
    memset(&d, 0, sizeof d);
    d.n = prim_list(b, &topo, &d.idx);
    if (!d.n)
        return;
    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        const uint8_t *alo, *ahi;
        if (!(b->attr_present & (1u << a)))
            continue;                       /* size 0: reads K[vdef_off + a] */
        if (!(b->attr_direct & (1u << a)) || !b->direct[a].ptr) {
            /* wgpu_queue.c hands every present attribute over as direct */
            once(&told_noattr, "a converted attribute arrived without direct data: read as its default");
            continue;
        }
        alo = (const uint8_t *)((uintptr_t)b->direct[a].ptr & ~(uintptr_t)3);
        ahi = b->direct[a].ptr + (size_t)(b->vertex_count - 1) * b->direct[a].stride
            + elem_bytes(b->direct[a].type, b->direct[a].size);
        if (!lo || alo < lo) lo = alo;
        if (!hi || ahi > hi) hi = ahi;
    }
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if ((b->attr_present & b->attr_direct & (1u << a)) && b->direct[a].ptr) {
            d.vf.attr[a].off = (uint32_t)(b->direct[a].ptr - lo);
            d.vf.attr[a].fmt = (b->direct[a].type & 0xFu) | (b->direct[a].size & 0xFu) << 4
                             | b->direct[a].stride << 8;
        }
    d.vlo = lo;
    d.vbytes = lo ? (uint32_t)(((size_t)(hi - lo) + 3) & ~(size_t)3) : 0;
    if (lo && (size_t)(hi - lo) > (64u << 20)) {
        once(&told_big, "a draw reading over 64 MB of vertex bytes: dropped");
        return;
    }

    wgsl_psh_key(r, &pk);
    for (i = 0; i < 4; i++) {
        wtex_stage(b, (int)i, &st[i]);
        dim[i] = (uint8_t)(st[i].tex ? st[i].dim : WG_TEXDIM_NONE);
    }
    wsurf_pass_begin();                     /* (a stage may have ended it) */
    t = wsurf_target();
    if (!t)
        return;
    if (!pipe_key(r, topo, t, dim, &key, bconst))
        return;
    vs = wpipe_vs(b);
    fs = vs ? wpipe_fs(&pk, dim) : 0;
    if (!vs || !fs)
        return;                             /* logged by wgpu_pipe.c */
    pipe = wpipe_get(vs, fs, &key);

    /* The record: nv2a_gl's uniforms (gl_draw_raw), in WsTarget's terms --
     * w, h the attachment, pw, ph the surface in real pixels. */
    zmax = (((r[0x208 / 4] >> 4) & 0xF) == 1) ? 0xFFFFu : 0xFFFFFFu;
    {
        float pw = (float)(t->pw ? t->pw : t->w), ph = (float)(t->ph ? t->ph : t->h);
        d.rec.surf[0] = 2.0f / pw;
        d.rec.surf[1] = 2.0f / ph;
        d.rec.surf[2] = 1.0f / (float)zmax;
        d.rec.surf[3] = 0.5f - 0.5f * pw / (float)t->w;     /* see nv2a_snap */
        d.rec.aa[0] = b->aa_sx > 0 ? b->aa_sx : 1.0f;
        d.rec.aa[1] = b->aa_sy > 0 ? b->aa_sy : 1.0f;
        d.rec.aa[2] = 0.5f - 0.5f * ph / (float)t->h;       /* snapy: differs with square pixels */
        d.rec.aa[3] = t->wide > 1.0f ? 1.0f / t->wide : 0.0f;  /* Hor+ squeeze (nv2a_clip) */
        /* A clip narrower than the frame (MC3's minimap, 128 wide at x 47):
         * its 2D map has corners far off screen, which the edge rule left
         * unsqueezed (the map k times wider than its squeezed ring). No
         * full-screen quad lives in such a pass: squeeze every vertex (< 0). */
        uint32_t cw = (r[0x200 / 4] >> 16) * (b->aa_sx > 0 ? (uint32_t)b->aa_sx : 1u);
        if (d.rec.aa[3] > 0.0f && cw && cw < (uint32_t)pw)
            d.rec.aa[3] = -d.rec.aa[3];
    }
    memcpy(d.rec.vpoff, b->vp_offset, sizeof d.rec.vpoff);
    nv2a_ffp_fog_uniforms(r, mv, d.rec.fogplane, d.rec.fog0, d.rec.fog1);
    memcpy(d.rec.mv, mv, sizeof d.rec.mv);
    memcpy(d.rec.m, b->composite, sizeof d.rec.m);
    for (i = 0; i < 4; i++) {
        d.rec.tscale[i / 2][(i & 1) * 2] = st[i].tscale[0];
        d.rec.tscale[i / 2][(i & 1) * 2 + 1] = st[i].tscale[1];
        if (st[i].tex)
            d.rec.stage_info |= (WG_SI_BOUND | (st[i].dim == WG_TEXDIM_CUBE ? WG_SI_CUBE : 0)) << (8 * i);
    }
    d.rec.xform = b->xform;
    d.rec.vcount = b->vertex_count;
    d.rec.afunc = r[0x300 / 4] ? r[0x33C / 4] : 0x207;
    d.rec.aref = (float)(r[0x340 / 4] & 0xFF);
    for (i = 0; i < 8; i++) {               /* combiner factors, fog colour (R in the low byte) */
        argb_vec4(r[0xA60 / 4 + i], d.pk[WG_PK_C0 + i]);
        argb_vec4(r[0xA80 / 4 + i], d.pk[WG_PK_C1 + i]);
    }
    argb_vec4(r[0x1E20 / 4], d.pk[WG_PK_FC0]);
    argb_vec4(r[0x1E24 / 4], d.pk[WG_PK_FC1]);
    {
        uint32_t fc = r[0x2A8 / 4];
        d.pk[WG_PK_FOG][0] = byte_f(fc, 0);  d.pk[WG_PK_FOG][1] = byte_f(fc, 8);
        d.pk[WG_PK_FOG][2] = byte_f(fc, 16); d.pk[WG_PK_FOG][3] = byte_f(fc, 24);
    }

    /* Into the arena; when full, submit what is there and lay out again. */
    for (tries = 0; (rec_ix = lay_out(b, &d, &first)) == UINT32_MAX; tries++) {
        if (tries) {
            once(&told_full, "a draw larger than the whole frame arena: dropped"
                 " (RECOMP_WGPU_ARENA_MB)");
            return;
        }
        wg_flush_partial();
        wsurf_pass_begin();
        if (!(t = wsurf_target()))
            return;
    }

    /* Pass state, only what differs from what the pass has. */
    if (!s_ps.valid || s_ps.seq != t->pass_seq)
        ps_reset(t);
    {
        float vp[6] = { 0, 0, (float)t->w, (float)t->h, 0, 1 };
        if (memcmp(vp, s_ps.vp, sizeof vp)) {
            WgSetViewport *v = (WgSetViewport *)wg_emit(WG_OP_SET_VIEWPORT, sizeof *v);
            v->x = vp[0]; v->y = vp[1]; v->width = vp[2]; v->height = vp[3];
            v->min_depth = vp[4]; v->max_depth = vp[5];
            memcpy(s_ps.vp, vp, sizeof vp);
        }
    }
    scissor_of(r, b, t, sc);
    if (memcmp(sc, s_ps.sc, sizeof sc)) {
        WgSetScissor *s = (WgSetScissor *)wg_emit(WG_OP_SET_SCISSOR, sizeof *s);
        s->x = sc[0]; s->y = sc[1]; s->width = sc[2]; s->height = sc[3];
        memcpy(s_ps.sc, sc, sizeof sc);
    }
    if (key.blend_enable && memcmp(bconst, s_ps.bc, sizeof bconst)) {
        WgSetBlendConst *c = (WgSetBlendConst *)wg_emit(WG_OP_SET_BLEND_CONST, sizeof *c);
        memcpy(c->color, bconst, sizeof c->color);
        memcpy(s_ps.bc, bconst, sizeof bconst);
    }
    if (key.stencil_enable && (r[0x368 / 4] & 0xFF) != s_ps.sref) {
        WgSetStencilRef *s = (WgSetStencilRef *)wg_emit(WG_OP_SET_STENCIL_REF, sizeof *s);
        s->ref = s_ps.sref = r[0x368 / 4] & 0xFF;
    }
    {
        uint32_t tex[4], smp[4];
        int pipe_new = pipe != s_ps.pipe;
        for (i = 0; i < 4; i++) {
            tex[i] = st[i].tex;
            smp[i] = st[i].tex ? st[i].smp : 0;
        }
        if (pipe_new) {
            WgSetPipe *p = (WgSetPipe *)wg_emit(WG_OP_SET_PIPE, sizeof *p);
            p->id = s_ps.pipe = pipe;
        }
        /* Group 1 follows the pipeline's layout: sent again with every new one. */
        if (pipe_new || memcmp(tex, s_ps.tex, sizeof tex) || memcmp(smp, s_ps.smp, sizeof smp)) {
            WgSetTex *s = (WgSetTex *)wg_emit(WG_OP_SET_TEX, sizeof *s);
            memcpy(s->tex, tex, sizeof tex);
            memcpy(s->smp, smp, sizeof smp);
            memcpy(s_ps.tex, tex, sizeof tex);
            memcpy(s_ps.smp, smp, sizeof smp);
        }
    }
    {
        WgDraw *w = (WgDraw *)wg_emit(WG_OP_DRAW, sizeof *w);
        w->first_index = first;
        w->index_count = d.n;
        w->record = rec_ix;
    }
}
