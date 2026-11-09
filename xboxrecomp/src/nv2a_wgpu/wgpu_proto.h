/*
 * wgpu_proto.h -- the contract between the WebGPU back end's C side
 * (xboxrecomp/src/nv2a_wgpu/, on the render pthread) and its JS encoder
 * (web/wgpu.js, --js-library, the same worker). ADR 0011, docs/webgpu-plan.md.
 *
 * Structure (a control block the encoder fills, a frame header handed over
 * per frame, fixed-size records with a common header, offsets pinned by
 * static asserts and mirrored in JS, a per-draw record the shaders index
 * with firstInstance) follows nfsu2-web's runtime/web/gpu_proto.h by th0mk
 * (MIT).
 *
 * Everything here lives in the shared wasm memory. Only fixed-width fields:
 * wasm addresses are uint32_t (wasm32), never pointers, so this header has
 * the same layout on a 64-bit host (wgsl_test, the compile check).
 *
 * --- Hand-off -------------------------------------------------------------
 *
 *   1. Once, on the render pthread (the canvas owner): wgpu_js_init(&ctl).
 *      JS sets ctl.dev_state = WG_DEV_PENDING at once, then asynchronously
 *      requests the adapter and device, configures
 *      getContext('webgpu') on the transferred OffscreenCanvas, fills the
 *      device fields and stores WG_DEV_READY (or WG_DEV_FAILED) with
 *      Atomics.store + Atomics.notify. The promises only resolve once the
 *      render thread returns to its event loop (emscripten_set_main_loop
 *      turns), so the render thread never blocks on dev_state; the executor
 *      thread (Nv2aBackend.prepare) may futex-wait on it, with a timeout.
 *   2. Per frame, on the render pthread: C writes the command list (records
 *      below, back to back) and the frame's arena into its own wasm buffers,
 *      fills a WgFrame and calls wgpu_js_frame(&frame). JS first copies the
 *      arena into its GPU buffer (one queue.writeBuffer), then walks
 *      [cmd_ptr, cmd_ptr + cmd_bytes), encodes, submits, and returns.
 *      queue.writeBuffer/writeTexture copy at call time, so every buffer
 *      (list, arena, WgFrame, upload sources) is C's again as soon as the
 *      call returns: one set of buffers is enough.
 *   3. Pacing: JS bumps ctl.frames_submitted at each submit and
 *      ctl.frames_done from onSubmittedWorkDone (Atomics.add + notify).
 *      In flight = submitted - done. The render thread does not start a
 *      frame while in flight >= ctl.max_in_flight; it returns to its event
 *      loop instead and tries on the next turn. It must NOT futex-wait for
 *      frames_done: the callback that bumps it runs on that same event loop
 *      (deadlock). The executor stays RECOMP_GL_QUEUE_FRAMES flips ahead of
 *      the render thread, as with nv2a_gl's q_flip.
 *
 * --- Command list rules ---------------------------------------------------
 *
 *   - Every record starts with WgCmd: op (WG_OP_*) and size, the record's
 *     total bytes including the header, a multiple of 4. JS advances by
 *     size, so it skips ops it does not know.
 *   - Records only ever grow at their end (WG_VERSION bumps). A reader that
 *     wants a later field checks size first. Opcode numbers never change:
 *     new ops take a free number in their range (below).
 *   - Where an op may sit (JS logs a misplaced one once and drops it):
 *       encoder ops, outside a pass: BEGIN_PASS, PRESENT, COPY_TEX, BLIT,
 *         ZCONV (each of the last four is its own copy or pass);
 *       pass ops, between BEGIN_PASS and END_PASS: 0x04-0x0F and 0x30-0x3F;
 *       resource ops, anywhere: 0x10-0x2F (device and queue calls, not
 *         encoder calls).
 *   - Resource ops take effect for the WHOLE submit they are in: queue
 *     writes (TEX_UPLOAD, the arena) land before any command of that
 *     submit. So C never writes one texture twice in one frame: the second
 *     write goes to a new id (wgpu_tex.c renames). Created objects (TEX,
 *     SAMPLER, SHADER, PIPE) may be used by any later record.
 *   - TEX_FREE takes effect after the frame's submit (JS defers destroy():
 *     a texture destroyed before submit fails the whole command buffer).
 *   - Pass state is WebGPU's: every BEGIN_PASS starts with the full-target
 *     viewport and scissor, blend constant 0, stencil reference 0 and no
 *     pipeline or group 1. JS binds group 0 (the arena) itself at every
 *     BEGIN_PASS. C sends the rest again after each BEGIN_PASS before the
 *     first DRAW.
 *   - At most one PRESENT per frame, outside a pass. The frame ends at the
 *     end of the list: JS finishes the encoder and submits once (twice for a
 *     list with WG_FF_PARTIAL set: no present, more of the frame follows;
 *     each part has its own arena).
 *   - Ids (textures, samplers, shaders, pipelines) are C's: nonzero, unique
 *     among live objects of their kind, one id space per kind. Id 0 means
 *     "none" (no attachment, an unbound stage).
 *   - Clear colours are 0..1 floats in r, g, b, a order (C converts the
 *     NV2A's ARGB); clear depth is 0..1, clear stencil 0..255.
 *
 * --- Frame arena (WgFrame.arena_ptr / arena_bytes) ------------------------
 *
 *   One GPU buffer (usage STORAGE | INDEX | COPY_DST; JS grows it, a power
 *   of two, when arena_bytes exceeds it) gets the arena with one writeBuffer
 *   per WgFrame. Group 0 binds that buffer whole, from byte 0, three times
 *   (WG_WGSL_GROUP0): A as array<u32>, K as array<vec4f>, D as
 *   array<WgDrawRec>. Every offset below is therefore absolute in the arena:
 *     - draw records: at byte offsets that are multiples of
 *       sizeof(WgDrawRec); DRAW.record = offset / sizeof(WgDrawRec), passed
 *       as firstInstance, read by WGSL as D[instance_index];
 *     - constants and attribute defaults: 16-byte aligned rows, K[i];
 *       WgDrawRec.k_off (raw transform constant 0, 192 rows),
 *       .pk_off (WG_PK_ROWS rows of combiner constants), .vdef_off (16 rows)
 *     - vertex-fetch descriptors (WgVFetch): 16-byte aligned,
 *       WgDrawRec.vf_off = byte offset / 4 (an index into A);
 *     - vertex bytes: 4-byte aligned runs as the title stored them;
 *       WgDrawRec.vtx_base = byte offset of gathered vertex 0;
 *     - indices: u32, setIndexBuffer(arena, 'uint32', 0) once per pass;
 *       DRAW.first_index = byte offset / 4. Index values count from the
 *       draw's vertex 0 (vertex_index in WGSL; base vertex is always 0).
 *   Order and packing beyond that are C's (wgpu_draw.c).
 *
 * --- JS mirror (web/wgpu.js must use exactly these byte offsets) ----------
 *
 *   WgCtl  (84 bytes)  magic 0, version 4, size 8, dev_state 12 (i32, atomic),
 *          canvas_format 16, features 20, max_tex_2d 24, max_storage_binding 28,
 *          max_in_flight 32 (C), frames_submitted 36 (atomic),
 *          frames_done 40 (atomic, notified), frames_presented 44,
 *          canvas_w 48, canvas_h 52, st_cmds 56, st_passes 60, st_draws 64,
 *          st_enc_us 68, st_errors 72, st_pipelines 76, st_tex_live 80
 *   WgFrame (48 bytes) magic 0, version 4, size 8, seq 12, flags 16,
 *          cmd_ptr 20, cmd_bytes 24, cmd_count 28, arena_ptr 32,
 *          arena_bytes 36, translate_us 40, draws 44
 *   WgCmd   (4)  u32 word 0: op = w & 0xFFFF, size = w >>> 16
 *   0x01 BEGIN_PASS (60): color_tex 4, depth_tex 8, color_load 12,
 *          color_store 16, depth_load 20, depth_store 24, stencil_load 28,
 *          stencil_store 32, clear_color 36 (f32 x4: r 36, g 40, b 44, a 48),
 *          clear_depth 52 (f32), clear_stencil 56
 *   0x02 END_PASS    (4): header only
 *   0x03 PRESENT    (68): src_tex 4, src_x 8, src_y 12, src_w 16, src_h 20,
 *          out_w 24, out_h 28, filter 32, flags 36 (WG_PF_*), crop_x 40,
 *          crop_y 44, crop_w 48, crop_h 52, band_l 56, band_r 60,
 *          sharp 64 (f32)
 *   0x04 SET_VIEWPORT (28): x 4, y 8, width 12, height 16, min_depth 20, max_depth 24
 *          (all f32)
 *   0x05 SET_SCISSOR (20): x 4, y 8, width 12, height 16
 *   0x06 SET_BLEND_CONST (20): color 4 (f32 x4: r 4, g 8, b 12, a 16)
 *   0x07 SET_STENCIL_REF (8): ref 4
 *   0x08 CLEAR_RECT (48): x 4, y 8, width 12, height 16, flags 20 (WG_CR_*),
 *          color 24 (f32 x4: r 24, g 28, b 32, a 36), depth 40 (f32),
 *          stencil 44
 *   0x10 TEX_CREATE (36): id 4, format 8, width 12, height 16, layers 20,
 *          mips 24, usage 28, dim 32
 *   0x11 TEX_FREE    (8): id 4
 *   0x12 TEX_UPLOAD (48): id 4, mip 8, layer 12, x 16, y 20, width 24, height 28,
 *          src_ptr 32, bytes_per_row 36, rows 40, src_bytes 44
 *   0x13 SAMPLER    (44): id 4, addr_u 8, addr_v 12, addr_w 16, mag 20,
 *          min 24, mip 28, max_aniso 32, lod_min 36 (f32), lod_max 40 (f32)
 *   0x20 SHADER     (20): id 4, src_ptr 8, src_bytes 12, stages 16
 *   0x21 PIPE_CREATE (44): id 4, vs 8, fs 12, key 16 (WgPipeKey, bytes:
 *          topology 16, cull 17, front_face 18, unclipped_depth 19,
 *          color_fmt 20, depth_fmt 21, color_mask 22, blend_enable 23,
 *          blend_src 24, blend_dst 25, blend_op 26, depth_write 27,
 *          depth_cmp 28, stencil_enable 29, stencil_cmp 30, stencil_fail 31,
 *          stencil_zfail 32, stencil_pass 33, stencil_read_mask 34,
 *          stencil_write_mask 35, tex_dim 36..39 (stage 0..3), 40..43 zero)
 *   0x30 SET_PIPE    (8): id 4
 *   0x31 SET_TEX    (36): tex 4 (x4: 4, 8, 12, 16), smp 20 (x4: 20, 24, 28, 32)
 *   0x32 DRAW       (16): first_index 4, index_count 8, record 12
 *   0x40 COPY_TEX   (52): src 4, dst 8, src_x 12, src_y 16, dst_x 20,
 *          dst_y 24, width 28, height 32, src_mip 36, dst_mip 40, src_layer 44,
 *          dst_layer 48
 *   0x41 BLIT       (48): src 4, dst 8, sx 12, sy 16, sw 20, sh 24, dx 28,
 *          dy 32, dw 36, dh 40, filter 44
 *   0x42 ZCONV      (28): src 4, dst 8, width 12, height 16, zmax 20, flags 24
 *   WgDrawRec (304) and WgVFetch (128) are read by WGSL only (WG_WGSL_*
 *   below), never by JS.
 *
 * All fields u32 unless marked; every offset of a u32/f32 is a multiple of
 * 4, so JS reads them as HEAPU32[(p + off) >> 2] / HEAPF32[(p + off) >> 2]
 * (WgPipeKey: HEAPU8[p + off]). With memory growth, JS takes fresh views on
 * wasmMemory.buffer at each call.
 */
