/*
 * Asynchronous file reads, browser only (2026-11-16).
 *
 * XACT streams its wave banks (*.xwb: music, engine sounds, ambience) with
 * overlapped reads: XAPI ReadFile on a handle opened with
 * FILE_FLAG_OVERLAPPED (no FILE_SYNCHRONOUS_IO_* in CreateOptions), an
 * OVERLAPPED of its own, then GetOverlappedResult(bWait = FALSE) from each
 * XACTEngineDoWork until the status block leaves STATUS_PENDING (MC3 Remix:
 * read helper sub_00407B0A, its callers all pass an OVERLAPPED; poll at
 * 0x0040828E). The bridge completed every read before returning, on the
 * thread that asked -- the main thread, in DoWork -- so a read that went to
 * the network (a cold disc cache) froze the game for its whole length: the
 * freeze at each music track change of a new player (owner, Chrome
 * incognito, 2026-11-15/16).
 *
 * Here such a read is queued and NtReadFile returns STATUS_PENDING; one
 * worker thread reads it (pread: the handle's position stays the caller's),
 * writes the status block, then sets the event. The game goes on; the sound
 * starts when its data is in, as from a DVD. Only reads of an asynchronous
 * handle to a .xwb file, with a byte offset, no APC routine and a status
 * block that is not on the caller's stack (a synchronous ReadFile on an
 * overlapped handle keeps its OVERLAPPED on the stack and waits on the file
 * handle). A wait on the file handle (GetOverlappedResult(bWait = TRUE)
 * without an event) and NtClose wait for its queued reads first.
 * RECOMP_ASYNC_READ=0 turns it off. '[IO] <file>: reads in the background
 * ...' once per file, '[IO] background read ... took N ms' for slow ones.
 */
#include "kernel.h"

#ifdef __EMSCRIPTEN__
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <emscripten.h>

#define AR_MAX 64                   /* queued reads; a full queue reads synchronously */

struct ar_job {
    int       fd;
    uint32_t  token;                /* the guest's handle, for NtClose and waits */
    uint8_t  *buf;
    uint32_t  len;
    int64_t   off;
    uint32_t *ios;                  /* the guest's IO_STATUS_BLOCK: Status, Information */
    HANDLE    ev;
    double    t0;
    char      name[40];
};

static struct ar_job   s_q[AR_MAX];
static unsigned        s_head, s_tail;    /* queued: [s_head, s_tail); s_head is the one being read */
static pthread_mutex_t s_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_more = PTHREAD_COND_INITIALIZER, s_done = PTHREAD_COND_INITIALIZER;
static int             s_state;           /* 0 not started, 1 running, -1 off */

static void *ar_worker(void *arg)
{
    static int slow_logged;
    (void)arg;
    pthread_mutex_lock(&s_m);
    for (;;) {
        struct ar_job j;
        size_t done = 0;
        ssize_t n = 0;
        uint32_t st;
        double ms;
        while (s_head == s_tail)
            pthread_cond_wait(&s_more, &s_m);
        j = s_q[s_head % AR_MAX];         /* stays counted until done: NtClose waits for it */
        pthread_mutex_unlock(&s_m);

        while (done < j.len) {
            n = pread(j.fd, j.buf + done, j.len - done, (off_t)(j.off + (int64_t)done));
            if (n <= 0)
                break;
            done += (size_t)n;
        }
        st = n < 0 && !done ? (uint32_t)STATUS_UNSUCCESSFUL
           : !done && j.len ? (uint32_t)STATUS_END_OF_FILE : (uint32_t)STATUS_SUCCESS;
        /* Information before Status: the poll reads Status first */
        __atomic_store_n(&j.ios[1], (uint32_t)done, __ATOMIC_SEQ_CST);
        __atomic_store_n(&j.ios[0], st, __ATOMIC_SEQ_CST);
        if (j.ev)
            SetEvent(j.ev);

        ms = emscripten_get_now() - j.t0;
        if (ms >= 250.0 && slow_logged < 40) {
            slow_logged++;
            fprintf(stderr, "[IO] background read %s @%lld %u KB took %.0f ms (the game went on meanwhile)\n",
                    j.name, (long long)j.off, j.len >> 10, ms);
        }

        pthread_mutex_lock(&s_m);
        s_head++;
        pthread_cond_broadcast(&s_done);
    }
    return NULL;
}

