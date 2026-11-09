/* xnet_hle.c -- XNet socket API emulation (System Link) over a pluggable
 * transport; the native one, host UDP sockets, is at the end. See xnet_hle.h.
 *
 * Native transport switches:
 *   RECOMP_NET_PORT_BASE  host port = base + guest port (default 20000: the
 *                         titles' ports, e.g. 1000, are privileged on Linux).
 *                         Every machine in a game uses the same base.
 *   RECOMP_NET_ADDR       the player's IPv4 (default: the host's LAN address);
 *                         sockets bind to it. Two instances on one machine:
 *                         127.0.0.1 and 127.0.0.2.
 *   RECOMP_NET_PEERS      ip,ip,...: a broadcast goes to each of these by
 *                         unicast instead of to 255.255.255.255 (one machine,
 *                         or networks that drop broadcasts).
 */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "xnet_hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SOL_SOCKET_XB   0xFFFF
#define SO_BROADCAST_XB 0x0020
#define FIONBIO_XB      0x8004667Eu
#define AF_INET_XB      2
#define SOCK_DGRAM_XB   2
#define IPPROTO_UDP_XB  17
#define IPPROTO_VDP_XB  254
#define BROADCAST_N     0xFFFFFFFFu
#define NSOCK           64

typedef struct {
    int used, nonblock, bcast, bound;
    intptr_t h;
} xsock;

static xsock g_s[NSOCK];
static int g_up;
static uint32_t g_ip;   /* network order */
static _Thread_local int g_err;
#ifdef __EMSCRIPTEN__
static const xnet_transport *g_t = &xnet_transport_web;   /* xnet_web.c */
#else
static const xnet_transport *g_t = &xnet_transport_udp;
#endif

