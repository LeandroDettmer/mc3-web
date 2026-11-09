/* roam_web.c -- free roam's platform in the browser (Emscripten): one virtual
 * UDP endpoint (port ROAM_PORT) on the page's link to the server's /lan
 * switch, the same link and the same 10.77.x.x address System Link uses.
 * After nfsu2-web's runtime/web/roam_web.c (MIT License, Copyright (c) 2026
 * th0mk).
 *
 *   game thread / network thread --TX ring--> page (web/net.js) --WebSocket--> switch
 *   network thread <--RX ring-- page <--WebSocket-- switch
 *
 * The mailbox is xnet_web.c's layout (header of 32 u32 words, SPSC TX and RX
 * rings, slot: len, ip, src_port | dst_port << 16, 0, datagram) with its own
 * magic, so web/net.js attaches it as a second mailbox and hands it the
 * switch's kind 3 frames for port ROAM_PORT (the others go to System Link).
 * The page also bumps W_RX_BELL and notifies after each datagram it queues.
 *
 * The network thread (a pthread) waits on that bell up to 10 ms
 * (memory.atomic.wait32), feeds each datagram to the session and runs
 * rc_tick, under the lock.  Sending (either thread, lock held) queues a slot
 * and rings W_TX_BELL; the switch puts our address on the frame.  The clock is
 * CLOCK_MONOTONIC (performance.now: one clock in every worker of the page). */
#ifdef __EMSCRIPTEN__
#include "roam_plat.h"
#include <emscripten.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/random.h>

#define RW_MAGIC    0x52574D52u   /* 'RMWR' */
#define RW_TX       128u
#define RW_RX       256u
#define RW_SLOT     1280u
#define RW_SLOT_HDR 16u
#define RW_HDR      128u
#define RW_BCAST    0xFFFFFFFFu

/* header words: xnet_web.c's indices, web/net.js mirrors them */
enum {
    W_MAGIC, W_VERSION, W_TX_SLOTS, W_RX_SLOTS, W_SLOT_BYTES, W_TX_OFF, W_RX_OFF, W_HDR_BYTES,
    W_TX_HEAD, W_TX_TAIL, W_RX_HEAD, W_RX_TAIL, W_TX_BELL, W_STATE, W_VIP, W_PEERS,
    W_TX_DROP, W_RX_DROP, W_C_TX_FULL, W_C_RX_LOST, W_RX_BELL
};
enum { ST_NONE, ST_CONNECTING, ST_ONLINE, ST_OFFLINE };

typedef struct {
    uint32_t h[RW_HDR / 4];
    uint8_t tx[RW_TX][RW_SLOT];
    uint8_t rx[RW_RX][RW_SLOT];
} rw_mailbox;

static _Alignas(16) rw_mailbox g_mb;
static pthread_t       g_thr;
static int             g_thr_on;
static volatile int    g_stop;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static rc_session     *g_s;
static uint16_t        g_port;

EMSCRIPTEN_KEEPALIVE uintptr_t roam_web_mailbox(void) { return (uintptr_t)&g_mb; }

static uint32_t ld(int w){ return __atomic_load_n(&g_mb.h[w], __ATOMIC_ACQUIRE); }
static void st(int w, uint32_t v){ __atomic_store_n(&g_mb.h[w], v, __ATOMIC_RELEASE); }
static void bump(int w){ __atomic_fetch_add(&g_mb.h[w], 1, __ATOMIC_ACQ_REL); }

