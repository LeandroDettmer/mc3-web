/* XNet HLE over the native UDP transport, on loopback: two guest sockets
 * (UDP :1000, VDP :1001) exchange a broadcast and a unicast, with the
 * Winsock errors a title tests on the way. */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#include "xnet_hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
static void nap(void) { Sleep(1); }
static void set_env(const char *k, const char *v) { _putenv_s(k, v); }
#else
#include <unistd.h>
static void nap(void) { usleep(1000); }
static void set_env(const char *k, const char *v) { setenv(k, v, 1); }
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s (last error %d)\n", __FILE__, __LINE__, #c, xnet_last_error()); fails++; } } while (0)

static void sa(uint8_t out[16], uint32_t ip_be_bytes, uint16_t port)
{
    memset(out, 0, 16);
    out[0] = 2;
    out[2] = (uint8_t)(port >> 8);
    out[3] = (uint8_t)port;
    memcpy(out + 4, &ip_be_bytes, 4);
}

/* poll a non-blocking socket up to ~1 s */
static int recv_wait(uint32_t s, uint8_t *buf, int len, uint8_t from[16])
{
    uint8_t fl[4] = {16, 0, 0, 0};
    int i, r = -1;
    for (i = 0; i < 1000; i++) {
        r = xnet_recvfrom(s, buf, len, 0, from, fl);
        if (r >= 0 || xnet_last_error() != XNET_WSAEWOULDBLOCK) break;
        nap();
    }
    return r;
}

int main(void)
{
    static const uint8_t lo[4] = {127, 0, 0, 1};
    uint32_t lo_n, bc_n = 0xFFFFFFFFu, a, b, one = 1;
    uint8_t xna[36], addr[16], from[16], buf[64];
    char base[16];

    snprintf(base, sizeof base, "%d", 30000 + (int)((unsigned)time(NULL) % 20000));
    set_env("RECOMP_NET_PORT_BASE", base);
    set_env("RECOMP_NET_ADDR", "127.0.0.1");
    set_env("RECOMP_NET_PEERS", "127.0.0.1");
    memcpy(&lo_n, lo, 4);

    CHECK(xnet_socket(2, 2, 17) == XNET_INVALID_SOCKET && xnet_last_error() == XNET_WSANOTINITIALISED);
    CHECK(xnet_startup() == 0);
    CHECK(xnet_link_status() == 0x0B);
    CHECK(xnet_get_title_xnaddr(xna) == 0x06);
    CHECK(!memcmp(xna, lo, 4) && xna[10] == 0x00 && xna[11] == 0x50 && xna[12] == 0xF2);

    a = xnet_socket(2, 2, 17);
    b = xnet_socket(2, 2, 254);   /* VDP: plain UDP */
    CHECK(a != XNET_INVALID_SOCKET && b != XNET_INVALID_SOCKET && a != b);
    CHECK(xnet_socket(2, 1, 6) == XNET_INVALID_SOCKET && xnet_last_error() == XNET_WSAEPROTONOSUPPORT);
    sa(addr, 0, 1000);
    CHECK(xnet_bind(a, addr, 16) == 0);
    sa(addr, 0, 1001);
    CHECK(xnet_bind(b, addr, 16) == 0);
    CHECK(xnet_ioctlsocket(a, 0x8004667Eu, (uint8_t *)&one) == 0);
    CHECK(xnet_ioctlsocket(b, 0x8004667Eu, (uint8_t *)&one) == 0);
    CHECK(xnet_recvfrom(a, buf, sizeof buf, 0, NULL, NULL) == -1 && xnet_last_error() == XNET_WSAEWOULDBLOCK);

    /* broadcast needs SO_BROADCAST, as in Winsock */
    sa(addr, bc_n, 1000);
    CHECK(xnet_sendto(b, (const uint8_t *)"hello", 5, 0, addr, 16) == -1 && xnet_last_error() == XNET_WSAEACCES);
    CHECK(xnet_setsockopt(b, 0xFFFF, 0x20, (const uint8_t *)&one, 4) == 0);
    CHECK(xnet_sendto(b, (const uint8_t *)"hello", 5, 0, addr, 16) == 5);
    memset(buf, 0, sizeof buf);
    CHECK(recv_wait(a, buf, sizeof buf, from) == 5 && !memcmp(buf, "hello", 5));
    CHECK(from[0] == 2 && from[2] == 0x03 && from[3] == 0xE9 && !memcmp(from + 4, lo, 4));   /* 127.0.0.1:1001 */

    /* unicast reply to the sender's address */
    sa(addr, lo_n, 1001);
    CHECK(xnet_sendto(a, (const uint8_t *)"reply!", 6, 0, addr, 16) == 6);
    memset(buf, 0, sizeof buf);
    CHECK(recv_wait(b, buf, sizeof buf, from) == 6 && !memcmp(buf, "reply!", 6));
    CHECK(from[2] == 0x03 && from[3] == 0xE8);   /* :1000 */

    CHECK(xnet_closesocket(a) == 0 && xnet_closesocket(b) == 0);
    CHECK(xnet_recvfrom(a, buf, sizeof buf, 0, NULL, NULL) == -1 && xnet_last_error() == XNET_WSAENOTSOCK);

    if (fails) { fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
    puts("PASS: XNet HLE broadcast + unicast over loopback UDP.");
    return 0;
}
