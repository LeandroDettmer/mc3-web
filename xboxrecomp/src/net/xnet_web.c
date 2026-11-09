/* xnet_web.c -- the XNet HLE's browser transport (Emscripten): System Link
 * over the server's virtual LAN switch (WebSocket /lan; mc3-web
 * web/server/crates/lan, after nfsu2-web's, MIT, th0mk).
 *
 * The page owns the WebSocket (web/net.js, main thread); the guest's
 * datagrams cross between it and this file through a mailbox in the shared
 * wasm memory, the design of nfsu2-web's runtime/web/net_proto.h, cut down:
 *
 *   header (128 bytes, u32 words, every head/tail/state word atomic)
 *   TX ring   XW_TX slots, C produces, the page sends them to the switch
 *   RX ring   XW_RX slots, the page fills them from the switch, C consumes
 *
 * Rings are single-producer single-consumer, head and tail free-running slot
 * counters (slot = counter % slots); the producer fills a slot, then stores
 * head; the consumer reads it, then stores tail. C rings tx_bell
 * (memory.atomic.notify) after it queued a datagram; the page waits on it
 * with Atomics.waitAsync. A slot: len, ip (host order: the destination in
 * TX, the source in RX), src_port | dst_port << 16, 0, then the datagram.
 * On the switch the page sends kind 3 frames: [0] 3 [1..3] 0 [4..7] source
 * vip [8..11] destination vip (0xFFFFFFFF broadcast) [12..13] source port
 * [14..15] destination port [16..] the datagram, big-endian.
 *
 * Sockets: each has a receive queue; whatever the page queued is sorted into
 * them by destination port at every recvfrom. Broadcasts and datagrams to
 * our own address are also delivered here, to our own sockets on that port,
 * as a LAN stack loops them back (the switch never sends a broadcast back
 * to its sender). Every socket is non-blocking (xnet_hle.h).
 *
 * The player's address is the vip the switch handed the page (its welcome);
 * the XNADDR's MAC is derived from it by xnet_hle.c, the same one the
 * switch reports. local_addr waits for the page up to MC3_NET_WAIT_MS
 * (default 5000) at XNetStartup: the page opens the link on Play. */
#ifdef __EMSCRIPTEN__

#include "xnet_hle.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <emscripten.h>

#define XW_MAGIC    0x42574E58u   /* 'XNWB' */
#define XW_VERSION  1u
#define XW_TX       64u
#define XW_RX       128u
#define XW_SLOT     1536u
#define XW_SLOT_HDR 16u
#define XW_MAX      1472           /* an Ethernet MTU's UDP payload */
#define XW_HDR      128u
#define XW_NSOCK    64
#define XW_QLEN     64             /* datagrams queued per socket; more: dropped, as a full socket buffer drops */
#define XW_BCAST    0xFFFFFFFFu

/* header words; web/net.js mirrors them by index */
enum {
    W_MAGIC, W_VERSION, W_TX_SLOTS, W_RX_SLOTS, W_SLOT_BYTES, W_TX_OFF, W_RX_OFF, W_HDR_BYTES,
    W_TX_HEAD,   /* C */
    W_TX_TAIL,   /* page */
    W_RX_HEAD,   /* page */
    W_RX_TAIL,   /* C */
    W_TX_BELL,   /* C: +1 and notify after queuing */
    W_STATE,     /* page: 0 none, 1 connecting, 2 online, 3 offline (failed or lost) */
    W_VIP,       /* page: our address, host order */
    W_PEERS,     /* page: players on the switch, us included */
    W_TX_DROP,   /* page: sent while offline */
    W_RX_DROP,   /* page: RX ring full */
    W_C_TX_FULL, /* C: TX ring full */
    W_C_RX_LOST  /* C: datagrams nobody's socket took (no socket on that port, its queue full) */
};
enum { ST_NONE, ST_CONNECTING, ST_ONLINE, ST_OFFLINE };

typedef struct {
    uint32_t h[XW_HDR / 4];
    uint8_t tx[XW_TX][XW_SLOT];
    uint8_t rx[XW_RX][XW_SLOT];
} xw_mailbox;
_Static_assert(sizeof(((xw_mailbox *)0)->h) == XW_HDR, "header");

static _Alignas(16) xw_mailbox g_mb;