#ifndef WGPU_PROTO_H
#define WGPU_PROTO_H

#include <stdint.h>
#include <stddef.h>

#define WG_MAGIC   0x55504757u          /* "WGPU" little-endian */
#define WG_VERSION 2u                   /* bump on any change below; JS refuses another.
                                         * 2: M2 (draws, textures, pipelines, copies) */

/* ---- Opcodes: ranges reserved so later milestones never renumber ------- */

/* 0x00        NOP (size bytes of padding)
 * 0x01-0x0F   frame and pass control, dynamic pass state
 * 0x10-0x1F   textures and samplers
 * 0x20-0x2F   shaders and pipelines
 * 0x30-0x3F   draw state and draws (inside a pass only)
 * 0x40-0x4F   copies, blits, depth-as-colour
 * 0x50-0x5F   buffers and the vertex pool (M5)
 * 0x60-0x6F   debug, timestamps, read-back (M4/M5)
 * 0x70-0xFFFF unassigned
 * Ops marked "reserved" have their number fixed but no record layout yet:
 * the lead defines it in the milestone that needs it. */
enum {
    WG_OP_NOP          = 0x00,

    WG_OP_BEGIN_PASS   = 0x01,  /* WgBeginPass (M1) */
    WG_OP_END_PASS     = 0x02,  /* WgEndPass (M1) */
    WG_OP_PRESENT      = 0x03,  /* WgPresent (M1; fill fields M2) */
    WG_OP_SET_VIEWPORT = 0x04,  /* WgSetViewport (M2) */
    WG_OP_SET_SCISSOR  = 0x05,  /* WgSetScissor (M2) */
    WG_OP_SET_BLEND_CONST = 0x06, /* WgSetBlendConst (M2) */
    WG_OP_SET_STENCIL_REF = 0x07, /* WgSetStencilRef (M2; used from M3) */
    WG_OP_CLEAR_RECT   = 0x08,  /* WgClearRect (M2): partial clear as a quad */

    WG_OP_TEX_CREATE   = 0x10,  /* WgTexCreate (M1) */
    WG_OP_TEX_FREE     = 0x11,  /* WgTexFree (M1) */
    WG_OP_TEX_UPLOAD   = 0x12,  /* WgTexUpload (M2): writeTexture from wasm memory */
    WG_OP_SAMPLER      = 0x13,  /* WgSampler (M2) */

    WG_OP_SHADER       = 0x20,  /* WgShader (M2): WGSL module from wasm memory */
    WG_OP_PIPE_CREATE  = 0x21,  /* WgPipeCreate (M2) */

    WG_OP_SET_PIPE     = 0x30,  /* WgSetPipe (M2) */
    WG_OP_SET_TEX      = 0x31,  /* WgSetTex (M2): group 1 */
    WG_OP_DRAW         = 0x32,  /* WgDraw (M2): drawIndexed, firstInstance = record */

