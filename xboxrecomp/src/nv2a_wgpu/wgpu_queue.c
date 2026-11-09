/*
 * wgpu_queue.c -- the WebGPU back end's Nv2aBackend callbacks, its record
 * ring and the render pthread (browser only; ADR 0011, docs/webgpu-plan.md).
 *
 * As nv2a_gl's threaded path (q_* in nv2a_gl.c): the executor's callbacks
 * copy what they are given into a ring and return; a render pthread, which
 * owns the transferred OffscreenCanvas, applies the records. Unlike the GL
 * thread it does not draw as it goes: it writes a command list
 * (wgpu_proto.h) and, at each flip, hands it to web/wgpu.js in one call
 * (wgpu_js_frame), which encodes and submits. The worker then returns to its
 * event loop (timer turns), which presents the canvas and runs WebGPU's
 * promise callbacks -- device set-up and onSubmittedWorkDone among them.
 *
 * Pacing: the executor stays at most RECOMP_GL_QUEUE_FRAMES (2) flips ahead
 * of the render thread (waits in wq_flip, as q_flip); the render thread
 * submits a frame only while fewer than wg_ctl.max_in_flight (2) are on the
 * GPU. It never waits for frames_done: the callback that counts it runs on
 * this thread's own event loop, so it returns and tries again next turn.
 *
 * M2: a draw record carries the whole batch as nv2a_gl's q_draw does (the
 * method shadow's changed blocks, program and constant rows, vertex bytes,
 * indices), rebuilt on the render thread for wdraw_batch (wgpu_draw.c);
 * blits go to wsurf_blit; a full frame arena flushes the part so far
 * (wg_flush_partial).
 */
#include "nv2a_wgpu.h"
#include "wgpu_int.h"
#include "wgpu_stats.h"
#include "../nv2a_gl/nv2a_gl.h"

#include <SDL.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void xbox_sdl_keyboard_handle(const SDL_Event *e);   /* input/sdl_keyboard.c */

WgCtl    wg_ctl;
uint32_t wg_frame;

static int s_active;

static uint64_t wq_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ── Command list (render thread) ─────────────────────────────────── */

static uint8_t *s_list;
static uint32_t s_list_len, s_list_cap, s_list_count, s_list_draws;
static uint32_t s_tex_next = 1;

void *wg_emit(uint32_t op, uint32_t size)
{
    WgCmd *c;
    if (s_list_len + size > s_list_cap) {
        uint32_t cap = s_list_cap ? s_list_cap : 64u << 10;
        while (cap < s_list_len + size)
            cap *= 2;
        s_list = (uint8_t *)realloc(s_list, cap);
        if (!s_list) {
            fprintf(stderr, "  [WGPU] command list of %u bytes: out of memory\n", cap);
            abort();
        }
        s_list_cap = cap;
    }
    c = (WgCmd *)(s_list + s_list_len);
    memset(c, 0, size);
    c->op = (uint16_t)op;
    c->size = (uint16_t)size;
    s_list_len += size;
    s_list_count++;
    s_list_draws += op == WG_OP_DRAW;
    return c;
}

uint32_t wg_tex_create(uint32_t format, uint32_t w, uint32_t h, uint32_t usage)
{
    WgTexCreate *t = (WgTexCreate *)wg_emit(WG_OP_TEX_CREATE, sizeof *t);
    t->id = s_tex_next++;
    if (!s_tex_next)
        s_tex_next = 1;                        /* 0 is "none" */
    t->format = format;
    t->width = w;
    t->height = h;
    t->layers = 1;
    t->mips = 1;
    t->usage = usage;
    t->dim = WG_DIM_2D;
    return t->id;
}

void wg_tex_free(uint32_t id)
{
    WgTexFree *t;
    if (!id)
        return;
    t = (WgTexFree *)wg_emit(WG_OP_TEX_FREE, sizeof *t);
    t->id = id;
}

/* ── Record ring: executor -> render thread ───────────────────────── */

enum { WQ_WRAP = 0, WQ_DRAW = 1, WQ_CLEAR = 2, WQ_FLIP = 3, WQ_BLIT = 4 };
typedef struct { uint32_t type, size, pad[2]; } WQHdr;
/* A draw's body: this header, nreg shadow blocks, nprog + nconst rows, the
 * batch (pointers as offsets from the record's start), vertex bytes,
 * indices (q_draw's layout). */
