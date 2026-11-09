/*
 * System Link in the browser, end to end: xnet_hle.c over xnet_web.c's
 * mailbox, web/net.js, web/lan.js's WebSocket and the server's switch
 * (web/server). Each page does what MC3's System Link does: XNetStartup,
 * its XNADDR, a non-blocking UDP socket on :1000 with SO_BROADCAST, a
 * broadcast to 255.255.255.255:1000 every 250 ms, and receives. It prints
 * the first datagrams from another address as
 *   [RX] 10.77.x.y:1000 hello from 10.77.x.y #n
 * which check.mjs looks for in two headless Chromes.
 *
 * From the repo root (WSL, emsdk sourced):
 *   emcc -O2 -pthread -sPROXY_TO_PTHREAD -sEXIT_RUNTIME=0 -Ixboxrecomp/src/net \
 *        xboxrecomp/tests/xnet_web/bcast.c xboxrecomp/src/net/xnet_hle.c \
 *        xboxrecomp/src/net/xnet_web.c --pre-js web/pre.js --pre-js web/net.js \
 *        -o <dir>/bcast.js
 *   cp xboxrecomp/tests/xnet_web/index.html <dir>/
 *   web/server/target/release/mc3-server --web <dir> --web web --port 3002
 *   node xboxrecomp/tests/xnet_web/check.mjs http://127.0.0.1:3002/
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "xnet_hle.h"

#define IP(p) (p)[0], (p)[1], (p)[2], (p)[3]

int main(void)
{
    uint8_t xna[36], me[16] = {2, 0, 0x03, 0xE8}, to[16] = {2, 0, 0x03, 0xE8, 255, 255, 255, 255};
    uint8_t from[16], fl[4], buf[1500];
    uint32_t one = 1, s, flags, rx = 0;
    int t, n;

    if (xnet_startup()) { printf("bcast: no network\n"); return 1; }
    flags = xnet_get_title_xnaddr(xna);
    printf("bcast: XNADDR flags 0x%X, %u.%u.%u.%u, MAC %02X:%02X:%02X:%02X:%02X:%02X\n", flags, IP(xna),
           xna[10], xna[11], xna[12], xna[13], xna[14], xna[15]);
    if (!(flags & XNET_GET_XNADDR_ETHERNET)) return 1;
    s = xnet_socket(2, 2, 17);
    if (s == XNET_INVALID_SOCKET || xnet_setsockopt(s, 0xFFFF, 0x20, (uint8_t *)&one, 4) ||
        xnet_ioctlsocket(s, 0x8004667Eu, (uint8_t *)&one) || xnet_bind(s, me, 16)) {
        printf("bcast: socket setup failed, error %d\n", xnet_last_error());
        return 1;
    }
    for (t = 0;; t++) {
        if (t % 25 == 0) {
            n = snprintf((char *)buf, sizeof buf, "hello from %u.%u.%u.%u #%d", IP(xna), t / 25);
            if (xnet_sendto(s, buf, n, 0, to, 16) != n) printf("bcast: sendto error %d\n", xnet_last_error());
        }
        for (;;) {
            fl[0] = 16; fl[1] = fl[2] = fl[3] = 0;
            n = xnet_recvfrom(s, buf, (int)sizeof buf - 1, 0, from, fl);
            if (n < 0) break;
            if (!memcmp(from + 4, xna, 4)) continue;   /* our own broadcast, looped back */
            buf[n] = 0;
            if (++rx <= 3) printf("[RX] %u.%u.%u.%u:%u %s\n", IP(from + 4), from[2] << 8 | from[3], buf);
        }
        if (xnet_last_error() != XNET_WSAEWOULDBLOCK) printf("bcast: recvfrom error %d\n", xnet_last_error());
        usleep(10000);
    }
}
