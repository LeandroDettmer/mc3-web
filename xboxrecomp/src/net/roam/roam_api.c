/* roam_api.c -- free-roam multiplayer: the game layer's API (roam_api.h) over
 * the core (roam_core.h) and the platform (roam_plat.h).  After nfsu2-web's
 * roam_game.c session handling (MIT License, Copyright (c) 2026 th0mk),
 * without its NFSU2 game layer.
 *
 * Environment: MC3_ROAM_K (links wanted from the hub, default 7),
 * MC3_ROAM_DELAY (render delay ms, default 100), MC3_ROAM_HUB=0 (no hub:
 * discovery by broadcast and gossip, groups of up to 8). */
#include "roam_plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXTRAP_CAP_US 500000        /* past the newest state a car goes on this long, then holds */

static rc_session g_s;
static int        g_on;
static int64_t    g_last_eval;
static float      g_me[3];
static int        g_k = ROAM_K_DEFAULT;
static uint32_t   g_room;

static int env_int(const char *k, int def, int lo, int hi){
    const char *v = getenv(k);
    if (!v || !*v) return def;
    int x = atoi(v);
    return x < lo ? lo : x > hi ? hi : x;
}
static void on_event(void *user, int ev, int peer, const char *text){
    (void)user; (void)ev; (void)peer;
    fprintf(stderr, "[roam] %s\n", text);
}

