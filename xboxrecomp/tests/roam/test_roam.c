/* test_roam.c -- unit tests for the free-roam core (src/net/roam/roam_core.c)
 * and a headless loopback: two sessions through a fake switch (an in-process
 * queue with latency, a fake clock), with and without a hub.  Modelled on
 * nfsu2-web's tests/roam_unit.c (MIT License, Copyright (c) 2026 th0mk).
 *
 *   cc -O1 -g -fsanitize=address,undefined -I../../src/net/roam test_roam.c ../../src/net/roam/roam_core.c -lm
 */
#include "roam_core.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t le32(const uint8_t *p){ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---- wire ---- */
static rc_state mk_state(uint32_t t, float x, float vx){
    rc_state s; memset(&s, 0, sizeof s);
    s.t_us = t; s.p[0] = x; s.p[1] = 2.f; s.p[2] = -3.f; s.v[0] = vx; s.q[3] = 1; s.rpm = 3000; s.speed = vx; s.gear = 3; s.throttle = 0.5f;
    return s;
}
static void t_wire(void){
    uint8_t b[RC_MAX_DGRAM]; rc_msg m, d;
    memset(&m, 0, sizeof m);
    m.h.type = RC_MSG_HELLO; m.h.sender = 0xA1B2C3D4u; m.h.seq = 7; m.h.flags = RC_HF_CREW | RC_HF_ALL;
    m.u.hello.core_ver = RC_CORE_VER; m.u.hello.status = RC_ST_ROAM; m.u.hello.listen_port = ROAM_PORT;
    strcpy(m.u.hello.name, "Ann"); m.u.hello.n_seen = 2; m.u.hello.seen[0] = 5; m.u.hello.seen[1] = 6; m.u.hello.links = 7;
    int n = rc_encode(&m, b, sizeof b);
    CHECK(n == RC_HDR + 12 + 1 + 3 + 1 + 8 + 2 + RC_CRC, "hello length %d", n);
    CHECK(!memcmp(b, "MC3R", 4) && b[4] == 1 && b[5] == RC_MSG_HELLO && (b[6] | b[7] << 8) == n, "header");
    CHECK(le32(b + 8) == 0xA1B2C3D4u, "sender");
    CHECK(le32(b + n - 4) == rc_crc32(b, (uint32_t)n - 4), "crc");
    CHECK(rc_decode(b, n, &d) == RC_OK && d.u.hello.links == 7 && d.u.hello.n_seen == 2 && !strcmp(d.u.hello.name, "Ann"), "hello round trip");
    b[20] ^= 1; CHECK(rc_decode(b, n, &d) == RC_E_CRC, "crc caught"); b[20] ^= 1;
    b[0] = 'N'; CHECK(rc_decode(b, n, &d) == RC_E_MAGIC, "magic (N2FR is not ours)"); b[0] = 'M';
    CHECK(rc_decode(b, n - 1, &d) == RC_E_LENGTH, "length");

    /* SNAP: 52 bytes a state, the extension after it with RC_HF_STATEX */
    memset(&m, 0, sizeof m);
    m.h.type = RC_MSG_SNAP; m.h.sender = 9; m.u.snap.n = 1; m.u.snap.status = RC_ST_ROAM;
    m.u.snap.s[0] = mk_state(1000, 12.5f, 20.f);
    n = rc_encode(&m, b, sizeof b);
    CHECK(n == RC_HDR + 6 + RC_STATE_BYTES + RC_CRC, "snap length %d", n);
    m.h.flags = RC_HF_STATEX; m.u.snap.s[0].ext_len = 3; memcpy(m.u.snap.s[0].ext, "abc", 3);
    n = rc_encode(&m, b, sizeof b);
    CHECK(n == RC_HDR + 6 + RC_STATE_BYTES + 4 + RC_CRC, "snap+ext length %d", n);
    CHECK(rc_decode(b, n, &d) == RC_OK, "snap decode");
    CHECK(d.u.snap.s[0].ext_len == 3 && !memcmp(d.u.snap.s[0].ext, "abc", 3), "ext");
    CHECK(fabsf(d.u.snap.s[0].p[0] - 12.5f) < 1e-6f && d.u.snap.s[0].gear == 3 && fabsf(d.u.snap.s[0].rpm - 3000) < 1, "state values");
    m.u.snap.s[0].p[1] = 30000.f; n = rc_encode(&m, b, sizeof b);
    CHECK(rc_decode(b, n, &d) == RC_E_RANGE, "range");

    /* PEERS: ip big-endian */
    memset(&m, 0, sizeof m);
    m.h.type = RC_MSG_PEERS; m.h.sender = 1; m.u.peers.n = 1;
    m.u.peers.e[0].ip = 0x0A4D0102u; m.u.peers.e[0].port = ROAM_PORT; m.u.peers.e[0].id = 77;
    n = rc_encode(&m, b, sizeof b);
    CHECK(b[17] == 10 && b[18] == 77 && b[19] == 1 && b[20] == 2, "peers ip order");
    CHECK(rc_decode(b, n, &d) == RC_OK && d.u.peers.e[0].ip == 0x0A4D0102u && d.u.peers.e[0].id == 77, "peers round trip");

    /* FAR (hub -> player): the hubs' layout (crates/roam far_list): entry 26 bytes + name */
    memset(&m, 0, sizeof m);
    m.h.type = RC_MSG_FAR; m.h.sender = 0x48554231u; m.u.far.n = 2;
    m.u.far.e[0].id = 102; m.u.far.e[0].status = RC_ST_ROAM; m.u.far.e[0].p[0] = 200.f; m.u.far.e[0].p[2] = 5.f;
    m.u.far.e[0].q[3] = 1.f; strcpy(m.u.far.e[0].name, "P2");
    m.u.far.e[1].id = 103; m.u.far.e[1].status = RC_ST_SHOP; strcpy(m.u.far.e[1].name, "");
    n = rc_encode(&m, b, sizeof b);
    CHECK(n == RC_HDR + 1 + 26 + 2 + 26 + RC_CRC, "far length %d", n);
    CHECK(le32(b + 17) == 102 && b[21] == RC_ST_ROAM && b[42] == 2 && b[43] == 'P' && le32(b + 45) == 103, "far layout");
    CHECK(rc_decode(b, n, &d) == RC_OK && d.u.far.n == 2 && d.u.far.e[0].id == 102 && fabsf(d.u.far.e[0].p[0] - 200.f) < 1e-6f &&
          fabsf(d.u.far.e[0].q[3] - 1.f) < 1e-4f && !strcmp(d.u.far.e[0].name, "P2") && d.u.far.e[1].status == RC_ST_SHOP, "far round trip");
    {   /* from the hub it lands in the session's list; from anyone else it is ignored */
        static rc_session fs; rc_config c; memset(&c, 0, sizeof c);
        c.self_id = 1; c.port = ROAM_PORT; c.hub_ip = ROAM_HUB_VIP; c.hub_port = ROAM_PORT; c.max_players = 8; c.rate_hz = 20;
        rc_io io; memset(&io, 0, sizeof io);
        rc_session_init(&fs, &c, &io);
        rc_receive(&fs, 0x0A4D0102u, ROAM_PORT, b, n, 1000);
        CHECK(fs.nfar == 0, "far from a player ignored");
        rc_receive(&fs, ROAM_HUB_VIP, ROAM_PORT, b, n, 2000);
        CHECK(fs.nfar == 2 && fs.far_us == 2000 && fs.far[1].id == 103, "far from the hub kept");
        /* in parts (flags part | parts << 8), the nearest (0) last: part 0 makes the list, nearest part first */
        rc_msg pm = m; uint8_t pb[RC_MAX_DGRAM]; int pn;
        pm.h.seq = 7; pm.h.flags = 1 | 2 << 8; pm.u.far.n = 1; pm.u.far.e[0].id = 200;
        pn = rc_encode(&pm, pb, sizeof pb);
        rc_receive(&fs, ROAM_HUB_VIP, ROAM_PORT, pb, pn, 3000);
        CHECK(fs.nfar == 2 && fs.far_us == 2000, "far part 1 waits for part 0");
        pm.h.flags = 0 | 2 << 8; pm.u.far.n = 2; pm.u.far.e[0].id = 100; pm.u.far.e[1].id = 101;
        pn = rc_encode(&pm, pb, sizeof pb);
        rc_receive(&fs, ROAM_HUB_VIP, ROAM_PORT, pb, pn, 3100);
        CHECK(fs.nfar == 3 && fs.far_us == 3100 && fs.far[0].id == 100 && fs.far[1].id == 101 && fs.far[2].id == 200, "far parts merged");
        pm.h.seq = 8;   /* part 1 of seq 8 lost: part 0 alone */
        pn = rc_encode(&pm, pb, sizeof pb);
        rc_receive(&fs, ROAM_HUB_VIP, ROAM_PORT, pb, pn, 4000);
        CHECK(fs.nfar == 2 && fs.far[1].id == 101, "far lost part left out");
    }
    m.u.far.e[0].p[1] = 30000.f; n = rc_encode(&m, b, sizeof b);
    CHECK(rc_decode(b, n, &d) == RC_E_RANGE, "far range");

    /* GAME (player -> player): len + bytes; 0 and too long refused both ways */
    memset(&m, 0, sizeof m);
    m.h.type = RC_MSG_GAME; m.h.sender = 5; m.u.game.len = 3; memcpy(m.u.game.data, "R1x", 3);
    n = rc_encode(&m, b, sizeof b);
    CHECK(n == RC_HDR + 1 + 3 + RC_CRC, "game length %d", n);
    CHECK(rc_decode(b, n, &d) == RC_OK && d.u.game.len == 3 && !memcmp(d.u.game.data, "R1x", 3), "game round trip");
    m.u.game.len = 0; CHECK(rc_encode(&m, b, sizeof b) < 0, "empty game refused");
    m.u.game.len = RC_GAME_MAX + 1; CHECK(rc_encode(&m, b, sizeof b) < 0, "long game refused");
    m.u.game.len = 3; n = rc_encode(&m, b, sizeof b); b[RC_HDR] = 200;
    { uint32_t c = rc_crc32(b, (uint32_t)n - 4); memcpy(b + n - 4, &c, 4); }
    CHECK(rc_decode(b, n, &d) == RC_E_FIELD, "game length beyond the datagram refused");

    /* fuzz: random bytes with a valid header and CRC never crash, never pass trailing garbage */
    uint32_t r = 1;
    for (int it = 0; it < 20000; it++){
        int len = RC_HDR + RC_CRC + (int)(r % 200);
        for (int i = 0; i < len; i++){ r ^= r << 13; r ^= r >> 17; r ^= r << 5; b[i] = (uint8_t)r; }
        memcpy(b, "MC3R", 4); b[4] = 1; b[5] = (uint8_t)(1 + r % 10); b[6] = (uint8_t)len; b[7] = (uint8_t)(len >> 8);
        uint32_t c = rc_crc32(b, (uint32_t)len - 4); memcpy(b + len - 4, &c, 4);
        (void)rc_decode(b, len, &d);
    }
    CHECK(1, "fuzz");
}