    WG_OP_COPY_TEX     = 0x40,  /* WgCopyTex (M2): copyTextureToTexture */
    WG_OP_BLIT         = 0x41,  /* WgBlit (M2): scaled copy as a quad */
    WG_OP_ZCONV        = 0x42,  /* WgZconv (layout M2, used M4): depth -> RGBA8 */

    WG_OP_COPY_BUF     = 0x50,  /* reserved (M5): arena -> vertex pool */
};

/* ---- Enums carried in records ------------------------------------------ */

/* Texture formats (WgTexCreate.format, WgCtl.canvas_format, WgPipeKey
 * color_fmt / depth_fmt). JS maps them to GPUTextureFormat strings in this
 * order. */
enum {
    WG_FMT_NONE                 = 0,
    WG_FMT_BGRA8UNORM           = 1,  /* 'bgra8unorm': surfaces, decoded textures, canvas */
    WG_FMT_RGBA8UNORM           = 2,  /* 'rgba8unorm' (also a possible canvas format) */
    WG_FMT_DEPTH24PLUS_STENCIL8 = 3,  /* 'depth24plus-stencil8': zeta */
    WG_FMT_DEPTH32FLOAT         = 4,  /* 'depth32float' */
    WG_FMT_R8UNORM              = 5,  /* 'r8unorm' */
    WG_FMT_RG8UNORM             = 6,  /* 'rg8unorm' */
    WG_FMT_BC1_RGBA_UNORM       = 7,  /* 'bc1-rgba-unorm' (WG_FEAT_BC) */
    WG_FMT_BC2_RGBA_UNORM       = 8,  /* 'bc2-rgba-unorm' (WG_FEAT_BC) */
    WG_FMT_BC3_RGBA_UNORM       = 9,  /* 'bc3-rgba-unorm' (WG_FEAT_BC) */
    WG_FMT_RGBA16FLOAT          = 10, /* 'rgba16float' */
    WG_FMT_COUNT
};

/* WgTexCreate.usage: GPUTextureUsage's own bit values, passed through. */
#define WG_TU_COPY_SRC          0x01u
#define WG_TU_COPY_DST          0x02u
#define WG_TU_TEXTURE_BINDING   0x04u
#define WG_TU_STORAGE_BINDING   0x08u
#define WG_TU_RENDER_ATTACHMENT 0x10u

/* WgTexCreate.dim */
enum { WG_DIM_2D = 0, WG_DIM_CUBE = 1 };   /* cube: layers = 6 */

/* Load and store ops (WgBeginPass). JS leaves out the stencil ops for a
 * depth format without stencil, as WebGPU requires. */
enum { WG_LOAD_LOAD = 0, WG_LOAD_CLEAR = 1 };
enum { WG_STORE_STORE = 0, WG_STORE_DISCARD = 1 };

/* WgPresent.filter, WgBlit.filter, WgSampler.mag/min/mip */
enum { WG_FILTER_NEAREST = 0, WG_FILTER_LINEAR = 1 };

/* WgPresent.flags. The 4:3 fill (nv2a_gl's present_fxaa / fill_steady_pass,
 * RECOMP_GL_FILL): the picture is src_*; its 4:3 middle is crop_*; band_l
 * and band_r are source columns inside the left and right side bands. With
 * WG_PF_FILL, crop_* is shown full size when both bands are black from top
 * to bottom (decided on the GPU, no read-back); with WG_PF_STEADY too, that
 * decision goes through JS's steady state (fill_steady_pass: it changes only
 * after the bands hold for some frames); WG_PF_FREEZE keeps the steady
 * decision as it is (nv2a_wgpu_fill_freeze). No WG_PF_FILL: src_* as is
 * (RECOMP_GL_FILL=0, MC3_FILL_SKIP, nv2a_wgpu_fill_hold > 0, AA surfaces). */
#define WG_PF_FILL   0x1u
#define WG_PF_STEADY 0x2u
#define WG_PF_FREEZE 0x4u

/* WgClearRect.flags: what to clear; bits 8-11 the colour write mask
 * (WG_CM_*), as SET_COLOR_MASK limits a colour clear on the NV2A. */
#define WG_CR_COLOR   0x1u
#define WG_CR_DEPTH   0x2u
#define WG_CR_STENCIL 0x4u
#define WG_CR_MASK_SHIFT 8

/* WgSampler.addr_* (NV097 wrap 1 wrap, 2 mirror, 3/4 clamp; border clamp
 * has no WebGPU equivalent: clamp-to-edge) */
enum { WG_ADDR_REPEAT = 0, WG_ADDR_MIRROR = 1, WG_ADDR_CLAMP = 2 };

/* WgShader.stages: entry points the module has */
#define WG_SH_VERTEX   0x1u     /* @vertex fn vs_main */
#define WG_SH_FRAGMENT 0x2u     /* @fragment fn fs_main */

/* WgPipeKey.topology */
enum { WG_TOPO_POINT_LIST = 0, WG_TOPO_LINE_LIST = 1, WG_TOPO_TRIANGLE_LIST = 2 };
/* WgPipeKey.cull / front_face */
enum { WG_CULL_NONE = 0, WG_CULL_FRONT = 1, WG_CULL_BACK = 2 };
enum { WG_FACE_CCW = 0, WG_FACE_CW = 1 };
/* WgPipeKey.color_mask, WG_CR_MASK: GPUColorWrite's own bits */
#define WG_CM_R 0x1u
#define WG_CM_G 0x2u
#define WG_CM_B 0x4u
#define WG_CM_A 0x8u
/* GPUBlendFactor in WebGPU's order. NV2A's CONSTANT_ALPHA /
 * ONE_MINUS_CONSTANT_ALPHA have no equivalent: C maps them to CONSTANT /
 * ONE_MINUS_CONSTANT with the blend constant's rgb set to its alpha. */
enum {
    WG_BF_ZERO = 0, WG_BF_ONE, WG_BF_SRC, WG_BF_ONE_MINUS_SRC, WG_BF_SRC_ALPHA,
    WG_BF_ONE_MINUS_SRC_ALPHA, WG_BF_DST, WG_BF_ONE_MINUS_DST, WG_BF_DST_ALPHA,
    WG_BF_ONE_MINUS_DST_ALPHA, WG_BF_SRC_ALPHA_SATURATED, WG_BF_CONSTANT,
    WG_BF_ONE_MINUS_CONSTANT,
};
/* GPUBlendOperation */
enum { WG_BO_ADD = 0, WG_BO_SUBTRACT, WG_BO_REVERSE_SUBTRACT, WG_BO_MIN, WG_BO_MAX };
/* GPUCompareFunction in NV2A/GL order: GL value - 0x200 */
enum {
    WG_CMP_NEVER = 0, WG_CMP_LESS, WG_CMP_EQUAL, WG_CMP_LESS_EQUAL, WG_CMP_GREATER,
    WG_CMP_NOT_EQUAL, WG_CMP_GREATER_EQUAL, WG_CMP_ALWAYS,
};
/* GPUStencilOperation */
enum {
    WG_SO_KEEP = 0, WG_SO_ZERO, WG_SO_REPLACE, WG_SO_INVERT, WG_SO_INCR_CLAMP,
    WG_SO_DECR_CLAMP, WG_SO_INCR_WRAP, WG_SO_DECR_WRAP,
};
/* WgPipeKey.tex_dim[i]: the stage's binding type in group 1 */
enum { WG_TEXDIM_NONE = 0, WG_TEXDIM_2D = 1, WG_TEXDIM_CUBE = 2 };