EMSCRIPTEN_KEEPALIVE uintptr_t xnet_web_mailbox(void) { return (uintptr_t)&g_mb; }

static uint32_t ld(int w) { return __atomic_load_n(&g_mb.h[w], __ATOMIC_ACQUIRE); }
static void st(int w, uint32_t v) { __atomic_store_n(&g_mb.h[w], v, __ATOMIC_RELEASE); }
static void bump(int w) { __atomic_fetch_add(&g_mb.h[w], 1, __ATOMIC_ACQ_REL); }

typedef struct { uint32_t ip; uint16_t port, len; uint8_t d[XW_MAX]; } dgram;
typedef struct {
    int used;
    uint16_t port;             /* 0: not bound */
    unsigned head, tail;       /* queue: q[tail % XW_QLEN] .. */
    dgram *q;
} wsock;

static wsock g_ws[XW_NSOCK];
static uint32_t g_vip;         /* host order */
static uint16_t g_eph = 49152;

static uint32_t bswap(uint32_t v) { return __builtin_bswap32(v); }   /* wasm is little-endian */

static void deliver(uint32_t src, uint16_t sport, uint16_t dport, const uint8_t *d, int n)
{
    int i, taken = 0;
    for (i = 0; i < XW_NSOCK; i++) {
        wsock *w = &g_ws[i];
        dgram *g;
        if (!w->used || w->port != dport || w->head - w->tail >= XW_QLEN) continue;
        g = &w->q[w->head % XW_QLEN];
        g->ip = src; g->port = sport; g->len = (uint16_t)n;
        memcpy(g->d, d, (size_t)n);
        w->head++;
        taken = 1;   /* a port is bound once, so one socket takes it */
        break;
    }
    if (!taken) bump(W_C_RX_LOST);
}

/* The page's link reconnects by itself (web/lan.js) and asks for our address back; if it was taken meanwhile the
 * switch gives another: follow it.  Peers of a race under way still send to the old one (lost); the next session
 * (host / join again) is on the new one: xnet_hle.c reads it for the XNADDR. */
static uint32_t w_cur_addr(void)
{
    uint32_t v = ld(W_VIP);
    if (g_vip && v && v != g_vip && ld(W_STATE) == ST_ONLINE) {
        fprintf(stderr, "[XNET] web: the switch link came back as %u.%u.%u.%u (was %u.%u.%u.%u)\n",
                v >> 24, v >> 16 & 255, v >> 8 & 255, v & 255, g_vip >> 24, g_vip >> 16 & 255, g_vip >> 8 & 255, g_vip & 255);
        g_vip = v;
    }
    return bswap(g_vip);
}

/* what the page queued, into the sockets' queues */
static void pump(void)
{
    uint32_t tail = ld(W_RX_TAIL), head = ld(W_RX_HEAD);
    w_cur_addr();
    for (; tail != head; tail++) {
        const uint8_t *s = g_mb.rx[tail % XW_RX];
        uint32_t len, ip, ports;
        memcpy(&len, s, 4); memcpy(&ip, s + 4, 4); memcpy(&ports, s + 8, 4);
        if (len <= XW_MAX) deliver(ip, (uint16_t)ports, (uint16_t)(ports >> 16), s + XW_SLOT_HDR, (int)len);
        st(W_RX_TAIL, tail + 1);
    }
}

static uint32_t w_local_addr(void)
{
    const char *e = getenv("MC3_NET_WAIT_MS");
    int ms = e && *e ? atoi(e) : 5000, waited = 0;
    if (ld(W_MAGIC) != XW_MAGIC) {
        g_mb.h[W_VERSION] = XW_VERSION;
        g_mb.h[W_TX_SLOTS] = XW_TX;
        g_mb.h[W_RX_SLOTS] = XW_RX;
        g_mb.h[W_SLOT_BYTES] = XW_SLOT;
        g_mb.h[W_TX_OFF] = (uint32_t)offsetof(xw_mailbox, tx);
        g_mb.h[W_RX_OFF] = (uint32_t)offsetof(xw_mailbox, rx);
        g_mb.h[W_HDR_BYTES] = XW_HDR;
        st(W_MAGIC, XW_MAGIC);   /* last: the page attaches when it sees it */
    }
    while (ld(W_STATE) != ST_ONLINE && ld(W_STATE) != ST_OFFLINE && waited < ms) {
        usleep(10000);
        waited += 10;
    }
    g_vip = ld(W_STATE) == ST_ONLINE ? ld(W_VIP) : 0;
    fprintf(stderr, "[XNET] web: %s after %d ms\n", g_vip ? "on the switch" : "no switch (the page has no link)", waited);
    return bswap(g_vip);
}

