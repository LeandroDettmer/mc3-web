/*
 * wgpu_tex.c -- the WebGPU back end's texture cache (browser only; ADR 0011,
 * docs/webgpu-plan.md, M2 agent C). Render pthread only, except wtex_bytes.
 *
 * nv2a_gl's texture code (tex_get, bind_stage) with WebGPU's upload rules:
 *   - Cache: (va, format, size, levels) -> entry, hashed buckets, LRU under
 *     RECOMP_GL_TEX_MB (default 256). Change check as nv2a_gl: a texture
 *     that never changed is hashed (sampled) once per frame; linear images,
 *     and any texture once rewritten, in full; a rewritten one at every use,
 *     or with texfast (RECOMP_GL_TEXFAST, default on) once per segment (a
 *     new vertex epoch or frame). The palette is part of an indexed
 *     texture's content.
 *   - Texfast stage memo: a stage whose nine registers, image and palette are
 *     what it had last, with nothing in between that changes what it binds
 *     (s_gen), gets the same WtStage without a lookup. Surfaces are looked
 *     up every time.
 *   - Decode: everything becomes bgra8unorm (0xAARRGGBB little-endian is
 *     B, G, R, A), written straight into the frame arena: swizzled
 *     A8R8G8B8/X8R8G8B8 and the indices of SZ_I8_A8R8G8B8 through the
 *     toolkit's xbox_unswizzle_rect (d3d8_swizzle.h), the palette lookup
 *     here, every other format through nv2a_backend_decode_texture (the
 *     executor's decoder: linear 32-bit as row copies, 16/8-bit, YUV, DXT).
 *     DXT1/3/5 stay compressed (bc1/2/3-rgba-unorm) when the device has
 *     texture-compression-bc and the size is a multiple of 4 (RECOMP_GL_DXT=0
 *     decodes them). Not decodable: a 1x1 magenta texture, logged once per
 *     format.
 *   - Upload: the data goes into the frame arena (wg_arena_alloc, so one
 *     writeBuffer per frame carries it and the bytes stay put until JS's
 *     writeTexture), then TEX_UPLOAD points at it. Arena full: partial flush,
 *     the pass reopened, one retry.
 *   - One write per texture per submitted part: queue writes land before the
 *     part's commands, so a texture already written in this part and
 *     rewritten gets a new id (the old one freed; TEX_FREE waits for the
 *     submit). Same size and format otherwise: the same id, written again.
 *   - Mip levels (M4, nv2a_gl's mips_wanted): the title's levels follow
 *     level 0 in memory, each swizzled (or DXT) on its own; every format
 *     uploads them as they are (nv2a_gl generates them on the GPU for
 *     formats other than DXT and A8R8G8B8/X8R8G8B8; WebGPU has no
 *     generateMipmap, and the title's own levels are what the NV2A samples).
 *     Single-level, non-power-of-two and linear (pitch) textures stay single
 *     level. RECOMP_GL_MIPS=0: level 0 only.
 *   - Cube maps (M4, format bit 2): six square faces in order +X -X +Y -Y
 *     +Z -Z (WebGPU's layer order), each with its levels, every face's
 *     chain padded to 128 bytes (NV2A_CUBEFACE_ALIGNMENT, as xemu). The
 *     texture is a 6-layer 'cube' (WG_DIM_CUBE); the stage's dim is CUBE.
 *   - TEX_CREATE is emitted here (tex_create): wg_tex_create makes 2D
 *     single-level textures only.
 *   - Samplers: deduped by value. Address modes from SET_TEXTURE_ADDRESS
 *     (wrap, mirror, clamp; border clamp has no WebGPU equivalent: clamp to
 *     edge), filters from SET_TEXTURE_FILTER as nv2a_gl's bind_stage: on a
 *     texture with levels, the mipmapped minification modes go trilinear
 *     (nearest-based ones keep nearest texels) and LINEAR_MIPMAP_LINEAR
 *     anisotropic (RECOMP_GL_ANISO, 1..16, default 16); without levels,
 *     level 0 only (lodMaxClamp 0).
 *
 * Gaps: no LOD bias (RECOMP_GL_LOD_BIAS: a WebGPU sampler has none; needs
 * textureSampleBias in the fragment shader), no border colour, no 3D
 * textures, no depth-as-colour or upscaled movie/UI copies.
 */
