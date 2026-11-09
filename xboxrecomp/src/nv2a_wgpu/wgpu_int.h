/*
 * wgpu_int.h -- what the WebGPU back end's C files share (render pthread
 * only, unless marked). Not part of the JS contract (wgpu_proto.h).
 *
 * --- Interfaces between agents (M2, docs/webgpu-plan.md) -----------------
 *
 * Each file has one owner; only the owner edits it. A change to anything in
 * wgpu_proto.h or to a prototype here goes through the lead, who edits these
 * two headers. CMakeLists.txt: each agent adds only its own .c line(s) to
 * add_library.
 *
 *   A  wgsl_psh.c/.h, wgsl_test.c     register combiners -> WGSL fragment
 *                                     module; the host test program
 *                                     (includes B's wgsl_vsh_test.inc)
 *   B  wgsl_vsh.c/.h, wgsl_vsh_test.inc, tools/wgsl_check.sh
 *                                     vertex modules: attribute fetch from
 *                                     the arena (every NV097 type, WgVFetch),
 *                                     pre-transformed and fixed-function +
 *                                     fog; program bodies are M3
 *   C  wgpu_tex.c                     texture cache, hash, texfast, decode,
 *                                     TEX_CREATE/TEX_UPLOAD/SAMPLER, renaming
 *                                     (one write per texture per submit)
 *   D  wgpu_draw.c, wgpu_pipe.c, and the M2 hunks of wgpu_queue.c
 *                                     draw: primitive -> list, arena
 *                                     (vertices, indices, constants,
 *                                     WgVFetch, WgDrawRec), pipeline key,
 *                                     SET_PIPE/SET_TEX/DRAW and pass state;
 *                                     shader/pipeline registries; queue: the
 *                                     whole batch in the draw record (as
 *                                     nv2a_gl's q_draw), the blit record,
 *                                     arena hand-off in WgFrame, partial flush
 *   E  web/wgpu.js                    every op of wgpu_proto.h, the arena
 *                                     buffer and group 0, pipeline / bind
 *                                     group / sampler / shader tables
 *   F  wgpu_surf.c, web/wgpu_fixed.js surfaces and passes, partial clears
 *                                     (CLEAR_RECT), NV062/NV09F blits
 *                                     (COPY_TEX or BLIT), present fill flags;
 *                                     the fixed WGSL
 *
 * Call graph on the render thread, per queued draw (D drives):
 *   wdraw_batch(b)                                          D
 *     wsurf_draw(&b->surface, b->zeta_va)  bind the target  F
 *     wsurf_pass_begin(); t = wsurf_target()                F
 *     wgsl_psh_key(b->regs, &pk)                            A
 *     wtex_stage(b, i, &st[i])  i = 0..3                    C  (may emit
 *       resource ops; may call wsurf_texture)                  F
 *     wpipe_vs(b), wpipe_fs(&pk, dims)                      D  (calls
 *       wgsl_vsh_fixed / wgsl_vsh_program / wgsl_psh once     B / A
 *       per new id, emits SHADER)
 *     wpipe_get(vs, fs, &key) (emits PIPE_CREATE once)      D
 *     wg_arena_alloc(...) for vertices, indices, constants, WgVFetch,
 *       WgDrawRec; on UINT32_MAX: wg_flush_partial(), retry D
 *     pass state if t->pass_seq changed, SET_PIPE, SET_TEX, DRAW
 * At a flip (wgpu_queue.c, D): wsurf_frame_end (F) -> wg_arena_take (D)
 *   -> wgpu_js_frame -> wtex_frame_end (C).
 *
 * JS, E <-> F: web/wgpu_fixed.js exports WGSL strings in $WGPU_FIXED
 * (PRESENT with the fill, CLEAR for CLEAR_RECT, BLIT; ZCONV in M4) and
 * documents, beside each, its bindings and the byte layout of its uniform
 * block. web/wgpu.js makes those pipelines and fills the uniforms from the
 * records exactly as documented there; F changes a layout only together
 * with a note to E.
 */
#ifndef WGPU_INT_H
#define WGPU_INT_H

#include <stddef.h>
#include <stdint.h>
#include "wgpu_proto.h"
#include "../kernel/nv2a_backend.h"
#include "../nv2a_gl/gl_psh.h"          /* Nv2aPshKey */

/* WGSL every generated module starts with (A's and B's): the draw record,
 * group 0 (the arena) and the stage interface. rec is the draw record's
 * index (instance_index), flat, for the fragment stage. */
#define WGSL_VSOUT \
    "struct VsOut {\n" \
    "  @builtin(position) pos: vec4f,\n" \
    "  @location(0) d0: vec4f,\n" \
    "  @location(1) d1: vec4f,\n" \
    "  @location(2) t0: vec4f,\n" \
    "  @location(3) t1: vec4f,\n" \
    "  @location(4) t2: vec4f,\n" \
    "  @location(5) t3: vec4f,\n" \
    "  @location(6) fog: f32,\n" \
    "  @location(7) @interpolate(flat) rec: u32,\n" \
    "};\n"