/* WgZconv.flags */
#define WG_ZC_SWAP_RB 0x1u      /* destination read as A8B8G8R8: red and blue swapped */

/* WgCtl.dev_state */
enum {
    WG_DEV_NONE    = 0,   /* wgpu_js_init not called yet */
    WG_DEV_PENDING = 1,   /* adapter/device requested */
    WG_DEV_READY   = 2,   /* device and canvas context configured */
    WG_DEV_FAILED  = 3,   /* no adapter/device: the page falls back to WebGL on reload */
    WG_DEV_LOST    = 4,   /* device lost after READY: JS reloads with RECOMP_GPU=webgl */
};

/* WgCtl.features (JS, from the device it got) */
#define WG_FEAT_BC                 0x1u   /* texture-compression-bc */
#define WG_FEAT_DEPTH_CLIP_CONTROL 0x2u   /* unclippedDepth */
#define WG_FEAT_TIMESTAMP          0x4u   /* timestamp-query */

/* WgFrame.flags */
#define WG_FF_PARTIAL 0x1u   /* mid-frame flush: submit, no PRESENT, the frame goes on */

/* ---- Control block: one per process, owned by C, filled mostly by JS --- */

typedef struct {
    uint32_t magic;            /*  0 C: WG_MAGIC */
    uint32_t version;          /*  4 C: WG_VERSION */
    uint32_t size;             /*  8 C: sizeof(WgCtl) */
    int32_t  dev_state;        /* 12 JS: WG_DEV_*, Atomics.store + notify */
    uint32_t canvas_format;    /* 16 JS: WG_FMT_* from getPreferredCanvasFormat() */
    uint32_t features;         /* 20 JS: WG_FEAT_* */
    uint32_t max_tex_2d;       /* 24 JS: limits.maxTextureDimension2D */
    uint32_t max_storage_binding; /* 28 JS: limits.maxStorageBufferBindingSize (clamped to 2^32-1) */
    uint32_t max_in_flight;    /* 32 C: frames allowed in flight (2) */
    int32_t  frames_submitted; /* 36 JS: queue.submit count (frame ends, partials not counted); Atomics.add */
    int32_t  frames_done;      /* 40 JS: onSubmittedWorkDone count; Atomics.add + notify */
    uint32_t frames_presented; /* 44 JS: PRESENTs drawn */
    uint32_t canvas_w;         /* 48 JS: canvas size in use */
    uint32_t canvas_h;         /* 52 */
    uint32_t st_cmds;          /* 56 JS: records walked (total) */
    uint32_t st_passes;        /* 60 JS: render passes (total, present included) */
    uint32_t st_draws;         /* 64 JS: draw calls (total) */
    uint32_t st_enc_us;        /* 68 JS: wgpu_js_frame time (total, microseconds, wraps) */
    uint32_t st_errors;        /* 72 JS: uncapturederror events */
    uint32_t st_pipelines;     /* 76 JS: pipelines created */
    uint32_t st_tex_live;      /* 80 JS: live textures */
} WgCtl;

/* ---- Frame header: one per wgpu_js_frame call --------------------------- */

typedef struct {
    uint32_t magic;            /*  0 WG_MAGIC */
    uint32_t version;          /*  4 WG_VERSION */
    uint32_t size;             /*  8 sizeof(WgFrame) */
    uint32_t seq;              /* 12 frame number (C's flip count) */
    uint32_t flags;            /* 16 WG_FF_* */
    uint32_t cmd_ptr;          /* 20 wasm address of the first record */
    uint32_t cmd_bytes;        /* 24 bytes of records */
    uint32_t cmd_count;        /* 28 records (a check: JS logs a mismatch once) */
    uint32_t arena_ptr;        /* 32 wasm address of the frame arena (16-byte aligned; 0: none) */
    uint32_t arena_bytes;      /* 36 arena bytes to writeBuffer (multiple of 16) */
    uint32_t translate_us;     /* 40 C time spent building this list, for RECOMP_WGPU_STATS */
    uint32_t draws;            /* 44 DRAW records in the list */
} WgFrame;

/* ---- Records ------------------------------------------------------------ */

typedef struct {
    uint16_t op;               /* WG_OP_* */
    uint16_t size;             /* total record bytes, header included, multiple of 4 */
} WgCmd;

typedef struct {               /* WG_OP_BEGIN_PASS */
    WgCmd    h;                /*  0 */
    uint32_t color_tex;        /*  4 texture id, 0 = no colour attachment */
    uint32_t depth_tex;        /*  8 texture id, 0 = no depth/stencil attachment */
    uint32_t color_load;       /* 12 WG_LOAD_* */
    uint32_t color_store;      /* 16 WG_STORE_* */
    uint32_t depth_load;       /* 20 */
    uint32_t depth_store;      /* 24 */
    uint32_t stencil_load;     /* 28 */
    uint32_t stencil_store;    /* 32 */
    float    clear_color[4];   /* 36 r, g, b, a in 0..1 */
    float    clear_depth;      /* 52 0..1 */
    uint32_t clear_stencil;    /* 56 0..255 */
} WgBeginPass;

typedef struct {               /* WG_OP_END_PASS */
    WgCmd    h;                /*  0 */
} WgEndPass;

typedef struct {               /* WG_OP_PRESENT: draw src into getCurrentTexture() */
    WgCmd    h;                /*  0 */
    uint32_t src_tex;          /*  4 texture id (a surface) */
    uint32_t src_x, src_y;     /*  8, 12 source rectangle, pixels */
    uint32_t src_w, src_h;     /* 16, 20 */
    uint32_t out_w, out_h;     /* 24, 28 canvas size: JS resizes the canvas when it differs */
    uint32_t filter;           /* 32 WG_FILTER_* */
    uint32_t flags;            /* 36 WG_PF_* (0: src as is) */
    /* M2 (a record of 40 bytes has none: no fill) */
    uint32_t crop_x, crop_y;   /* 40, 44 the 4:3 middle, source pixels */
    uint32_t crop_w, crop_h;   /* 48, 52 */
    uint32_t band_l, band_r;   /* 56, 60 a source column inside each side band */
    float    sharp;            /* 64 CAS weight, 0 = off (M5) */
} WgPresent;

typedef struct {               /* WG_OP_SET_VIEWPORT: setViewport */
    WgCmd    h;                /*  0 */
    float    x, y, width, height; /*  4, 8, 12, 16 attachment pixels */
    float    min_depth, max_depth; /* 20, 24 0..1 */
} WgSetViewport;

typedef struct {               /* WG_OP_SET_SCISSOR: setScissorRect (JS clamps to the attachment) */
    WgCmd    h;                /*  0 */
    uint32_t x, y, width, height; /*  4, 8, 12, 16 attachment pixels */
} WgSetScissor;

typedef struct {               /* WG_OP_SET_BLEND_CONST: setBlendConstant */
    WgCmd    h;                /*  0 */
    float    color[4];         /*  4 r, g, b, a in 0..1 */
} WgSetBlendConst;

typedef struct {               /* WG_OP_SET_STENCIL_REF: setStencilReference */
    WgCmd    h;                /*  0 */
    uint32_t ref;              /*  4 0..255 */
} WgSetStencilRef;

