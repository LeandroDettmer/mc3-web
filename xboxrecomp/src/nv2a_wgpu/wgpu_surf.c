/*
 * wgpu_surf.c -- surfaces, depth buffers, passes, clears, 2D blits and the
 * present command of the WebGPU back end (browser only; ADR 0011,
 * docs/webgpu-plan.md). Render thread only, unless marked.
 *
 * Same rules as nv2a_gl.c's surf_get/depth_get/surf_bind (the reference):
 *   - one bgra8unorm texture per colour address; a clip that fits uses it,
 *     one that does not grows it to cover both, never shrinks it (NFSU2's
 *     split screen, MC3's 175-wide minimap pass);
 *   - stored size = title size x RECOMP_GL_SCALE (RECOMP_GL_SMALL_SCALE for
 *     targets narrower than 512, square pixels in 16:9 with
 *     RECOMP_GL_SQUARE), capped by the device's texture limit;
 *   - one depth24plus-stencil8 per (zeta address, stored size), shared by
 *     every colour surface drawn with that zeta (NFSU2's light flares); a
 *     draw with no zeta has no depth attachment (nv2a_gl binds the
 *     surface's own s->ds then, but apply_state tests neither depth nor
 *     stencil without a zeta);
 *   - new storage starts cleared: colour (0,0,0,1), depth 1, stencil 0.
 *
 * Viewport and scissor: wgpu_draw.c sends them from wsurf_target (the
 * attachment's size, and the clip through to_stored's rounding); the
 * fixed ops (CLEAR_RECT, BLIT, PRESENT) carry stored pixels from ws_to.
 *
 * A draw that samples its own target (WebGPU forbids a pass attachment in
 * a bind group): the pass is suspended, the target copied whole into the
 * surface's shadow texture (COPY_TEX) and the pass reopened with LOAD; the
 * stage gets the shadow. Once per pass segment: later draws of the same
 * segment read that copy (without their predecessors' writes). Any other
 * surface sampled is in an ended pass already (a target change ends it).
 *
 * Passes: the target's BEGIN_PASS is held back until something must be
 * drawn into it (wsurf_pass_begin), so whole-surface clears that come
 * first become its load ops and clear values. A clear after draws ends the
 * pass and starts another. A partial clear (a smaller clear rectangle, or a
 * colour clear with a channel mask) is a CLEAR_RECT quad inside the pass.
 * A target change, a blit or the flip ends the pass; a target that only
 * has clears still gets its BEGIN_PASS/END_PASS then.
 *
 * Blits (NV062/NV09F, gl_blit): COPY_TEX when both rectangles come out the
 * same size in stored pixels, BLIT (a scaled quad) otherwise. A blit within
 * one surface goes through its shadow (WebGPU copies and passes cannot read
 * and write one texture): the source rectangle is copied there first. The present
 * carries the 4:3 fill (present_fxaa, fill_steady_pass) as WG_PF_* flags
 * and rectangles; web/wgpu_fixed.js PRESENT decides it on the GPU.
 *
 * A zeta buffer sampled as A8R8G8B8 / A8B8G8R8 (MC3's motion blur mask,
 * nv2a_gl's depth_as_color): wsurf_depth_texture packs the depth into a
 * bgra8unorm copy with ZCONV (wgpu_fixed.js ZCONV, nv2a_gl's zconv rules:
 * z = depth * 0xFFFFFF, stencil 0), again only after the depth was bound
 * since. RECOMP_GL_DEPTH_TEX=0 off; RECOMP_GL_SHARED_Z=0 (one depth per
 * colour surface, nv2a_gl's old way) turns it off too.
 */
#include "nv2a_wgpu.h"
#include "wgpu_int.h"

#include <emscripten.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int xbox_video_widescreen(void);

typedef struct {
    uint32_t va, w, h, pw, ph;          /* title pixels (w, h), stored pixels (pw, ph) */
    uint32_t vw, vw_frame;              /* widest clip drawn this frame: what is shown */
    uint32_t id, used, aa_sx, aa_sy;
    uint32_t shadow, shadow_seq;        /* copy for draws sampling their own target; its pass */
    int      fresh;                     /* storage not yet cleared */
    float    kw;                        /* Hor+ widening (ws_wide), 1: none */
} WsSurf;

typedef struct {
    uint32_t va, pw, ph, id, used;
    int      fresh;
    uint32_t lw, lh, ax, ay;            /* the last colour surface bound with it: title px, aa */
    uint32_t wseq, cseq, cfmt;          /* bound at / converted at, cfmt WG_ZC_* + 1 */
    uint32_t rgba;                      /* its colour copy (ZCONV), 0: none yet */
} WsDepth;

#define WS_MAX_SURF  32
#define WS_MAX_DEPTH 16
static WsSurf  s_surf[WS_MAX_SURF];
static WsDepth s_depth[WS_MAX_DEPTH];
static WsSurf *s_last;                  /* last surface drawn or cleared: presented */
static int     s_drew_any;              /* the title has drawn something */
static uint32_t s_pass_seq;             /* BEGIN_PASSes emitted (WsTarget.pass_seq) */
static uint32_t s_zseq;                 /* zeta binds (WsDepth.wseq) */
static int      s_shared_z = -1;        /* RECOMP_GL_SHARED_Z=0: a depth per colour surface */

