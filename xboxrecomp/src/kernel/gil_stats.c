/* gil_stats.c -- how often the guest lock changes hands, and to whom the
 * main thread loses it.
 *
 * The lock sampler (MC3_PERF) says who holds the lock; it cannot say what a
 * hand-over costs. On the Switch each one is a scheduler round trip: in
 * MC3's flythrough the main thread waited 25-30% of the time for a lock the
 * other threads held 11-15% of it (2026-10-21). This counts hand-overs per
 * thread and, for each wait of the main thread, which thread it waited
 * behind and for how long -- the number a batching or deferral change has
 * to bring down.
 *
 * Fed by gil_lock (kernel_bridge.c) under the lock's own critical section;
 * read by the perf report. Threads are named by xbox_gil_label (the timer
 * thread: "timer", i.e. ISRs, DPCs, timers); unnamed ones by thread id. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xbox_memory_layout.h"     /* RECOMP_TLS */

#define GS_SLOTS 16
typedef struct {
    uint32_t    tid;
    const char *label;
    int         main;
    long        takes;          /* acquisitions after another thread had it */
    long        main_waits;     /* main-thread waits spent behind this one */
    long long   main_wait_us;
} GsSlot;

static GsSlot          s_slot[GS_SLOTS];
static long            s_main_waits, s_main_free;
static long long       s_main_wait_us;
static uint32_t        s_last_tid;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static RECOMP_TLS const char *t_label;

void xbox_gil_label(const char *name) { t_label = name; }
const char *xbox_gil_my_label(void) { return t_label; }

static GsSlot *slot_for(uint32_t tid, const char *label)
{
    int i;
    for (i = 0; i < GS_SLOTS; i++)
        if (s_slot[i].tid == tid)
            return &s_slot[i];
    for (i = 0; i < GS_SLOTS; i++)
        if (!s_slot[i].tid) {
            s_slot[i].tid = tid;
            s_slot[i].label = label;
            return &s_slot[i];
        }
    return NULL;
}

/* One acquisition: by `tid` (`label` may be NULL), the main thread or not;
 * if it had to wait, behind `blocker` (its id and label, 0 when the lock
 * was between holders) for `wait_us`. */
void xbox_gil_stat_take(uint32_t tid, const char *label, int is_main, int waited,
                        uint32_t blocker, const char *blocker_label, long long wait_us)
{
    GsSlot *s;
    pthread_mutex_lock(&s_lock);
    if (tid != s_last_tid && (s = slot_for(tid, label)) != NULL) {
        s->takes++;
        s->main = is_main;
    }
    s_last_tid = tid;
    if (is_main) {
        if (waited) {
            s_main_waits++;
            s_main_wait_us += wait_us;
            if (blocker && (s = slot_for(blocker, blocker_label)) != NULL) {
                s->main_waits++;
                s->main_wait_us += wait_us;
            }
        } else {
            s_main_free++;
        }
    }
    pthread_mutex_unlock(&s_lock);
}

/* "[perf] guest lock: ..." lines for the last `secs` seconds, then reset. */
void xbox_gil_stat_report(double secs)
{
    long takes = 0;
    int i, k;
    GsSlot copy[GS_SLOTS];
    long mw, mf;
    long long mus;
    pthread_mutex_lock(&s_lock);
    memcpy(copy, s_slot, sizeof copy);
    mw = s_main_waits; mf = s_main_free; mus = s_main_wait_us;
    memset(s_slot, 0, sizeof s_slot);
    s_main_waits = s_main_free = 0;
    s_main_wait_us = 0;
    pthread_mutex_unlock(&s_lock);
    if (secs <= 0)
        return;
    for (i = 0; i < GS_SLOTS; i++)
        takes += copy[i].takes;
    fprintf(stderr, "[perf] guest lock: %.0f hand-overs/s; main thread took it %.0f/s, "
                    "%.0f/s after a wait (avg %.0f us)\n",
            takes / secs, (mw + mf) / secs, mw / secs, mw ? (double)mus / mw : 0.0);
    for (k = 0; k < 6; k++) {
        int best = -1;
        for (i = 0; i < GS_SLOTS; i++)
            if (copy[i].tid && !copy[i].main
                && (best < 0 || copy[i].main_wait_us > copy[best].main_wait_us))
                best = i;
        if (best < 0 || !copy[best].takes)
            break;
        if (copy[best].label)
            fprintf(stderr, "[perf]    %-8s", copy[best].label);
        else
            fprintf(stderr, "[perf]    thr %-4u", copy[best].tid);
        fprintf(stderr, " took it %.0f/s; main waited behind it %.0f/s, %.0f ms/s\n",
                copy[best].takes / secs, copy[best].main_waits / secs,
                copy[best].main_wait_us / secs / 1000.0);
        copy[best].tid = 0;
    }
}