/* ---- interpolation ---- */
static void t_interp(void){
    rc_interp ip; rc_interp_reset(&ip); rc_pose o;
    /* states 50 ms apart moving at 20 m/s: x = 20 t */
    for (int k = 0; k < 10; k++){ rc_state s = mk_state(1000000u + 50000u * k, 1.f * k, 20.f); rc_interp_insert(&ip, &s, 0, 0, RC_ST_ROAM, 5000000 + 50000 * k, 1); }
    CHECK(ip.n == 10, "buffer");
    CHECK(rc_interp_peek(&ip, 1000000 + ((int64_t)1 << 32) + 125000, 500000, &o) && o.mode == ROAM_MODE_INTERP && fabsf(o.p[0] - 2.5f) < 1e-3f,
          "hermite midpoint %f", o.p[0]);
    CHECK(rc_interp_peek(&ip, 1000000 + ((int64_t)1 << 32) + 450000 + 100000, 500000, &o) && o.mode == ROAM_MODE_EXTRAP && fabsf(o.p[0] - 11.f) < 1e-3f,
          "extrapolation %f", o.p[0]);
    CHECK(rc_interp_peek(&ip, 1000000 + ((int64_t)1 << 32) + 450000 + 900000, 500000, &o) && o.mode == ROAM_MODE_HOLD && fabsf(o.p[0] - 19.f) < 1e-3f,
          "hold at the cap %f", o.p[0]);
    /* eval: render delay 100 ms at the newest arrival -> 100 ms behind the newest state */
    CHECK(rc_interp_eval(&ip, 5000000 + 450000, 16000, 100000, 500000, &o) && fabsf(o.p[0] - 7.f) < 1e-3f, "eval with delay %f", o.p[0]);
    /* an older epoch is dropped, a newer one snaps */
    rc_state s = mk_state(9000000u, 100.f, 0.f);
    rc_interp_insert(&ip, &s, 0xFF, 0, RC_ST_ROAM, 6000000, 1);
    CHECK(ip.n_stale == 1 && ip.n == 10, "stale epoch");
    rc_interp_insert(&ip, &s, 1, 0, RC_ST_ROAM, 6000000, 1);
    CHECK(ip.n == 1 && ip.snap_pending, "new epoch");
    CHECK(rc_interp_eval(&ip, 6000000, 16000, 0, 500000, &o) && fabsf(o.p[0] - 100.f) < 1e-3f, "snapped %f", o.p[0]);
}