static intptr_t w_open(void)
{
    int i;
    for (i = 0; i < XW_NSOCK && g_ws[i].used; i++)
        ;
    if (i == XW_NSOCK) return -XNET_WSAEMFILE;
    if (!g_ws[i].q && !(g_ws[i].q = (dgram *)malloc(sizeof(dgram) * XW_QLEN))) return -XNET_WSAENOBUFS;
    g_ws[i].used = 1;
    g_ws[i].port = 0;
    g_ws[i].head = g_ws[i].tail = 0;
    return i;
}

static int port_used(uint16_t p)
{
    int i;
    for (i = 0; i < XW_NSOCK; i++)
        if (g_ws[i].used && g_ws[i].port == p) return 1;
    return 0;
}

static int w_bind(intptr_t h, uint16_t port)
{
    int k;
    if (!port) {
        for (k = 0; k < 16384 && port_used(g_eph); k++)
            g_eph = (uint16_t)(g_eph == 65535 ? 49152 : g_eph + 1);
        if (k == 16384) return -XNET_WSAEADDRINUSE;
        port = g_eph;
        g_eph = (uint16_t)(g_eph == 65535 ? 49152 : g_eph + 1);
    } else if (port_used(port)) {
        return -XNET_WSAEADDRINUSE;
    }
    g_ws[h].port = port;
    return 0;
}

static int w_sendto(intptr_t h, const void *p, int n, uint32_t ip_n, uint16_t port)
{
    uint32_t dst = bswap(ip_n), head, ports;
    uint8_t *s;
    if (n > XW_MAX) return -XNET_WSAEMSGSIZE;
    if (!g_vip) return -XNET_WSAENETDOWN;
    if (dst == XW_BCAST || dst == g_vip || (dst >> 24) == 127) {
        pump();   /* keep the order of what came before */
        deliver(g_vip, g_ws[h].port, port, (const uint8_t *)p, n);
        if (dst != XW_BCAST) return n;
    }
    head = ld(W_TX_HEAD);
    if (head - ld(W_TX_TAIL) >= XW_TX) { bump(W_C_TX_FULL); return n; }   /* dropped, as on a congested LAN */
    s = g_mb.tx[head % XW_TX];
    ports = g_ws[h].port | (uint32_t)port << 16;
    memcpy(s, &(uint32_t){(uint32_t)n}, 4); memcpy(s + 4, &dst, 4); memcpy(s + 8, &ports, 4);
    memset(s + 12, 0, 4);
    memcpy(s + XW_SLOT_HDR, p, (size_t)n);
    st(W_TX_HEAD, head + 1);
    bump(W_TX_BELL);
    __builtin_wasm_memory_atomic_notify((int *)&g_mb.h[W_TX_BELL], 1);
    return n;
}

static int w_recvfrom(intptr_t h, void *p, int n, uint32_t *ip_n, uint16_t *port)
{
    wsock *w = &g_ws[h];
    dgram *g;
    int r;
    pump();
    if (w->head == w->tail) return -XNET_WSAEWOULDBLOCK;
    g = &w->q[w->tail % XW_QLEN];
    r = g->len;
    if (r > n) {   /* Winsock: the rest is lost, WSAEMSGSIZE */
        memcpy(p, g->d, (size_t)n);
        w->tail++;
        return -XNET_WSAEMSGSIZE;
    }
    memcpy(p, g->d, (size_t)r);
    *ip_n = bswap(g->ip);
    *port = g->port;
    w->tail++;
    return r;
}

static void w_close(intptr_t h) { g_ws[h].used = 0; g_ws[h].port = 0; }

const xnet_transport xnet_transport_web = {
    "web", w_local_addr, w_open, w_bind, w_sendto, w_recvfrom, w_close, w_cur_addr,
};

#endif /* __EMSCRIPTEN__ */