typedef struct {               /* WG_OP_CLEAR_RECT: a quad over the rectangle (wgpu_fixed.js CLEAR);
                                * JS puts back the pass state it had (pipeline, groups, viewport,
                                * scissor, blend constant, stencil reference) */
    WgCmd    h;                /*  0 */
    uint32_t x, y, width, height; /*  4, 8, 12, 16 attachment pixels */
    uint32_t flags;            /* 20 WG_CR_* | colour mask << WG_CR_MASK_SHIFT */
    float    color[4];         /* 24 r, g, b, a in 0..1 */
    float    depth;            /* 40 0..1 */
    uint32_t stencil;          /* 44 0..255 */
} WgClearRect;

typedef struct {               /* WG_OP_TEX_CREATE */
    WgCmd    h;                /*  0 */
    uint32_t id;               /*  4 C's id, nonzero */
    uint32_t format;           /*  8 WG_FMT_* */
    uint32_t width, height;    /* 12, 16 */
    uint32_t layers;           /* 20 1, or 6 for WG_DIM_CUBE */
    uint32_t mips;             /* 24 mip level count, >= 1 */
    uint32_t usage;            /* 28 WG_TU_* */
    uint32_t dim;              /* 32 WG_DIM_* */
} WgTexCreate;

typedef struct {               /* WG_OP_TEX_FREE */
    WgCmd    h;                /*  0 */
    uint32_t id;               /*  4 */
} WgTexFree;

typedef struct {               /* WG_OP_TEX_UPLOAD: queue.writeTexture({texture, mipLevel,
                                * origin {x, y, z: layer}}, HEAPU8 at src_ptr,
                                * {bytesPerRow, rowsPerImage: rows}, {width, height, 1}) */
    WgCmd    h;                /*  0 */
    uint32_t id;               /*  4 texture id */
    uint32_t mip;              /*  8 */
    uint32_t layer;            /* 12 cube face (0 for 2D) */
    uint32_t x, y, width, height; /* 16, 20, 24, 28 texels (BC: multiples of 4, or to the edge) */
    uint32_t src_ptr;          /* 32 wasm address of the data (C's until the call returns) */
    uint32_t bytes_per_row;    /* 36 BC: bytes per row of blocks */
    uint32_t rows;             /* 40 rows in the source (BC: rows of blocks) */
    uint32_t src_bytes;        /* 44 bytes at src_ptr */
} WgTexUpload;

typedef struct {               /* WG_OP_SAMPLER: createSampler under C's id (C dedupes by value) */
    WgCmd    h;                /*  0 */
    uint32_t id;               /*  4 */
    uint32_t addr_u, addr_v, addr_w; /* 8, 12, 16 WG_ADDR_* */
    uint32_t mag, min, mip;    /* 20, 24, 28 WG_FILTER_* */
    uint32_t max_aniso;        /* 32 1..16 (> 1 needs all three filters linear; JS enforces) */
    float    lod_min, lod_max; /* 36, 40 lodMinClamp, lodMaxClamp */
} WgSampler;

typedef struct {               /* WG_OP_SHADER: createShaderModule({code}) under C's id. JS logs
                                * compile errors once (getCompilationInfo) with the id; pipelines
                                * using a failed module are not made, their draws dropped. */
    WgCmd    h;                /*  0 */
    uint32_t id;               /*  4 */
    uint32_t src_ptr;          /*  8 wasm address of the WGSL text (UTF-8, no NUL needed) */
    uint32_t src_bytes;        /* 12 */
    uint32_t stages;           /* 16 WG_SH_*: which of vs_main / fs_main it has */
} WgShader;

/* What WebGPU bakes into a pipeline, and nothing else (ADR 0011 item 3).
 * One byte per field: C hashes and compares the bytes, JS reads HEAPU8.
 * Vertex layout is not here: shaders pull attributes from the arena. */
typedef struct {
    uint8_t topology;          /*  0 WG_TOPO_* */
    uint8_t cull;              /*  1 WG_CULL_* */
    uint8_t front_face;        /*  2 WG_FACE_* */
    uint8_t unclipped_depth;   /*  3 1: unclippedDepth (only with WG_FEAT_DEPTH_CLIP_CONTROL) */
    uint8_t color_fmt;         /*  4 WG_FMT_* of the colour attachment, 0: none */
    uint8_t depth_fmt;         /*  5 WG_FMT_* of the depth attachment, 0: none */
    uint8_t color_mask;        /*  6 WG_CM_* */
    uint8_t blend_enable;      /*  7 */
    uint8_t blend_src;         /*  8 WG_BF_* (colour and alpha alike, as the NV2A) */
    uint8_t blend_dst;         /*  9 WG_BF_* */
    uint8_t blend_op;          /* 10 WG_BO_* */
    uint8_t depth_write;       /* 11 */
    uint8_t depth_cmp;         /* 12 WG_CMP_* (ALWAYS with the depth test off) */
    uint8_t stencil_enable;    /* 13 0: stencil state left at WebGPU's defaults */
    uint8_t stencil_cmp;       /* 14 WG_CMP_* (both faces) */
    uint8_t stencil_fail;      /* 15 WG_SO_* */
    uint8_t stencil_zfail;     /* 16 WG_SO_* */
    uint8_t stencil_pass;      /* 17 WG_SO_* */
    uint8_t stencil_read_mask; /* 18 */
    uint8_t stencil_write_mask;/* 19 */
    uint8_t tex_dim[4];        /* 20 WG_TEXDIM_* per stage: group 1's layout */
    uint8_t zero[4];           /* 24 0 */
} WgPipeKey;

typedef struct {               /* WG_OP_PIPE_CREATE: createRenderPipeline under C's id.
                                * Layout: group 0 = the arena (WG_WGSL_GROUP0, VERTEX|FRAGMENT),
                                * group 1 = 4 textures (bindings 0-3, tex_dim) and 4 samplers
                                * (bindings 4-7), FRAGMENT. */
    WgCmd    h;                /*  0 */
    uint32_t id;               /*  4 */
    uint32_t vs;               /*  8 shader id with vs_main */
    uint32_t fs;               /* 12 shader id with fs_main (may equal vs) */
    WgPipeKey key;             /* 16 */
} WgPipeCreate;

typedef struct {               /* WG_OP_SET_PIPE */
    WgCmd    h;                /*  0 */
    uint32_t id;               /*  4 pipeline id */
} WgSetPipe;

typedef struct {               /* WG_OP_SET_TEX: group 1 for the current pipeline's layout (JS caches
                                * bind groups; an unbound stage gets a 1x1 white texture of the
                                * pipeline's tex_dim and a default sampler). After SET_PIPE. */
    WgCmd    h;                /*  0 */
    uint32_t tex[4];           /*  4 texture ids per stage, 0: none */
    uint32_t smp[4];           /* 20 sampler ids per stage, 0: none */
} WgSetTex;

typedef struct {               /* WG_OP_DRAW: drawIndexed(index_count, 1, first_index, 0, record) */
    WgCmd    h;                /*  0 */
    uint32_t first_index;      /*  4 arena byte offset of the first index / 4 */
    uint32_t index_count;      /*  8 */
    uint32_t record;           /* 12 arena byte offset of the WgDrawRec / sizeof(WgDrawRec) */
} WgDraw;

typedef struct {               /* WG_OP_COPY_TEX: copyTextureToTexture (same format, both in range) */
    WgCmd    h;                /*  0 */
    uint32_t src, dst;         /*  4, 8 texture ids (not the same texture) */
    uint32_t src_x, src_y;     /* 12, 16 */
    uint32_t dst_x, dst_y;     /* 20, 24 */
    uint32_t width, height;    /* 28, 32 */
    uint32_t src_mip, dst_mip; /* 36, 40 */
    uint32_t src_layer, dst_layer; /* 44, 48 */
} WgCopyTex;