int roam_start(const char *name, uint32_t vip){
    (void)vip;   /* the switch puts our address on every frame; the core never needs it */
    if (g_on) return -1;
    rc_config c;
    memset(&c, 0, sizeof c);
    c.self_id = roam_plat_random();
    c.port = ROAM_PORT;
    snprintf(c.name, sizeof c.name, "%s", name ? name : "");
    c.platform = RC_PLAT_WEB;
    c.discovery = 1;                 /* only while no hub answers */
    c.max_players = 8;               /* without a hub; the hub's lists lift it */
    c.rate_hz = 20;
    c.snap_states = 1;               /* the WebSocket loses nothing (nfsu2-web: one state per SNAP) */
    if (env_int("MC3_ROAM_HUB", 1, 0, 1)){ c.hub_ip = ROAM_HUB_VIP; c.hub_port = ROAM_PORT; }
    g_k = env_int("MC3_ROAM_K", g_k, 1, ROAM_K_MAX);
    c.links = g_k;
    c.room = g_room;
    rc_io io = { 0, 0, 0, on_event };
    rc_session_init(&g_s, &c, &io);
    if (roam_plat_start(&g_s, ROAM_PORT) != 0) return -1;
    g_on = 1; g_last_eval = 0;
    fprintf(stderr, "[roam] started as \"%s\" (id %08x, %d links wanted)\n", g_s.conf.name, (unsigned)c.self_id, g_k);
    return 0;
}
void roam_stop(void){
    if (!g_on) return;
    roam_plat_lock(); rc_shutdown(&g_s, 2); roam_plat_unlock();
    roam_plat_stop();
    g_on = 0;
}
int roam_running(void){ return g_on; }
void roam_set_room(uint32_t room){
    if (room == g_room) return;
    g_room = room;
    if (!g_on) return;
    roam_plat_lock(); rc_set_room(&g_s, room); roam_plat_unlock();
}
void roam_set_k(int k){
    g_k = k < 1 ? 1 : k > ROAM_K_MAX ? ROAM_K_MAX : k;
    if (!g_on) return;
    roam_plat_lock(); rc_set_links(&g_s, g_k); roam_plat_unlock();
}
void roam_set_status(uint8_t st){
    if (!g_on) return;
    roam_plat_lock(); rc_set_status(&g_s, st); roam_plat_unlock();
}
void roam_set_local(const rc_state *st){
    if (!g_on || !st) return;
    roam_plat_lock();
    int64_t now = roam_plat_now_us();
    rc_state s = *st;
    if (!s.t_us) s.t_us = (uint32_t)now ? (uint32_t)now : 1u;
    memcpy(g_me, s.p, sizeof g_me);
    rc_push_state(&g_s, &s, now);
    roam_plat_unlock();
}
void roam_teleport(void){
    if (!g_on) return;
    roam_plat_lock(); rc_bump_epoch(&g_s); roam_plat_unlock();
}
int roam_peers(roam_peer *out, int max){
    if (!g_on || !out || max <= 0) return 0;
    int delay_ms = env_int("MC3_ROAM_DELAY", 100, 0, 1000), n = 0;
    roam_plat_lock();
    int64_t now = roam_plat_now_us(), fdt = g_last_eval ? now - g_last_eval : 0;
    g_last_eval = now;
    roam_peer tmp[RC_MAX_PEERS];
    for (int i = 0; i < RC_TABLE && n < RC_MAX_PEERS; i++){
        rc_peer *p = &g_s.peer[i];
        rc_pose o;
        if (p->state != RC_PS_EST || p->lost) continue;
        if (!rc_interp_eval(&p->mo, now, fdt, (int64_t)delay_ms * 1000, EXTRAP_CAP_US, &o)) continue;
        roam_peer *r = &tmp[n++];
        memset(r, 0, sizeof *r);
        r->id = p->id; memcpy(r->name, p->name, sizeof r->name);
        r->status = p->status; r->mode = o.mode;
        memcpy(r->p, o.p, sizeof r->p); memcpy(r->q, o.q, sizeof r->q); memcpy(r->v, o.v, sizeof r->v); memcpy(r->w, o.w, sizeof r->w);
        r->steer = o.steer; r->throttle = o.throttle; r->brake = o.brake; r->rpm = o.rpm; r->speed = o.speed;
        r->gear = o.gear; r->bits = o.bits; r->ext_len = o.ext_len; memcpy(r->ext, o.ext, sizeof r->ext);
        r->cfg_gen = p->cfg_len ? p->cfg_gen : 0;
        float dx = o.p[0] - g_me[0], dy = o.p[1] - g_me[1], dz = o.p[2] - g_me[2];
        r->dist = rc_sqrtf(dx * dx + dy * dy + dz * dz);
    }
    roam_plat_unlock();
    /* nearest first (insertion sort: 64 at most) */
    for (int i = 1; i < n; i++){
        roam_peer t = tmp[i]; int j = i;
        while (j > 0 && tmp[j - 1].dist > t.dist){ tmp[j] = tmp[j - 1]; j--; }
        tmp[j] = t;
    }
    if (n > max) n = max;
    memcpy(out, tmp, sizeof *out * (size_t)n);
    return n;
}
void roam_set_cfg(const void *blob, int len){
    if (!g_on || !blob || len <= 0 || len > ROAM_CFG_MAX) return;
    roam_plat_lock(); rc_set_cfg(&g_s, (const uint8_t *)blob, len, roam_plat_now_us()); roam_plat_unlock();
}
int roam_far(roam_far_peer *out, int max){
    if (!g_on || !out || max <= 0) return 0;
    int n = 0;
    roam_plat_lock();
    if (g_s.far_us && roam_plat_now_us() - g_s.far_us < 6000000){
        for (int i = 0; i < g_s.nfar && n < max; i++){
            const roam_far_peer *e = &g_s.far[i];
            int car = 0;   /* linked meanwhile: roam_peers shows it as a car (once a snapshot came) */
            for (int k = 0; k < RC_TABLE && !car; k++)
                car = g_s.peer[k].state == RC_PS_EST && !g_s.peer[k].lost && g_s.peer[k].mo.n > 0 && g_s.peer[k].id == e->id;
            if (car) continue;
            out[n] = *e;
            float dx = e->p[0] - g_me[0], dy = e->p[1] - g_me[1], dz = e->p[2] - g_me[2];
            out[n++].dist = rc_sqrtf(dx * dx + dy * dy + dz * dz);
        }
    }
    roam_plat_unlock();
    return n;
}
int roam_peer_cfg(uint32_t id, void *out, int cap){
    int r = -1;
    if (!g_on || !id) return -1;
    roam_plat_lock();
    for (int i = 0; i < RC_TABLE; i++){
        rc_peer *p = &g_s.peer[i];
        if (p->state != RC_PS_EST || p->id != id) continue;
        r = p->cfg_len < cap ? p->cfg_len : cap;
        if (r > 0 && out) memcpy(out, p->cfg, (size_t)r);
        if (r < 0) r = 0;
        break;
    }
    roam_plat_unlock();
    return r;
}
int roam_send_msg(uint32_t to, const void *data, int len){
    if (!g_on || !data) return -1;
    roam_plat_lock(); int r = rc_send_game(&g_s, to, (const uint8_t *)data, len); roam_plat_unlock();
    return r;
}
int roam_recv_msg(uint32_t *from, void *out, int cap){
    if (!g_on || !out || cap <= 0) return 0;
    roam_plat_lock(); int r = rc_recv_game(&g_s, from, (uint8_t *)out, cap); roam_plat_unlock();
    return r;
}