typedef struct { uint32_t nreg, nprog, nconst, pad; } WQDraw;
typedef struct { uint32_t idx, pad[3]; uint32_t w[16]; } WQRegChunk;   /* 64 B of shadow */
typedef struct { uint32_t idx, pad[3]; uint32_t w[4]; } WQRow;         /* 16 B row */
typedef struct {
    Nv2aSurface s;
    uint32_t zeta_va, flags, argb, zs;
    WsClearRegs r;
} WQClear;

/* Draws carry their vertex data: as large as nv2a_gl's ring. */
#define WQ_BYTES   (48u << 20)
#define WQ_ALIGN(x) (((x) + 15u) & ~(size_t)15u)
#define WQ_REG_WORDS  (0x2000 / 4)
#define WQ_PROG_ROWS  136
#define WQ_CONST_ROWS 192

static uint8_t *s_q;
static uint64_t s_q_head, s_q_tail;     /* bytes written / read, ever (64-bit: see nv2a_gl.c) */
static int s_q_consumer_waits, s_q_producer_waits, s_q_flips, s_q_max_flips = 2;
static pthread_mutex_t s_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_q_cv = PTHREAD_COND_INITIALIZER;
/* Perf report: render thread idle, executor held back. ns. */
static uint64_t s_q_idle_ns, s_q_exec_wait_ns;

static void wq_timed_wait(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 2000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(&s_q_cv, &s_q_lock, &ts);
}

static void wq_wake(int *flag)
{
    if (__atomic_load_n(flag, __ATOMIC_SEQ_CST)) {
        pthread_mutex_lock(&s_q_lock);
        pthread_cond_broadcast(&s_q_cv);
        pthread_mutex_unlock(&s_q_lock);
    }
}

/* Executor thread: room for a record of `size` bytes (16-aligned, header
 * included), contiguous; wq_publish(size) after filling it. */
static uint8_t *wq_reserve(size_t size)
{
    size_t off = s_q_head % WQ_BYTES, need = size;
    WQHdr *h;

    if (off + size > WQ_BYTES)
        need += WQ_BYTES - off;                 /* the wrap filler */
    if (WQ_BYTES - (s_q_head - __atomic_load_n(&s_q_tail, __ATOMIC_ACQUIRE)) < need) {
        uint64_t t0 = wq_now_ns();
        pthread_mutex_lock(&s_q_lock);
        __atomic_store_n(&s_q_producer_waits, 1, __ATOMIC_SEQ_CST);
        while (WQ_BYTES - (s_q_head - __atomic_load_n(&s_q_tail, __ATOMIC_ACQUIRE)) < need)
            wq_timed_wait();
        __atomic_store_n(&s_q_producer_waits, 0, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s_q_lock);
        __atomic_add_fetch(&s_q_exec_wait_ns, wq_now_ns() - t0, __ATOMIC_RELAXED);
    }
    if (off + size > WQ_BYTES) {
        h = (WQHdr *)(s_q + off);
        h->type = WQ_WRAP;
        h->size = (uint32_t)(WQ_BYTES - off);
        __atomic_store_n(&s_q_head, s_q_head + (WQ_BYTES - off), __ATOMIC_RELEASE);
        off = 0;
    }
    return s_q + off;
}

static void wq_publish(size_t size)
{
    __atomic_store_n(&s_q_head, s_q_head + size, __ATOMIC_SEQ_CST);
    wq_wake(&s_q_consumer_waits);
}

/* Queue one record (header + n bytes of body). */
static void wq_put(uint32_t type, const void *body, size_t n)
{
    size_t size = sizeof(WQHdr) + WQ_ALIGN(n);
    WQHdr *h = (WQHdr *)wq_reserve(size);
    memset(h, 0, sizeof *h);
    h->type = type;
    h->size = (uint32_t)size;
    if (n)
        memcpy(h + 1, body, n);
    wq_publish(size);
}

/* ── Shadow, program and constants: only what changed (as q_reg_*) ── */

/* Executor thread: what was last queued; the executor marks the 64-byte
 * shadow blocks it writes (nv2a_pb_reg_dirty), those that really differ
 * wait in s_q_pend until a draw carries them. */
extern uint8_t *nv2a_pb_reg_dirty(void);
static uint32_t s_q_regs[WQ_REG_WORDS], s_q_prog[WQ_PROG_ROWS][4];
static uint32_t s_q_consts[WQ_CONST_ROWS][4];
static uint32_t s_q_prog_gen, s_q_const_gen;
static uint16_t s_q_pend[WQ_REG_WORDS / 16];
static uint32_t s_q_npend;
/* Render thread: its copies, rebuilt from the draw records. */
static uint32_t g_regs[WQ_REG_WORDS], g_prog[WQ_PROG_ROWS][4];
static float g_consts[WQ_CONST_ROWS][4];