static int ar_has(uint32_t token)
{
    unsigned i;
    for (i = s_head; i != s_tail; i++)
        if (s_q[i % AR_MAX].token == token)
            return 1;
    return 0;
}

/* Queue a read of `len` bytes at `off` of host handle h (guest handle token)
 * into buf, completed into ios (guest IO_STATUS_BLOCK) and ev (may be NULL).
 * 1 when queued (Status is STATUS_PENDING until done), 0 to read it as before. */
int xbox_async_read(HANDLE h, uint32_t token, void *buf, uint32_t len, int64_t off,
                    uint32_t *ios, HANDLE ev)
{
    static char seen[8][40];
    const char *path, *base;
    size_t pl;
    int fd, k;
    struct ar_job *j;

    if (s_state < 0 || !len || off < 0)
        return 0;
    if (s_state == 0) {
        const char *e = getenv("RECOMP_ASYNC_READ");
        if (e && *e == '0') { s_state = -1; return 0; }
    }
    path = w32_handle_path(h);
    fd = w32_handle_fd(h);
    if (!path || fd < 0 || (pl = strlen(path)) < 4 || strcasecmp(path + pl - 4, ".xwb"))
        return 0;
    base = strrchr(path, '/');
    base = base ? base + 1 : path;

    pthread_mutex_lock(&s_m);
    if (s_state == 0) {
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 64 * 1024);
        s_state = pthread_create(&t, &a, ar_worker, NULL) == 0 ? 1 : -1;
        pthread_attr_destroy(&a);
        if (s_state < 0) {
            pthread_mutex_unlock(&s_m);
            fprintf(stderr, "[IO] no thread for background reads: XACT's reads wait as before\n");
            return 0;
        }
    }
    if (s_tail - s_head >= AR_MAX) {
        pthread_mutex_unlock(&s_m);
        return 0;
    }
    ios[1] = 0;
    __atomic_store_n(&ios[0], (uint32_t)STATUS_PENDING, __ATOMIC_SEQ_CST);
    if (ev)
        ResetEvent(ev);
    j = &s_q[s_tail % AR_MAX];
    j->fd = fd; j->token = token; j->buf = (uint8_t *)buf; j->len = len; j->off = off;
    j->ios = ios; j->ev = ev; j->t0 = emscripten_get_now();
    snprintf(j->name, sizeof j->name, "%s", base);
    s_tail++;
    pthread_cond_signal(&s_more);
    pthread_mutex_unlock(&s_m);

    for (k = 0; k < 8 && seen[k][0]; k++)
        if (!strcmp(seen[k], j->name))
            break;
    if (k < 8 && !seen[k][0]) {
        snprintf(seen[k], sizeof seen[k], "%s", base);
        fprintf(stderr, "[IO] %s: reads in the background (overlapped handle): the game no longer waits for them\n", base);
    }
    return 1;
}

/* Any read of this guest handle still queued or being read? */
int xbox_async_read_pending(uint32_t token)
{
    int r;
    /* every guest wait asks: nothing queued is the answer without the lock */
    if (s_state <= 0 || __atomic_load_n(&s_head, __ATOMIC_ACQUIRE) == __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE))
        return 0;
    pthread_mutex_lock(&s_m);
    r = ar_has(token);
    pthread_mutex_unlock(&s_m);
    return r;
}

/* Wait until no read of this guest handle is queued or being read. */
void xbox_async_read_drain(uint32_t token)
{
    if (s_state <= 0)
        return;
    pthread_mutex_lock(&s_m);
    while (ar_has(token))
        pthread_cond_wait(&s_done, &s_m);
    pthread_mutex_unlock(&s_m);
}
#endif
