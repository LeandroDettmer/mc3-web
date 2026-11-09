/*
 * nv2a_wgpu.h -- WebGPU renderer for the NV2A pushbuffer executor (browser
 * only; ADR 0011, docs/webgpu-plan.md). The command contract with
 * web/wgpu.js is in wgpu_proto.h.
 *
 * src/main.c calls nv2a_wgpu_install() instead of nv2a_gl_install() when
 * RECOMP_GPU=webgpu (web builds only), before the title starts rendering,
 * with the executor enabled (RECOMP_PB_EXEC).
 *
 * The game-facing nv2a_gl_* calls (nv2a_gl.h, plus the ones games and main.c
 * declare themselves) keep their names: an __EMSCRIPTEN__ hunk at the top of
 * each in nv2a_gl.c forwards to the nv2a_wgpu_* twin below while
 * nv2a_wgpu_active() is nonzero. Same arguments, same meaning, same threads.
 */
#ifndef NV2A_WGPU_H
#define NV2A_WGPU_H

#include <stddef.h>
#include <stdint.h>

/* Register the WebGPU back end (Nv2aBackend) and start its render pthread,
 * which takes the transferred #canvas. */
void nv2a_wgpu_install(void);

/* Nonzero once nv2a_wgpu_install() has run: nv2a_gl.c forwards then. */
int nv2a_wgpu_active(void);

/* Forwarding twins of nv2a_gl.c's game-facing calls. */
void     nv2a_wgpu_prewarm_start(void);
void     nv2a_wgpu_fill_hold(int delta);
void     nv2a_wgpu_fill_freeze(int delta);
void     nv2a_wgpu_movie_frame(uint32_t va, uint32_t w, uint32_t h,
                               const uint8_t *y, int ys, const uint8_t *u, int us,
                               const uint8_t *v, int vs, const float m[12], float alpha);
void     nv2a_wgpu_movie_end(uint32_t va);
int      nv2a_wgpu_movie_shown(uint32_t va);   /* 0: the title's own copy shows the frame */
uint32_t nv2a_wgpu_frame_count(void);
/* Hor+ (RECOMP_ASPECT): how many times wider 16:9 frames are shown; 1: off
 * or not this renderer. The title's camera culls that much wider. */
float    nv2a_wgpu_wide(void);
uint64_t nv2a_wgpu_texture_bytes(void);
void     nv2a_wgpu_prof_stats(double secs, char *out, size_t n);
void     nv2a_wgpu_queue_stats(double secs, int *gl_idle_pct, int *exec_wait_pct);

#endif /* NV2A_WGPU_H */