static uint32_t wq_reg_collect(const uint32_t *live)
{
    uint8_t *dirty = nv2a_pb_reg_dirty();
    uint32_t c, k;
    for (c = 0; c < WQ_REG_WORDS / 16; c++) {
        if (!dirty[c])
            continue;
        dirty[c] = 0;
        if (memcmp(live + c * 16, s_q_regs + c * 16, 64) != 0) {
            for (k = 0; k < s_q_npend && s_q_pend[k] != c; k++) { }
            if (k == s_q_npend)
                s_q_pend[s_q_npend++] = (uint16_t)c;
        }
    }
    return s_q_npend;
}

static void wq_reg_emit(const uint32_t *live, WQRegChunk *out)
{
    uint32_t k;
    for (k = 0; k < s_q_npend; k++) {
        uint32_t c = s_q_pend[k];
        out[k].idx = c;
        memcpy(out[k].w, live + c * 16, 64);
        memcpy(s_q_regs + c * 16, live + c * 16, 64);
    }
    s_q_npend = 0;
}

/* Rows of live that differ from sent; with out, written there and into
 * sent. Returns the count. */
static uint32_t wq_rows_diff(const uint32_t (*live)[4], uint32_t (*sent)[4], uint32_t rows,
                             WQRow *out)
{
    uint32_t r, n = 0;
    for (r = 0; r < rows; r++)
        if (memcmp(live[r], sent[r], 16) != 0) {
            if (out) {
                out[n].idx = r;
                memcpy(out[n].w, live[r], 16);
                memcpy(sent[r], live[r], 16);
            }
            n++;
        }
    return n;
}

/* The byte ranges the direct attributes of b span, merged where they
 * overlap (interleaved attributes share one), each from a 4-aligned start;
 * of_run[a]: attribute a's range (nv2a_gl's vertex_runs). */
typedef struct { const uint8_t *lo, *hi; } WQRun;
static uint32_t wq_vertex_runs(const Nv2aRawBatch *b, WQRun *run, int *of_run)
{
    uint32_t nv = b->vertex_count, nrun = 0, a, i;

    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        const uint8_t *lo, *hi;
        uint32_t elem;
        if (!(b->attr_direct & (1u << a)))
            continue;
        switch (b->direct[a].type) {
        case 0: case 6: elem = 4; break;
        case 2: elem = 4 * b->direct[a].size; break;
        case 4: elem = b->direct[a].size; break;
        default: elem = 2 * b->direct[a].size; break;
        }
        lo = (const uint8_t *)((uintptr_t)b->direct[a].ptr & ~(uintptr_t)3);
        hi = b->direct[a].ptr + (size_t)(nv - 1) * b->direct[a].stride + elem;
        for (i = 0; i < nrun; i++)
            if (lo <= run[i].hi && run[i].lo <= hi)
                break;
        if (i == nrun) {
            run[nrun].lo = lo;
            run[nrun].hi = hi;
            nrun++;
        } else {
            if (lo < run[i].lo) run[i].lo = lo;
            if (hi > run[i].hi) run[i].hi = hi;
        }
        of_run[a] = (int)i;
    }
    for (i = 0; i + 1 < nrun; ) {           /* a widened run can overlap another */
        uint32_t j;
        for (j = i + 1; j < nrun; j++)
            if (run[j].lo <= run[i].hi && run[i].lo <= run[j].hi)
                break;
        if (j == nrun) { i++; continue; }
        if (run[j].lo < run[i].lo) run[i].lo = run[j].lo;
        if (run[j].hi > run[i].hi) run[i].hi = run[j].hi;
        for (a = 0; a < NV2A_RAW_ATTRS; a++) {
            if (!(b->attr_direct & (1u << a)))
                continue;
            if (of_run[a] == (int)j)
                of_run[a] = (int)i;
            else if (of_run[a] == (int)(nrun - 1))
                of_run[a] = (int)j;
        }
        run[j] = run[nrun - 1];
        nrun--;
    }
    return nrun;
}

/* ── Back-end callbacks (executor thread) ─────────────────────────── */

static const uint32_t *s_live_regs;     /* the method shadow, from the first draw */

/* The whole batch into one record (nv2a_gl's q_draw). Attributes the
 * executor converted (conv) go in packed float4, and become direct ones of
 * type 2, size 4: the render thread sees every present attribute as stored
 * bytes, as the vertex fetch reads them (WgVFetch). */