/* While > 0, 4:3 screens are shown as the title framed them (no fill) /
 * the steady fill keeps its decision. Any thread. */
static volatile int s_fill_hold, s_fill_freeze;

/* The pass being built: its target, and what its BEGIN_PASS will say. */
static struct {
    WsSurf  *s;                         /* the target, NULL: none */
    WsDepth *d;
    uint32_t color, depth;              /* texture ids; color 0: none */
    int      open;                      /* BEGIN_PASS emitted, END_PASS not yet */
    uint32_t color_load, depth_load, stencil_load;
    float    clear_color[4], clear_depth;
    uint32_t clear_stencil;
} s_pass;

/* Title pixels -> stored pixels (nv2a_gl's to_stored). */
static uint32_t ws_to(uint32_t v, uint32_t p, uint32_t l)
{
    return (uint32_t)(((uint64_t)v * p + l / 2) / l);
}

static void ws_pass_emit(void)
{
    WgBeginPass *p = (WgBeginPass *)wg_emit(WG_OP_BEGIN_PASS, sizeof *p);
    p->color_tex = s_pass.color;
    p->depth_tex = s_pass.depth;
    p->color_load = s_pass.color_load;
    p->color_store = WG_STORE_STORE;
    p->depth_load = s_pass.depth_load;
    p->depth_store = WG_STORE_STORE;
    p->stencil_load = s_pass.stencil_load;
    p->stencil_store = WG_STORE_STORE;
    memcpy(p->clear_color, s_pass.clear_color, sizeof p->clear_color);
    p->clear_depth = s_pass.clear_depth;
    p->clear_stencil = s_pass.clear_stencil;
    s_pass.open = 1;
    s_pass_seq++;
}

/* End the pass, emitting a clears-only one if clears are pending. */
static void ws_pass_end(void)
{
    if (!s_pass.open && s_pass.color
        && (s_pass.color_load == WG_LOAD_CLEAR || s_pass.depth_load == WG_LOAD_CLEAR
            || s_pass.stencil_load == WG_LOAD_CLEAR))
        ws_pass_emit();
    if (s_pass.open)
        wg_emit(WG_OP_END_PASS, sizeof(WgEndPass));
    s_pass.open = 0;
}

/* ... and forget the target. */
static void ws_pass_close(void)
{
    ws_pass_end();
    memset(&s_pass, 0, sizeof s_pass);      /* loads WG_LOAD_LOAD, target none */
}

static void ws_target(WsSurf *s, WsDepth *d)
{
    uint32_t z = d ? d->id : 0;
    if (s_pass.color == s->id && s_pass.depth == z)
        return;
    ws_pass_close();
    s_pass.s = s;
    s_pass.d = d;
    s_pass.color = s->id;
    s_pass.depth = z;
    if (s->fresh) {
        s_pass.color_load = WG_LOAD_CLEAR;
        s_pass.clear_color[3] = 1.0f;       /* black, opaque */
        s->fresh = 0;
    }
    if (d && d->fresh) {
        s_pass.depth_load = s_pass.stencil_load = WG_LOAD_CLEAR;
        s_pass.clear_depth = 1.0f;
        d->fresh = 0;
    }
}

/* A surface about to be read or written outside a pass (blit): its first
 * clear must land first. Ends any pass. */
static void ws_settle(WsSurf *s)
{
    if (s->fresh)
        ws_target(s, NULL);
    ws_pass_close();
}

/* A texture about to be freed: no pass may still name it. */
static void ws_forget(uint32_t id)
{
    if (id && (s_pass.color == id || s_pass.depth == id))
        ws_pass_close();
    wg_tex_free(id);
}

static WsSurf *ws_find(uint32_t va)
{
    int i;
    for (i = 0; i < WS_MAX_SURF; i++)
        if (s_surf[i].id && s_surf[i].va == va)
            return &s_surf[i];
    return NULL;
}

uint32_t wsurf_x(uint32_t x, uint32_t real, uint32_t stored, float k)
{
    if (!(k > 1.0f))
        return ws_to(x, stored, real);
    if (x == 0)
        return 0;
    if (x >= real)
        return stored;
    return (uint32_t)((((double)x - real * 0.5) / k + real * 0.5) * stored / real + 0.5);
}

/* RECOMP_ASPECT (Hor+): the screen's shape, "21:9" or a ratio (2.39).
 * 16:9 frames (720-wide surfaces) are stored k = aspect / (16/9) times wider
 * and every draw into them is squeezed to the middle 1/k (wgsl_vsh
 * nv2a_clip): 3D sees as far up and down as before and further to the
 * sides, 2D keeps its shape in the middle 16:9, and a 2D vertex on the
 * frame's edge stays on the edge (full screen quads, fades, post passes
 * still cover it all). Off (k = 1) in 4:3, 16:9 or narrower. Unset or
 * "auto": the shape of the game's canvas as the page shows it, which the
 * page writes into s_page_aspect (web/index.html; width / height x 10000,
 * 0 = not known yet: off) and ws_aspect_poll takes at a frame's end.
 * Any thread. */
