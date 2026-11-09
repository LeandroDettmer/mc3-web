/* xnet_hle.h -- high-level emulation of the Xbox XNet socket API (System Link).
 *
 * A title's statically linked XNET library (CXn object, NIC driver, secure
 * key exchange) is replaced at its API: the game's overrides of the XNet
 * thunks (socket, bind, sendto, XNetGetTitleXnAddr, ...) call these instead.
 * No XNet security: both ends run this code, so an XNADDR's `ina` is the
 * peer's plain address (XNetXnAddrToInAddr returns it) and keys are no-ops.
 *
 * Datagram sockets only (UDP, and VDP -- protocol 254 -- carried as plain
 * UDP, its payload passed whole). Guest structures are taken as native
 * pointers into guest memory (XBOX_PTR) in their Xbox byte layout:
 *   sockaddr_in  16 bytes: family u16 LE, port u16 BE, addr u32 BE, zero[8]
 *   XNADDR       36 bytes: ina, inaOnline, wPortOnline, abEnet[6], abOnline[20]
 *   XNKID 8 bytes, XNKEY 16 bytes.
 * Ports and addresses below are the guest's: a transport maps them to its
 * medium. Not thread-safe: guest code runs under the guest lock (GIL).
 *
 * Shared code: no game addresses here (CLAUDE.md, ADR 0006). */
#ifndef XNET_HLE_H
#define XNET_HLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XNET_INVALID_SOCKET 0xFFFFFFFFu
#define XNET_SOCKET_ERROR   (-1)

/* Winsock error codes the titles test (WSAGetLastError). */
#define XNET_WSAEINTR           10004
#define XNET_WSAEBADF           10009
#define XNET_WSAEACCES          10013
#define XNET_WSAEFAULT          10014
#define XNET_WSAEINVAL          10022
#define XNET_WSAEMFILE          10024
#define XNET_WSAEWOULDBLOCK     10035
#define XNET_WSAENOTSOCK        10038
#define XNET_WSAEMSGSIZE        10040
#define XNET_WSAEPROTONOSUPPORT 10043
#define XNET_WSAEAFNOSUPPORT    10047
#define XNET_WSAEADDRINUSE      10048
#define XNET_WSAENETDOWN        10050
#define XNET_WSAECONNRESET      10054
#define XNET_WSAENOBUFS         10055
#define XNET_WSAEHOSTUNREACH    10065
#define XNET_WSANOTINITIALISED  10093

/* XNetGetTitleXnAddr result bits, XNetGetEthernetLinkStatus bits,
 * XNetGetConnectStatus values (XDK xnet.h). */
#define XNET_GET_XNADDR_ETHERNET 0x0002u
#define XNET_GET_XNADDR_STATIC   0x0004u
#define XNET_ETHERNET_LINK_ACTIVE      0x01u
#define XNET_ETHERNET_LINK_100MBPS     0x02u
#define XNET_ETHERNET_LINK_FULL_DUPLEX 0x08u
#define XNET_CONNECT_STATUS_CONNECTED  2u

/* The medium. Handles are the transport's own; ports are guest ports; ip_n
 * is an IPv4 address in network byte order (as in sockaddr_in). Errors are
 * returned as a negative Winsock code (-XNET_WSAE...). Every socket is
 * non-blocking at this level: recvfrom with nothing queued returns
 * -XNET_WSAEWOULDBLOCK.
 *
 * Native: xnet_transport_udp (host UDP sockets, xnet_hle.c).
 * Web: xnet_transport_web (xnet_web.c), the default in the Emscripten
 * build: datagrams through a shared-memory mailbox to the page, which
 * carries them over a WebSocket to the server's virtual-LAN switch
 * (web/server/crates/lan, after nfsu2-web's, MIT, th0mk): kind 3 frames,
 * the page's virtual address 10.77.x.x as local_addr; broadcasts reach
 * every other player with System Link open and loop back to our own
 * sockets on that port. */
typedef struct xnet_transport {
    const char *name;
    uint32_t (*local_addr)(void);       /* the player's IPv4 (network order); 0: no network.
                                           Called first: a transport sets itself up here. */
    intptr_t (*open)(void);             /* a datagram socket, or -error */
    int (*bind)(intptr_t h, uint16_t port);  /* port 0: any */
    int (*sendto)(intptr_t h, const void *p, int n, uint32_t ip_n, uint16_t port);
    int (*recvfrom)(intptr_t h, void *p, int n, uint32_t *ip_n, uint16_t *port);
    void (*close)(intptr_t h);
    uint32_t (*cur_addr)(void);         /* may be 0: the address now, when it can change after local_addr
                                           (the web link reconnected with another one); 0: unchanged */
} xnet_transport;

extern const xnet_transport xnet_transport_udp;    /* not in the Emscripten build */
extern const xnet_transport xnet_transport_web;    /* the Emscripten build only */

/* Pick the medium; before xnet_startup(). Default: xnet_transport_udp on
 * desktop/console, xnet_transport_web in the web build. */
void xnet_set_transport(const xnet_transport *t);

/* XNetStartup / WSAStartup: 0, or a Winsock error code. Idempotent. */
int xnet_startup(void);

/* Winsock calls, with Winsock's returns (SOCKET / 0 / bytes, else
 * XNET_INVALID_SOCKET / XNET_SOCKET_ERROR and the code in xnet_last_error).
 * `from` and `fromlen` may be NULL. */
uint32_t xnet_socket(int af, int type, int proto);
int xnet_closesocket(uint32_t s);
int xnet_ioctlsocket(uint32_t s, uint32_t cmd, uint8_t *argp);
int xnet_setsockopt(uint32_t s, int level, int opt, const uint8_t *val, int len);
int xnet_bind(uint32_t s, const uint8_t *sa, int salen);
int xnet_sendto(uint32_t s, const uint8_t *buf, int len, int flags, const uint8_t *to, int tolen);
int xnet_recvfrom(uint32_t s, uint8_t *buf, int len, int flags, uint8_t *from, uint8_t *fromlen);
int xnet_last_error(void);

/* The player's XNADDR (ina = local address, abEnet = a MAC derived from
 * it); returns XNET_GET_XNADDR_* bits, 0 (pending) without a network. */
uint32_t xnet_get_title_xnaddr(uint8_t xna[36]);
void xnet_random(uint8_t *p, uint32_t n);
void xnet_create_key(uint8_t kid[8], uint8_t key[16]);   /* a System Link XNKID */
uint32_t xnet_link_status(void);   /* XNET_ETHERNET_LINK_* (0x0B), 0 without a network */

#ifdef __cplusplus
}
#endif

#endif