static void wq_draw(const Nv2aRawBatch *b)
{
    static uint64_t dropped;
    WQRun run[NV2A_RAW_ATTRS];
    int of_run[NV2A_RAW_ATTRS];
    uint32_t nrun = wq_vertex_runs(b, run, of_run), nreg, nprog = 0, nconst = 0, np = 0, i, a, v;
    uint16_t conv = b->attr_present & (uint16_t)~b->attr_direct;
    size_t size, at, runs_at[NV2A_RAW_ATTRS], pack_at, idx_at, batch_at;
    Nv2aRawBatch *qb;
    WQHdr *h;
    WQDraw *d;
    uint8_t *rec;

    s_live_regs = b->regs;
    nreg = wq_reg_collect(b->regs);
    if (b->vp_program && b->vp_prog_gen != s_q_prog_gen && b->vp_slots <= WQ_PROG_ROWS)
        nprog = wq_rows_diff(b->vp_program, s_q_prog, WQ_PROG_ROWS, NULL);
    if (b->vp_consts && b->vp_const_gen != s_q_const_gen && b->vp_const_count <= WQ_CONST_ROWS)
        nconst = wq_rows_diff((const uint32_t (*)[4])b->vp_consts, s_q_consts, WQ_CONST_ROWS, NULL);
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        np += (conv >> a) & 1u;
    size = sizeof(WQHdr) + sizeof(WQDraw) + (size_t)nreg * sizeof(WQRegChunk)
         + (size_t)(nprog + nconst) * sizeof(WQRow);
    batch_at = size;
    size += WQ_ALIGN(sizeof(Nv2aRawBatch));
    for (i = 0; i < nrun; i++) {
        runs_at[i] = size;
        size += WQ_ALIGN((size_t)(run[i].hi - run[i].lo));
    }
    pack_at = size;
    size += WQ_ALIGN((size_t)b->vertex_count * np * 16);
    idx_at = size;
    size += WQ_ALIGN((size_t)b->index_count * 4);
    if (size > WQ_BYTES / 4) {
        if ((dropped++ & 1023) == 0)
            fprintf(stderr, "  [WGPU] queued draw of %zu bytes dropped (%llu so far)\n",
                    size, (unsigned long long)dropped);
        return;
    }

    rec = wq_reserve(size);
    h = (WQHdr *)rec;
    memset(h, 0, sizeof *h);
    h->type = WQ_DRAW;
    h->size = (uint32_t)size;
    d = (WQDraw *)(h + 1);
    memset(d, 0, sizeof *d);
    d->nreg = nreg;
    at = sizeof(WQHdr) + sizeof(WQDraw);
    wq_reg_emit(b->regs, (WQRegChunk *)(rec + at));
    at += (size_t)nreg * sizeof(WQRegChunk);
    if (nprog) {
        d->nprog = wq_rows_diff(b->vp_program, s_q_prog, WQ_PROG_ROWS, (WQRow *)(rec + at));
        at += (size_t)d->nprog * sizeof(WQRow);
    }
    if (nconst) {
        d->nconst = wq_rows_diff((const uint32_t (*)[4])b->vp_consts, s_q_consts, WQ_CONST_ROWS,
                                 (WQRow *)(rec + at));
        at += (size_t)d->nconst * sizeof(WQRow);
    }
    if (b->vp_program && b->vp_slots <= WQ_PROG_ROWS)
        s_q_prog_gen = b->vp_prog_gen;
    if (b->vp_consts && b->vp_const_count <= WQ_CONST_ROWS)
        s_q_const_gen = b->vp_const_gen;

    qb = (Nv2aRawBatch *)(rec + batch_at);
    *qb = *b;
    /* Pointers become offsets into the record; the render thread fixes them. */
    for (i = 0; i < nrun; i++)
        memcpy(rec + runs_at[i], run[i].lo, (size_t)(run[i].hi - run[i].lo));
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if (b->attr_direct & (1u << a))
            qb->direct[a].ptr = (const uint8_t *)(uintptr_t)
                (runs_at[of_run[a]] + (size_t)(b->direct[a].ptr - run[of_run[a]].lo));
    for (a = 0, i = 0; a < NV2A_RAW_ATTRS; a++) {
        if (!(conv & (1u << a)))
            continue;
        for (v = 0; v < b->vertex_count; v++)
            memcpy(rec + pack_at + ((size_t)v * np + i) * 16,
                   b->attrs + ((size_t)v * NV2A_RAW_ATTRS + a) * 4, 16);
        qb->direct[a].ptr = (const uint8_t *)(uintptr_t)(pack_at + (size_t)i * 16);
        qb->direct[a].type = 2;
        qb->direct[a].size = 4;
        qb->direct[a].stride = np * 16;
        i++;
    }
    qb->attr_direct |= conv;
    qb->attrs = NULL;
    memcpy(rec + idx_at, b->indices, (size_t)b->index_count * 4);
    qb->indices = (const uint32_t *)(uintptr_t)idx_at;
    wq_publish(size);
}

