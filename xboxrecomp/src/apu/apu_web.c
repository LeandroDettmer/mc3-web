/* Host audio output for the browser (Emscripten): the xa2_* contract of
 * apu_xaudio2.c's SDL path, without SDL. SDL2's Emscripten audio needs the
 * page's main thread (Module.SDL2.audioContext) and the APU runs in a
 * worker (PROXY_TO_PTHREAD), so opening it crashed.
 *
 * Design after nfsu2-web (MIT, th0mk: runtime/web/audio_proto.h and
 * web/src/audio/worklet.ts): the APU frame thread writes float32 stereo
 * frames into a single-producer single-consumer ring in the shared wasm
 * memory, and the page's AudioWorklet (web/audio.js) reads them out in each
 * render quantum. No lock, no copy through the main thread: both sides see
 * the same bytes. w and r only grow (mod 2^32); fill = w - r.
 *
 * The ring is 48 kHz, the APU's rate. The page asks its AudioContext for
 * 48 kHz; a context that runs at another rate resamples in the worklet.
 *
 * The page finds the ring through apu_web_ring() (exported): the header's
 * magic says the output is open. While the page's context is not running
 * (not created yet, suspended, RECOMP_AUDIO=0 on the page) nothing is
 * written and xa2_queued() reports a full queue, so the APU keeps its wall
 * clock and never bursts ahead (apu_core.c renders without waiting while the
 * queue is below half its target).
 *
 * Same switches as the SDL path: RECOMP_AUDIO=0 (no output),
 * RECOMP_AUDIO_BLOCKS (queue depth in 256-sample blocks, default 12 =
 * 64 ms: a browser's audio thread renders in bursts), RECOMP_AUDIO_VOLUME,
 * RECOMP_AUDIO_DUMP=<path>,<start s>,<seconds> (in the browser's file
 * system; written when full or at exit). */
#ifdef __EMSCRIPTEN__

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>
#include "apu_xaudio2.h"

#define WA_MAGIC        0x58415544u     /* 'XAUD' */
#define WA_VERSION      1u
#define WA_RATE         48000
#define WA_RING_FRAMES  16384u          /* power of two: 341 ms */
#define WA_BLOCK        256
#define WA_MAX_SAMPLES  1024
#define WA_DEFAULT_BLOCKS 12

/* Header words; web/audio.js mirrors them by index. */
enum {
    AR_MAGIC = 0, AR_VERSION = 1, AR_FRAMES = 2, AR_RATE = 3,
    AR_W = 4,           /* producer: frames written */
    AR_R = 5,           /* worklet: frames read */
    AR_UNDERRUNS = 6,   /* worklet: quanta short of frames (after the first data) */
    AR_CTX = 7,         /* page: 1 = context running and worklet attached */
    AR_QUANTA = 8,      /* worklet: process() calls */
    AR_LOUD = 9,        /* worklet: quanta that were not silent */
    AR_PAGE_RATE = 10,  /* page: the context's sample rate */
    AR_HDR_WORDS = 32
};

typedef struct {
    _Atomic uint32_t h[AR_HDR_WORDS];
    float pcm[WA_RING_FRAMES * 2];      /* interleaved L, R */
} WebAudioRing;
_Static_assert(offsetof(WebAudioRing, pcm) == 128, "web audio ring layout");

static _Alignas(16) WebAudioRing g_ring;
static int      g_wa_open = 0;
static int      g_wa_target = WA_DEFAULT_BLOCKS;
static uint32_t g_wa_underruns_seen = 0;
static Xa2Stats g_wa_stats;

EMSCRIPTEN_KEEPALIVE uintptr_t apu_web_ring(void)
{
    return (uintptr_t)&g_ring;
}

static int wa_running(void)
{
    return atomic_load(&g_ring.h[AR_CTX]) == 1;
}

/* ---- RECOMP_AUDIO_DUMP: the PCM handed to the ring, s16 stereo 48 kHz ---- */
static struct {
    int on;
    char path[256];
    uint64_t skip, left;
    int16_t *buf;
    size_t n, cap;
} s_dump;

static void dump_write(void)
{
    FILE *f;
    if (!s_dump.on) return;
    s_dump.on = 0;
    f = fopen(s_dump.path, "wb");
    if (f) {
        fwrite(s_dump.buf, sizeof(int16_t), s_dump.n, f);
        fclose(f);
    }
    fprintf(stderr, "[AUDIO] dump written: %s (%zu bytes)%s\n", s_dump.path,
            s_dump.n * sizeof(int16_t), f ? "" : " -- open failed");
}

static void dump_init(void)
{
    const char *e = getenv("RECOMP_AUDIO_DUMP");
    char *c1, *c2;
    if (!e || !*e) return;
    snprintf(s_dump.path, sizeof s_dump.path, "%s", e);
    c1 = strchr(s_dump.path, ',');
    c2 = c1 ? strchr(c1 + 1, ',') : NULL;
    if (!c1 || !c2) return;
    *c1 = 0;
    s_dump.skip = (uint64_t)atoi(c1 + 1) * WA_RATE;
    s_dump.left = (uint64_t)atoi(c2 + 1) * WA_RATE;
    s_dump.cap = (size_t)s_dump.left * 2;
    s_dump.buf = s_dump.cap ? (int16_t *)malloc(s_dump.cap * sizeof(int16_t)) : NULL;
    if (!s_dump.buf) return;
    s_dump.on = 1;
    atexit(dump_write);
    fprintf(stderr, "[AUDIO] dump: %llu s from %llu s to %s\n",
            (unsigned long long)(s_dump.left / WA_RATE),
            (unsigned long long)(s_dump.skip / WA_RATE), s_dump.path);
}