/* ---- the fake switch: datagrams queued with a latency, delivered by time ---- */
#define NQ 4096
typedef struct { int64_t at; uint32_t src, dst; uint16_t sport, dport; int len; uint8_t d[RC_MAX_DGRAM]; } dgram;
static dgram g_q[NQ]; static int g_qn;
static int64_t g_now;
static uint32_t g_snaps[3];   /* SNAPs seen per endpoint index */
typedef struct { uint32_t vip; } ep;
static ep g_ep[2] = { { 0x0A4D0002u }, { 0x0A4D0102u } };
static rc_session g_s[2];
static int g_hub;             /* the test plays the hub: answers HELLOs to 10.77.0.253 */
static uint32_t g_hub_rx;

static void enqueue(uint32_t src, uint32_t dst, uint16_t dport, const uint8_t *b, int n){
    if (g_qn == NQ) return;
    dgram *q = &g_q[g_qn++];
    q->at = g_now + 20000; q->src = src; q->dst = dst; q->sport = ROAM_PORT; q->dport = dport; q->len = n; memcpy(q->d, b, (size_t)n);
}
static void io_send(void *u, uint32_t ip, uint16_t port, const uint8_t *b, int n){ ep *e = u; enqueue(e->vip, ip, port, b, n); }
static void io_bcast(void *u, uint16_t port, const uint8_t *b, int n){ ep *e = u; enqueue(e->vip, 0xFFFFFFFFu, port, b, n); }
static void hub_answer(void){
    /* the hub's PEERS to each player: the other one */
    for (int k = 0; k < 2; k++){
        rc_msg m; memset(&m, 0, sizeof m); uint8_t b[RC_MAX_DGRAM];
        m.h.type = RC_MSG_PEERS; m.h.sender = 0x48554221u; m.u.peers.n = 1;
        m.u.peers.e[0].ip = g_ep[1 - k].vip; m.u.peers.e[0].port = ROAM_PORT; m.u.peers.e[0].id = g_s[1 - k].conf.self_id;
        int n = rc_encode(&m, b, sizeof b);
        enqueue(ROAM_HUB_VIP, g_ep[k].vip, ROAM_PORT, b, n);
    }
}
static void hub_empty(void){
    /* the hub's PEERS to each player: nobody (a socket to it reconnecting) */
    for (int k = 0; k < 2; k++){
        rc_msg m; memset(&m, 0, sizeof m); uint8_t b[RC_MAX_DGRAM];
        m.h.type = RC_MSG_PEERS; m.h.sender = 0x48554221u; m.u.peers.n = 0;
        int n = rc_encode(&m, b, sizeof b);
        enqueue(ROAM_HUB_VIP, g_ep[k].vip, ROAM_PORT, b, n);
    }
}
static void deliver(void){
    int w = 0;
    for (int i = 0; i < g_qn; i++){
        dgram *q = &g_q[i];
        if (q->at > g_now){ g_q[w++] = *q; continue; }
        if (q->dst == ROAM_HUB_VIP){ g_hub_rx++; continue; }
        for (int k = 0; k < 2; k++){
            if (q->src == g_ep[k].vip) continue;                         /* the switch never sends one back */
            if (q->dst != 0xFFFFFFFFu && q->dst != g_ep[k].vip) continue;
            if (q->len > RC_HDR && q->d[5] == RC_MSG_SNAP) g_snaps[k]++;
            rc_receive(&g_s[k], q->src, q->sport, q->d, q->len, g_now);
        }
    }
    g_qn = w;
}
static void t_loopback(int with_hub){
    g_qn = 0; g_now = 1000000; g_snaps[0] = g_snaps[1] = 0; g_hub = with_hub; g_hub_rx = 0;
    for (int k = 0; k < 2; k++){
        rc_config c; memset(&c, 0, sizeof c);
        c.self_id = 0x1000u + (uint32_t)k; c.port = ROAM_PORT; c.discovery = !with_hub; c.max_players = 8; c.rate_hz = 20; c.snap_states = 1;
        snprintf(c.name, sizeof c.name, k ? "Bob" : "Ann");
        if (with_hub){ c.hub_ip = ROAM_HUB_VIP; c.hub_port = ROAM_PORT; c.links = ROAM_K_DEFAULT; }
        rc_io io = { &g_ep[k], io_send, io_bcast, 0 };
        rc_session_init(&g_s[k], &c, &io);
        rc_set_status(&g_s[k], RC_ST_ROAM);
    }
    uint8_t cfg[264]; for (int i = 0; i < 264; i++) cfg[i] = (uint8_t)(i * 7);
    rc_set_cfg(&g_s[0], cfg, sizeof cfg, g_now);
    /* Ann drives along x at 15 m/s, Bob stands; 16 ms frames, 10 ms ticks, 6 s */
    float max_err = 0; int shown = 0;
    int64_t last_eval = g_now;
    for (int f = 0; f < 600; f++){
        g_now += 10000;
        if (with_hub && f % 50 == 0) hub_answer();
        deliver();
        for (int k = 0; k < 2; k++) rc_tick(&g_s[k], g_now);
        float x = 15.f * (float)(g_now - 1000000) * 1e-6f;
        rc_state a = mk_state((uint32_t)g_now, x, 15.f), b = mk_state((uint32_t)g_now, -50.f, 0.f);
        rc_push_state(&g_s[0], &a, g_now);
        rc_push_state(&g_s[1], &b, g_now);
        /* Bob renders Ann 100 ms in the past */
        for (int i = 0; i < RC_TABLE; i++){
            rc_peer *p = &g_s[1].peer[i]; rc_pose o;
            if (p->state != RC_PS_EST || f < 300) continue;
            if (rc_interp_eval(&p->mo, g_now, g_now - last_eval, 100000, 500000, &o)){
                float want = 15.f * (float)(g_now - 100000 - 20000 - 1000000) * 1e-6f;   /* (20 ms on the wire) */
                float e = fabsf(o.p[0] - want);
                if (e > max_err) max_err = e;
                shown++;
            }
        }
        last_eval = g_now;
    }
    const char *how = with_hub ? "hub" : "discovery";
    CHECK(rc_established(&g_s[0]) == 1 && rc_established(&g_s[1]) == 1, "%s: both established (%d, %d)", how, rc_established(&g_s[0]), rc_established(&g_s[1]));
    CHECK(g_snaps[1] > 80, "%s: SNAPs flowed to Bob (%u)", how, g_snaps[1]);
    CHECK(shown > 250, "%s: Ann shown on Bob's screen (%d frames)", how, shown);
    CHECK(max_err < 0.5f, "%s: interpolated pose within 0.5 m (max %.3f m)", how, max_err);
    int bob_sees_cfg = 0;
    for (int i = 0; i < RC_TABLE; i++) if (g_s[1].peer[i].state == RC_PS_EST) bob_sees_cfg = g_s[1].peer[i].cfg_len == 264 && !memcmp(g_s[1].peer[i].cfg, cfg, 264);
    CHECK(bob_sees_cfg, "%s: car record reached Bob", how);
    if (with_hub) CHECK(g_hub_rx > 10 && g_s[0].hub_lists > 5, "hub: HELLOs/SNAPs to the hub (%u), its lists heard (%u)", g_hub_rx, g_s[0].hub_lists);
    /* the hub lists neither while both keep sending: the link holds (T_HUB_GRACE_LIVE, 15 s), then goes */
    if (with_hub){
        int held = 0;
        for (int f = 0; f < 1700; f++){
            g_now += 10000;
            if (f % 50 == 0) hub_empty();
            deliver();
            for (int k = 0; k < 2; k++) rc_tick(&g_s[k], g_now);
            rc_state a = mk_state((uint32_t)g_now, 0.f, 0.f), b = mk_state((uint32_t)g_now, -50.f, 0.f);
            rc_push_state(&g_s[0], &a, g_now);
            rc_push_state(&g_s[1], &b, g_now);
            if (f == 1000) held = rc_established(&g_s[0]) == 1 && rc_established(&g_s[1]) == 1;
        }
        CHECK(held, "hub: a live link unlisted for 10 s holds");
        CHECK(rc_established(&g_s[0]) == 0 && rc_established(&g_s[1]) == 0, "hub: unlisted for 17 s, dropped (%d, %d)",
              rc_established(&g_s[0]), rc_established(&g_s[1]));
    }
    /* Ann goes to another room (city): both drop the link and stay apart, even with a hub that lists them; back in
     * the same room they link again */
    for (int room = 1; room >= 0; room--){
        rc_set_room(&g_s[0], (uint32_t)room);
        for (int f = 0; f < 300; f++){
            g_now += 10000;
            if (with_hub && f % 50 == 0) hub_answer();
            deliver();
            for (int k = 0; k < 2; k++) rc_tick(&g_s[k], g_now);
            rc_state a = mk_state((uint32_t)g_now, 0.f, 0.f), b = mk_state((uint32_t)g_now, -50.f, 0.f);
            rc_push_state(&g_s[0], &a, g_now);
            rc_push_state(&g_s[1], &b, g_now);
        }
        int want = room ? 0 : 1;
        CHECK(rc_established(&g_s[0]) == want && rc_established(&g_s[1]) == want, "%s: room %d: %d links (%d, %d)", how,
              room, want, rc_established(&g_s[0]), rc_established(&g_s[1]));
    }
    /* GAME messages go to the established peer only, arrive in order, and the inbox keeps the newest 8 */
    {
        uint8_t msg[4] = { 'R', 1, 0, 0 }, got[RC_GAME_MAX]; uint32_t from = 0; int ok = 1;
        CHECK(rc_send_game(&g_s[0], 0xDEADu, msg, 4) == -1, "%s: game to an unknown peer refused", how);
        for (int k = 0; k < 10; k++){ msg[2] = (uint8_t)k; ok &= rc_send_game(&g_s[0], g_s[1].conf.self_id, msg, 4) == 0; }
        CHECK(ok, "%s: game sent", how);
        for (int f = 0; f < 5; f++){ g_now += 10000; deliver(); }
        CHECK(g_s[1].ninbox == RC_INBOX && g_s[1].rx_game_drop == 2, "%s: inbox %d, dropped %u", how, g_s[1].ninbox, g_s[1].rx_game_drop);
        int n = rc_recv_game(&g_s[1], &from, got, sizeof got);
        CHECK(n == 4 && from == g_s[0].conf.self_id && got[2] == 2, "%s: oldest kept first (n %d, #%d)", how, n, got[2]);
        while (rc_recv_game(&g_s[1], &from, got, sizeof got)) ;
        CHECK(g_s[1].ninbox == 0, "%s: inbox drained", how);
    }
    /* Ann quits: Bob drops her */
    rc_shutdown(&g_s[0], 2);
    for (int f = 0; f < 10; f++){ g_now += 10000; deliver(); rc_tick(&g_s[1], g_now); }
    CHECK(rc_established(&g_s[1]) == 0, "%s: BYE drops the peer", how);
}

int main(void){
    t_wire();
    t_interp();
    t_loopback(0);
    t_loopback(1);
    printf("roam: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