static void wq_blit(const Nv2aBlit *b)
{
    wq_put(WQ_BLIT, b, sizeof *b);
}

static void wq_clear(const Nv2aSurface *sf, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zs)
{
    WQClear c;
    memset(&c, 0, sizeof c);
    c.s = *sf;
    c.zeta_va = rs ? rs->zeta_va : 0;
    c.flags = flags;
    c.argb = argb;
    c.zs = zs;
    c.r.zmax = 0xFFFFFFu;
    if (s_live_regs) {
        c.r.have = 1;
        c.r.rect_h = s_live_regs[0x1D98 / 4];   /* SET_CLEAR_RECT_HORIZONTAL */
        c.r.rect_v = s_live_regs[0x1D9C / 4];   /* SET_CLEAR_RECT_VERTICAL */
        if (((s_live_regs[0x208 / 4] >> 4) & 0xF) == 1)
            c.r.zmax = 0xFFFFu;                  /* Z16 */
    }
    wq_put(WQ_CLEAR, &c, sizeof c);
}

static void wq_flip(void)
{
    if (__atomic_load_n(&s_q_flips, __ATOMIC_ACQUIRE) >= s_q_max_flips) {
        uint64_t t0 = wq_now_ns();
        pthread_mutex_lock(&s_q_lock);
        __atomic_store_n(&s_q_producer_waits, 1, __ATOMIC_SEQ_CST);
        while (__atomic_load_n(&s_q_flips, __ATOMIC_ACQUIRE) >= s_q_max_flips)
            wq_timed_wait();
        __atomic_store_n(&s_q_producer_waits, 0, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s_q_lock);
        __atomic_add_fetch(&s_q_exec_wait_ns, wq_now_ns() - t0, __ATOMIC_RELAXED);
    }
    __atomic_add_fetch(&s_q_flips, 1, __ATOMIC_ACQ_REL);
    wq_put(WQ_FLIP, NULL, 0);
}

/* Before the title's first pushbuffer work: wait for the device, which the
 * render thread's event loop brings up (dev_state, Atomics.notify). The
 * executor may block here; the render thread never does. */
static void wq_prepare(void)
{
    double t0 = emscripten_get_now(), left;
    int st;
    while ((st = __atomic_load_n(&wg_ctl.dev_state, __ATOMIC_SEQ_CST)) == WG_DEV_NONE
           || st == WG_DEV_PENDING) {
        left = 15000.0 - (emscripten_get_now() - t0);
        if (left <= 0.0) {
            fprintf(stderr, "  [WGPU] no device after 15 s: the title starts anyway\n");
            return;
        }
        emscripten_futex_wait(&wg_ctl.dev_state, (uint32_t)st, left < 100.0 ? left : 100.0);
    }
    fprintf(stderr, "  [WGPU] device %s after %.0f ms\n",
            st == WG_DEV_READY ? "ready" : "failed", emscripten_get_now() - t0);
}

static const Nv2aBackend s_backend = {
    wq_clear,
    NULL,                       /* draw: everything arrives through draw_raw */
    wq_flip,
    wq_draw,
    NV2A_BACKEND_RAW_DIRECT,    /* attributes as stored: the shaders fetch them */
    wq_prepare,
    wq_blit,                    /* after the executor's copy in guest memory */
};

/* ── Render thread ────────────────────────────────────────────────── */

static uint32_t s_draws;                /* this frame's draws */
static uint64_t s_busy_ns;              /* this frame's list building */
static uint64_t s_turn_end;
static int s_stats = -1;