static volatile double s_wide = -1.0;
static int s_aspect_auto;
static volatile uint32_t s_page_aspect;

EMSCRIPTEN_KEEPALIVE uintptr_t nv2a_wgpu_aspect_box(void) { return (uintptr_t)&s_page_aspect; }
/* The page's FPS overlay reads this u32 (PRESENTs drawn, written by web/wgpu.js) and divides by time. */
EMSCRIPTEN_KEEPALIVE uintptr_t nv2a_wgpu_fps_box(void) { return (uintptr_t)&wg_ctl.frames_presented; }

static double ws_k_of(double a)
{
    double k = a / (16.0 / 9.0);
    if (k < 1.01 || !xbox_video_widescreen())
        k = 1.0;
    return k > 3.0 ? 3.0 : k;
}

static double ws_wide(void)
{
    double k = s_wide;
    if (k < 0.0) {
        const char *e = getenv("RECOMP_ASPECT");
        double a = 16.0 / 9.0, p, q;
        if (e && sscanf(e, "%lf:%lf", &p, &q) == 2 && p > 0.0 && q > 0.0)
            a = p / q;
        else if (e && atof(e) > 0.0)
            a = atof(e);
        else
            s_aspect_auto = 1;
        k = ws_k_of(a);
        if (k > 1.0)
            fprintf(stderr, "  [WGPU] Hor+: 16:9 frames %.3fx wider, aspect %.3f (RECOMP_ASPECT)\n",
                    k, k * 16.0 / 9.0);
        s_wide = k;
    }
    return k;
}

/* Auto: the page's canvas shape, at a frame boundary (render thread). A new
 * k rebuilds the widened surfaces at their next use (ws_surf_get). */
static void ws_aspect_poll(void)
{
    static uint32_t seen;
    uint32_t v = __atomic_load_n(&s_page_aspect, __ATOMIC_RELAXED);
    double k;

    ws_wide();
    if (!v || v == seen)   /* the page writes fixed shapes here too (web/pre.js mc3SetAspect) */
        return;
    seen = v;
    k = ws_k_of(v / 10000.0);
    if (fabs(k - s_wide) < 0.005)
        return;
    fprintf(stderr, "  [WGPU] Hor+: canvas %.3f:1, 16:9 frames %.3fx wider (RECOMP_ASPECT auto)\n",
            v / 10000.0, k);
    s_wide = k;
}

/* The widening of a surface of w x h title pixels: only 16:9 frames. */
static float ws_kw(uint32_t w, uint32_t h, uint32_t aa_sx, uint32_t aa_sy)
{
    double k = ws_wide();
    return k > 1.0 && w / aa_sx > 640 && h / aa_sy >= 400 && h / aa_sy <= 576 ? (float)k : 1.0f;
}

/* For the title's camera (src/games/remix): how much wider its frustum must
 * cull, so objects in the new side areas are drawn. 1 when off, or with
 * another renderer. Auto: a new canvas shape reaches the surfaces only at
 * the render thread's next frame end, a frame or two after the camera has
 * seen the page's box; culling for the wider of both meanwhile keeps the
 * sides from popping either way (too wide only costs draws). Game thread. */
float nv2a_wgpu_wide(void)
{
    double k, p;
    uint32_t v;
    if (!nv2a_wgpu_active())
        return 1.0f;
    k = ws_wide();
    v = __atomic_load_n(&s_page_aspect, __ATOMIC_RELAXED);
    if (v && (p = ws_k_of(v / 10000.0)) > k)
        k = p;
    return (float)k;
}

static double s_scale = -1.0, s_small = -1.0;
static int    s_square = -1;