#include "nv2a_wgpu.h"
#include "wgpu_int.h"
#include "../kernel/xbox_guest_ptr.h"
#include "../d3d/d3d8_swizzle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Formats ──────────────────────────────────────────────────────── */

/* Swizzled and DXT formats carry their size in the format word; linear
 * images use IMAGE_RECT (nv2a_gl's tex_size_from_format). */
static int size_from_format(uint32_t color)
{
    switch (color) {
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
    case 0x16: case 0x17: case 0x18: case 0x1B: case 0x1C: case 0x1D:
    case 0x1E: case 0x1F: case 0x20: case 0x24: case 0x25: case 0x26:
    case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x34: case 0x35:
    case 0x36: case 0x37: case 0x40: case 0x41: case 0x43: case 0x44:
    case 0x45: case 0x46:
        return 0;
    default:
        return 1;
    }
}

/* Bytes a texel of a swizzled format takes. */
static uint32_t texel_bytes(uint32_t color)
{
    switch (color) {
    case 0x00: case 0x01: case 0x0B: case 0x19:                       /* L8 AY8 I8 A8 */
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05:                       /* 16-bit RGB */
    case 0x1A: case 0x27: case 0x28: case 0x29: case 0x2C: case 0x2D:
        return 2;
    default:
        return 4;
    }
}

static uint32_t dxt_bytes(uint32_t color, uint32_t w, uint32_t h)
{
    return ((w + 3) / 4) * ((h + 3) / 4) * d3d8_format_dxt_block_bytes(color);
}

/* Bytes one level of a swizzled or DXT image takes in guest memory. */
static uint32_t level_bytes(uint32_t color, uint32_t lw, uint32_t lh)
{
    return d3d8_format_dxt_block_bytes(color) ? dxt_bytes(color, lw, lh)
                                              : lw * lh * texel_bytes(color);
}

/* One cube face's chain, padded to NV2A_CUBEFACE_ALIGNMENT. */
static uint32_t face_stride(uint32_t chain)
{
    return (chain + 127u) & ~127u;
}

/* The bytes that are the texture's own: level 0 and the title's levels
 * after it, six faces of them for a cube (nv2a_gl's tex_own_bytes: a
 * neighbour's changes must not make a 1- or 2-byte texture "rewritten"). */
static uint32_t own_bytes(uint32_t color, uint32_t w, uint32_t h, uint32_t pitch,
                          uint32_t levels, int cube)
{
    uint32_t total = 0, l;
    if (pitch)
        return pitch * h;
    if (levels < 1) levels = 1;
    for (l = 0; l < levels && l < 13; l++) {
        uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
        total += level_bytes(color, lw, lh);
        if (lw == 1 && lh == 1)
            break;
    }
    return cube ? face_stride(total) * 6 : total;
}

/* ── Hashes (nv2a_gl's bytes_hash / bytes_hash_full) ─────────────── */

static uint32_t hash_sampled(uint32_t va, uint32_t bytes)
{
    const uint8_t *mem = (const uint8_t *)xbox_ptr(va);
    uint32_t h = 2166136261u ^ bytes, step, i, w;
    if (bytes < 4)
        return h;
    step = bytes / 256;
    if (step < 4) step = 4;
    step &= ~3u;
    for (i = 0; i + 4 <= bytes; i += step) {
        memcpy(&w, mem + i, 4);
        h = (h ^ w) * 16777619u;
    }
    memcpy(&w, mem + bytes - 4, 4);
    return (h ^ w) * 16777619u;
}

static uint32_t hash_full(uint32_t va, uint32_t bytes)
{
    const uint8_t *mem = (const uint8_t *)xbox_ptr(va);
    uint64_t h0 = 1469598103934665603ull ^ bytes, h1 = 0x9E3779B97F4A7C15ull;
    uint32_t i;
    for (i = 0; i + 16 <= bytes; i += 16) {
        uint64_t a, b;
        memcpy(&a, mem + i, 8);
        memcpy(&b, mem + i + 8, 8);
        h0 = (h0 ^ a) * 1099511628211ull;
        h1 = (h1 ^ b) * 0xBF58476D1CE4E5B9ull;
    }
    for (; i < bytes; i++)
        h0 = (h0 ^ mem[i]) * 1099511628211ull;
    h0 ^= h1 * 31u;
    return (uint32_t)(h0 ^ (h0 >> 32));
}