/* RECOMP_WGPU_STATS=1: one line a second. */
static void wq_stats(void)
{
    static uint64_t t0;
    static uint32_t frames0, passes0, enc0, pipes0, draws_sum, frames_sum;
    static uint64_t busy_sum;
    uint64_t now = wq_now_ns();
    double dt;
    uint32_t frames;
    static WgGtSnap gt;
    char gtx[160];

    draws_sum += s_draws;
    busy_sum += s_busy_ns;
    frames_sum++;
    if (!t0) {
        t0 = now;
        frames0 = wg_ctl.frames_presented;
        passes0 = wg_ctl.st_passes;
        enc0 = wg_ctl.st_enc_us;
        pipes0 = wg_ctl.st_pipelines;
        draws_sum = 0; busy_sum = 0; frames_sum = 0;
        return;
    }
    dt = (now - t0) / 1e9;
    if (dt < 1.0)
        return;
    frames = wg_ctl.frames_presented - frames0;
    wgpu_stats_text(&gt, frames_sum, gtx, sizeof gtx);
    fprintf(stderr, "[WGPU] %.1f fps presented, %u frames ended, %.0f draws/frame,"
            " %.1f passes/frame, %.1f pipelines made/s, translate %.2f ms, encode %.2f ms/frame,"
            " %u textures live, in flight %d, %u errors, %s\n",
            frames / dt, frames_sum, (double)draws_sum / frames_sum,
            (double)(wg_ctl.st_passes - passes0) / frames_sum,
            (wg_ctl.st_pipelines - pipes0) / dt, busy_sum / 1e6 / frames_sum,
            (wg_ctl.st_enc_us - enc0) / 1e3 / frames_sum, wg_ctl.st_tex_live,
            __atomic_load_n(&wg_ctl.frames_submitted, __ATOMIC_SEQ_CST)
                - __atomic_load_n(&wg_ctl.frames_done, __ATOMIC_SEQ_CST),
            wg_ctl.st_errors, gtx);
    t0 = now;
    frames0 = wg_ctl.frames_presented;
    passes0 = wg_ctl.st_passes;
    enc0 = wg_ctl.st_enc_us;
    pipes0 = wg_ctl.st_pipelines;
    draws_sum = 0; busy_sum = 0; frames_sum = 0;
}

/* The list so far and its arena to web/wgpu.js, then both start empty and
 * the texture write-once window opens again. */
static void wq_submit(uint32_t flags, uint32_t translate_us)
{
    WgFrame f;
    memset(&f, 0, sizeof f);
    f.magic = WG_MAGIC;
    f.version = WG_VERSION;
    f.size = sizeof f;
    f.seq = wg_frame;
    f.flags = flags;
    f.cmd_ptr = (uint32_t)(uintptr_t)s_list;
    f.cmd_bytes = s_list_len;
    f.cmd_count = s_list_count;
    wg_arena_take(&f.arena_ptr, &f.arena_bytes);
    f.translate_us = translate_us;
    f.draws = s_list_draws;
    wgpu_js_frame(&f);
    s_list_len = s_list_count = s_list_draws = 0;
    wtex_frame_end();
}

void wg_flush_partial(void)
{
    static int told;
    if (!told++)
        fprintf(stderr, "  [WGPU] frame arena full: the frame goes out in parts"
                " (RECOMP_WGPU_ARENA_MB; logged once)\n");
    wsurf_pass_suspend();
    wq_submit(WG_FF_PARTIAL, 0);
}


static void wq_end_frame(void)
{
    int w = 0, h = 0;
    SDL_Event e;

    emscripten_get_canvas_element_size("#canvas", &w, &h);
    wsurf_frame_end(w > 0 ? (uint32_t)w : 0, h > 0 ? (uint32_t)h : 0);
    wq_submit(0, (uint32_t)(s_busy_ns / 1000));
    wg_frame++;
    wpipe_frame_end();                  /* pipeline list: prewarm step, save */
    if (s_stats)
        wq_stats();
    s_draws = 0;
    s_busy_ns = 0;
    /* Input: the canvas is this thread's, as on the GL thread (gl_flip). */
    while (SDL_PollEvent(&e))
        xbox_sdl_keyboard_handle(&e);
}

/* Apply records until a flip is applied or the ring runs dry. live = 0
 * (no device): consume them all without drawing, so the executor runs on. */