#define WGSL_COMMON WG_WGSL_DRAWREC WG_WGSL_GROUP0 WGSL_VSOUT

/* ---- wgpu_queue.c (M1; M2 hunks: D) ---- */

extern WgCtl    wg_ctl;          /* the control block JS fills */
extern uint32_t wg_frame;        /* frames ended (flips applied) on the render thread */

/* A zeroed record of `size` bytes (header included, multiple of 4) at the
 * end of the frame's command list, header filled. Valid until the next
 * wg_emit (the list may move). */
void *wg_emit(uint32_t op, uint32_t size);

/* TEX_CREATE with a fresh id (nonzero, never reused); TEX_FREE. */
uint32_t wg_tex_create(uint32_t format, uint32_t w, uint32_t h, uint32_t usage);
void     wg_tex_free(uint32_t id);

/* Mid-frame flush (arena or list full): wsurf_pass_suspend(), send the list
 * so far with WG_FF_PARTIAL and its arena, reset both, wtex_frame_end().
 * The caller then reopens with wsurf_pass_begin() and sends its pass state
 * again (pass_seq changed). */
void wg_flush_partial(void);

/* ---- wgpu_surf.c (F) ---- */

/* Executor-thread values a clear needs from the method shadow, copied into
 * its record: the clear rectangle and the depth format. have = 0 before the
 * first draw (no shadow pointer yet): the whole surface. */
typedef struct {
    uint32_t have, rect_h, rect_v, zmax;
} WsClearRegs;

/* A whole-surface clear becomes the next pass's loadOp; a partial one a
 * CLEAR_RECT in the pass. */
void wsurf_clear(const Nv2aSurface *sf, uint32_t zeta_va, uint32_t flags,
                 uint32_t argb, uint32_t zs, const WsClearRegs *r);
/* Bind sf (+ zeta) as the target of the draws that follow; a different
 * target ends the open pass. Opens nothing by itself. */
void wsurf_draw(const Nv2aSurface *sf, uint32_t zeta_va);
/* Open the pass on the bound target if it is not open. */
void wsurf_pass_begin(void);
/* End the open pass but keep the target: the next wsurf_pass_begin opens
 * it again with LOAD (wg_flush_partial, a COPY_TEX before a draw). */
void wsurf_pass_suspend(void);
/* At a flip: close the pass, then PRESENT the surface to show (if any)
 * into an out_w x out_h canvas, with the WG_PF_* fill fields. */
void wsurf_frame_end(uint32_t out_w, uint32_t out_h);

/* What the open pass draws into (after wsurf_draw + wsurf_pass_begin);
 * NULL when no pass is open. Valid until the next wsurf_* call. */
typedef struct {
    uint32_t color_tex, depth_tex;  /* attachment ids, 0: none */
    uint32_t color_fmt, depth_fmt;  /* WG_FMT_* for WgPipeKey (WG_FMT_NONE: none) */
    uint32_t w, h;                  /* attachment size, pixels (nv2a_gl s->w, s->h) */
    uint32_t pw, ph;                /* the guest surface it stands for, real pixels (s->pw, s->ph) */
    float    scale_x, scale_y;      /* attachment pixels per real pixel (RECOMP_GL_SCALE, square pixels) */
    uint32_t pass_seq;              /* bumped at every BEGIN_PASS: pass state must be sent again */
    float    wide;                  /* Hor+ widening k (RECOMP_ASPECT), 1: none */
} WsTarget;
const WsTarget *wsurf_target(void);

/* Real pixel column x of a surface `real` wide to a column of its `stored`
 * wide attachment. Widened (k > 1, Hor+): the title's picture sits in the
 * middle 1/k (wgsl_vsh nv2a_clip), the surface's edges stay at the
 * attachment's; k = 1: to_stored's rounding. */
uint32_t wsurf_x(uint32_t x, uint32_t real, uint32_t stored, float k);

/* An NV062/NV09F blit, after the executor copied the bytes in guest memory:
 * COPY_TEX at equal scale, BLIT otherwise, between surfaces the GPU holds
 * (a source not on the GPU: nothing to do, the guest copy stands). Ends the
 * open pass. */
void wsurf_blit(const Nv2aBlit *bl);

/* The surface texture that holds guest address va as its pixel (0,0), so a
 * stage sampling a render target reads the GPU copy (nv2a_gl's surface
 * lookup in bind_stage): its id and size in the title's logical pixels
 * (title pixels / anti-aliasing factor, as rt_scale's lw, lh); 0 when none. */
uint32_t wsurf_texture(uint32_t va, uint32_t *w, uint32_t *h);
/* A depth buffer sampled as A8R8G8B8 / A8B8G8R8 (MC3's motion blur): ZCONV
 * into a colour texture; 0 when va is no depth surface. w, h as above. */