/* exact_w: the surface must be w wide (a widened frame, ws_bind). */
static WsSurf *ws_surf_get(uint32_t va, uint32_t w, uint32_t h, uint32_t aa_sx, uint32_t aa_sy, int exact_w)
{
    WsSurf *s = ws_find(va), *victim = NULL;
    uint32_t maxs = wg_ctl.max_tex_2d ? wg_ctl.max_tex_2d : 8192;
    double kx, ky;
    int i;

    /* the clip fits and the widening is still the screen's: keep it */
    if (s && s->w >= w && s->h >= h && (!exact_w || s->w == w)
        && s->kw == ws_kw(s->w, s->h, s->aa_sx, s->aa_sy)) {
        s->used = wg_frame;
        return s;
    }
    if (s) {                                    /* grow to cover both, never shrink */
        if (s->w > w && !exact_w) w = s->w;
        if (s->h > h) h = s->h;
    } else {
        for (i = 0; i < WS_MAX_SURF; i++) {
            if (!s_surf[i].id) { victim = &s_surf[i]; break; }
            if (!victim || s_surf[i].used < victim->used) victim = &s_surf[i];
        }
        s = victim;
    }
    if (s == s_last)
        s_last = NULL;                          /* its picture goes with it */
    ws_forget(s->id);
    wg_tex_free(s->shadow);                     /* never an attachment */
    memset(s, 0, sizeof *s);
    s->va = va; s->w = w; s->h = h; s->used = wg_frame;
    s->aa_sx = aa_sx; s->aa_sy = aa_sy;

    if (s_scale < 0.0) {                        /* nv2a_gl's settings, same meaning */
        const char *e = getenv("RECOMP_GL_SCALE");
        double k = e ? atof(e) : 1.0;
        if (!(k > 0.0)) k = 1.0;
        s_scale = k < 0.5 ? 0.5 : k > 5.0 ? 5.0 : k;
        e = getenv("RECOMP_GL_SMALL_SCALE");
        s_small = e && atof(e) > 0.0 ? atof(e) : 0.0;
        e = getenv("RECOMP_GL_SQUARE");
        s_square = !(e && *e == '0') && xbox_video_widescreen();
        if (s_scale != 1.0 || s_small > 0.0)
            fprintf(stderr, "  [WGPU] surfaces at %gx (RECOMP_GL_SCALE), narrower than 512 at %gx\n",
                    s_scale, s_small > 0.0 && s_small < s_scale ? s_small : s_scale);
    }
    kx = ky = s_scale;
    if (s_small > 0.0 && w < 512 && s_small < s_scale)
        kx = ky = s_small;
    if (s_square && aa_sx <= 1 && w >= 512 && h >= 400 && h <= 576)
        kx *= w > 640 ? 32.0 / 27.0 : 4.0 / 3.0;
    s->kw = ws_kw(w, h, aa_sx, aa_sy);
    kx *= s->kw;
    if (w * kx > (double)maxs) kx = (double)maxs / w;
    if (h * ky > (double)maxs) ky = (double)maxs / h;
    s->pw = (uint32_t)(w * kx + 0.5); s->ph = (uint32_t)(h * ky + 0.5);
    if (!s->pw) s->pw = 1;
    if (!s->ph) s->ph = 1;
    s->id = wg_tex_create(WG_FMT_BGRA8UNORM, s->pw, s->ph,
                          WG_TU_RENDER_ATTACHMENT | WG_TU_TEXTURE_BINDING
                          | WG_TU_COPY_SRC | WG_TU_COPY_DST);
    s->fresh = 1;
    return s;
}

static WsDepth *ws_depth_get(uint32_t va, uint32_t pw, uint32_t ph)
{
    WsDepth *d, *victim = NULL;
    int i;

    for (i = 0; i < WS_MAX_DEPTH; i++) {
        if (s_depth[i].id && s_depth[i].va == va && s_depth[i].pw == pw && s_depth[i].ph == ph) {
            s_depth[i].used = wg_frame;
            return &s_depth[i];
        }
        if (!victim || (victim->id && (!s_depth[i].id || s_depth[i].used < victim->used)))
            victim = &s_depth[i];
    }
    d = victim;
    ws_forget(d->id);
    wg_tex_free(d->rgba);                       /* never an attachment of a game pass */
    memset(d, 0, sizeof *d);
    d->va = va; d->pw = pw; d->ph = ph; d->used = wg_frame;
    d->id = wg_tex_create(WG_FMT_DEPTH24PLUS_STENCIL8, pw, ph,
                          WG_TU_RENDER_ATTACHMENT | WG_TU_TEXTURE_BINDING);
    d->fresh = 1;
    return d;
}

/* surf_bind: the surface (and zeta) sf draws into becomes the target. */
static WsSurf *ws_bind(const Nv2aSurface *sf, uint32_t zeta_va, WsDepth **dout)
{
    uint32_t bpp = sf->bytes_per_pixel ? sf->bytes_per_pixel : 4;
    uint32_t w = sf->pitch ? sf->pitch / bpp : sf->width;
    uint32_t h = sf->clip_y + sf->height;       /* the clip need not start at 0 */
    uint32_t cw, ax = sf->aa_sx ? sf->aa_sx : 1, ay = sf->aa_sy ? sf->aa_sy : 1;
    int exact = 0;
    WsSurf *s;
    WsDepth *d;

    cw = sf->clip_x + sf->width;
    if (w < cw) w = cw;
    if (!w || !h || !sf->color_va)
        return NULL;
    /* Hor+: a frame drawn narrower than its pitch (MC3: 720 in a 768-pixel
     * pitch) is widened about the middle of what is drawn, not of the
     * pitch, or the picture lands left of the canvas' middle with black at
     * the right. A narrower pass into it (the 175-wide minimap) keeps it. */
    if (w > cw && ws_wide() > 1.0) {
        WsSurf *o = ws_find(sf->color_va);
        if (ws_kw(cw, h, ax, ay) > 1.0f)
            w = cw, exact = 1;
        else if (o && o->kw > 1.0f && o->w >= cw)
            w = o->w;
    }
    s = ws_surf_get(sf->color_va, w, h, ax, ay, exact);
    /* The widest clip of the frame: a narrower pass into the same surface
     * (MC3's minimap, 175 wide) must not crop what is shown. */
    cw = sf->clip_x + sf->width < s->w ? sf->clip_x + sf->width : s->w;
    if (s->vw_frame != wg_frame || cw > s->vw)
        s->vw = cw;
    s->vw_frame = wg_frame;
    if (s_shared_z < 0) {
        const char *e = getenv("RECOMP_GL_SHARED_Z");
        s_shared_z = !(e && e[0] == '0');
    }
    /* Not shared: keyed by the colour surface, as nv2a_gl's own s->ds. */
    d = zeta_va ? ws_depth_get(s_shared_z ? zeta_va : sf->color_va, s->pw, s->ph) : NULL;
    if (d) {                                    /* drawn or cleared from here */
        d->lw = s->w; d->lh = s->h; d->ax = s->aa_sx; d->ay = s->aa_sy;
        d->wseq = ++s_zseq;
    }
    ws_target(s, d);
    s_last = s;
    *dout = d;
    return s;
}