/* ── Cache ────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t va, color, w, h, pitch, levels, cube, hash, bytes;
    uint32_t id, fmt, tw, th;           /* WebGPU texture, WG_FMT_*, its size */
    uint32_t mips, faces;               /* levels and layers it holds (1 or 6) */
    uint32_t used, checked;             /* wg_frame of the last use / hash check */
    uint32_t written;                   /* s_part of the last upload, 0: none */
    uint32_t gen;                       /* hash found unchanged as of s_gen (texfast) */
    int      rewrites;                  /* content changes at this address */
} WtTex;

#define WT_MAX     1024
#define WT_BUCKETS 2048
static WtTex    s_tex[WT_MAX];
static uint16_t s_head[WT_BUCKETS], s_next[WT_MAX];   /* index + 1, chained */
static uint64_t s_bytes;                /* read by wtex_bytes on any thread */
static uint32_t s_part = 1;             /* submitted parts (wtex_frame_end) */
static uint32_t s_gen = 1;              /* anything that changes what a stage binds */
static uint32_t s_pal_reg, s_pal_va;    /* stage being looked up */
static const Nv2aRawBatch *s_batch;     /* draw being translated */

static int texfast(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GL_TEXFAST");
        on = !(e && *e == '0');
    }
    return on;
}

static uint32_t bucket(uint32_t va, uint32_t color, uint32_t w, uint32_t h)
{
    uint32_t k = va ^ (color << 24) ^ (w << 12) ^ (h << 4);
    k ^= k >> 15; k *= 0x2C1B3C6Du; k ^= k >> 12;
    return k & (WT_BUCKETS - 1);
}

static void index_add(WtTex *t)
{
    uint32_t b = bucket(t->va, t->color, t->w, t->h);
    uint16_t i = (uint16_t)(t - s_tex);
    s_next[i] = s_head[b];
    s_head[b] = (uint16_t)(i + 1);
}

static void index_del(WtTex *t)
{
    uint16_t *at = &s_head[bucket(t->va, t->color, t->w, t->h)];
    uint16_t i = (uint16_t)(t - s_tex);
    while (*at) {
        if (*at == i + 1) {
            *at = s_next[i];
            return;
        }
        at = &s_next[*at - 1];
    }
}

static void bytes_add(int64_t d)
{
    __atomic_store_n(&s_bytes, s_bytes + (uint64_t)d, __ATOMIC_RELAXED);
}

/* Drop an entry and its texture (TEX_FREE takes effect after the submit). */
static void entry_free(WtTex *t)
{
    wg_tex_free(t->id);
    bytes_add(-(int64_t)t->bytes);
    index_del(t);
    memset(t, 0, sizeof *t);
    s_gen++;
}

/* t now holds `bytes` of GPU memory; over budget, the least recently used
 * entries not used this frame go (nv2a_gl's tex_account). */
static void account(WtTex *t, uint32_t bytes)
{
    static uint64_t budget;
    if (!budget) {
        const char *e = getenv("RECOMP_GL_TEX_MB");
        budget = (uint64_t)(e && atoi(e) > 0 ? atoi(e) : 256) << 20;
    }
    bytes_add((int64_t)bytes - (int64_t)t->bytes);
    t->bytes = bytes;
    while (s_bytes > budget) {
        WtTex *lru = NULL;
        uint32_t i;
        for (i = 0; i < WT_MAX; i++) {
            WtTex *c = &s_tex[i];
            if (c->id && c != t && c->used != wg_frame && (!lru || c->used < lru->used))
                lru = c;
        }
        if (!lru)
            break;                      /* everything left is in use this frame */
        entry_free(lru);
    }
}

static void mips_poll(void);

/* s_gen, moved on at a new segment (texfast): a new vertex epoch
 * (semaphore release, trap) or frame. */
static uint32_t tex_seg(void)
{
    static uint32_t epoch, frame;
    uint32_t ep = s_batch ? s_batch->vtx_epoch : 0;
    if (ep != epoch || wg_frame != frame) {
        if (wg_frame != frame)
            mips_poll();
        epoch = ep;
        frame = wg_frame;
        s_gen++;
    }
    return s_gen;
}

/* ── Decode and upload ────────────────────────────────────────────── */

/* `bytes` in the frame arena, 16-aligned; a full arena is flushed (the pass
 * reopened on the same target) and asked once more. NULL: does not fit. */