typedef struct {               /* WG_OP_BLIT: its own pass on dst (load, store), a quad sampling src
                                * (wgpu_fixed.js BLIT); src != dst */
    WgCmd    h;                /*  0 */
    uint32_t src, dst;         /*  4, 8 texture ids */
    uint32_t sx, sy, sw, sh;   /* 12, 16, 20, 24 source rectangle, pixels */
    uint32_t dx, dy, dw, dh;   /* 28, 32, 36, 40 destination rectangle, pixels */
    uint32_t filter;           /* 44 WG_FILTER_* */
} WgBlit;

typedef struct {               /* WG_OP_ZCONV: its own pass on dst: z * zmax packed into RGBA8 as
                                * nv2a_gl's zconv (wgpu_fixed.js ZCONV, M4) */
    WgCmd    h;                /*  0 */
    uint32_t src;              /*  4 depth texture id */
    uint32_t dst;              /*  8 colour texture id (bgra8unorm) */
    uint32_t width, height;    /* 12, 16 pixels, from (0, 0) */
    uint32_t zmax;             /* 20 0xFFFFFF (Z24S8) or 0xFFFF (Z16) */
    uint32_t flags;            /* 24 WG_ZC_* */
} WgZconv;

/* ---- Arena contents read by WGSL ----------------------------------------
 *
 * WgDrawRec: one per draw, D[instance_index]. Built from one X-macro into
 * the C struct and its WGSL struct text (WG_WGSL_DRAWREC), so the two cannot
 * drift. Rules that keep C's and WGSL's layouts equal: vec4 fields first
 * (V4: vec4f, VA: array<vec4f, n>, both 16-byte multiples), then 4-byte
 * scalars (U: u32, F: f32), total a multiple of 16; new fields go at the
 * end of their group's run with the pads adjusted, WG_VERSION bumped, the
 * asserts updated.
 *
 * The fields are nv2a_gl's per-draw uniforms (GlUni, gl_vsh/gl_psh u_*),
 * same meaning and units, plus where the draw's data sits in the arena. */
#define WG_DRAWREC_FIELDS(V4, VA, U, F) \
    V4(surf)        /*   0 u_surf: 2/W, 2/H, 1/zmax, scale offset x */         \
    V4(aa)          /*  16 u_aa.xy (logical -> real pixels), u_snapy, Hor+ 1/k (0: off) */ \
    V4(vpoff)       /*  32 u_vpoff: fixed-function viewport offset */          \
    V4(fogplane)    /*  48 u_fogplane */                                       \
    V4(fog0)        /*  64 u_fog0: enabled, gen mode, equation, abs */         \
    V4(fog1)        /*  80 u_fog1: param0, param1, 0, 0 */                     \
    VA(m, 4)        /*  96 u_m: fixed-function composite rows */               \
    VA(mv, 4)       /* 160 u_mv: model-view rows (fog distance) */             \
    VA(tscale, 2)   /* 224 u_tscale: stage 0 xy, 1 zw; stage 2 xy, 3 zw */     \
    U(xform)        /* 256 0 pre-transformed, 1 fixed-function, 2 program */   \
    U(vcount)       /* 260 gathered vertices (index values are below it) */    \
    U(vf_off)       /* 264 WgVFetch at A[vf_off] (byte offset / 4) */          \
    U(vtx_base)     /* 268 byte offset of vertex 0; WgVAttr.off adds to it */  \
    U(vdef_off)     /* 272 K row of attribute 0's default (16 rows) */         \
    U(k_off)        /* 276 K row of raw transform constant 0 (192 rows) */     \
    U(pk_off)       /* 280 K row of the combiner constants (WG_PK_*) */        \
    U(afunc)        /* 284 u_alpha_func: GL 0x200..0x207, 0x207 = test off */  \
    F(aref)         /* 288 u_alpha_ref: 0..255 */                              \
    U(stage_info)   /* 292 byte i = stage i: WG_SI_* */                        \
    U(pad0)         /* 296 0 */                                                \
    U(pad1)         /* 300 0 */

#define WG_C_V4(n)    float n[4];
#define WG_C_VA(n, k) float n[k][4];
#define WG_C_U(n)     uint32_t n;
#define WG_C_F(n)     float n;
typedef struct { WG_DRAWREC_FIELDS(WG_C_V4, WG_C_VA, WG_C_U, WG_C_F) } WgDrawRec;

#define WG_S_V4(n)    "  " #n ": vec4f,\n"
#define WG_S_VA(n, k) "  " #n ": array<vec4f, " #k ">,\n"
#define WG_S_U(n)     "  " #n ": u32,\n"
#define WG_S_F(n)     "  " #n ": f32,\n"
#define WG_WGSL_DRAWREC \
    "struct WgDrawRec {\n" WG_DRAWREC_FIELDS(WG_S_V4, WG_S_VA, WG_S_U, WG_S_F) "};\n"

/* WgDrawRec.stage_info, per stage byte */
#define WG_SI_BOUND 0x1u        /* a texture is bound (else the stage samples (0, 0, 0, 1)) */
#define WG_SI_CUBE  0x2u        /* cube map: texture_cube in group 1 */

/* Combiner constants block at K[pk_off]: SET_COMBINER_FACTOR0/1 per stage,
 * SPECULAR_FOG_FACTOR0/1 and the fog colour, as nv2a_gl's u_c0/u_c1/u_fc0/
 * u_fc1/u_fogcolor (0..1 RGBA). Shared by draws with equal values. */
#define WG_PK_C0    0           /* 8 rows */
#define WG_PK_C1    8           /* 8 rows */
#define WG_PK_FC0   16
#define WG_PK_FC1   17
#define WG_PK_FOG   18
#define WG_PK_ROWS  20          /* row 19: 0 */

/* Vertex-fetch descriptor at A[vf_off]: the 16 NV2A attribute arrays.
 * Attribute a of vertex v (v = the index value) is read at arena byte
 *   vtx_base + attr[a].off + v * stride
 * fmt is the NV097 SET_VERTEX_DATA_ARRAY_FORMAT word as the title wrote it
 * (type bits 0-3, size bits 4-7, stride bits 8-31); an attribute the
 * executor gathered as float4 has type 2, size 4, stride 16. size 0: no
 * array, the attribute reads K[vdef_off + a] (Nv2aRawBatch.attr_const, the
 * last SET_VERTEX_DATA value, as the hardware). Components past size read
 * 0, 0, 0, 1. Conversions are the executor's (nv2a_pb_exec.c):
 *   0 UB_D3D (D3DCOLOR): 4 bytes B, G, R, A -> (R, G, B, A) / 255, any size
 *   1 S1: int16 / 32767, clamped to -1
 *   2 F: float
 *   4 UB_OGL: uint8 / 255 in order
 *   5 S32K: int16 as is
 *   6 CMP (NORMPACKED3): one u32, x = bits 0-10 / 1023, y = bits 11-21 / 1023,
 *     z = bits 22-31 / 511, all signed; w = 1
 * Addresses may be 2-byte aligned (shorts, odd strides): the WGSL fetch reads
 * the u32s around them. Shared by draws with equal descriptors. */
