/* wgpu_stats.h -- GPU timing counters of the WebGPU renderer (M4). */
#ifndef WGPU_STATS_H
#define WGPU_STATS_H

#include <stddef.h>
#include <stdint.h>

/* Bind the counters web/wgpu_stats.js fills (render thread, after wgpu_js_init).
 * RECOMP_WGPU_GPUTIME=0 leaves GPU timing off. */
void wgpu_stats_init(void);

/* Counter snapshot for one consumer's delta (opaque). */
typedef struct { uint32_t v[8]; } WgGtSnap;

/* "gpu 5.2 ms/frame (scene 4.1, aux 0.3, present 0.8; 120 passes timed)" for
 * the time since the snapshot, over `frames` frames; "gpu n/a" when timing is
 * off or unsupported. The snapshot is advanced. Returns characters written. */
size_t wgpu_stats_text(WgGtSnap *snap, uint32_t frames, char *out, size_t n);

#endif