static void *arena_get(uint32_t bytes)
{
    void *p;
    if (wg_arena_alloc(bytes, 16, &p) != UINT32_MAX)
        return p;
    wg_flush_partial();
    wsurf_pass_begin();
    if (wg_arena_alloc(bytes, 16, &p) != UINT32_MAX)
        return p;
    return NULL;
}

/* SZ_I8_A8R8G8B8: 8-bit swizzled indices into the A8R8G8B8 palette named
 * by SET_TEXTURE_PALETTE (length code in [3:2]: 256, 128, 64, 32 entries). */
static int decode_indexed(uint32_t va, uint32_t w, uint32_t h, uint32_t *out)
{
    static uint8_t *idx;
    static size_t cap;
    uint32_t entries = 256u >> ((s_pal_reg >> 2) & 3), pal[256], k;
    if (!(s_pal_reg & ~0x3Fu) || !s_pal_va)
        return 0;
    if ((size_t)w * h > cap) {
        free(idx);
        cap = (size_t)w * h;
        idx = (uint8_t *)malloc(cap);
        if (!idx) {
            cap = 0;
            return 0;
        }
    }
    memcpy(pal, xbox_ptr(s_pal_va), entries * 4);
    xbox_unswizzle_rect(idx, xbox_ptr(va), w, h, 1);
    for (k = 0; k < w * h; k++)
        out[k] = idx[k] < entries ? pal[idx[k]] : pal[0];
    return 1;
}

/* Level 0 of the image as 0xAARRGGBB texels, w * h of them. */
static int decode(uint32_t va, uint32_t color, uint32_t w, uint32_t h, uint32_t pitch,
                  uint32_t *out)
{
    Nv2aTexture nt;
    if (color == 0x06 || color == 0x07) {          /* swizzled A8R8G8B8 / X8R8G8B8 */
        xbox_unswizzle_rect(out, xbox_ptr(va), w, h, 4);
        if (color == 0x07) {
            uint32_t k;
            for (k = 0; k < w * h; k++)
                out[k] |= 0xFF000000u;
        }
        return 1;
    }
    if (color == 0x0B)
        return decode_indexed(va, w, h, out);
    memset(&nt, 0, sizeof nt);
    nt.offset = va; nt.width = w; nt.height = h; nt.pitch = pitch; nt.color = color;
    nt.addr_u = nt.addr_v = 1;
    return nv2a_backend_decode_texture(&nt, out);
}

static int bc_on(void)
{
    static int env = -1;
    if (env < 0) {
        const char *e = getenv("RECOMP_GL_DXT");
        env = !(e && *e == '0');
    }
    return env && (wg_ctl.features & WG_FEAT_BC);
}

/* RECOMP_GL_MIPS (default on); the page's live value (WG_LIVE_MIPS) first. */
static int mips_on(void)
{
    static int on = -1;
    int32_t l = wg_live(WG_LIVE_MIPS);
    if (on < 0) {
        const char *e = getenv("RECOMP_GL_MIPS");
        on = !(e && *e == '0');
    }
    return l ? l == 2 : on;
}

/* A live RECOMP_GL_MIPS change: every cached texture goes, each uploaded
 * again with or without its levels at its next use (TEX_FREE waits for the
 * submit). At a frame's first lookup (tex_seg). */
static void mips_poll(void)
{
    static int last = -1;
    int on = mips_on(), i, n = 0;
    if (last < 0 || on == last) {
        last = on;
        return;
    }
    last = on;
    for (i = 0; i < WT_MAX; i++)
        if (s_tex[i].id) {
            entry_free(&s_tex[i]);
            n++;
        }
    fprintf(stderr, "  [WGPU] mipmaps %s: %d textures uploaded again at their next use (RECOMP_GL_MIPS, in-game"
            " Settings)\n", on ? "on" : "off", n);
}

/* Levels to upload: the title's, down to 1x1, when it has more than one and
 * they can be found in memory (nv2a_gl's mips_wanted); else 1. */