typedef struct {
    uint32_t off;              /* byte offset from WgDrawRec.vtx_base */
    uint32_t fmt;              /* NV097 format word: type | size << 4 | stride << 8 */
} WgVAttr;
typedef struct {
    WgVAttr attr[16];
} WgVFetch;

#define WG_VF_TYPE(f)   ((f) & 0xFu)
#define WG_VF_SIZE(f)   (((f) >> 4) & 0xFu)
#define WG_VF_STRIDE(f) ((f) >> 8)

/* Group 0, the arena, as every generated module declares it. */
#define WG_WGSL_GROUP0 \
    "@group(0) @binding(0) var<storage, read> A: array<u32>;\n" \
    "@group(0) @binding(1) var<storage, read> K: array<vec4f>;\n" \
    "@group(0) @binding(2) var<storage, read> D: array<WgDrawRec>;\n"

/* ---- JS entry points (web/wgpu.js, --js-library), render pthread only --- */

void wgpu_js_init(WgCtl *ctl);           /* once; starts the asynchronous device set-up */
void wgpu_js_frame(const WgFrame *frame); /* writeBuffer the arena, encode the list, submit, return */

/* ---- Layout checks (the JS mirror above must match) --------------------- */

_Static_assert(sizeof(WgCtl) == 84, "WgCtl");
_Static_assert(offsetof(WgCtl, dev_state) == 12 && offsetof(WgCtl, canvas_format) == 16
               && offsetof(WgCtl, features) == 20 && offsetof(WgCtl, max_tex_2d) == 24, "WgCtl a");
_Static_assert(offsetof(WgCtl, max_storage_binding) == 28 && offsetof(WgCtl, max_in_flight) == 32
               && offsetof(WgCtl, frames_submitted) == 36 && offsetof(WgCtl, frames_done) == 40, "WgCtl b");
_Static_assert(offsetof(WgCtl, frames_presented) == 44 && offsetof(WgCtl, canvas_w) == 48
               && offsetof(WgCtl, canvas_h) == 52 && offsetof(WgCtl, st_cmds) == 56, "WgCtl c");
_Static_assert(offsetof(WgCtl, st_passes) == 60 && offsetof(WgCtl, st_draws) == 64
               && offsetof(WgCtl, st_enc_us) == 68 && offsetof(WgCtl, st_errors) == 72
               && offsetof(WgCtl, st_pipelines) == 76 && offsetof(WgCtl, st_tex_live) == 80, "WgCtl d");

_Static_assert(sizeof(WgFrame) == 48, "WgFrame");
_Static_assert(offsetof(WgFrame, seq) == 12 && offsetof(WgFrame, flags) == 16
               && offsetof(WgFrame, cmd_ptr) == 20 && offsetof(WgFrame, cmd_bytes) == 24
               && offsetof(WgFrame, cmd_count) == 28, "WgFrame a");
_Static_assert(offsetof(WgFrame, arena_ptr) == 32 && offsetof(WgFrame, arena_bytes) == 36
               && offsetof(WgFrame, translate_us) == 40 && offsetof(WgFrame, draws) == 44, "WgFrame b");

_Static_assert(sizeof(WgCmd) == 4 && offsetof(WgCmd, size) == 2, "WgCmd");

_Static_assert(sizeof(WgBeginPass) == 60, "WgBeginPass");
_Static_assert(offsetof(WgBeginPass, color_tex) == 4 && offsetof(WgBeginPass, depth_tex) == 8
               && offsetof(WgBeginPass, color_load) == 12 && offsetof(WgBeginPass, color_store) == 16, "WgBeginPass a");
_Static_assert(offsetof(WgBeginPass, depth_load) == 20 && offsetof(WgBeginPass, depth_store) == 24
               && offsetof(WgBeginPass, stencil_load) == 28 && offsetof(WgBeginPass, stencil_store) == 32, "WgBeginPass b");
_Static_assert(offsetof(WgBeginPass, clear_color) == 36 && offsetof(WgBeginPass, clear_depth) == 52
               && offsetof(WgBeginPass, clear_stencil) == 56, "WgBeginPass c");

_Static_assert(sizeof(WgEndPass) == 4, "WgEndPass");

_Static_assert(sizeof(WgPresent) == 68, "WgPresent");
_Static_assert(offsetof(WgPresent, src_tex) == 4 && offsetof(WgPresent, src_x) == 8
               && offsetof(WgPresent, src_y) == 12 && offsetof(WgPresent, src_w) == 16
               && offsetof(WgPresent, src_h) == 20, "WgPresent a");
_Static_assert(offsetof(WgPresent, out_w) == 24 && offsetof(WgPresent, out_h) == 28
               && offsetof(WgPresent, filter) == 32 && offsetof(WgPresent, flags) == 36, "WgPresent b");
_Static_assert(offsetof(WgPresent, crop_x) == 40 && offsetof(WgPresent, crop_y) == 44
               && offsetof(WgPresent, crop_w) == 48 && offsetof(WgPresent, crop_h) == 52
               && offsetof(WgPresent, band_l) == 56 && offsetof(WgPresent, band_r) == 60
               && offsetof(WgPresent, sharp) == 64, "WgPresent c");

_Static_assert(sizeof(WgSetViewport) == 28 && offsetof(WgSetViewport, x) == 4
               && offsetof(WgSetViewport, y) == 8 && offsetof(WgSetViewport, width) == 12
               && offsetof(WgSetViewport, height) == 16 && offsetof(WgSetViewport, min_depth) == 20
               && offsetof(WgSetViewport, max_depth) == 24, "WgSetViewport");
_Static_assert(sizeof(WgSetScissor) == 20 && offsetof(WgSetScissor, x) == 4
               && offsetof(WgSetScissor, y) == 8 && offsetof(WgSetScissor, width) == 12
               && offsetof(WgSetScissor, height) == 16, "WgSetScissor");
_Static_assert(sizeof(WgSetBlendConst) == 20 && offsetof(WgSetBlendConst, color) == 4, "WgSetBlendConst");
_Static_assert(sizeof(WgSetStencilRef) == 8 && offsetof(WgSetStencilRef, ref) == 4, "WgSetStencilRef");
_Static_assert(sizeof(WgClearRect) == 48 && offsetof(WgClearRect, x) == 4 && offsetof(WgClearRect, y) == 8
               && offsetof(WgClearRect, width) == 12 && offsetof(WgClearRect, height) == 16
               && offsetof(WgClearRect, flags) == 20 && offsetof(WgClearRect, color) == 24
               && offsetof(WgClearRect, depth) == 40 && offsetof(WgClearRect, stencil) == 44, "WgClearRect");

_Static_assert(sizeof(WgTexCreate) == 36, "WgTexCreate");
_Static_assert(offsetof(WgTexCreate, id) == 4 && offsetof(WgTexCreate, format) == 8
               && offsetof(WgTexCreate, width) == 12 && offsetof(WgTexCreate, height) == 16, "WgTexCreate a");
_Static_assert(offsetof(WgTexCreate, layers) == 20 && offsetof(WgTexCreate, mips) == 24
               && offsetof(WgTexCreate, usage) == 28 && offsetof(WgTexCreate, dim) == 32, "WgTexCreate b");

_Static_assert(sizeof(WgTexFree) == 8 && offsetof(WgTexFree, id) == 4, "WgTexFree");