void wsurf_clear(const Nv2aSurface *sf, uint32_t zeta_va, uint32_t flags,
                 uint32_t argb, uint32_t zs, const WsClearRegs *r)
{
    WsDepth *d;
    WsSurf *s = ws_bind(sf, zeta_va, &d);
    uint32_t x0 = 0, y0 = 0, x1, y1, mask, z;
    int partial = 0, color, depth, stencil;
    float rgba[4], zf;

    if (!s)
        return;
    x1 = s->w; y1 = s->h;
    if (r->have) {
        /* The clear rectangle is in logical pixels, like SURFACE_CLIP; the
         * surface is larger by the anti-aliasing factor (gl_clear). */
        x0 = (r->rect_h & 0xFFFF) * s->aa_sx; x1 = ((r->rect_h >> 16) + 1) * s->aa_sx;
        y0 = (r->rect_v & 0xFFFF) * s->aa_sy; y1 = ((r->rect_v >> 16) + 1) * s->aa_sy;
        if (x1 > s->w) x1 = s->w;
        if (y1 > s->h) y1 = s->h;
        if (x1 <= x0 || y1 <= y0)
            return;
        partial = x1 - x0 < s->w || y1 - y0 < s->h;
    }
    mask = (flags >> 4) & 0xF;                  /* R 0x10, G 0x20, B 0x40, A 0x80 = WG_CM_* */
    color = mask != 0;
    depth = (flags & 0x1) && d;
    stencil = (flags & 0x2) && d;
    if (!color && !depth && !stencil)
        return;
    rgba[0] = (float)((argb >> 16) & 0xFF) / 255.0f;
    rgba[1] = (float)((argb >> 8) & 0xFF) / 255.0f;
    rgba[2] = (float)(argb & 0xFF) / 255.0f;
    rgba[3] = (float)(argb >> 24) / 255.0f;
    z = r->zmax == 0xFFFFu ? (zs & 0xFFFF) : (zs >> 8);
    zf = (float)((double)z / (double)r->zmax);

    if (partial || (color && mask != 0xF)) {    /* a quad inside the pass */
        WgClearRect *c;
        wsurf_pass_begin();
        c = (WgClearRect *)wg_emit(WG_OP_CLEAR_RECT, sizeof *c);
        c->x = wsurf_x(x0, s->w, s->pw, s->kw);
        c->y = ws_to(y0, s->ph, s->h);
        c->width = wsurf_x(x1, s->w, s->pw, s->kw) - c->x;
        c->height = ws_to(y1, s->ph, s->h) - c->y;
        c->flags = (color ? WG_CR_COLOR : 0) | (depth ? WG_CR_DEPTH : 0)
                   | (stencil ? WG_CR_STENCIL : 0) | (mask << WG_CR_MASK_SHIFT);
        memcpy(c->color, rgba, sizeof c->color);
        c->depth = zf;
        c->stencil = zs & 0xFF;
        return;
    }
    if (s_pass.open) {                          /* drawn into: clear in a new pass */
        ws_pass_close();
        ws_target(s, d);
    }
    if (color) {
        s_pass.color_load = WG_LOAD_CLEAR;
        memcpy(s_pass.clear_color, rgba, sizeof rgba);
    }
    if (depth) {
        s_pass.depth_load = WG_LOAD_CLEAR;
        s_pass.clear_depth = zf;
    }
    if (stencil) {
        s_pass.stencil_load = WG_LOAD_CLEAR;
        s_pass.clear_stencil = zs & 0xFF;
    }
}

void wsurf_draw(const Nv2aSurface *sf, uint32_t zeta_va)
{
    WsDepth *d;
    if (ws_bind(sf, zeta_va, &d))
        s_drew_any = 1;
}

void wsurf_pass_begin(void)
{
    if (!s_pass.open && s_pass.color)
        ws_pass_emit();
}

void wsurf_pass_suspend(void)
{
    ws_pass_end();
    s_pass.color_load = s_pass.depth_load = s_pass.stencil_load = WG_LOAD_LOAD;
}