static void dump_block(const int16_t *pcm, int num_samples)
{
    size_t take;
    if (!s_dump.on) return;
    if (s_dump.skip >= (uint64_t)num_samples) { s_dump.skip -= num_samples; return; }
    pcm += s_dump.skip * 2;
    num_samples -= (int)s_dump.skip;
    s_dump.skip = 0;
    take = (size_t)num_samples * 2;
    if (take > s_dump.cap - s_dump.n) take = s_dump.cap - s_dump.n;
    memcpy(s_dump.buf + s_dump.n, pcm, take * sizeof(int16_t));
    s_dump.n += take;
    if (s_dump.n >= s_dump.cap) dump_write();
}

/* ---- xa2_* ---- */
int xa2_init(void)
{
    const char *e;

    if (g_wa_open) return 1;
    e = getenv("RECOMP_AUDIO");
    if (e && strcmp(e, "0") == 0) {
        fprintf(stderr, "[AUDIO] host output off (RECOMP_AUDIO=0)\n");
        return 0;
    }
    e = getenv("RECOMP_AUDIO_BLOCKS");
    if (e && atoi(e) >= 2 && atoi(e) <= 64) g_wa_target = atoi(e);
    e = getenv("RECOMP_AUDIO_VOLUME");
    if (e) xa2_set_master_volume((float)atoi(e) / 100.0f);

    atomic_store(&g_ring.h[AR_VERSION], WA_VERSION);
    atomic_store(&g_ring.h[AR_FRAMES], WA_RING_FRAMES);
    atomic_store(&g_ring.h[AR_RATE], WA_RATE);
    atomic_store(&g_ring.h[AR_MAGIC], WA_MAGIC);   /* last: the page may attach */
    g_wa_open = 1;
    dump_init();
    fprintf(stderr, "[AUDIO] web output: AudioWorklet ring at 0x%lx, %d Hz, %u frames,"
                    " queue %d x %d samples\n", (unsigned long)apu_web_ring(), WA_RATE,
            WA_RING_FRAMES, g_wa_target, WA_BLOCK);
    return 1;
}

void xa2_shutdown(void)
{
    if (!g_wa_open) return;
    atomic_store(&g_ring.h[AR_MAGIC], 0);
    g_wa_open = 0;
    fprintf(stderr, "[AUDIO] shut down (%llu blocks written, %u underruns,"
                    " %llu dropped)\n", (unsigned long long)g_wa_stats.submitted,
            atomic_load(&g_ring.h[AR_UNDERRUNS]), (unsigned long long)g_wa_stats.dropped);
}

int xa2_is_active(void)
{
    return g_wa_open;
}

int xa2_queued(void)
{
    if (!g_wa_open) return 0;
    if (!wa_running()) return g_wa_target;   /* nobody drains: pace by wall clock */
    return (int)((atomic_load(&g_ring.h[AR_W]) - atomic_load(&g_ring.h[AR_R])) / WA_BLOCK);
}

int xa2_queue_target(void)
{
    return g_wa_target;
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    int16_t buf[WA_MAX_SAMPLES * 2];
    uint32_t w, r, u;
    int i, n, cap;

    if (!g_wa_open || !wa_running()) return 0;
    if (num_samples > WA_MAX_SAMPLES) num_samples = WA_MAX_SAMPLES;

    /* A worklet that stopped draining must not be overrun: drop, as the SDL
     * path drops on a full queue. */
    w = atomic_load(&g_ring.h[AR_W]);
    r = atomic_load(&g_ring.h[AR_R]);
    cap = g_wa_target * 4;
    if (cap > (int)(WA_RING_FRAMES / WA_BLOCK) - 1) cap = (int)(WA_RING_FRAMES / WA_BLOCK) - 1;
    if ((int)((w - r) / WA_BLOCK) >= cap) {
        g_wa_stats.dropped++;
        return 0;
    }
    u = atomic_load(&g_ring.h[AR_UNDERRUNS]);
    if (u != g_wa_underruns_seen) {
        if (getenv("RECOMP_APU_TRACE") && g_wa_underruns_seen < 20)
            fprintf(stderr, "[AUDIO] underrun (%u quanta)\n", u - g_wa_underruns_seen);
        g_wa_underruns_seen = u;
    }

    n = num_samples * 2;
    for (i = 0; i < n; i++) {
        int a = samples[i] < 0 ? -samples[i] : samples[i];
        if (a >= 32767) g_wa_stats.clipped++;
        if (a > g_wa_stats.peak) g_wa_stats.peak = a;
    }
    g_wa_stats.samples += (uint64_t)num_samples;
    apu_output_safety(samples, buf, n);
    dump_block(buf, num_samples);

    for (i = 0; i < num_samples; i++) {
        uint32_t j = ((w + (uint32_t)i) & (WA_RING_FRAMES - 1)) * 2;
        g_ring.pcm[j]     = buf[2 * i]     * (1.0f / 32768.0f);
        g_ring.pcm[j + 1] = buf[2 * i + 1] * (1.0f / 32768.0f);
    }
    atomic_store(&g_ring.h[AR_W], w + (uint32_t)num_samples);   /* release: frames first */
    g_wa_stats.submitted++;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return WA_BLOCK;
}

void xa2_get_stats(Xa2Stats *out)
{
    *out = g_wa_stats;
    out->underruns = atomic_load(&g_ring.h[AR_UNDERRUNS]);   /* the worklet's: audible gaps */
    g_wa_stats.peak = 0;
}

#endif /* __EMSCRIPTEN__ */