_Static_assert(sizeof(WgTexUpload) == 48 && offsetof(WgTexUpload, id) == 4
               && offsetof(WgTexUpload, mip) == 8 && offsetof(WgTexUpload, layer) == 12
               && offsetof(WgTexUpload, x) == 16 && offsetof(WgTexUpload, y) == 20
               && offsetof(WgTexUpload, width) == 24 && offsetof(WgTexUpload, height) == 28, "WgTexUpload a");
_Static_assert(offsetof(WgTexUpload, src_ptr) == 32 && offsetof(WgTexUpload, bytes_per_row) == 36
               && offsetof(WgTexUpload, rows) == 40 && offsetof(WgTexUpload, src_bytes) == 44, "WgTexUpload b");

_Static_assert(sizeof(WgSampler) == 44 && offsetof(WgSampler, id) == 4
               && offsetof(WgSampler, addr_u) == 8 && offsetof(WgSampler, addr_v) == 12
               && offsetof(WgSampler, addr_w) == 16 && offsetof(WgSampler, mag) == 20
               && offsetof(WgSampler, min) == 24 && offsetof(WgSampler, mip) == 28, "WgSampler a");
_Static_assert(offsetof(WgSampler, max_aniso) == 32 && offsetof(WgSampler, lod_min) == 36
               && offsetof(WgSampler, lod_max) == 40, "WgSampler b");

_Static_assert(sizeof(WgShader) == 20 && offsetof(WgShader, id) == 4 && offsetof(WgShader, src_ptr) == 8
               && offsetof(WgShader, src_bytes) == 12 && offsetof(WgShader, stages) == 16, "WgShader");

_Static_assert(sizeof(WgPipeKey) == 28, "WgPipeKey");
_Static_assert(offsetof(WgPipeKey, topology) == 0 && offsetof(WgPipeKey, cull) == 1
               && offsetof(WgPipeKey, front_face) == 2 && offsetof(WgPipeKey, unclipped_depth) == 3
               && offsetof(WgPipeKey, color_fmt) == 4 && offsetof(WgPipeKey, depth_fmt) == 5
               && offsetof(WgPipeKey, color_mask) == 6 && offsetof(WgPipeKey, blend_enable) == 7, "WgPipeKey a");
_Static_assert(offsetof(WgPipeKey, blend_src) == 8 && offsetof(WgPipeKey, blend_dst) == 9
               && offsetof(WgPipeKey, blend_op) == 10 && offsetof(WgPipeKey, depth_write) == 11
               && offsetof(WgPipeKey, depth_cmp) == 12 && offsetof(WgPipeKey, stencil_enable) == 13
               && offsetof(WgPipeKey, stencil_cmp) == 14 && offsetof(WgPipeKey, stencil_fail) == 15, "WgPipeKey b");
_Static_assert(offsetof(WgPipeKey, stencil_zfail) == 16 && offsetof(WgPipeKey, stencil_pass) == 17
               && offsetof(WgPipeKey, stencil_read_mask) == 18 && offsetof(WgPipeKey, stencil_write_mask) == 19
               && offsetof(WgPipeKey, tex_dim) == 20 && offsetof(WgPipeKey, zero) == 24, "WgPipeKey c");

_Static_assert(sizeof(WgPipeCreate) == 44 && offsetof(WgPipeCreate, id) == 4
               && offsetof(WgPipeCreate, vs) == 8 && offsetof(WgPipeCreate, fs) == 12
               && offsetof(WgPipeCreate, key) == 16, "WgPipeCreate");
_Static_assert(sizeof(WgSetPipe) == 8 && offsetof(WgSetPipe, id) == 4, "WgSetPipe");
_Static_assert(sizeof(WgSetTex) == 36 && offsetof(WgSetTex, tex) == 4 && offsetof(WgSetTex, smp) == 20, "WgSetTex");
_Static_assert(sizeof(WgDraw) == 16 && offsetof(WgDraw, first_index) == 4
               && offsetof(WgDraw, index_count) == 8 && offsetof(WgDraw, record) == 12, "WgDraw");

_Static_assert(sizeof(WgCopyTex) == 52 && offsetof(WgCopyTex, src) == 4 && offsetof(WgCopyTex, dst) == 8
               && offsetof(WgCopyTex, src_x) == 12 && offsetof(WgCopyTex, src_y) == 16
               && offsetof(WgCopyTex, dst_x) == 20 && offsetof(WgCopyTex, dst_y) == 24, "WgCopyTex a");
_Static_assert(offsetof(WgCopyTex, width) == 28 && offsetof(WgCopyTex, height) == 32
               && offsetof(WgCopyTex, src_mip) == 36 && offsetof(WgCopyTex, dst_mip) == 40
               && offsetof(WgCopyTex, src_layer) == 44 && offsetof(WgCopyTex, dst_layer) == 48, "WgCopyTex b");
_Static_assert(sizeof(WgBlit) == 48 && offsetof(WgBlit, src) == 4 && offsetof(WgBlit, dst) == 8
               && offsetof(WgBlit, sx) == 12 && offsetof(WgBlit, sy) == 16 && offsetof(WgBlit, sw) == 20
               && offsetof(WgBlit, sh) == 24, "WgBlit a");
_Static_assert(offsetof(WgBlit, dx) == 28 && offsetof(WgBlit, dy) == 32 && offsetof(WgBlit, dw) == 36
               && offsetof(WgBlit, dh) == 40 && offsetof(WgBlit, filter) == 44, "WgBlit b");
_Static_assert(sizeof(WgZconv) == 28 && offsetof(WgZconv, src) == 4 && offsetof(WgZconv, dst) == 8
               && offsetof(WgZconv, width) == 12 && offsetof(WgZconv, height) == 16
               && offsetof(WgZconv, zmax) == 20 && offsetof(WgZconv, flags) == 24, "WgZconv");

/* WgDrawRec: WGSL's layout (vec4 align 16, struct size rounded to 16) */
_Static_assert(sizeof(WgDrawRec) == 304 && sizeof(WgDrawRec) % 16 == 0, "WgDrawRec");
_Static_assert(offsetof(WgDrawRec, surf) == 0 && offsetof(WgDrawRec, aa) == 16
               && offsetof(WgDrawRec, vpoff) == 32 && offsetof(WgDrawRec, fogplane) == 48
               && offsetof(WgDrawRec, fog0) == 64 && offsetof(WgDrawRec, fog1) == 80, "WgDrawRec a");
_Static_assert(offsetof(WgDrawRec, m) == 96 && offsetof(WgDrawRec, mv) == 160
               && offsetof(WgDrawRec, tscale) == 224 && offsetof(WgDrawRec, xform) == 256, "WgDrawRec b");
_Static_assert(offsetof(WgDrawRec, vcount) == 260 && offsetof(WgDrawRec, vf_off) == 264
               && offsetof(WgDrawRec, vtx_base) == 268 && offsetof(WgDrawRec, vdef_off) == 272
               && offsetof(WgDrawRec, k_off) == 276 && offsetof(WgDrawRec, pk_off) == 280, "WgDrawRec c");
_Static_assert(offsetof(WgDrawRec, afunc) == 284 && offsetof(WgDrawRec, aref) == 288
               && offsetof(WgDrawRec, stage_info) == 292 && offsetof(WgDrawRec, pad0) == 296
               && offsetof(WgDrawRec, pad1) == 300, "WgDrawRec d");

_Static_assert(sizeof(WgVAttr) == 8 && sizeof(WgVFetch) == 128, "WgVFetch");

#endif /* WGPU_PROTO_H */