static void wq_run(int live)
{
    int dry = 0;
    uint64_t t0 = wq_now_ns();

    for (;;) {
        uint64_t avail = __atomic_load_n(&s_q_head, __ATOMIC_SEQ_CST) - s_q_tail;
        const uint8_t *rec;
        const WQHdr *h;
        int flip;

        if (!avail) {
            uint64_t tw = wq_now_ns();
            s_busy_ns += tw - t0;
            if (dry++ >= 4)                 /* ~8 ms for the rest of the frame, then a turn */
                return;
            pthread_mutex_lock(&s_q_lock);
            __atomic_store_n(&s_q_consumer_waits, 1, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&s_q_head, __ATOMIC_SEQ_CST) == s_q_tail)
                wq_timed_wait();
            __atomic_store_n(&s_q_consumer_waits, 0, __ATOMIC_SEQ_CST);
            pthread_mutex_unlock(&s_q_lock);
            t0 = wq_now_ns();
            __atomic_add_fetch(&s_q_idle_ns, t0 - tw, __ATOMIC_RELAXED);
            continue;
        }
        rec = s_q + s_q_tail % WQ_BYTES;
        h = (const WQHdr *)rec;
        flip = h->type == WQ_FLIP;
        if (flip && live
            && __atomic_load_n(&wg_ctl.frames_submitted, __ATOMIC_SEQ_CST)
               - __atomic_load_n(&wg_ctl.frames_done, __ATOMIC_SEQ_CST)
               >= (int32_t)wg_ctl.max_in_flight) {
            s_busy_ns += wq_now_ns() - t0;  /* the GPU is behind: next turn */
            return;
        }
        if (h->type == WQ_DRAW) {
            /* The shadow changes always apply (the next record's diff
             * builds on them), the draw only with a device. */
            const WQDraw *d = (const WQDraw *)(h + 1);
            size_t at = sizeof(WQHdr) + sizeof(WQDraw);
            uint32_t i, a;
            for (i = 0; i < d->nreg; i++, at += sizeof(WQRegChunk)) {
                const WQRegChunk *c = (const WQRegChunk *)(rec + at);
                memcpy(g_regs + c->idx * 16, c->w, 64);
            }
            for (i = 0; i < d->nprog; i++, at += sizeof(WQRow)) {
                const WQRow *r = (const WQRow *)(rec + at);
                memcpy(g_prog[r->idx], r->w, 16);
            }
            for (i = 0; i < d->nconst; i++, at += sizeof(WQRow)) {
                const WQRow *r = (const WQRow *)(rec + at);
                memcpy(g_consts[r->idx], r->w, 16);
            }
            if (live) {
                Nv2aRawBatch bb = *(const Nv2aRawBatch *)(rec + at);
                bb.regs = g_regs;
                bb.vp_program = (const uint32_t (*)[4])g_prog;
                bb.vp_consts = (const float (*)[4])g_consts;
                for (a = 0; a < NV2A_RAW_ATTRS; a++)
                    if (bb.attr_direct & (1u << a))
                        bb.direct[a].ptr = rec + (uintptr_t)bb.direct[a].ptr;
                bb.indices = (const uint32_t *)(rec + (uintptr_t)bb.indices);
                wdraw_batch(&bb);
            }
            s_draws++;
        } else if (live && h->type == WQ_BLIT) {
            wsurf_blit((const Nv2aBlit *)(h + 1));
        } else if (live && h->type == WQ_CLEAR) {
            const WQClear *c = (const WQClear *)(h + 1);
            wsurf_clear(&c->s, c->zeta_va, c->flags, c->argb, c->zs, &c->r);
        } else if (live && flip) {
            s_busy_ns += wq_now_ns() - t0;
            wq_end_frame();
            t0 = wq_now_ns();
        }
        if (flip)
            __atomic_sub_fetch(&s_q_flips, 1, __ATOMIC_ACQ_REL);
        __atomic_store_n(&s_q_tail, s_q_tail + h->size, __ATOMIC_SEQ_CST);
        wq_wake(&s_q_producer_waits);
        if (flip) {
            s_busy_ns += wq_now_ns() - t0;
            return;                         /* the canvas shows it when the turn ends */
        }
    }
}

static void wq_turn(void)
{
    static int told;
    uint64_t t = wq_now_ns();
    int st = __atomic_load_n(&wg_ctl.dev_state, __ATOMIC_SEQ_CST);

    if (s_turn_end)                         /* between turns: idle too */
        __atomic_add_fetch(&s_q_idle_ns, t - s_turn_end, __ATOMIC_RELAXED);
    if (st == WG_DEV_READY) {
        if (!told) {
            told = 1;
            fprintf(stderr, "  [WGPU] device ready: canvas format %u, features 0x%X,"
                    " textures up to %u\n", wg_ctl.canvas_format, wg_ctl.features,
                    wg_ctl.max_tex_2d);
        }
        wq_run(1);
    } else if (st >= WG_DEV_FAILED) {
        if (told != 2) {
            told = 2;
            fprintf(stderr, "  [WGPU] device %s: nothing is drawn (RECOMP_GPU=webgl to use WebGL)\n",
                    st == WG_DEV_LOST ? "lost" : "failed");
        }
        wq_run(0);
    }                                       /* NONE/PENDING: the device is on its way */
    s_turn_end = wq_now_ns();
}