static uint32_t mips_wanted(const WtTex *t)
{
    uint32_t n = 0, w = t->w, h = t->h;
    if (!mips_on() || t->levels < 2 || t->pitch || w < 2 || h < 2
        || (w & (w - 1)) || (h & (h - 1)))
        return 1;
    while (n < t->levels && n < 13) {
        n++;
        if (w == 1 && h == 1)
            break;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return n;
}

/* TEX_CREATE with levels and cube faces, which wg_tex_create (2D, one
 * level) does not take. shortcut: ids from the top half so they never meet
 * wg_tex_create's (counting up from 1); move to a wg_tex_create with mips,
 * layers and dim in wgpu_queue.c when that file is free. */
static uint32_t tex_create(uint32_t fmt, uint32_t w, uint32_t h, uint32_t mips, uint32_t faces)
{
    static uint32_t next = 0x80000000u;
    WgTexCreate *c = (WgTexCreate *)wg_emit(WG_OP_TEX_CREATE, sizeof *c);
    c->id = next++;
    if (!next)
        next = 0x80000000u;
    c->format = fmt;
    c->width = w;
    c->height = h;
    c->layers = faces;
    c->mips = mips;
    c->usage = WG_TU_TEXTURE_BINDING | WG_TU_COPY_DST;
    c->dim = faces == 6 ? WG_DIM_CUBE : WG_DIM_2D;
    return c->id;
}

/* One level (lw x lh at va) into the arena as fmt: *bpr, *rows, *bytes of
 * it. NULL: does not fit; *ok = 0: not decodable (BGRA8 only). */
static void *level_data(uint32_t fmt, uint32_t va, uint32_t color, uint32_t lw, uint32_t lh,
                        uint32_t pitch, uint32_t *bpr, uint32_t *rows, uint32_t *bytes, int *ok)
{
    void *dst;
    *ok = 1;
    if (fmt != WG_FMT_BGRA8UNORM) {                 /* BC: the blocks as they are */
        *bpr = (lw + 3) / 4 * d3d8_format_dxt_block_bytes(color);
        *rows = (lh + 3) / 4;
        *bytes = *bpr * *rows;
        if ((dst = arena_get(*bytes)) != NULL)
            memcpy(dst, xbox_ptr(va), *bytes);
        return dst;
    }
    *bpr = lw * 4;
    *rows = lh;
    *bytes = *bpr * lh;
    if ((dst = arena_get(*bytes)) != NULL)
        *ok = decode(va, color, lw, lh, pitch, (uint32_t *)dst);
    return dst;
}

static void level_upload(uint32_t id, uint32_t mip, uint32_t layer, uint32_t w, uint32_t h,
                         const void *src, uint32_t bpr, uint32_t rows, uint32_t bytes)
{
    WgTexUpload *u = (WgTexUpload *)wg_emit(WG_OP_TEX_UPLOAD, sizeof *u);
    u->id = id;
    u->mip = mip;
    u->layer = layer;
    u->width = w;
    u->height = h;
    u->src_ptr = (uint32_t)(uintptr_t)src;
    u->bytes_per_row = bpr;
    u->rows = rows;
    u->src_bytes = bytes;
}

/* Fill t's texture with the image now in guest memory: every level of
 * every face decoded into the arena and uploaded, level 0 of face 0 first
 * (it decides the format), the texture (re)created if its size, format,
 * levels or faces changed or it was already written in this part. Every
 * later piece is no larger than the first, so once that fits, the rest do
 * (after a partial flush at worst). 0: nothing to bind. */
static int upload(WtTex *t)
{
    uint32_t va = t->va, color = t->color, w = t->w, h = t->h;
    uint32_t block = d3d8_format_dxt_block_bytes(color);
    uint32_t fmt, tw = w, th = h, bpr, rows, bytes, total, stride = 0;
    uint32_t n = mips_wanted(t), nf = t->cube ? 6 : 1, f, l;
    int ok;
    void *dst;

    fmt = !block || !bc_on() || (w & 3) || (h & 3) ? WG_FMT_BGRA8UNORM
        : color == 0x0C ? WG_FMT_BC1_RGBA_UNORM
        : color == 0x0E ? WG_FMT_BC2_RGBA_UNORM : WG_FMT_BC3_RGBA_UNORM;
    if (nf > 1)
        stride = face_stride(own_bytes(color, w, h, 0, t->levels, 0));
    if (!(dst = level_data(fmt, va, color, w, h, t->pitch, &bpr, &rows, &bytes, &ok)))
        goto too_big;
    if (!ok) {
        static uint32_t logged[8];
        if (!(logged[(color & 0xFF) >> 5] & (1u << (color & 31)))) {
            logged[(color & 0xFF) >> 5] |= 1u << (color & 31);
            fprintf(stderr, "  [WGPU] texture format 0x%02X (%ux%u at 0x%08X) not decodable: magenta\n",
                    color, w, h, va);
        }
        tw = th = 1;
        n = nf = 1;
        bpr = 4;
        rows = 1;
        bytes = 4;
        *(uint32_t *)dst = 0xFFFF00FFu;
    }
    if (!t->id || t->fmt != fmt || t->tw != tw || t->th != th || t->mips != n
        || t->faces != nf || t->written == s_part) {
        if (t->id) {
            wg_tex_free(t->id);
            s_gen++;                    /* stages kept with the old id */
        }
        t->id = tex_create(fmt, tw, th, n, nf);
        t->fmt = fmt; t->tw = tw; t->th = th; t->mips = n; t->faces = nf;
    }
    level_upload(t->id, 0, 0, tw, th, dst, bpr, rows, bytes);
    total = bytes;
    for (f = 0; f < nf; f++) {
        uint32_t off = 0;
        for (l = 0; l < n; l++) {
            uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
            if (f || l) {
                dst = level_data(fmt, va + f * stride + off, color, lw, lh, 0,
                                 &bpr, &rows, &bytes, &ok);
                if (!dst)
                    goto too_big;       /* not met: no piece is larger than the first */
                if (!ok)
                    memset(dst, 0, bytes);
                /* BC: whole blocks, the level's size rounded up to 4 */
                level_upload(t->id, l, f, fmt == WG_FMT_BGRA8UNORM ? lw : (lw + 3) & ~3u,
                             fmt == WG_FMT_BGRA8UNORM ? lh : (lh + 3) & ~3u,
                             dst, bpr, rows, bytes);
                total += bytes;
            }
            off += level_bytes(color, lw, lh);
        }
    }
    t->written = s_part;
    account(t, total);
    return 1;

too_big:
    {
        static int once;
        if (!once++)
            fprintf(stderr, "  [WGPU] texture 0x%08X %ux%u (%u bytes) does not fit the frame arena: unbound\n",
                    va, w, h, w * h * 4);
    }
    return 0;
}

/* nv2a_gl's tex_get: the entry for the image, looked up, checked and
 * uploaded as needed. NULL: nothing to bind. */
static WtTex *tex_get(uint32_t va, uint32_t color, uint32_t w, uint32_t h,
                      uint32_t pitch, uint32_t levels, uint32_t cube)
{
    WtTex *t = NULL, *victim = NULL;
    uint32_t i, hash, own;

    for (i = s_head[bucket(va, color, w, h)]; i; i = s_next[i - 1]) {
        WtTex *c = &s_tex[i - 1];
        if (c->id && c->va == va && c->color == color && c->w == w && c->h == h
            && c->levels == levels && c->cube == cube) {
            t = c;
            break;
        }
    }
    /* Never changed: one check a frame. Rewritten before: every use, or
     * with texfast once per segment (the CPU rewrites it only after a fence,
     * which ends the segment). */
    if (t && t->checked == wg_frame && !t->rewrites) {
        t->used = wg_frame;
        return t;
    }
    if (t && t->rewrites && texfast() && t->checked == wg_frame && t->gen == tex_seg()) {
        t->used = wg_frame;
        return t;
    }
    own = own_bytes(color, w, h, pitch, levels, (int)cube);
    if (size_from_format(color) && !(t && t->rewrites))
        hash = hash_sampled(va, own);
    else
        hash = hash_full(va, own);
    if (color == 0x0B && s_pal_va)              /* the palette is content too */
        hash ^= hash_sampled(s_pal_va, 1024) * 31u;
    if (t && t->hash == hash) {
        t->used = t->checked = wg_frame;
        t->gen = tex_seg();
        return t;
    }
    if (t) {
        t->rewrites++;                          /* same image, new content */
        s_gen++;
    } else {
        for (i = 0; i < WT_MAX; i++) {          /* a new texture: a free slot or the LRU */
            WtTex *c = &s_tex[i];
            if (!c->id) {
                if (!victim || victim->id) victim = c;
            } else if (!victim || (victim->id && c->used < victim->used)) {
                victim = c;
            }
        }
        t = victim;
        if (t->id)
            entry_free(t);
        t->va = va; t->color = color; t->w = w; t->h = h; t->levels = levels; t->cube = cube;
        index_add(t);
        s_gen++;
    }
    t->pitch = pitch;
    t->hash = hash;
    t->used = t->checked = wg_frame;
    t->gen = tex_seg();
    if (!upload(t)) {
        if (t->id) {
            entry_free(t);
        } else {
            index_del(t);
            memset(t, 0, sizeof *t);
        }
        return NULL;
    }
    return t;
}

/* ── Samplers ─────────────────────────────────────────────────────── */

/* NV097 address modes (nv2a_gl's wrap_mode): 1 wrap, 2 mirror, 3 clamp to
 * edge, 4 border, 5 clamp (OpenGL's). */
static uint32_t addr_mode(uint32_t m)
{
    switch (m) {
    case 2:  return WG_ADDR_MIRROR;
    case 3: case 4: case 5:
        return WG_ADDR_CLAMP;           /* border clamp: no WebGPU equivalent */
    default: return WG_ADDR_REPEAT;
    }
}

/* RECOMP_GL_ANISO, 1..16 (default 16; JS keeps it to WebGPU's 16 and to
 * all-linear samplers); the page's live value (WG_LIVE_ANISO) first: the
 * level is in the sampler key, so a new one makes new samplers. Logged with
 * the mipmap switch at first use, as nv2a_gl, and at each change. */
static uint32_t aniso(void)
{
    static uint32_t a, env;
    int32_t l = wg_live(WG_LIVE_ANISO);
    uint32_t want;
    if (!env) {
        const char *e = getenv("RECOMP_GL_ANISO");
        int v = e ? atoi(e) : 16;
        env = v < 1 ? 1u : v > 16 ? 16u : (uint32_t)v;
    }
    want = l > 0 ? (l > 16 ? 16u : (uint32_t)l) : env;
    if (want != a) {
        fprintf(stderr, "  [WGPU] mipmaps %s, anisotropic filtering %ux%s\n", mips_on() ? "on" : "off", want,
                a ? " (in-game Settings)" : "");
        a = want;
    }
    return a;
}

/* Sampler id for the stage's ADDRESS and FILTER registers on a texture with
 * (mipped) or without levels, SAMPLER emitted the first time a value is
 * met. Filters as nv2a_gl's bind_stage: SET_TEXTURE_FILTER mag [27:24]
 * (1 nearest), min [23:16] (1 nearest, 2 linear, 3..6 the mipmapped modes).
 * With levels, a mipmapped mode samples trilinearly, NEAREST_MIPMAP_* with
 * nearest texels, LINEAR_MIPMAP_LINEAR anisotropic too; anything else
 * reads level 0 only. */
static uint32_t sampler_get(uint32_t addr, uint32_t filter, int mipped)
{
    static uint32_t keys[256], n;
    uint32_t nmin = (filter >> 16) & 0xFF;
    uint32_t mag = ((filter >> 24) & 0xF) == 1 ? WG_FILTER_NEAREST : WG_FILTER_LINEAR;
    uint32_t mip = mipped && nmin >= 3 && nmin <= 6;
    uint32_t min = nmin == 1 || (mip && (nmin == 3 || nmin == 5)) ? WG_FILTER_NEAREST
                                                                  : WG_FILTER_LINEAR;
    uint32_t an = mip && min == WG_FILTER_LINEAR && mag == WG_FILTER_LINEAR ? aniso() : 1;
    uint32_t au = addr_mode(addr & 0xF), av = addr_mode((addr >> 8) & 0xF);
    uint32_t aw = addr_mode((addr >> 16) & 0xF);
    uint32_t key = au | av << 2 | aw << 4 | mag << 6 | min << 7 | mip << 8 | an << 9 | 0x8000u, i;
    WgSampler *s;

    for (i = 0; i < n; i++)
        if (keys[i] == key)
            return i + 1;
    if (n == 256)
        return 1;                       /* never met: 2 * 3 * 3^3 values at most */
    keys[n++] = key;
    s = (WgSampler *)wg_emit(WG_OP_SAMPLER, sizeof *s);
    s->id = n;
    s->addr_u = au; s->addr_v = av; s->addr_w = aw;
    s->mag = mag; s->min = min;
    s->mip = mip ? WG_FILTER_LINEAR : WG_FILTER_NEAREST;
    s->max_aniso = an;
    s->lod_min = 0.0f;
    s->lod_max = mip ? 32.0f : 0.0f;
    return n;
}

/* ── Stages ───────────────────────────────────────────────────────── */

/* What each stage got last (texfast): kept while s_gen stays and the
 * stage's nine registers, image and palette are the same. Only stages from
 * tex_get or unbound; surfaces are looked up every time. */
typedef struct {
    uint32_t gen, va, pal, reg[9];
    WtStage  st;
} WtMemo;
static WtMemo s_memo[4];

static void stage_keep(int i, const uint32_t *r, const WtStage *st)
{
    WtMemo *m = &s_memo[i];
    if (!texfast())
        return;
    m->gen = s_gen;
    m->va = s_batch->tex_va[i];
    m->pal = s_batch->pal_va[i];
    memcpy(m->reg, r, sizeof m->reg);
    m->st = *st;
}

void wtex_stage(const Nv2aRawBatch *b, int i, WtStage *out)
{
    const uint32_t *regs = b->regs;
    uint32_t base = (0x1B00u + (uint32_t)i * 0x40u) / 4;
    uint32_t control0 = regs[base + 3], format = regs[base + 1];
    uint32_t color = (format >> 8) & 0xFF;
    uint32_t va, w, h, pitch = 0, sw = 0, sh = 0, id, cube;
    WtTex *t;

    s_batch = b;
    if (texfast()) {
        WtMemo *m = &s_memo[i];
        if (m->gen == tex_seg() && m->va == b->tex_va[i] && m->pal == b->pal_va[i]
            && !memcmp(m->reg, regs + base, sizeof m->reg)) {
            *out = m->st;
            return;
        }
        m->gen = 0;
    }
    memset(out, 0, sizeof *out);
    out->tscale[0] = out->tscale[1] = 1.0f;
    if (!(control0 & 0x40000000u) || !regs[base] || !b->tex_va[i]) {
        stage_keep(i, regs + base, out);
        return;
    }
    va = b->tex_va[i];
    if (size_from_format(color)) {
        w = 1u << ((format >> 20) & 0xF);
        h = 1u << ((format >> 24) & 0xF);
    } else {
        uint32_t rect = regs[base + 7];
        w = rect >> 16;
        h = rect & 0xFFFF;
        pitch = regs[base + 4] >> 16;
        if (w) out->tscale[0] = 1.0f / (float)w;
        if (h) out->tscale[1] = 1.0f / (float)h;
    }
    if (!w || !h || w > 4096 || h > 4096) {
        out->tscale[0] = out->tscale[1] = 1.0f;
        stage_keep(i, regs + base, out);
        return;
    }
    /* SET_TEXTURE_FORMAT bit 2: a cube map (square, swizzled or DXT). */
    cube = (format & 0x4u) && !pitch && w == h;
    /* A render target sampled as a texture: the GPU copy (nv2a_gl's
     * surf_find + rt_scale). sw, sh: the image the surface holds in the
     * title's logical pixels; the title's coordinates cover w x h of it. */
    /* else a depth buffer read as colour (motion blur): ZCONV'd copy. */
    if (!cube && ((id = wsurf_texture(va, &sw, &sh)) != 0
                  || (id = wsurf_depth_texture(va, color, &sw, &sh)) != 0)) {
        if (sw && w < sw) out->tscale[0] *= (float)w / (float)sw;
        if (sh && h < sh) out->tscale[1] *= (float)h / (float)sh;
        out->tex = id;
        out->dim = WG_TEXDIM_2D;
        out->smp = sampler_get(regs[base + 2], regs[base + 5], 0);
        return;
    }
    s_pal_reg = regs[base + 8];                 /* SET_TEXTURE_PALETTE */
    s_pal_va = b->pal_va[i];
    t = tex_get(va, color, w, h, pitch, (format >> 16) & 0xF, cube);
    if (t) {
        out->tex = t->id;
        out->dim = t->faces == 6 ? WG_TEXDIM_CUBE : WG_TEXDIM_2D;
        out->smp = sampler_get(regs[base + 2], regs[base + 5], t->mips > 1);
    } else {
        out->tscale[0] = out->tscale[1] = 1.0f;
    }
    stage_keep(i, regs + base, out);
}

void wtex_frame_end(void)
{
    s_part++;
}

uint64_t wtex_bytes(void)
{
    return __atomic_load_n(&s_bytes, __ATOMIC_RELAXED);
}