const WsTarget *wsurf_target(void)
{
    static WsTarget t;
    WsSurf *s = s_pass.s;

    if (!s_pass.open || !s)
        return NULL;
    t.color_tex = s_pass.color;
    t.depth_tex = s_pass.depth;
    t.color_fmt = WG_FMT_BGRA8UNORM;
    t.depth_fmt = s_pass.depth ? WG_FMT_DEPTH24PLUS_STENCIL8 : WG_FMT_NONE;
    t.w = s->pw; t.h = s->ph;                   /* the attachment */
    t.pw = s->w; t.ph = s->h;                   /* the guest surface, title pixels */
    t.scale_x = (float)s->pw / (float)s->w;
    t.scale_y = (float)s->ph / (float)s->h;
    t.pass_seq = s_pass_seq;
    t.wide = s->kw;
    return &t;
}

/* The surface's shadow: a copy for draws sampling their own target and for
 * blits within the surface. Never an attachment. */
static uint32_t ws_shadow(WsSurf *s)
{
    if (!s->shadow)
        s->shadow = wg_tex_create(WG_FMT_BGRA8UNORM, s->pw, s->ph,
                                  WG_TU_TEXTURE_BINDING | WG_TU_COPY_SRC | WG_TU_COPY_DST);
    return s->shadow;
}

void wsurf_blit(const Nv2aBlit *bl)
{
    WsSurf *src = ws_find(bl->src_va), *dst = ws_find(bl->dst_va);
    uint32_t sw, sh, dw, dh, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, sid;

    if (!src || !dst)                           /* written by the CPU: the guest copy stands */
        return;
    /* Clamp to both surfaces (title pixels). */
    sw = bl->sx < src->w ? src->w - bl->sx : 0;
    sh = bl->sy < src->h ? src->h - bl->sy : 0;
    dw = bl->dx < dst->w ? dst->w - bl->dx : 0;
    dh = bl->dy < dst->h ? dst->h - bl->dy : 0;
    if (sw > bl->w) sw = bl->w;
    if (sh > bl->h) sh = bl->h;
    if (dw > sw) dw = sw;
    if (dh > sh) dh = sh;
    if (!dw || !dh)
        return;
    sx0 = wsurf_x(bl->sx, src->w, src->pw, src->kw);
    sx1 = wsurf_x(bl->sx + dw, src->w, src->pw, src->kw);
    sy0 = ws_to(bl->sy, src->ph, src->h); sy1 = ws_to(bl->sy + dh, src->ph, src->h);
    dx0 = wsurf_x(bl->dx, dst->w, dst->pw, dst->kw);
    dx1 = wsurf_x(bl->dx + dw, dst->w, dst->pw, dst->kw);
    dy0 = ws_to(bl->dy, dst->ph, dst->h); dy1 = ws_to(bl->dy + dh, dst->ph, dst->h);
    if (sx1 <= sx0 || sy1 <= sy0 || dx1 <= dx0 || dy1 <= dy0)
        return;
    ws_settle(src);
    ws_settle(dst);
    sid = src->id;
    if (src == dst) {                           /* the source rectangle into the shadow first */
        uint32_t sh = ws_shadow(src);           /* TEX_CREATE first, then the record */
        WgCopyTex *c = (WgCopyTex *)wg_emit(WG_OP_COPY_TEX, sizeof *c);
        c->src = src->id; c->dst = sh;
        c->src_x = c->dst_x = sx0; c->src_y = c->dst_y = sy0;
        c->width = sx1 - sx0; c->height = sy1 - sy0;
        sid = src->shadow;
        src->shadow_seq = 0;                    /* no longer a copy of a whole pass segment */
    }
    if (sx1 - sx0 == dx1 - dx0 && sy1 - sy0 == dy1 - dy0) {
        WgCopyTex *c = (WgCopyTex *)wg_emit(WG_OP_COPY_TEX, sizeof *c);
        c->src = sid; c->dst = dst->id;
        c->src_x = sx0; c->src_y = sy0;
        c->dst_x = dx0; c->dst_y = dy0;
        c->width = sx1 - sx0; c->height = sy1 - sy0;
    } else {
        WgBlit *b = (WgBlit *)wg_emit(WG_OP_BLIT, sizeof *b);
        b->src = sid; b->dst = dst->id;
        b->sx = sx0; b->sy = sy0; b->sw = sx1 - sx0; b->sh = sy1 - sy0;
        b->dx = dx0; b->dy = dy0; b->dw = dx1 - dx0; b->dh = dy1 - dy0;
        b->filter = WG_FILTER_NEAREST;          /* glBlitFramebuffer GL_NEAREST */
    }
    if (dst->vw_frame != wg_frame || dst->vw < src->vw)   /* what is shown of it */
        dst->vw = src->vw;
    dst->vw_frame = wg_frame;
}