static void *wq_thread(void *arg)
{
    (void)arg;
    /* SDL's window: the canvas size (1280x720) and input, as on the GL
     * thread (gl_make_window), with no GL surface. */
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0
        || !SDL_CreateWindow("NV2A", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             1280, 720, SDL_WINDOW_RESIZABLE))
        fprintf(stderr, "  [WGPU] SDL window: %s (no keyboard)\n", SDL_GetError());
    wgpu_js_init(&wg_ctl);
    wgpu_stats_init();   /* M4 GPU timing */
    /* Timer turns (fps 1000: setTimeout, 0-4 ms), not requestAnimationFrame
     * (whole vblanks per frame; see q_gl_thread). Does not return. */
    emscripten_set_main_loop(wq_turn, 1000, 1);
    return NULL;
}

/* ── Install and the game-facing twins ────────────────────────────── */

void nv2a_wgpu_install(void)
{
    pthread_t t;
    pthread_attr_t at;
    const char *e = getenv("RECOMP_GL_QUEUE_FRAMES");

    s_q_max_flips = e && atoi(e) >= 1 ? atoi(e) : 2;
    e = getenv("RECOMP_WGPU_STATS");
    s_stats = e && *e == '1';
    wg_ctl.magic = WG_MAGIC;
    wg_ctl.version = WG_VERSION;
    wg_ctl.size = sizeof wg_ctl;
    wg_ctl.max_in_flight = 2;
    s_q = (uint8_t *)malloc(WQ_BYTES + 256);
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1u << 20);
    /* The render thread owns the canvas (OffscreenCanvas). */
    emscripten_pthread_attr_settransferredcanvases(&at, "#canvas");
    if (!s_q || pthread_create(&t, &at, wq_thread, NULL) != 0) {
        free(s_q);
        s_q = NULL;
        fprintf(stderr, "  [WGPU] no render thread: WebGL instead\n");
        nv2a_gl_install();
        return;
    }
    pthread_detach(t);
    s_active = 1;
    nv2a_backend_register(&s_backend);
    fprintf(stderr, "  [WGPU] NV2A WebGPU renderer registered (RECOMP_GPU=webgpu), %d frame(s) queued at most\n",
            s_q_max_flips);
}

int nv2a_wgpu_active(void) { return s_active; }

/* nv2a_wgpu_prewarm_start: wgpu_pipe.c (the pipeline list, M3). */

/* Movies take the ordinary texture path (ADR 0011); nothing is shown by
 * the renderer itself. */
void nv2a_wgpu_movie_frame(uint32_t va, uint32_t w, uint32_t h,
                           const uint8_t *y, int ys, const uint8_t *u, int us,
                           const uint8_t *v, int vs, const float m[12], float alpha)
{
    (void)va; (void)w; (void)h; (void)y; (void)ys; (void)u; (void)us;
    (void)v; (void)vs; (void)m; (void)alpha;
}
void nv2a_wgpu_movie_end(uint32_t va) { (void)va; }
int nv2a_wgpu_movie_shown(uint32_t va) { (void)va; return 0; }

uint32_t nv2a_wgpu_frame_count(void) { return wg_frame; }
uint64_t nv2a_wgpu_texture_bytes(void) { return wtex_bytes(); }

void nv2a_wgpu_prof_stats(double secs, char *out, size_t n)
{
    static uint32_t frames0, enc0;
    static WgGtSnap gt;
    uint32_t frames = wg_frame - frames0;
    char gtx[160];
    wgpu_stats_text(&gt, frames, gtx, sizeof gtx);
    snprintf(out, n, "WebGPU: %.1f frames/s, encode %.2f ms/frame, %u textures live, %u errors, %s",
             frames / secs, frames ? (wg_ctl.st_enc_us - enc0) / 1e3 / frames : 0.0,
             wg_ctl.st_tex_live, wg_ctl.st_errors, gtx);
    frames0 = wg_frame;
    enc0 = wg_ctl.st_enc_us;
}

void nv2a_wgpu_queue_stats(double secs, int *gl_idle_pct, int *exec_wait_pct)
{
    if (!s_active) {
        *gl_idle_pct = *exec_wait_pct = -1;
        return;
    }
    *gl_idle_pct = (int)(__atomic_exchange_n(&s_q_idle_ns, 0, __ATOMIC_RELAXED) / (secs * 1e7));
    *exec_wait_pct = (int)(__atomic_exchange_n(&s_q_exec_wait_ns, 0, __ATOMIC_RELAXED) / (secs * 1e7));
}