static int fail(int e) { g_err = e; return XNET_SOCKET_ERROR; }
static xsock *get(uint32_t s) { return s >= 1 && s <= NSOCK && g_s[s - 1].used ? &g_s[s - 1] : NULL; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t ld32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void st32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

void xnet_set_transport(const xnet_transport *t) { g_t = t; }
int xnet_last_error(void) { return g_err; }

int xnet_startup(void)
{
    if (!g_t)
        return XNET_WSAENETDOWN;
    if (!g_up) {
        const uint8_t *q = (const uint8_t *)&g_ip;
        g_ip = g_t->local_addr();
        g_up = 1;
        fprintf(stderr, "[XNET] HLE up: transport %s, address %u.%u.%u.%u\n",
                g_t->name, q[0], q[1], q[2], q[3]);
    }
    return 0;
}

uint32_t xnet_socket(int af, int type, int proto)
{
    int i;
    intptr_t h;
    if (!g_up) { g_err = XNET_WSANOTINITIALISED; return XNET_INVALID_SOCKET; }
    if (af != AF_INET_XB) { g_err = XNET_WSAEAFNOSUPPORT; return XNET_INVALID_SOCKET; }
    if (type != SOCK_DGRAM_XB || (proto && proto != IPPROTO_UDP_XB && proto != IPPROTO_VDP_XB)) {
        g_err = XNET_WSAEPROTONOSUPPORT;   /* TCP: only Xbox Live uses it */
        return XNET_INVALID_SOCKET;
    }
    for (i = 0; i < NSOCK && g_s[i].used; i++)
        ;
    if (i == NSOCK) { g_err = XNET_WSAEMFILE; return XNET_INVALID_SOCKET; }
    h = g_t->open();
    if (h < 0) { g_err = (int)-h; return XNET_INVALID_SOCKET; }
    memset(&g_s[i], 0, sizeof g_s[i]);
    g_s[i].used = 1;
    g_s[i].h = h;
    return (uint32_t)i + 1;
}

int xnet_closesocket(uint32_t s)
{
    xsock *x = get(s);
    if (!x) return fail(XNET_WSAENOTSOCK);
    g_t->close(x->h);
    x->used = 0;
    return 0;
}

int xnet_ioctlsocket(uint32_t s, uint32_t cmd, uint8_t *argp)
{
    xsock *x = get(s);
    if (!x) return fail(XNET_WSAENOTSOCK);
    if (!argp) return fail(XNET_WSAEFAULT);
    if (cmd != FIONBIO_XB) return fail(XNET_WSAEINVAL);   /* FIONREAD etc.: unused by the titles so far */
    x->nonblock = ld32(argp) != 0;
    return 0;
}

int xnet_setsockopt(uint32_t s, int level, int opt, const uint8_t *val, int len)
{
    xsock *x = get(s);
    if (!x) return fail(XNET_WSAENOTSOCK);
    if (level == SOL_SOCKET_XB && opt == SO_BROADCAST_XB) {
        if (!val || len < 1) return fail(XNET_WSAEFAULT);
        x->bcast = len >= 4 ? ld32(val) != 0 : val[0] != 0;
    }
    return 0;   /* buffer sizes, linger...: accepted, nothing to tune */
}

static int do_bind(xsock *x, uint16_t port)
{
    int r = g_t->bind(x->h, port);
    if (r < 0) return fail(-r);
    x->bound = 1;
    return 0;
}

int xnet_bind(uint32_t s, const uint8_t *sa, int salen)
{
    xsock *x = get(s);
    if (!x) return fail(XNET_WSAENOTSOCK);
    if (!sa || salen < 16) return fail(XNET_WSAEFAULT);
    if ((sa[0] | sa[1] << 8) != AF_INET_XB) return fail(XNET_WSAEAFNOSUPPORT);
    if (x->bound) return fail(XNET_WSAEINVAL);
    return do_bind(x, be16(sa + 2));   /* the address: the titles bind INADDR_ANY */
}

int xnet_sendto(uint32_t s, const uint8_t *buf, int len, int flags, const uint8_t *to, int tolen)
{
    xsock *x = get(s);
    uint32_t ip;
    int r;
    (void)flags;
    if (!x) return fail(XNET_WSAENOTSOCK);
    if (!to || tolen < 16 || len < 0 || (len && !buf)) return fail(XNET_WSAEFAULT);   /* no connected sockets */
    ip = ld32(to + 4);
    if (ip == BROADCAST_N && !x->bcast) return fail(XNET_WSAEACCES);
    if (!x->bound && do_bind(x, 0)) return XNET_SOCKET_ERROR;   /* implicit bind, as Winsock does */
    r = g_t->sendto(x->h, buf, len, ip, be16(to + 2));
    return r < 0 ? fail(-r) : r;
}

/* shortcut: blocking sockets behave as non-blocking (a guest blocked in a host
 * call would hold the guest lock); MC3 sets FIONBIO on all of its sockets.
 * Make it a wait outside the lock if a title needs blocking receives. */
int xnet_recvfrom(uint32_t s, uint8_t *buf, int len, int flags, uint8_t *from, uint8_t *fromlen)
{
    xsock *x = get(s);
    uint32_t ip = 0;
    uint16_t port = 0;
    int r;
    (void)flags;
    if (!x) return fail(XNET_WSAENOTSOCK);
    if (!x->bound) return fail(XNET_WSAEINVAL);
    if (len < 0 || (len && !buf)) return fail(XNET_WSAEFAULT);
    if (from && (!fromlen || (int32_t)ld32(fromlen) < 16)) return fail(XNET_WSAEFAULT);
    r = g_t->recvfrom(x->h, buf, len, &ip, &port);
    if (r < 0) return fail(-r);
    if (from) {
        memset(from, 0, 16);
        from[0] = AF_INET_XB;
        from[2] = (uint8_t)(port >> 8);
        from[3] = (uint8_t)port;
        st32(from + 4, ip);
        st32(fromlen, 16);
    }
    return r;
}

uint32_t xnet_get_title_xnaddr(uint8_t xna[36])
{
    const uint8_t *q = (const uint8_t *)&g_ip;
    memset(xna, 0, 36);
    if (xnet_startup() || !g_ip)
        return 0x0001u;   /* XNET_GET_XNADDR_NONE (not 0, "pending": MC3 polls that) */
    if (g_t->cur_addr && g_t->cur_addr())
        g_ip = g_t->cur_addr();
    st32(xna, g_ip);
    xna[10] = 0x00; xna[11] = 0x50; xna[12] = 0xF2;   /* Microsoft's OUI, as an Xbox NIC */
    xna[13] = q[1]; xna[14] = q[2]; xna[15] = q[3];
    return XNET_GET_XNADDR_ETHERNET | XNET_GET_XNADDR_STATIC;
}

void xnet_random(uint8_t *p, uint32_t n)
{
    static uint64_t x;
    uint32_t i;
    if (!x)
        x = ((uint64_t)time(NULL) << 20) ^ (uint64_t)clock() ^ (uint64_t)(uintptr_t)&i ^ 0x9E3779B97F4A7C15ull;
    for (i = 0; i < n; i++) {   /* xorshift64: session ids and nonces, nothing secret (no XNet security) */
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        p[i] = (uint8_t)(x >> 32);
    }
}

void xnet_create_key(uint8_t kid[8], uint8_t key[16])
{
    xnet_random(kid, 8);
    xnet_random(key, 16);
    kid[0] &= 0x0F;   /* XNET_XNKID_SYSTEM_LINK: flag nibble 0 */
}

uint32_t xnet_link_status(void)
{
    return !xnet_startup() && g_ip
        ? XNET_ETHERNET_LINK_ACTIVE | XNET_ETHERNET_LINK_100MBPS | XNET_ETHERNET_LINK_FULL_DUPLEX
        : 0;
}

/* ---- native transport: host UDP sockets ------------------------------------------ */
#ifndef __EMSCRIPTEN__

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET hsock;
typedef int socklen_t;
#define BAD_SOCK INVALID_SOCKET
static int u_err(void) { return WSAGetLastError(); }   /* already Winsock codes */
static void u_close(hsock s) { closesocket(s); }
static int u_nonblock(hsock s) { u_long one = 1; return ioctlsocket(s, FIONBIO, &one); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int hsock;
#define BAD_SOCK (-1)
static int u_err(void)
{
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return XNET_WSAEWOULDBLOCK;
    switch (errno) {
    case ECONNREFUSED: case ECONNRESET: return XNET_WSAECONNRESET;   /* ICMP port unreachable */
    case EHOSTUNREACH: case ENETUNREACH: return XNET_WSAEHOSTUNREACH;
    case ENOBUFS: case ENOMEM: return XNET_WSAENOBUFS;
    case EADDRINUSE: return XNET_WSAEADDRINUSE;
    case EACCES: return XNET_WSAEACCES;
    case EMSGSIZE: return XNET_WSAEMSGSIZE;
    default: return XNET_WSAENETDOWN;
    }
}
static void u_close(hsock s) { close(s); }
static int u_nonblock(hsock s) { int f = fcntl(s, F_GETFL, 0); return f < 0 ? -1 : fcntl(s, F_SETFL, f | O_NONBLOCK); }
#endif

static int u_base = -1;
static uint32_t u_own, u_bind_ip, u_peers[16];
static int u_npeers;

/* The address the host would use to reach the internet; connecting a UDP
 * socket sends nothing. */
static uint32_t u_probe(void)
{
    struct sockaddr_in a;
    socklen_t n = sizeof a;
    uint32_t ip = 0;
    hsock s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == BAD_SOCK) return 0;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = inet_addr("8.8.8.8");
    if (!connect(s, (struct sockaddr *)&a, sizeof a) && !getsockname(s, (struct sockaddr *)&a, &n))
        ip = a.sin_addr.s_addr;
    u_close(s);
    return ip;
}

static uint32_t u_local_addr(void)
{
    const char *e;
    if (u_base >= 0) return u_own;
#ifdef _WIN32
    {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w)) return 0;
    }