uint32_t wsurf_texture(uint32_t va, uint32_t *w, uint32_t *h)
{
    static int told;
    WsSurf *s = ws_find(va);
    uint32_t id;

    if (!s)
        return 0;
    id = s->id;
    /* Sampling the pass's own attachment is a WebGPU validation error that
     * fails the whole submit: the stage reads the shadow, copied once per
     * pass segment (pending clears land first: wsurf_pass_suspend). */
    if (id == s_pass.color) {
        if (!s_pass.open || s->shadow_seq != s_pass_seq) {
            WgCopyTex *c;
            uint32_t sh;
            if (!told++)
                fprintf(stderr, "  [WGPU] draw samples its own target %08X: copied first, once per pass\n", va);
            wsurf_pass_suspend();
            sh = ws_shadow(s);                  /* its TEX_CREATE must precede the COPY_TEX record */
            c = (WgCopyTex *)wg_emit(WG_OP_COPY_TEX, sizeof *c);
            c->src = s->id;
            c->dst = sh;
            c->width = s->pw;
            c->height = s->ph;
            wsurf_pass_begin();
            s->shadow_seq = s_pass_seq;
        }
        id = s->shadow;
    }
    /* The title's logical pixels (nv2a_gl rt_scale's lw, lh): the stage's
     * coordinate scale is w / lw; the stored size does not matter. */
    if (w) *w = s->w / s->aa_sx;
    if (h) *h = s->h / s->aa_sy;
    return id;
}

/* The zeta buffer at va sampled as colour format `color` (the stage's
 * NV097 format byte): its packed copy, or 0 (not a zeta buffer, not
 * A8R8G8B8 / A8B8G8R8, or turned off). w, h as wsurf_texture's. Converted
 * when the depth was bound since the last conversion; the open pass is
 * suspended around the ZCONV and reopened (pass_seq changes). */
uint32_t wsurf_depth_texture(uint32_t va, uint32_t color, uint32_t *w, uint32_t *h)
{
    static int on = -1;
    WsDepth *d = NULL;
    uint32_t flags;
    int i;

    if (color == 0x12 || color == 0x06)      flags = 0;              /* A8R8G8B8 */
    else if (color == 0x3F || color == 0x3A) flags = WG_ZC_SWAP_RB;  /* A8B8G8R8 */
    else return 0;
    if (on < 0) {
        const char *e = getenv("RECOMP_GL_DEPTH_TEX");
        on = !(e && *e == '0');
    }
    if (!on || s_shared_z <= 0)
        return 0;
    for (i = 0; i < WS_MAX_DEPTH; i++)          /* the largest: the scene's */
        if (s_depth[i].id && s_depth[i].va == va
            && (!d || s_depth[i].pw * s_depth[i].ph > d->pw * d->ph))
            d = &s_depth[i];
    if (!d)
        return 0;
    if (!d->rgba || d->cseq != d->wseq || d->cfmt != flags + 1) {
        int was_open = s_pass.open;
        WgZconv *z;
        if (!d->rgba) {
            d->rgba = wg_tex_create(WG_FMT_BGRA8UNORM, d->pw, d->ph,
                                    WG_TU_RENDER_ATTACHMENT | WG_TU_TEXTURE_BINDING);
            fprintf(stderr, "  [WGPU] depth 0x%08X (%ux%u) sampled as colour format 0x%02X\n",
                    va, d->pw, d->ph, color);
        }
        wsurf_pass_suspend();                   /* pending clears of the depth land first */
        z = (WgZconv *)wg_emit(WG_OP_ZCONV, sizeof *z);
        z->src = d->id;
        z->dst = d->rgba;
        z->width = d->pw;
        z->height = d->ph;
        z->zmax = 0xFFFFFFu;                    /* as nv2a_gl: depth * 0xFFFFFF */
        z->flags = flags;
        if (was_open)
            wsurf_pass_begin();
        d->cseq = d->wseq;
        d->cfmt = flags + 1;
    }
    if (w) *w = d->lw / (d->ax ? d->ax : 1);
    if (h) *h = d->lh / (d->ay ? d->ay : 1);
    return d->rgba;
}

/* present_width: the widest clip of the frame, less RECOMP_GL_EDGE_TRIM
 * title columns at the right (the Remix's race frames end in a column or
 * two the scene does not cover). */
static uint32_t ws_present_width(const WsSurf *s)
{
    static int trim = -1;
    uint32_t vw = s->vw ? s->vw : s->w;
    if (trim < 0) {
        const char *e = getenv("RECOMP_GL_EDGE_TRIM");
        trim = e && atoi(e) > 0 ? atoi(e) : 0;
        if (trim > 16) trim = 16;
        if (trim)
            fprintf(stderr, "  [WGPU] %d column(s) trimmed at the right edge (RECOMP_GL_EDGE_TRIM)\n", trim);
    }
    if (trim && vw > 640u + (uint32_t)trim)
        vw -= (uint32_t)trim;
    return vw;
}