uint32_t wsurf_depth_texture(uint32_t va, uint32_t color, uint32_t *w, uint32_t *h);

/* ---- wgpu_tex.c (C) ---- */

typedef struct {
    uint32_t tex;           /* texture id, 0: stage off or not decodable */
    uint32_t smp;           /* sampler id (WG_OP_SAMPLER, C dedupes), 0 with tex 0 */
    uint32_t dim;           /* WG_TEXDIM_* (NONE with tex 0) */
    float    tscale[2];     /* coordinate scale, as nv2a_gl bind_stage's tscale */
} WtStage;

/* Stage i (0..3) of draw b, from b->regs, b->tex_va[i], b->pal_va[i]:
 * look up or decode, emitting TEX_CREATE / TEX_UPLOAD / SAMPLER / TEX_FREE
 * as needed (allowed inside a pass); a texture written a second time in one
 * submit gets a new id. Prefers wsurf_texture(b->tex_va[i]). */
void     wtex_stage(const Nv2aRawBatch *b, int stage, WtStage *out);
/* After each submitted part (frame end or partial flush): the write-once
 * window starts again. */
void     wtex_frame_end(void);
/* Bytes of live texture memory (nv2a_wgpu_texture_bytes). Any thread. */
uint64_t wtex_bytes(void);

/* ---- wgpu_draw.c (D) ---- */

/* One queued draw (its pointers fixed up into the queue record, as
 * nv2a_gl's q_gl_run does), translated into arena data and records. */
void     wdraw_batch(const Nv2aRawBatch *b);
/* Room for `bytes` in the frame arena at a multiple of `align` (16, or
 * sizeof(WgDrawRec) for records), zeroed: its arena byte offset, *out the
 * place to write. UINT32_MAX: full (the caller flushes and retries). */
uint32_t wg_arena_alloc(uint32_t bytes, uint32_t align, void **out);
/* The arena for WgFrame (wasm address, bytes rounded to 16), then reset. */
void     wg_arena_take(uint32_t *ptr, uint32_t *bytes);

/* ---- wgpu_pipe.c (D) ---- */

/* Shader ids, the module emitted (WG_OP_SHADER) the first time: the vertex
 * module b needs (fixed/pre-transformed, or the program's hash), and the
 * fragment module for (k, tex_dim). 0: no module (emitter failed; the draw
 * is dropped, logged once). */
uint32_t wpipe_vs(const Nv2aRawBatch *b);
uint32_t wpipe_fs(const Nv2aPshKey *k, const uint8_t tex_dim[4]);
/* Pipeline id for (vs, fs, key), PIPE_CREATE emitted the first time. */
uint32_t wpipe_get(uint32_t vs, uint32_t fs, const WgPipeKey *key);
uint32_t wpipe_count(void);      /* pipelines made (stats) */
void     wpipe_frame_end(void);  /* pipeline list: prewarm step, OPFS save */

/* ---- wgsl_vsh.c (B) ---- */

/* Whole vertex modules: WGSL_COMMON, the fetch prelude (WgVFetch, every
 * NV097 type), then @vertex fn vs_main(@builtin(vertex_index) v: u32,
 * @builtin(instance_index) rec: u32) -> VsOut, rec copied to VsOut.rec.
 * Return bytes written (a NUL after them, not counted), -1 on overflow.
 * fixed: pre-transformed and fixed-function draws (D[rec].xform 0 / 1),
 * as nv2a_gl_vsh_fixed. program: a vertex program from slot `start` to its
 * FINAL instruction (xform 2), as nv2a_gl_vsh_program + main_program. */
int wgsl_vsh_fixed(char *buf, size_t cap);
int wgsl_vsh_program(const uint32_t (*prog)[4], uint32_t slots, uint32_t start,
                     char *buf, size_t cap);

/* ---- wgsl_psh.c (A) ---- */

/* Nv2aPshKey from the method shadow, as nv2a_gl's psh_key (every field
 * filled: the key is compared with memcmp). */
void wgsl_psh_key(const uint32_t *regs, Nv2aPshKey *k);
/* The whole fragment module for k: WGSL_COMMON, group 1 typed by tex_dim
 * (WG_TEXDIM_*; bindings 0-3 textures, 4-7 samplers), then @fragment fn
 * fs_main(in: VsOut, @builtin(front_facing) ...) -> @location(0) vec4f,
 * as nv2a_gl_psh: combiner constants from K[D[in.rec].pk_off + WG_PK_*],
 * tscale, stage_info and the alpha test from D[in.rec]. Return bytes
 * written (a NUL after them, not counted), -1 on overflow. */
int wgsl_psh(const Nv2aPshKey *k, const uint8_t tex_dim[4], char *buf, size_t cap);

#endif /* WGPU_INT_H */
