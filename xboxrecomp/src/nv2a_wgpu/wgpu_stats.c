/*
 * wgpu_stats.c -- GPU timing counters of the WebGPU renderer (M4).
 * web/wgpu_stats.js adds into g_wgpu_gt[] (microseconds / counts) when a
 * timestamp readback lands; readers take deltas against their own snapshot.
 *
 *   [0] scene passes us   [1] aux (blit/zconv) us   [2] present us
 *   [4] passes timed      [5] passes/submits skipped (no slot, no buffer)
 *   [6] 1 once the query set exists
 */
#include "wgpu_stats.h"

#include <stdio.h>
#include <stdlib.h>

uint32_t g_wgpu_gt[8];

extern void wgpu_gt_js_bind(uint32_t *ptr, int on);

void wgpu_stats_init(void)
{
    const char *e = getenv("RECOMP_WGPU_GPUTIME");
    wgpu_gt_js_bind(g_wgpu_gt, !(e && *e == '0'));
}

size_t wgpu_stats_text(WgGtSnap *snap, uint32_t frames, char *out, size_t n)
{
    uint32_t d[8];
    double f = frames ? frames : 1, ms[3];
    int i, ret;

    for (i = 0; i < 8; i++) {
        uint32_t cur = __atomic_load_n(&g_wgpu_gt[i], __ATOMIC_RELAXED);
        d[i] = cur - snap->v[i];
        snap->v[i] = cur;
    }
    if (!__atomic_load_n(&g_wgpu_gt[6], __ATOMIC_RELAXED)) {
        ret = snprintf(out, n, "gpu n/a");
        return ret < 0 ? 0 : (size_t)ret;
    }
    for (i = 0; i < 3; i++)
        ms[i] = d[i] / 1e3 / f;
    ret = snprintf(out, n, "gpu %.2f ms/frame (scene %.2f, aux %.2f, present %.2f; %u passes timed, %u skipped)",
                   ms[0] + ms[1] + ms[2], ms[0], ms[1], ms[2], d[4], d[5]);
    return ret < 0 ? 0 : (size_t)ret;
}