void wsurf_frame_end(uint32_t out_w, uint32_t out_h)
{
    static int fill_on = -1, steady_on = -1;
    WgPresent *p;
    WsSurf *s = s_last;
    uint32_t vw, minh;
    int ws43, held43, wide;

    ws_pass_close();
    ws_aspect_poll();
    /* Nothing to show yet: no surface, or only cleared ones (a title clears
     * to black long before its first frame). The canvas keeps what it had. */
    if (!s || !s_drew_any)
        return;
    if (fill_on < 0) {
        const char *e = getenv("RECOMP_GL_FILL");
        fill_on = !(e && *e == '0') && xbox_video_widescreen();
        e = getenv("RECOMP_GL_FILL_STEADY");
        steady_on = e && e[0] == '1';
        fprintf(stderr, "  [WGPU] 4:3 screens filled to 16:9 %s (RECOMP_GL_FILL)%s\n",
                fill_on ? "on" : "off",
                fill_on && steady_on ? ", changes only after 4-5 frames (RECOMP_GL_FILL_STEADY)" : "");
    }
    wide = s->kw > 1.0f;
    vw = wide ? s->w : ws_present_width(s);    /* widened: the whole width (no edge trim) */
    /* A widescreen frame wider than 640 whose middle 640 columns may be a
     * 4:3 screen the title pillarboxed itself (MC3's menus and movies). */
    ws43 = xbox_video_widescreen() && s->aa_sx <= 1 && vw > 640 && s->h >= 400;
    held43 = ws43 && __atomic_load_n(&s_fill_hold, __ATOMIC_RELAXED) > 0;

    /* The canvas: the caller's height (it reads back what we set), at
     * least the title's 480 lines at RECOMP_GL_SCALE, 16:9 in widescreen.
     * Never the caller's width: a held 4:3 frame narrows the canvas and the
     * next frame would keep it. object-fit: contain shows it unstretched. */
    minh = (uint32_t)(480.0 * (s_scale > 0.0 ? s_scale : 1.0) + 0.5);
    (void)out_w;
    if (!out_h) out_h = 720;
    if (out_h < minh) out_h = minh;

    p = (WgPresent *)wg_emit(WG_OP_PRESENT, sizeof *p);
    p->src_tex = s->id;
    p->src_w = wide ? s->pw : ws_to(vw, s->pw, s->w);
    p->src_h = s->ph;
    p->out_h = out_h;
    p->out_w = xbox_video_widescreen() ? (out_h * 16 + 8) / 9 : (out_h * 4 + 2) / 3;
    if (wide)                                   /* Hor+: the screen's shape */
        p->out_w = (uint32_t)(out_h * 16.0 / 9.0 * s->kw + 0.5);
    p->filter = WG_FILTER_LINEAR;
    if (held43) {
        /* Held (nv2a_wgpu_fill_hold: MC3_FILL_SKIP's boot movies): the
         * middle 640 columns at their own 4:3 shape, not stretched with the
         * frame to 16:9. */
        uint32_t x0 = (vw - 640) / 2;
        p->src_x = wsurf_x(x0, s->w, s->pw, s->kw);
        p->src_w = wsurf_x(x0 + 640, s->w, s->pw, s->kw) - p->src_x;
        p->out_w = (out_h * 4 + 2) / 3;
    } else if (fill_on && ws43) {
        /* RECOMP_GL_FILL: if both side bands are black, the middle 640
         * columns fill the width, top and bottom trimmed to keep the shape
         * (27 of 480 lines each). */
        double kx = (double)s->pw / s->w, ky = (double)s->ph / s->h;
        double bandw = (vw - 640) * 0.5;                        /* title columns */
        double pa = (16.0 / 9.0) / ((double)vw / s->h);         /* pixel aspect */
        double keep = 640.0 * pa * 9.0 / 16.0;                  /* lines kept */
        double cy = (s->h - keep) * 0.5;
        if (cy < 0.0) cy = 0.0;
        p->flags = WG_PF_FILL
                   | (steady_on ? WG_PF_STEADY : 0)
                   | (steady_on && __atomic_load_n(&s_fill_freeze, __ATOMIC_RELAXED) > 0
                      ? WG_PF_FREEZE : 0);
        p->crop_x = (uint32_t)(bandw * kx + 0.5);
        p->crop_w = (uint32_t)((vw - bandw) * kx + 0.5) - p->crop_x;
        p->crop_y = (uint32_t)(cy * ky + 0.5);
        p->crop_h = s->ph - 2 * p->crop_y;
        p->band_l = (uint32_t)(bandw * 0.5 * kx);
        p->band_r = (uint32_t)((vw - bandw * 0.5) * kx);
        if (wide) {
            /* Widened: crop_* (the 640 columns' width x k, about the middle)
             * already has the screen's shape, the 4:3 screen at 16:9 between
             * black sides. The bands: the middle of everything black left
             * and right of the 4:3 screen, the storage's own sides included. */
            uint32_t bw = (uint32_t)(bandw + 0.5);
            p->band_l = wsurf_x(bw, s->w, s->pw, s->kw) / 2;
            p->band_r = (wsurf_x(vw - bw, s->w, s->pw, s->kw) + s->pw) / 2;
        }
    }
}

void nv2a_wgpu_fill_hold(int delta) { __atomic_add_fetch(&s_fill_hold, delta, __ATOMIC_RELAXED); }
void nv2a_wgpu_fill_freeze(int delta) { __atomic_add_fetch(&s_fill_freeze, delta, __ATOMIC_RELAXED); }