#endif
    e = getenv("RECOMP_NET_PORT_BASE");
    u_base = e && *e ? atoi(e) : 20000;
    if (u_base < 0 || u_base > 64535) u_base = 20000;
    e = getenv("RECOMP_NET_ADDR");
    if (e && *e) u_own = u_bind_ip = inet_addr(e);
    if (!u_own || u_own == BROADCAST_N) u_own = u_probe();
    if (!u_own) u_own = inet_addr("127.0.0.1");
    e = getenv("RECOMP_NET_PEERS");
    while (e && *e && u_npeers < 16) {
        char ip[64];
        size_t n = strcspn(e, ",");
        if (n && n < sizeof ip) {
            memcpy(ip, e, n);
            ip[n] = 0;
            if (inet_addr(ip) != BROADCAST_N) u_peers[u_npeers++] = inet_addr(ip);
        }
        e += n + (e[n] == ',');
    }
    fprintf(stderr, "[XNET] udp: host port = %d + guest port, %d peer(s)%s\n",
            u_base, u_npeers, u_npeers ? " (broadcasts unicast to them)" : "");
    return u_own;
}

static intptr_t u_open(void)
{
    int one = 1;
    hsock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == BAD_SOCK) return -u_err();
    if (u_nonblock(s) || setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one)) {
        int e = u_err();
        u_close(s);
        return -e;
    }
    return (intptr_t)s;
}

static int u_bind(intptr_t h, uint16_t port)
{
    struct sockaddr_in a;
    if (port && u_base + port > 65535) return -XNET_WSAEINVAL;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)(port ? u_base + port : 0));
    a.sin_addr.s_addr = u_bind_ip;
    return bind((hsock)h, (struct sockaddr *)&a, sizeof a) ? -u_err() : 0;
}

static int u_send1(intptr_t h, const void *p, int n, uint32_t ip, uint16_t port)
{
    struct sockaddr_in a;
    int r;
    if (u_base + port > 65535) return -XNET_WSAEINVAL;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)(u_base + port));
    a.sin_addr.s_addr = ip;
    r = (int)sendto((hsock)h, (const char *)p, n, 0, (struct sockaddr *)&a, sizeof a);
    return r < 0 ? -u_err() : r;
}

static int u_sendto(intptr_t h, const void *p, int n, uint32_t ip, uint16_t port)
{
    int i, ok = 0, r = 0;
    if (ip != BROADCAST_N || !u_npeers)
        return u_send1(h, p, n, ip, port);
    for (i = 0; i < u_npeers; i++) {   /* sent if any peer took it */
        r = u_send1(h, p, n, u_peers[i], port);
        ok |= r >= 0;
    }
    return ok ? n : r;
}

static int u_recvfrom(intptr_t h, void *p, int n, uint32_t *ip, uint16_t *port)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int r = (int)recvfrom((hsock)h, (char *)p, n, 0, (struct sockaddr *)&a, &al);
    unsigned hp;
    if (r < 0) return -u_err();
    hp = ntohs(a.sin_port);
    *ip = a.sin_addr.s_addr;
    *port = (uint16_t)(hp >= (unsigned)u_base ? hp - (unsigned)u_base : hp);
    return r;
}

static void u_close_h(intptr_t h) { u_close((hsock)h); }

const xnet_transport xnet_transport_udp = {
    "udp", u_local_addr, u_open, u_bind, u_sendto, u_recvfrom, u_close_h,
};

#endif /* !__EMSCRIPTEN__ */