void roam_plat_lock(void){ pthread_mutex_lock(&g_mu); }
void roam_plat_unlock(void){ pthread_mutex_unlock(&g_mu); }
int64_t roam_plat_now_us(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
uint32_t roam_plat_random(void){ uint32_t r; do { if (getentropy(&r, sizeof r)) r = (uint32_t)roam_plat_now_us() * 2654435761u; } while (!r); return r; }

/* one datagram for the page (lock held) */
static void push(uint32_t dst, uint16_t dport, const uint8_t *buf, int len){
    static int down;   /* the page's link closed: every datagram silently gone, peers time us out */
    int on = ld(W_STATE) == ST_ONLINE;
    if (on != !down){
        down = !on;
        fprintf(stderr, down ? "[roam] web: the switch link is down (state %u): datagrams dropped\n"
                             : "[roam] web: the switch link is back (state %u)\n", ld(W_STATE));
    }
    if (len <= 0 || len > (int)(RW_SLOT - RW_SLOT_HDR) || !on) return;
    uint32_t head = ld(W_TX_HEAD);
    if (head - ld(W_TX_TAIL) >= RW_TX){ bump(W_C_TX_FULL); return; }   /* dropped, as a congested LAN drops */
    uint8_t *s = g_mb.tx[head % RW_TX];
    uint32_t n = (uint32_t)len, ports = g_port | (uint32_t)dport << 16, z = 0;
    memcpy(s, &n, 4); memcpy(s + 4, &dst, 4); memcpy(s + 8, &ports, 4); memcpy(s + 12, &z, 4);
    memcpy(s + RW_SLOT_HDR, buf, n);
    st(W_TX_HEAD, head + 1);
    bump(W_TX_BELL);
    __builtin_wasm_memory_atomic_notify((int *)&g_mb.h[W_TX_BELL], 1);
}
static void io_send(void *u, uint32_t ip, uint16_t port, const uint8_t *buf, int len){ (void)u; push(ip, port, buf, len); }
static void io_bcast(void *u, uint16_t port, const uint8_t *buf, int len){ (void)u; push(RW_BCAST, port, buf, len); }

static void *net_thread(void *arg){
    (void)arg;
    while (!g_stop){
        uint32_t bell = ld(W_RX_BELL);
        int got = 0;
        roam_plat_lock();
        int64_t now = roam_plat_now_us();
        uint32_t tail = ld(W_RX_TAIL), head = ld(W_RX_HEAD);
        for (; tail != head; tail++, got++){
            const uint8_t *s = g_mb.rx[tail % RW_RX];
            uint32_t len, ip, ports;
            memcpy(&len, s, 4); memcpy(&ip, s + 4, 4); memcpy(&ports, s + 8, 4);
            if (len <= RW_SLOT - RW_SLOT_HDR) rc_receive(g_s, ip, (uint16_t)ports, s + RW_SLOT_HDR, (int)len, now);
            st(W_RX_TAIL, tail + 1);
        }
        rc_tick(g_s, now);
        roam_plat_unlock();
        if (!got) __builtin_wasm_memory_atomic_wait32((int *)&g_mb.h[W_RX_BELL], (int)bell, 10 * 1000000LL);
    }
    return 0;
}

int roam_plat_start(rc_session *s, uint16_t port){
    if (g_thr_on) return -1;
    if (ld(W_MAGIC) != RW_MAGIC){
        g_mb.h[W_VERSION] = 1;
        g_mb.h[W_TX_SLOTS] = RW_TX;
        g_mb.h[W_RX_SLOTS] = RW_RX;
        g_mb.h[W_SLOT_BYTES] = RW_SLOT;
        g_mb.h[W_TX_OFF] = (uint32_t)offsetof(rw_mailbox, tx);
        g_mb.h[W_RX_OFF] = (uint32_t)offsetof(rw_mailbox, rx);
        g_mb.h[W_HDR_BYTES] = RW_HDR;
        st(W_MAGIC, RW_MAGIC);   /* last: the page attaches when it sees it */
    }
    /* the page attaches within 50 ms and says whether it has a link */
    for (int waited = 0; waited < 2000 && ld(W_STATE) != ST_ONLINE && ld(W_STATE) != ST_OFFLINE; waited += 10) usleep(10000);
    if (ld(W_STATE) != ST_ONLINE){
        fprintf(stderr, "[roam] web: no switch link (Online off on the page, or the server is gone)\n");
        return -1;
    }
    g_s = s; g_port = port;
    s->io.user = 0; s->io.send = io_send; s->io.broadcast = io_bcast;
    /* drop what queued before this session (an earlier one's datagrams) */
    st(W_RX_TAIL, ld(W_RX_HEAD));
    g_stop = 0;
    if (pthread_create(&g_thr, 0, net_thread, 0) != 0) return -1;
    g_thr_on = 1;
    return 0;
}
void roam_plat_stop(void){
    if (!g_thr_on) return;
    g_stop = 1;
    bump(W_RX_BELL);
    __builtin_wasm_memory_atomic_notify((int *)&g_mb.h[W_RX_BELL], 1);
    pthread_join(g_thr, 0);
    g_thr_on = 0;
}
#endif /* __EMSCRIPTEN__ */
