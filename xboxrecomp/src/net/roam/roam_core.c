/* roam_core.c -- free-roam multiplayer: the portable core (see roam_core.h).
 *
 * Ported from nfsu2-web's runtime/roam_core.c (MIT License, Copyright (c)
 * 2026 th0mk): its byte helpers, wire format, interpolation and session,
 * without the NFSU2 game layer.  No OS headers, no libm. */
#include "roam_core.h"
#include <stdio.h>
#define memcpy  __builtin_memcpy
#define memset  __builtin_memset
#define memmove __builtin_memmove
#define memcmp  __builtin_memcmp
#define strlen  __builtin_strlen

typedef char rc_assert_float[(sizeof(float) == 4) ? 1 : -1];
typedef char rc_assert_snap[(RC_HDR + 6 + RC_SNAP_STATES * (RC_STATE_BYTES + RC_STATEX_BYTES) + RC_CRC <= RC_MAX_DGRAM) ? 1 : -1];
typedef char rc_assert_cfg[(RC_CFG_MAX <= RC_CFG_FRAG * RC_CFG_MAXFRAG) ? 1 : -1];

/* ---- byte helpers ---- */
void rc_w_init(rc_w *w, uint8_t *buf, uint32_t cap){ w->p = buf; w->n = 0; w->cap = cap; w->err = 0; }
static int w_room(rc_w *w, uint32_t k){ if (w->err || w->cap - w->n < k){ w->err = 1; return 0; } return 1; }
void rc_put8(rc_w *w, uint32_t v){ if (w_room(w, 1)) w->p[w->n++] = (uint8_t)v; }
void rc_put16(rc_w *w, uint32_t v){ if (w_room(w, 2)){ w->p[w->n] = (uint8_t)v; w->p[w->n + 1] = (uint8_t)(v >> 8); w->n += 2; } }
void rc_put32(rc_w *w, uint32_t v){
    if (!w_room(w, 4)) return;
    w->p[w->n] = (uint8_t)v; w->p[w->n + 1] = (uint8_t)(v >> 8);
    w->p[w->n + 2] = (uint8_t)(v >> 16); w->p[w->n + 3] = (uint8_t)(v >> 24); w->n += 4;
}
void rc_putf(rc_w *w, float f){ rc_put32(w, rc_fbits(f)); }
void rc_putb(rc_w *w, const void *src, uint32_t n){ if (w_room(w, n)){ if (n) memcpy(w->p + w->n, src, n); w->n += n; } }
void rc_r_init(rc_r *r, const uint8_t *buf, uint32_t len){ r->p = buf; r->n = 0; r->len = len; r->err = 0; }
static int r_have(rc_r *r, uint32_t k){ if (r->err || r->len - r->n < k){ r->err = 1; return 0; } return 1; }
uint32_t rc_get8(rc_r *r){ return r_have(r, 1) ? r->p[r->n++] : 0u; }
uint32_t rc_get16(rc_r *r){
    if (!r_have(r, 2)) return 0;
    uint32_t v = (uint32_t)r->p[r->n] | (uint32_t)r->p[r->n + 1] << 8; r->n += 2; return v;
}
uint32_t rc_get32(rc_r *r){
    if (!r_have(r, 4)) return 0;
    uint32_t v = (uint32_t)r->p[r->n] | (uint32_t)r->p[r->n + 1] << 8 | (uint32_t)r->p[r->n + 2] << 16 |
                 (uint32_t)r->p[r->n + 3] << 24;
    r->n += 4; return v;
}
float rc_getf(rc_r *r){ return rc_bitsf(rc_get32(r)); }
void rc_getb(rc_r *r, void *dst, uint32_t n){ if (r_have(r, n)){ if (n) memcpy(dst, r->p + r->n, n); r->n += n; } else if (n) memset(dst, 0, n); }
uint32_t rc_fbits(float f){ uint32_t u; memcpy(&u, &f, 4); return u; }
float rc_bitsf(uint32_t u){ float f; memcpy(&f, &u, 4); return f; }
float rc_sqrtf(float x){ return x > 0.f ? __builtin_sqrtf(x) : 0.f; }
uint32_t rc_crc32(const uint8_t *p, uint32_t n){
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++){
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
uint32_t rc_fnv1a(const char *s){
    if (!s || !*s) return 0;
    uint32_t h = 0x811C9DC5u;
    for (; *s; s++){ h ^= (uint8_t)*s; h *= 0x01000193u; }
    return h ? h : 1u;
}
void rc_name_clean(char *dst, const char *src, uint32_t srclen){
    uint32_t n = 0;
    for (uint32_t i = 0; src && i < srclen && src[i] && n < RC_NAME_MAX; i++){
        uint8_t c = (uint8_t)src[i];
        if (c >= 0x20 && c < 0x7F && !(n == 0 && c == ' ')) dst[n++] = (char)c;
    }
    while (n && dst[n - 1] == ' ') n--;
    if (!n){ memcpy(dst, "Player", 7); return; }
    dst[n] = 0;
}
const char *rc_status_name(int st){
    static const char *n[RC_ST__N] = { "menus", "loading", "free roam", "event", "shop", "paused" };
    return (st >= 0 && st < RC_ST__N) ? n[st] : "?";
}

/* ---- float helpers ---- */
static int f_finite(float f){ uint32_t u = rc_fbits(f); return (u & 0x7F800000u) != 0x7F800000u; }
static float f_abs(float f){ return f < 0 ? -f : f; }
static float f_clamp(float f, float lo, float hi){ return f < lo ? lo : f > hi ? hi : f; }
static int32_t f_round(float f){
    if (!(f == f)) return 0;
    if (f >= 2147483520.f) return 2147483647;
    if (f <= -2147483520.f) return -2147483647 - 1;
    return (int32_t)(f >= 0 ? f + 0.5f : f - 0.5f);
}
static void q_norm(float *q){
    float n = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
    if (n < 1e-12f){ q[0] = q[1] = q[2] = 0; q[3] = 1; return; }
    float k = 1.f / rc_sqrtf(n);
    for (int i = 0; i < 4; i++) q[i] *= k;
}
float rc_expneg(float x){
    if (!(x > 0)) return 1.f;
    if (x > 30.f) return 0.f;
    int n = (int)x;
    float f = x - (float)n;
    float e = 1.f + f*(1.f + f*(1.f/2.f + f*(1.f/6.f + f*(1.f/24.f + f*(1.f/120.f + f*(1.f/720.f + f*(1.f/5040.f + f*(1.f/40320.f))))))));
    float r = 1.f / e;
    for (int i = 0; i < n; i++) r *= 0.36787944117f;
    return r;
}
static void q_mul(const float *a, const float *b, float *o){
    float x = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    float y = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    float z = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    float w = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
static void q_conj(const float *a, float *o){ o[0] = -a[0]; o[1] = -a[1]; o[2] = -a[2]; o[3] = a[3]; }
static void q_nlerp(const float *a, const float *b, float t, float *o){
    float d = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3], s = d < 0 ? -1.f : 1.f;
    for (int i = 0; i < 4; i++) o[i] = (1.f - t) * a[i] + t * s * b[i];
    q_norm(o);
}
static void q_integrate(const float *q0, const float *w, float dt, float *o){
    float q[4] = { q0[0], q0[1], q0[2], q0[3] }, h = dt * 0.25f;
    for (int k = 0; k < 4; k++){
        float wq[4] = { w[0], w[1], w[2], 0 }, d[4];
        q_mul(wq, q, d);
        for (int i = 0; i < 4; i++) q[i] += 0.5f * h * d[i];
        q_norm(q);
    }
    for (int i = 0; i < 4; i++) o[i] = q[i];
}

/* ---- states on the wire ---- */
static int16_t q16(float f, float scale){ int32_t v = f_round(f * scale); return (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v); }
static void put_state(rc_w *w, const rc_state *s){
    rc_put32(w, s->t_us);
    for (int i = 0; i < 3; i++) rc_putf(w, s->p[i]);
    for (int i = 0; i < 3; i++) rc_putf(w, s->v[i]);
    float q[4] = { s->q[0], s->q[1], s->q[2], s->q[3] }; q_norm(q);
    for (int i = 0; i < 4; i++) rc_put16(w, (uint16_t)q16(q[i], 32767.f));
    for (int i = 0; i < 3; i++) rc_put16(w, (uint16_t)q16(s->w[i], 1000.f));
    rc_put16(w, (uint16_t)q16(f_clamp(s->steer, -1.f, 1.f), 32767.f));
    rc_put8(w, (uint32_t)f_round(f_clamp(s->throttle, 0.f, 1.f) * 255.f));
    rc_put8(w, (uint32_t)f_round(f_clamp(s->brake, 0.f, 1.f) * 255.f));
    rc_put16(w, (uint32_t)f_round(f_clamp(s->rpm, 0.f, 65535.f)));
    rc_put16(w, (uint16_t)q16(f_clamp(s->speed, -327.f, 327.f), 100.f));
    rc_put8(w, (uint8_t)s->gear);
    rc_put8(w, s->bits);
}
/* the extension: its length (what follows), then the game's bytes */
static void put_statex(rc_w *w, const rc_state *s){
    uint32_t n = s->ext_len > ROAM_EXT_MAX ? ROAM_EXT_MAX : s->ext_len;
    rc_put8(w, n); rc_putb(w, s->ext, n);
}
static int get_statex(rc_r *r, rc_state *s){
    uint32_t n = rc_get8(r);
    if (r->err) return RC_E_SHORT;
    if (!r_have(r, n)) return RC_E_SHORT;
    uint32_t k = n > ROAM_EXT_MAX ? ROAM_EXT_MAX : n;
    memcpy(s->ext, r->p + r->n, k);
    s->ext_len = (uint8_t)k;
    r->n += n;                                                      /* a longer one's rest is skipped */
    return RC_OK;
}
static int get_state(rc_r *r, rc_state *s){
    s->ext_len = 0;
    s->t_us = rc_get32(r);
    for (int i = 0; i < 3; i++) s->p[i] = rc_getf(r);
    for (int i = 0; i < 3; i++) s->v[i] = rc_getf(r);
    for (int i = 0; i < 4; i++) s->q[i] = (float)(int16_t)rc_get16(r) / 32767.f;
    for (int i = 0; i < 3; i++) s->w[i] = (float)(int16_t)rc_get16(r) / 1000.f;
    s->steer = (float)(int16_t)rc_get16(r) / 32767.f;
    s->throttle = (float)rc_get8(r) / 255.f;
    s->brake = (float)rc_get8(r) / 255.f;
    s->rpm = (float)rc_get16(r);
    s->speed = (float)(int16_t)rc_get16(r) / 100.f;
    s->gear = (int8_t)rc_get8(r);
    s->bits = (uint8_t)rc_get8(r);
    if (r->err) return RC_E_SHORT;
    for (int i = 0; i < 3; i++) if (!f_finite(s->p[i]) || !f_finite(s->v[i])) return RC_E_RANGE;
    for (int i = 0; i < 3; i++) if (f_abs(s->p[i]) > 20000.f) return RC_E_RANGE;
    if (s->v[0]*s->v[0] + s->v[1]*s->v[1] + s->v[2]*s->v[2] > 200.f * 200.f) return RC_E_RANGE;
    float n = s->q[0]*s->q[0] + s->q[1]*s->q[1] + s->q[2]*s->q[2] + s->q[3]*s->q[3];
    if (n < 0.25f) return RC_E_RANGE;
    q_norm(s->q);
    return RC_OK;
}
void rc_state_put(rc_w *w, const rc_state *s, int ext){ put_state(w, s); if (ext) put_statex(w, s); }
int rc_state_get(rc_r *r, rc_state *s, int ext){
    int e = get_state(r, s);
    return e ? e : ext ? get_statex(r, s) : RC_OK;
}

/* ---- messages ---- */
static int printable(const char *s, uint32_t n){ for (uint32_t i = 0; i < n; i++) if ((uint8_t)s[i] < 0x20 || (uint8_t)s[i] >= 0x7F) return 0; return 1; }
/* a flag's code (nfsu2-web's flag_ok / flag_len): lower case letters and '-' */
static int flag_ok(const char *s, uint32_t n){ for (uint32_t i = 0; i < n; i++) if (!((s[i] >= 'a' && s[i] <= 'z') || s[i] == '-')) return 0; return 1; }
static uint32_t flag_len(const char *s){ uint32_t n = 0; while (n <= RC_FLAG_MAX && s[n]) n++; return n; }   /* RC_FLAG_MAX + 1: too long */

int rc_encode(const rc_msg *m, uint8_t *out, int cap){
    rc_w w; rc_w_init(&w, out, (uint32_t)(cap < RC_MAX_DGRAM ? cap : RC_MAX_DGRAM));
    rc_put8(&w, RC_MAGIC0); rc_put8(&w, RC_MAGIC1); rc_put8(&w, RC_MAGIC2); rc_put8(&w, RC_MAGIC3);
    rc_put8(&w, RC_PROTO_VER); rc_put8(&w, m->h.type); rc_put16(&w, 0);
    rc_put32(&w, m->h.sender); rc_put16(&w, m->h.seq); rc_put16(&w, m->h.flags);
    switch (m->h.type){
    case RC_MSG_HELLO: {
        uint32_t nl = (uint32_t)strlen(m->u.hello.name);
        if (nl > RC_NAME_MAX || m->u.hello.n_seen > RC_MAX_PEERS) return -1;
        rc_put16(&w, m->u.hello.core_ver); rc_put8(&w, m->u.hello.platform); rc_put8(&w, m->u.hello.status);
        rc_put16(&w, m->u.hello.listen_port); rc_put16(&w, m->u.hello.cfg_rev); rc_put32(&w, m->u.hello.room);
        rc_put8(&w, nl); rc_putb(&w, m->u.hello.name, nl);
        rc_put8(&w, m->u.hello.n_seen);
        for (int i = 0; i < m->u.hello.n_seen; i++) rc_put32(&w, m->u.hello.seen[i]);
        uint32_t fl = flag_len(m->u.hello.flag);
        if (fl > RC_FLAG_MAX || !flag_ok(m->u.hello.flag, fl)) return -1;
        if (m->u.hello.links || fl){                               /* no crews, then the links (nfsu2-web's core 10) */
            if (!(m->h.flags & RC_HF_CREW) || m->u.hello.links > RC_MAX_PEERS) return -1;
            rc_put8(&w, 0); rc_put8(&w, m->u.hello.links);
            if (fl){ rc_put32(&w, 0); rc_put8(&w, fl); rc_putb(&w, m->u.hello.flag, fl); }   /* no duel, the flag (its core 13) */
        }
        break; }
    case RC_MSG_PEERS:
        if (m->u.peers.n > RC_LIST_MAX) return -1;
        rc_put8(&w, m->u.peers.n);
        for (int i = 0; i < m->u.peers.n; i++){
            uint32_t ip = m->u.peers.e[i].ip;
            rc_put8(&w, ip >> 24); rc_put8(&w, ip >> 16); rc_put8(&w, ip >> 8); rc_put8(&w, ip);
            rc_put16(&w, m->u.peers.e[i].port); rc_put32(&w, m->u.peers.e[i].id);
        }
        break;
    case RC_MSG_SNAP:
        if (m->u.snap.n < 1 || m->u.snap.n > RC_SNAP_STATES) return -1;
        rc_put8(&w, m->u.snap.status); rc_put8(&w, m->u.snap.epoch); rc_put16(&w, m->u.snap.cfg_rev);
        rc_put8(&w, m->u.snap.n); rc_put8(&w, m->u.snap.clock);
        for (int i = 0; i < m->u.snap.n; i++) rc_state_put(&w, &m->u.snap.s[i], (m->h.flags & RC_HF_STATEX) != 0);
        break;
    case RC_MSG_CFG:
        if (m->u.cfg.nfrag < 1 || m->u.cfg.nfrag > RC_CFG_MAXFRAG || m->u.cfg.frag >= m->u.cfg.nfrag ||
            m->u.cfg.total > RC_CFG_MAX || m->u.cfg.dlen > RC_CFG_FRAG) return -1;
        rc_put16(&w, m->u.cfg.cfg_rev); rc_put8(&w, m->u.cfg.frag); rc_put8(&w, m->u.cfg.nfrag);
        rc_put16(&w, m->u.cfg.total); rc_put16(&w, m->u.cfg.dlen); rc_putb(&w, m->u.cfg.data, m->u.cfg.dlen);
        break;
    case RC_MSG_CFG_REQ: rc_put16(&w, m->u.cfgreq.cfg_rev); rc_put8(&w, m->u.cfgreq.have_mask); break;
    case RC_MSG_PING: rc_put32(&w, m->u.ping.t0); break;
    case RC_MSG_PONG: rc_put32(&w, m->u.ping.t0); rc_put32(&w, m->u.ping.t1); rc_put32(&w, m->u.ping.t2); break;
    case RC_MSG_BYE: rc_put8(&w, m->u.bye.reason); break;
    case RC_MSG_FAR:
        if (m->u.far.n > RC_FAR_PART) return -1;
        rc_put8(&w, m->u.far.n);
        for (int i = 0; i < m->u.far.n; i++){
            const roam_far_peer *e = &m->u.far.e[i];
            uint32_t nl = (uint32_t)strlen(e->name);
            if (nl > RC_NAME_MAX) return -1;
            rc_put32(&w, e->id); rc_put8(&w, e->status);
            for (int k = 0; k < 3; k++) rc_putf(&w, e->p[k]);
            for (int k = 0; k < 4; k++) rc_put16(&w, (uint16_t)q16(e->q[k], 32767.f));
            rc_put8(&w, nl); rc_putb(&w, e->name, nl);
        }
        break;
    case RC_MSG_GAME:
        if (m->u.game.len < 1 || m->u.game.len > RC_GAME_MAX) return -1;
        rc_put8(&w, m->u.game.len); rc_putb(&w, m->u.game.data, m->u.game.len);
        break;
    default: return -1;
    }
    if (w.err || w.n + RC_CRC > w.cap) return -1;
    uint32_t len = w.n + RC_CRC;
    out[6] = (uint8_t)len; out[7] = (uint8_t)(len >> 8);
    rc_put32(&w, rc_crc32(out, w.n));
    return w.err ? -1 : (int)w.n;
}

int rc_decode(const uint8_t *buf, int len, rc_msg *m){
    memset(m, 0, sizeof *m);
    if (len < RC_HDR + RC_CRC) return RC_E_SHORT;
    if (len > RC_MAX_DGRAM) return RC_E_LENGTH;
    if (buf[0] != RC_MAGIC0 || buf[1] != RC_MAGIC1 || buf[2] != RC_MAGIC2 || buf[3] != RC_MAGIC3) return RC_E_MAGIC;
    rc_r r; rc_r_init(&r, buf, (uint32_t)len - RC_CRC);
    r.n = 4;
    m->h.ver = (uint8_t)rc_get8(&r);
    if (m->h.ver != RC_PROTO_VER) return RC_E_VERSION;
    m->h.type = (uint8_t)rc_get8(&r); m->h.len = (uint16_t)rc_get16(&r);
    m->h.sender = rc_get32(&r); m->h.seq = (uint16_t)rc_get16(&r); m->h.flags = (uint16_t)rc_get16(&r);
    if (m->h.len != (uint32_t)len) return RC_E_LENGTH;
    uint32_t crc = (uint32_t)buf[len - 4] | (uint32_t)buf[len - 3] << 8 | (uint32_t)buf[len - 2] << 16 | (uint32_t)buf[len - 1] << 24;
    if (crc != rc_crc32(buf, (uint32_t)len - RC_CRC)) return RC_E_CRC;
    if (!m->h.sender) return RC_E_FIELD;
    switch (m->h.type){
    case RC_MSG_HELLO: {
        m->u.hello.core_ver = (uint16_t)rc_get16(&r); m->u.hello.platform = (uint8_t)rc_get8(&r);
        m->u.hello.status = (uint8_t)rc_get8(&r); m->u.hello.listen_port = (uint16_t)rc_get16(&r);
        m->u.hello.cfg_rev = (uint16_t)rc_get16(&r); m->u.hello.room = rc_get32(&r);
        uint32_t nl = rc_get8(&r);
        if (nl > RC_NAME_MAX) return RC_E_FIELD;
        rc_getb(&r, m->u.hello.name, nl); m->u.hello.name[nl] = 0;
        if (r.err) return RC_E_SHORT;
        if (!printable(m->u.hello.name, nl)) return RC_E_FIELD;
        m->u.hello.n_seen = (uint8_t)rc_get8(&r);
        if (m->u.hello.n_seen > RC_MAX_PEERS) return RC_E_FIELD;
        for (int i = 0; i < m->u.hello.n_seen; i++) m->u.hello.seen[i] = rc_get32(&r);
        if (m->u.hello.status >= RC_ST__N) return RC_E_FIELD;
        if ((m->h.flags & RC_HF_CREW) && !r.err && r.n < r.len){
            uint32_t nc = rc_get8(&r);
            if (nc > 4) return RC_E_FIELD;
            r.n += 4 * nc < r.len - r.n ? 4 * nc : r.len - r.n;   /* crews: not used here */
            if (r.n < r.len) m->u.hello.links = (uint8_t)rc_get8(&r);
            if (r.n + 4 <= r.len) r.n += 4;                       /* nfsu2-web's duel: not used here */
            if (r.n < r.len){                                     /* the flag (core 19): one that is not a code, none */
                uint32_t fl = rc_get8(&r);
                if (fl <= RC_FLAG_MAX && r.n + fl <= r.len){
                    rc_getb(&r, m->u.hello.flag, fl); m->u.hello.flag[fl] = 0;
                    if (!flag_ok(m->u.hello.flag, fl)) m->u.hello.flag[0] = 0;
                }
            }
            r.n = r.len;                                          /* a newer core's additions */
        }
        break; }
    case RC_MSG_PEERS:
        m->u.peers.n = (uint8_t)rc_get8(&r);
        if (m->u.peers.n > RC_LIST_MAX) return RC_E_FIELD;
        for (int i = 0; i < m->u.peers.n; i++){
            uint32_t a = rc_get8(&r), b = rc_get8(&r), c = rc_get8(&r), d = rc_get8(&r);
            m->u.peers.e[i].ip = a << 24 | b << 16 | c << 8 | d;
            m->u.peers.e[i].port = (uint16_t)rc_get16(&r); m->u.peers.e[i].id = rc_get32(&r);
            if (!r.err && (!m->u.peers.e[i].port || !m->u.peers.e[i].id)) return RC_E_FIELD;
        }
        if (!r.err && (m->h.flags & RC_HF_CREW)) r.n = r.len;      /* a hub's marks: not used here */
        break;
    case RC_MSG_SNAP: {
        m->u.snap.status = (uint8_t)rc_get8(&r); m->u.snap.epoch = (uint8_t)rc_get8(&r);
        m->u.snap.cfg_rev = (uint16_t)rc_get16(&r); m->u.snap.n = (uint8_t)rc_get8(&r); m->u.snap.clock = (uint8_t)rc_get8(&r);
        if (r.err) return RC_E_SHORT;
        if (m->u.snap.n < 1 || m->u.snap.n > RC_SNAP_STATES || m->u.snap.status >= RC_ST__N) return RC_E_FIELD;
        for (int i = 0; i < m->u.snap.n; i++){
            int e = rc_state_get(&r, &m->u.snap.s[i], (m->h.flags & RC_HF_STATEX) != 0);
            if (e) return e;
        }
        break; }
    case RC_MSG_CFG:
        m->u.cfg.cfg_rev = (uint16_t)rc_get16(&r); m->u.cfg.frag = (uint8_t)rc_get8(&r);
        m->u.cfg.nfrag = (uint8_t)rc_get8(&r); m->u.cfg.total = (uint16_t)rc_get16(&r);
        m->u.cfg.dlen = (uint16_t)rc_get16(&r);
        if (r.err) return RC_E_SHORT;
        if (m->u.cfg.nfrag < 1 || m->u.cfg.nfrag > RC_CFG_MAXFRAG || m->u.cfg.frag >= m->u.cfg.nfrag ||
            m->u.cfg.total > RC_CFG_MAX || m->u.cfg.dlen > RC_CFG_FRAG) return RC_E_FIELD;
        { uint32_t total = m->u.cfg.total, nf = (total + RC_CFG_FRAG - 1u) / RC_CFG_FRAG;
          uint32_t off = (uint32_t)m->u.cfg.frag * RC_CFG_FRAG;
          if (!total || nf != m->u.cfg.nfrag) return RC_E_FIELD;
          uint32_t want = m->u.cfg.frag + 1u < nf ? RC_CFG_FRAG : total - off;
          if (m->u.cfg.dlen != want) return RC_E_FIELD; }
        if (!r_have(&r, m->u.cfg.dlen)) return RC_E_SHORT;
        m->u.cfg.data = r.p + r.n; r.n += m->u.cfg.dlen;
        break;
    case RC_MSG_CFG_REQ: m->u.cfgreq.cfg_rev = (uint16_t)rc_get16(&r); m->u.cfgreq.have_mask = (uint8_t)rc_get8(&r); break;
    case RC_MSG_PING: m->u.ping.t0 = rc_get32(&r); break;
    case RC_MSG_PONG: m->u.ping.t0 = rc_get32(&r); m->u.ping.t1 = rc_get32(&r); m->u.ping.t2 = rc_get32(&r); break;
    case RC_MSG_BYE: m->u.bye.reason = (uint8_t)rc_get8(&r); break;
    case RC_MSG_FAR:
        m->u.far.n = (uint8_t)rc_get8(&r);
        if (m->u.far.n > RC_FAR_PART) return RC_E_FIELD;
        for (int i = 0; i < m->u.far.n; i++){
            roam_far_peer *e = &m->u.far.e[i];
            e->id = rc_get32(&r); e->status = (uint8_t)rc_get8(&r);
            for (int k = 0; k < 3; k++) e->p[k] = rc_getf(&r);
            for (int k = 0; k < 4; k++) e->q[k] = (float)(int16_t)rc_get16(&r) / 32767.f;
            uint32_t nl = rc_get8(&r);
            if (r.err) return RC_E_SHORT;
            if (!e->id || e->status >= RC_ST__N || nl > RC_NAME_MAX) return RC_E_FIELD;
            for (int k = 0; k < 3; k++) if (!(e->p[k] > -20000.f && e->p[k] < 20000.f)) return RC_E_RANGE;   /* NaN too */
            rc_getb(&r, e->name, nl); e->name[nl] = 0;
            if (r.err) return RC_E_SHORT;
            if (!printable(e->name, nl)) return RC_E_FIELD;
        }
        break;
    case RC_MSG_GAME:
        m->u.game.len = (uint8_t)rc_get8(&r);
        if (r.err) return RC_E_SHORT;
        if (m->u.game.len < 1 || m->u.game.len > RC_GAME_MAX) return RC_E_FIELD;
        rc_getb(&r, m->u.game.data, m->u.game.len);
        break;
    default: return RC_E_TYPE;
    }
    if (r.err) return RC_E_SHORT;
    if (r.n != r.len) return RC_E_TRAIL;
    return RC_OK;
}

/* ---- interpolation ---- */
void rc_interp_reset(rc_interp *ip){ memset(ip, 0, sizeof *ip); ip->err_q[3] = 1; ip->prev_q[3] = 1; }

static int64_t unwrap(rc_interp *ip, uint32_t t32){
    if (!ip->t_init){ ip->t_init = 1; ip->t_last = (int64_t)t32 + ((int64_t)1 << 32); return ip->t_last; }
    int64_t t = ip->t_last + (int32_t)(t32 - (uint32_t)ip->t_last);
    if (t > ip->t_last) ip->t_last = t;
    return t;
}
void rc_interp_insert(rc_interp *ip, const rc_state *s, uint8_t epoch, uint8_t clock, uint8_t status, int64_t recv_us, int newest){
    /* epochs only move forward (mod 256): a state of an older one arrived late */
    if (ip->epoch_init && (int8_t)(uint8_t)(epoch - ip->epoch) < 0){ ip->n_stale++; return; }
    if (ip->epoch_init && epoch == ip->epoch && ip->clock_init && (int8_t)(uint8_t)(clock - ip->clock) < 0){ ip->n_stale++; return; }
    if (ip->epoch_init && epoch == ip->epoch && ip->clock_init && clock != ip->clock){
        /* the sender re-anchored its clock: a new time line, blended (what is shown stays) */
        ip->n = 0; ip->t_init = 0; ip->odn = ip->odh = 0; ip->off_init = 0;
        ip->clock = clock; ip->n_epoch++;
    }
    if (!ip->epoch_init || epoch != ip->epoch){
        /* new epoch (teleport, restart): forget the past, snap */
        int had = ip->epoch_init;
        uint64_t keep[9] = { ip->n_eval, ip->n_interp, ip->n_extrap, ip->n_hold, ip->n_dup, ip->n_old, ip->n_snap, ip->n_epoch, ip->n_stale };
        float em = ip->err_max;
        rc_interp_reset(ip);
        ip->n_eval = keep[0]; ip->n_interp = keep[1]; ip->n_extrap = keep[2]; ip->n_hold = keep[3];
        ip->n_dup = keep[4]; ip->n_old = keep[5]; ip->n_snap = keep[6]; ip->n_epoch = keep[7] + (had ? 1 : 0); ip->n_stale = keep[8];
        ip->err_max = em;
        ip->epoch_init = 1; ip->epoch = epoch; ip->snap_pending = 1;
        ip->clock_init = 1; ip->clock = clock;
    }
    ip->status = status;
    int64_t t = unwrap(ip, s->t_us);
    if (newest){
        int k = ip->odh; ip->od[k].at = recv_us; ip->od[k].d = recv_us - t;
        ip->odh = (ip->odh + 1) % RC_OFFWIN; if (ip->odn < RC_OFFWIN) ip->odn++;
    }
    int i = ip->n;
    while (i > 0 && ip->t[i - 1] > t) i--;
    if (i > 0 && ip->t[i - 1] == t){ ip->n_dup++; return; }
    if (ip->n == RC_BUF_STATES){
        if (i == 0){ ip->n_old++; return; }
        memmove(&ip->t[0], &ip->t[1], sizeof ip->t[0] * (size_t)(RC_BUF_STATES - 1));
        memmove(&ip->s[0], &ip->s[1], sizeof ip->s[0] * (size_t)(RC_BUF_STATES - 1));
        ip->n--; i--;
    }
    memmove(&ip->t[i + 1], &ip->t[i], sizeof ip->t[0] * (size_t)(ip->n - i));
    memmove(&ip->s[i + 1], &ip->s[i], sizeof ip->s[0] * (size_t)(ip->n - i));
    ip->t[i] = t; ip->s[i] = *s; ip->n++;
}
/* receiver host time minus sender time: the smallest over the last 2 s */
static int off_estimate(const rc_interp *ip, int64_t *est){
    int64_t newest = 0; int have = 0;
    for (int k = 0; k < ip->odn; k++) if (!have || ip->od[k].at > newest){ newest = ip->od[k].at; have = 1; }
    if (!have) return 0;
    int64_t m = 0; int got = 0;
    for (int k = 0; k < ip->odn; k++)
        if (ip->od[k].at >= newest - 2000000 && (!got || ip->od[k].d < m)){ m = ip->od[k].d; got = 1; }
    *est = m; return got;
}
static void from_state(const rc_state *s, rc_pose *o){
    for (int i = 0; i < 3; i++){ o->p[i] = s->p[i]; o->v[i] = s->v[i]; o->w[i] = s->w[i]; }
    for (int i = 0; i < 4; i++) o->q[i] = s->q[i];
    o->steer = s->steer; o->throttle = s->throttle; o->brake = s->brake; o->rpm = s->rpm; o->speed = s->speed;
    o->gear = s->gear; o->bits = s->bits; o->ext_len = s->ext_len; memcpy(o->ext, s->ext, sizeof o->ext);
}
static void lerp_rest(const rc_state *a, const rc_state *b, float u, rc_pose *o){
    for (int i = 0; i < 3; i++){ o->v[i] = a->v[i] + u * (b->v[i] - a->v[i]); o->w[i] = a->w[i] + u * (b->w[i] - a->w[i]); }
    o->steer = a->steer + u * (b->steer - a->steer);
    o->throttle = a->throttle + u * (b->throttle - a->throttle);
    o->brake = a->brake + u * (b->brake - a->brake);
    o->rpm = a->rpm + u * (b->rpm - a->rpm);
    o->speed = a->speed + u * (b->speed - a->speed);
    const rc_state *n = u < 0.5f ? a : b;
    o->gear = n->gear; o->bits = n->bits; o->ext_len = n->ext_len; memcpy(o->ext, n->ext, sizeof o->ext);
}
/* Extrapolation goes in a straight line at the newest velocity (nfsu2-web's
 * arc mode assumes a z-up world and is left out). */
int rc_interp_peek(const rc_interp *ip, int64_t rt, int64_t cap_us, rc_pose *o){
    memset(o, 0, sizeof *o); o->q[3] = 1;
    if (ip->n == 0) return 0;
    if (rt <= ip->t[0]){ from_state(&ip->s[0], o); o->mode = ROAM_MODE_HOLD; return 1; }
    int n = ip->n;
    if (rt >= ip->t[n - 1]){
        const rc_state *s = &ip->s[n - 1];
        from_state(s, o);
        int64_t dt = rt - ip->t[n - 1];
        if (ip->status == RC_ST_PAUSED){ o->mode = ROAM_MODE_HOLD; for (int i = 0; i < 3; i++){ o->v[i] = 0; o->w[i] = 0; } o->speed = 0; return 1; }
        o->mode = dt > cap_us ? ROAM_MODE_HOLD : ROAM_MODE_EXTRAP;
        if (dt > cap_us) dt = cap_us;
        float ds = (float)(int32_t)dt * 1e-6f;
        for (int i = 0; i < 3; i++) o->p[i] = s->p[i] + s->v[i] * ds;
        q_integrate(s->q, s->w, ds, o->q);
        return 1;
    }
    int k = 0;
    while (k + 2 < n && ip->t[k + 1] <= rt) k++;
    const rc_state *a = &ip->s[k], *b = &ip->s[k + 1];
    int64_t span = ip->t[k + 1] - ip->t[k], into = rt - ip->t[k];
    if (span > 600000000) span = 600000000;
    if (into > span) into = span;
    if (into < 0) into = 0;
    float dt = (float)(int32_t)span * 1e-6f, u = (float)(int32_t)into / (float)(int32_t)span;
    u = f_clamp(u, 0.f, 1.f);
    /* cubic Hermite with the sender's velocities as tangents, unless the two
     * states disagree (a jump the epoch did not announce): then linear */
    float gap = 0;
    for (int i = 0; i < 3; i++){ float e = b->p[i] - (a->p[i] + 0.5f * (a->v[i] + b->v[i]) * dt); gap += e * e; }
    if (gap > 25.f || dt > 1.f){
        for (int i = 0; i < 3; i++) o->p[i] = a->p[i] + u * (b->p[i] - a->p[i]);
    } else {
        float u2 = u * u, u3 = u2 * u;
        float h00 = 2*u3 - 3*u2 + 1, h10 = u3 - 2*u2 + u, h01 = -2*u3 + 3*u2, h11 = u3 - u2;
        for (int i = 0; i < 3; i++) o->p[i] = h00 * a->p[i] + h10 * dt * a->v[i] + h01 * b->p[i] + h11 * dt * b->v[i];
    }
    q_nlerp(a->q, b->q, u, o->q);
    lerp_rest(a, b, u, o);
    o->mode = ROAM_MODE_INTERP;
    return 1;
}
int rc_interp_eval(rc_interp *ip, int64_t now_us, int64_t frame_dt_us, int64_t delay_us, int64_t cap_us, rc_pose *out){
    if (ip->n == 0){ memset(out, 0, sizeof *out); out->q[3] = 1; return 0; }
    int64_t est;
    if (off_estimate(ip, &est)){
        if (!ip->off_init || ip->snap_pending){ ip->off_r = est; ip->off_init = 1; }
        else {
            int32_t fdt = frame_dt_us > 1000000 ? 1000000 : frame_dt_us < 0 ? 0 : (int32_t)frame_dt_us;
            int64_t d = est - ip->off_r, lim = fdt / 20 + 1;              /* slew at most 5 % */
            if (d > 250000 || d < -250000){ ip->off_r = est; ip->n_snap++; }
            else ip->off_r += d > lim ? lim : d < -lim ? -lim : d;
        }
    }
    if (!ip->off_init){ memset(out, 0, sizeof *out); out->q[3] = 1; return 0; }
    int64_t rt = now_us - ip->off_r - delay_us;
    rc_pose tgt; rc_interp_peek(ip, rt, cap_us, &tgt);
    ip->n_eval++;
    if (tgt.mode == ROAM_MODE_INTERP) ip->n_interp++; else if (tgt.mode == ROAM_MODE_EXTRAP) ip->n_extrap++; else ip->n_hold++;
    if (!ip->shown || ip->snap_pending){
        for (int i = 0; i < 3; i++) ip->err_p[i] = 0;
        ip->err_q[0] = ip->err_q[1] = ip->err_q[2] = 0; ip->err_q[3] = 1;
        ip->snap_pending = 0;
    } else {
        /* new data since last frame is a correction, shown as an offset that decays (no pop) */
        rc_pose was; rc_interp_peek(ip, ip->prev_rt, cap_us, &was);
        float e2 = 0;
        for (int i = 0; i < 3; i++){ ip->err_p[i] += ip->prev_p[i] - was.p[i]; e2 += ip->err_p[i] * ip->err_p[i]; }
        float cq[4], ic[4], t4[4];
        q_conj(was.q, ic); q_mul(ip->prev_q, ic, cq);
        q_mul(cq, ip->err_q, t4); q_norm(t4);
        for (int i = 0; i < 4; i++) ip->err_q[i] = t4[i];
        int32_t fdt = frame_dt_us > 1000000 ? 1000000 : frame_dt_us < 0 ? 0 : (int32_t)frame_dt_us;
        float k = rc_expneg((float)fdt / 100000.f);                 /* tau = 100 ms */
        float em = rc_sqrtf(e2);
        if (em > ip->err_max) ip->err_max = em;
        if (em > 5.f){ for (int i = 0; i < 3; i++) ip->err_p[i] = 0; ip->err_q[0] = ip->err_q[1] = ip->err_q[2] = 0; ip->err_q[3] = 1; ip->n_snap++; }
        else {
            for (int i = 0; i < 3; i++) ip->err_p[i] *= k;
            static const float id[4] = { 0, 0, 0, 1 };
            float r[4]; q_nlerp(id, ip->err_q, k, r);
            for (int i = 0; i < 4; i++) ip->err_q[i] = r[i];
        }
    }
    ip->shown = 1; ip->prev_rt = rt;
    for (int i = 0; i < 3; i++) ip->prev_p[i] = tgt.p[i];
    for (int i = 0; i < 4; i++) ip->prev_q[i] = tgt.q[i];
    *out = tgt;
    for (int i = 0; i < 3; i++) out->p[i] = tgt.p[i] + ip->err_p[i];
    q_mul(ip->err_q, tgt.q, out->q); q_norm(out->q);
    return 1;
}

/* ---- session ---- */
#define T_HELLO_CAND   1000000
#define T_HELLO_EST    2000000
#define T_PING         1000000
#define T_PEERS       10000000
#define T_BEACON       2000000
#define T_LOST         3000000      /* hide the car */
#define T_DROP        10000000      /* forget the peer */
#define T_GOSSIP_FORGET 60000000
#define T_CFGREQ        500000
#define T_CFG_ANSWER    250000
#define T_HUB_HELLO    1000000
#define T_HUB_SNAP      500000
#define T_HUB_GRACE    2500000      /* a peer the hub has not listed for this long goes */
/* ... unless it is linked and still sending: then only after this long. With K above a city's size the hub drops a
 * live pair only when one side's socket to it reconnects (players' logs 2026-11-12: 'left: out of range' and 'joined'
 * again a second later while its P2P snapshots kept coming) */
#define T_HUB_GRACE_LIVE 15000000
#define T_NOSNAP       5000000      /* listed this long without a snapshot: logged once */
#define T_HUB_SILENT  10000000      /* no list for this long: discovery and gossip again */
#define T_FAR_FED      1500000
#define RL_DGRAMS      100          /* per second and source */
#define RL_BYTES     65536

static void cat(char *t, uint32_t *n, uint32_t cap, const char *x){ for (; x && *x && *n + 1 < cap; x++) t[(*n)++] = *x; t[*n] = 0; }
static void note(rc_session *s, int ev, int peer, const char *a, const char *b, const char *c){
    if (!s->io.event) return;
    char t[160]; uint32_t n = 0;
    cat(t, &n, sizeof t, a); cat(t, &n, sizeof t, b); cat(t, &n, sizeof t, c);
    s->io.event(s->io.user, ev, peer, t);
}
static void hdr(rc_session *s, rc_msg *m, int type){ memset(m, 0, sizeof *m); m->h.type = (uint8_t)type; m->h.sender = s->conf.self_id; m->h.seq = s->seq; }
static void tx_to(rc_session *s, uint32_t ip, uint16_t port, rc_msg *m){
    uint8_t b[RC_MAX_DGRAM]; int n = rc_encode(m, b, sizeof b);
    if (n <= 0 || !s->io.send || s->closed) return;
    s->io.send(s->io.user, ip, port, b, n); s->tx++; s->tx_bytes += (uint32_t)n;
}
static void tx(rc_session *s, const rc_peer *p, rc_msg *m){ tx_to(s, p->ip, p->port, m); }
/* the hub decides who our peers are: one is configured, and it answered lately (or the first 10 s) */
static int hub_mode(const rc_session *s, int64_t now){
    if (!s->conf.hub_ip) return 0;
    if (!s->hub_since) return 1;
    return now - (s->hub_heard_us ? s->hub_heard_us : s->hub_since) < T_HUB_SILENT;
}
int rc_established(const rc_session *s){ int n = 0; for (int i = 0; i < RC_TABLE; i++) if (s->peer[i].state == RC_PS_EST) n++; return n; }
static int in_group(const rc_session *s, const rc_peer *p){
    return s->hub_was != 2 || p->configured || p->hub_listed_us == s->hub_heard_us;
}
/* with a hub configured the cap is the hub era's even while it is silent (hub_was != 2): else a 10+ city's
   peers are refused 'full' at max_players (8) through a hub gap; plain LAN discovery keeps max_players */
static int group_max(const rc_session *s){
    return s->conf.hub_ip && s->conf.max_players < RC_LIST_MAX + 1 ? RC_LIST_MAX + 1 : s->conf.max_players;
}
static int group_count(const rc_session *s){
    int n = 0;
    for (int i = 0; i < RC_TABLE; i++) if (s->peer[i].state == RC_PS_EST && in_group(s, &s->peer[i])) n++;
    return n;
}
static int id_elsewhere(const rc_session *s, uint32_t id, int except){
    for (int i = 0; i < RC_TABLE; i++) if (i != except && s->peer[i].state != RC_PS_FREE && s->peer[i].id == id) return 1;
    return 0;
}
int rc_addr_private(uint32_t ip){
    return (ip >> 24) == 127 || (ip >> 24) == 10 || (ip >> 20) == ((172u << 4) | 1u) ||
           (ip >> 16) == ((192u << 8) | 168u) || (ip >> 16) == ((169u << 8) | 254u);
}
void rc_session_init(rc_session *s, const rc_config *cfg, const rc_io *io){
    memset(s, 0, sizeof *s);
    s->conf = *cfg; s->io = *io;
    if (s->conf.max_players < 2) s->conf.max_players = 2;
    if (s->conf.max_players > RC_MAX_PLAYERS) s->conf.max_players = RC_MAX_PLAYERS;
    if (s->conf.rate_hz < 10) s->conf.rate_hz = 10;
    if (s->conf.rate_hz > 60) s->conf.rate_hz = 60;
    if (s->conf.snap_states < 1 || s->conf.snap_states > RC_SNAP_STATES) s->conf.snap_states = RC_SNAP_STATES;
    if (s->conf.links < 0) s->conf.links = 0;
    if (s->conf.links > RC_MAX_PEERS) s->conf.links = RC_MAX_PEERS;
    if (!s->conf.self_id) s->conf.self_id = 1;
    rc_name_clean(s->conf.name, cfg->name, RC_NAME_MAX + 1);
    { uint32_t fl = flag_len(s->conf.flag); if (fl > RC_FLAG_MAX || !flag_ok(s->conf.flag, fl)) s->conf.flag[0] = 0; }
    for (int i = 0; i < RC_TABLE; i++) rc_interp_reset(&s->peer[i].mo);
    s->status = RC_ST_MENUS;
    s->last_beacon = -T_BEACON;
}
static int find(rc_session *s, uint32_t ip, uint16_t port){
    for (int i = 0; i < RC_TABLE; i++) if (s->peer[i].state != RC_PS_FREE && s->peer[i].ip == ip && s->peer[i].port == port) return i;
    return -1;
}
static void peer_clear(rc_peer *p, int keep_addr){
    uint32_t ip = p->ip; uint16_t port = p->port; int conf = p->configured; int64_t added = p->added_us;
    memset(p, 0, sizeof *p); rc_interp_reset(&p->mo);
    if (keep_addr){ p->ip = ip; p->port = port; p->configured = conf; p->added_us = added; p->state = RC_PS_CANDIDATE; p->last_hello_tx = -T_HELLO_CAND; }
}
int rc_add_peer(rc_session *s, uint32_t ip, uint16_t port, int configured, int64_t now_us){
    if (!ip || !port) return -1;
    if (!configured && !rc_addr_private(ip)) return -1;
    int i = find(s, ip, port);
    if (i >= 0){ if (configured) s->peer[i].configured = 1; return i; }
    for (i = 0; i < RC_TABLE; i++) if (s->peer[i].state == RC_PS_FREE) break;
    if (i == RC_TABLE){ s->rx_unknown_full++; return -1; }
    rc_peer *p = &s->peer[i];
    peer_clear(p, 0);
    p->state = RC_PS_CANDIDATE; p->ip = ip; p->port = port; p->configured = configured;
    p->added_us = now_us; p->last_hello_tx = now_us - T_HELLO_CAND;
    return i;
}
/* to: the peer it goes to (0: everyone, i.e. the beacon and the hub; the hub also hears the links we want) */
static void fill_hello(rc_session *s, rc_msg *m, const rc_peer *to){
    hdr(s, m, RC_MSG_HELLO);
    m->h.flags = RC_HF_STATEX | RC_HF_CREW;
    if (!to){ m->h.flags |= RC_HF_ALL; m->u.hello.links = (uint8_t)s->conf.links; }
    m->u.hello.core_ver = RC_CORE_VER; m->u.hello.platform = s->conf.platform; m->u.hello.status = s->status;
    m->u.hello.listen_port = s->conf.port; m->u.hello.cfg_rev = s->cfg_rev; m->u.hello.room = s->conf.room;
    memcpy(m->u.hello.name, s->conf.name, sizeof m->u.hello.name);
    if (to) memcpy(m->u.hello.flag, s->conf.flag, sizeof m->u.hello.flag);   /* our flag to each peer; the hubs read none */
    int n = 0, full = group_count(s) + 1 >= group_max(s);
    if (to){
        /* to one peer: its own id once established, or heard while the group has room */
        if (to->id && to->last_rx_us && (to->state == RC_PS_EST || (!full && to->state == RC_PS_HEARD))) m->u.hello.seen[n++] = to->id;
        m->u.hello.n_seen = (uint8_t)n;
        return;
    }
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < RC_TABLE && n < RC_PEERS_V1; i++){
            const rc_peer *p = &s->peer[i];
            if (!p->id || !p->last_rx_us) continue;
            if (pass < 2 && (p->state != RC_PS_EST || (pass == 0) != in_group(s, p))) continue;
            if (pass == 2 && (full || p->state != RC_PS_HEARD)) continue;
            m->u.hello.seen[n++] = p->id;
        }
    m->u.hello.n_seen = (uint8_t)n;
}
static void send_hello(rc_session *s, rc_peer *p, int64_t now){ rc_msg m; fill_hello(s, &m, p); tx(s, p, &m); p->last_hello_tx = now; }
static void send_cfg(rc_session *s, rc_peer *p, uint8_t have_mask){
    if (!s->cfg_len) return;
    int nf = (s->cfg_len + RC_CFG_FRAG - 1) / RC_CFG_FRAG;
    for (int f = 0; f < nf; f++){
        if (have_mask & (1u << f)) continue;
        rc_msg m; hdr(s, &m, RC_MSG_CFG);
        m.u.cfg.cfg_rev = s->cfg_rev; m.u.cfg.frag = (uint8_t)f; m.u.cfg.nfrag = (uint8_t)nf; m.u.cfg.total = (uint16_t)s->cfg_len;
        m.u.cfg.dlen = (uint16_t)(f + 1 < nf ? RC_CFG_FRAG : s->cfg_len - f * RC_CFG_FRAG);
        m.u.cfg.data = s->cfg + f * RC_CFG_FRAG;
        tx(s, p, &m);
    }
}
/* at most every T_CFG_ANSWER: a peer asking without end cannot spend our link */
static void answer_cfg(rc_session *s, rc_peer *p, uint8_t have_mask, int64_t now){
    if (now < p->cfg_answer_us && p->cfg_answer_us - now <= T_CFG_ANSWER) return;
    p->cfg_answer_us = now + T_CFG_ANSWER;
    send_cfg(s, p, have_mask);
}
static void drop_peer(rc_session *s, int i, const char *why){
    rc_peer *p = &s->peer[i];
    if (p->state == RC_PS_EST) note(s, RC_EV_LEAVE, i, p->name, " left: ", why);
    s->peers_dirty = 1;
    peer_clear(p, p->configured);
}
static void want_cfg(rc_session *s, rc_peer *p, uint16_t rev, int64_t now){
    if (p->state != RC_PS_EST || (p->cfg_len && p->cfg_rev == rev) || !rev) return;
    if (now - p->last_cfgreq_tx < T_CFGREQ) return;
    rc_msg m; hdr(s, &m, RC_MSG_CFG_REQ);
    m.u.cfgreq.cfg_rev = rev; m.u.cfgreq.have_mask = p->asm_rev == rev ? p->asm_mask : 0;
    tx(s, p, &m); p->last_cfgreq_tx = now;
}
static int rate_ok(rc_peer *p, int len, int64_t now){
    if (now - p->rl_t0 >= 1000000 || now < p->rl_t0){ p->rl_t0 = now; p->rl_n = 0; p->rl_bytes = 0; }
    p->rl_n++; p->rl_bytes += len;
    return p->rl_n <= RL_DGRAMS && p->rl_bytes <= RL_BYTES;
}
void rc_receive(rc_session *s, uint32_t ip, uint16_t port, const uint8_t *buf, int len, int64_t now){
    s->rx++;
    if (!rc_addr_private(ip)){
        int k = find(s, ip, port);
        if (k < 0 || !s->peer[k].configured){ s->rx_policy++; return; }
    }
    rc_msg m; int e = rc_decode(buf, len, &m);
    if (e == RC_E_VERSION){ s->rx_version++; return; }
    if (e){ s->rx_bad++; return; }
    if (m.h.sender == s->conf.self_id) return;                 /* our own beacon */
    if (s->conf.hub_ip && ip == s->conf.hub_ip && port == s->conf.hub_port){
        /* the players beyond our links: map markers only */
        if (m.h.type == RC_MSG_FAR){
            /* in parts (flags part | parts << 8; an older hub: 0, one part), the nearest (0) last: each kept at
             * part * RC_FAR_PART; part 0 makes the list of the parts of its seq that came (a lost one is left out) */
            int part = m.h.flags & 0xFF, parts = m.h.flags >> 8;
            if (parts < 1) parts = 1;
            if (part >= parts || parts > RC_FAR_PARTS) return;
            if (m.h.seq != s->far_seq){ s->far_seq = m.h.seq; s->far_got = 0; }
            memcpy(s->far_in + part * RC_FAR_PART, m.u.far.e, sizeof *s->far * m.u.far.n);
            s->far_cnt[part] = m.u.far.n; s->far_got |= 1u << part;
            if (part) return;
            s->nfar = 0;
            for (int k = 0; k < parts; k++)
                if (s->far_got >> k & 1){
                    memcpy(s->far + s->nfar, s->far_in + k * RC_FAR_PART, sizeof *s->far * s->far_cnt[k]);
                    s->nfar += s->far_cnt[k];
                }
            s->far_got = 0; s->far_us = now;
            return;
        }
        /* the hub's list of the players near us: each one a candidate, kept while listed */
        if (m.h.type != RC_MSG_PEERS) return;
        s->hub_heard_us = now; s->hub_lists++;
        for (int k = 0; k < m.u.peers.n; k++){
            uint32_t id = m.u.peers.e[k].id;
            if (id == s->conf.self_id) continue;
            int j = find(s, m.u.peers.e[k].ip, m.u.peers.e[k].port);
            if (j < 0){
                if (id_elsewhere(s, id, -1)) continue;
                j = rc_add_peer(s, m.u.peers.e[k].ip, m.u.peers.e[k].port, 0, now);
                if (j < 0) continue;
            }
            s->peer[j].hub_listed_us = now;
            if (!s->peer[j].hub_first_us) s->peer[j].hub_first_us = now;
        }
        return;
    }
    for (int k = 0; k < s->nbye; k++)
        if (s->bye[k].id == m.h.sender && now - s->bye[k].at < 3000000){ s->rx_after_bye++; return; }
    int i = find(s, ip, port);
    if (i < 0){
        if (m.h.type != RC_MSG_HELLO) return;
        if (hub_mode(s, now)){ s->rx_unlisted++; return; }        /* with a hub, only the players it lists */
        if (m.u.hello.room != s->conf.room){ s->rx_room++; return; }
        if (id_elsewhere(s, m.h.sender, -1)){ s->rx_dup_id++; return; }
        i = rc_add_peer(s, ip, port, 0, now);
        if (i < 0) return;
    }
    rc_peer *p = &s->peer[i];
    if (!rate_ok(p, len, now)){ s->rx_ratelimit++; p->rx_drop++; return; }
    if (!p->id && id_elsewhere(s, m.h.sender, i)){ s->rx_dup_id++; return; }
    if (p->id && p->id != m.h.sender){
        /* another id from a known address: only a HELLO says the player restarted */
        if (m.h.type != RC_MSG_HELLO){ s->rx_stray++; return; }
        if (id_elsewhere(s, m.h.sender, i)){ s->rx_dup_id++; return; }
        note(s, RC_EV_RESTART, i, p->name, " restarted", "");
        int conf = p->configured;
        drop_peer(s, i, "restart");
        p->state = RC_PS_CANDIDATE; p->configured = conf; p->ip = ip; p->port = port; p->added_us = now;
    }
    if (!p->id) p->id = m.h.sender;
    p->rx++; p->last_rx_us = now; p->lost = 0;
    switch (m.h.type){
    case RC_MSG_HELLO: {
        if (m.u.hello.room != s->conf.room){ s->rx_room++; if (p->state == RC_PS_EST) drop_peer(s, i, "other room"); return; }
        int first = !p->name[0];
        memcpy(p->name, m.u.hello.name, sizeof p->name);
        if (!(m.h.flags & RC_HF_ALL)) memcpy(p->flag, m.u.hello.flag, sizeof p->flag);   /* a beacon carries none */
        p->platform = m.u.hello.platform; p->status = m.u.hello.status; p->cfg_rev_seen = m.u.hello.cfg_rev;
        p->core_ver = m.u.hello.core_ver;
        p->statex = (m.h.flags & RC_HF_STATEX) != 0;
        int heard = 0;
        for (int k = 0; k < m.u.hello.n_seen; k++) if (m.u.hello.seen[k] == s->conf.self_id) heard = 1;
        int vague = !heard && (m.h.flags & RC_HF_ALL) && m.u.hello.n_seen >= RC_PEERS_V1;
        if (!(vague && p->state == RC_PS_EST)) p->hears_us = heard;
        if (p->hears_us) p->hears_miss = 0;
        if (p->state != RC_PS_EST){
            if (p->state == RC_PS_CANDIDATE) p->state = RC_PS_HEARD;
            if (p->hears_us){
                if (group_count(s) + 1 >= group_max(s)){
                    if (first) note(s, RC_EV_NOTE, i, p->name, ": the group is full", "");
                } else {
                    p->state = RC_PS_EST; s->peers_dirty = 1;
                    note(s, RC_EV_JOIN, i, p->name, " joined", "");
                    send_hello(s, p, now);
                    answer_cfg(s, p, 0, now);
                }
            } else if (now - p->last_hello_tx > 100000) send_hello(s, p, now);
        } else if (!p->hears_us && !vague){
            if (s->hub_was == 2 && !p->hears_miss){ p->hears_miss = 1; send_hello(s, p, now); }
            else { p->state = RC_PS_HEARD; s->peers_dirty = 1; note(s, RC_EV_LEAVE, i, p->name, " left: ", "no longer hears us"); }
        }
        want_cfg(s, p, p->cfg_rev_seen, now);
        break; }
    case RC_MSG_PEERS:
        if (p->state != RC_PS_EST || hub_mode(s, now)) return;   /* with a hub, only its list */
        for (int k = 0; k < m.u.peers.n; k++){
            if (m.u.peers.e[k].id == s->conf.self_id) continue;
            int j = find(s, m.u.peers.e[k].ip, m.u.peers.e[k].port);
            if (j < 0 && !id_elsewhere(s, m.u.peers.e[k].id, -1)) rc_add_peer(s, m.u.peers.e[k].ip, m.u.peers.e[k].port, 0, now);
        }
        break;
    case RC_MSG_SNAP:
        if (p->state != RC_PS_EST) return;
        p->rx_snap++; p->last_snap_rx = now; p->status = m.u.snap.status; p->cfg_rev_seen = m.u.snap.cfg_rev;
        if (p->rx_snap == 1 && p->said_nosnap){
            char t[64]; snprintf(t, sizeof t, ": linked, first snapshot %d s after the hub listed it", (int)((now - p->hub_first_us) / 1000000));
            note(s, RC_EV_NOTE, i, p->name, t, "");
        }
        if (p->seq_init){
            uint16_t d = (uint16_t)(m.h.seq - p->last_seq), every = (uint16_t)(m.h.flags & RC_HF_EVERY);
            if (every < 1) every = 1;
            if (d > every && d < 0x8000u) p->seq_lost += d - every;
            if (d && d < 0x8000u) p->last_seq = m.h.seq;
        }
        else { p->seq_init = 1; p->last_seq = m.h.seq; }
        for (int k = m.u.snap.n - 1; k >= 0; k--)
            rc_interp_insert(&p->mo, &m.u.snap.s[k], m.u.snap.epoch, m.u.snap.clock, m.u.snap.status, now, k == 0);
        want_cfg(s, p, m.u.snap.cfg_rev, now);
        break;
    case RC_MSG_CFG:
        if (p->state != RC_PS_EST) return;
        if (p->cfg_len && p->cfg_rev == m.u.cfg.cfg_rev) return;
        if (p->asm_rev != m.u.cfg.cfg_rev || p->asm_nfrag != m.u.cfg.nfrag || p->asm_total != m.u.cfg.total){
            p->asm_rev = m.u.cfg.cfg_rev; p->asm_nfrag = m.u.cfg.nfrag; p->asm_total = m.u.cfg.total; p->asm_mask = 0; }
        memcpy(p->asm_buf + (uint32_t)m.u.cfg.frag * RC_CFG_FRAG, m.u.cfg.data, m.u.cfg.dlen);
        p->asm_mask |= (uint8_t)(1u << m.u.cfg.frag);
        if (p->asm_mask == (uint8_t)((1u << p->asm_nfrag) - 1u)){
            /* the record is the game's, opaque here */
            memcpy(p->cfg, p->asm_buf, p->asm_total); p->cfg_len = p->asm_total; p->cfg_rev = p->asm_rev; p->cfg_gen++;
            note(s, RC_EV_CFG, i, p->name, "'s car record came", "");
            p->asm_mask = 0; p->asm_rev = 0;
        }
        break;
    case RC_MSG_CFG_REQ:
        if (p->state != RC_PS_EST) return;
        answer_cfg(s, p, m.u.cfgreq.cfg_rev == s->cfg_rev ? m.u.cfgreq.have_mask : 0, now);
        break;
    case RC_MSG_PING: {
        if (p->state != RC_PS_EST) return;
        rc_msg r; hdr(s, &r, RC_MSG_PONG);
        r.u.ping.t0 = m.u.ping.t0; r.u.ping.t1 = (uint32_t)now; r.u.ping.t2 = (uint32_t)now;
        tx(s, p, &r);
        break; }
    case RC_MSG_PONG:
        p->rtt_us = (int32_t)((uint32_t)now - m.u.ping.t0 - (m.u.ping.t2 - m.u.ping.t1));
        break;
    case RC_MSG_BYE: {
        int k = 0;
        if (s->nbye < 4) k = s->nbye++;
        else for (int j = 1; j < 4; j++) if (s->bye[j].at < s->bye[k].at) k = j;
        s->bye[k].id = m.h.sender; s->bye[k].at = now;
        drop_peer(s, i, m.u.bye.reason == 1 ? "switched off" : "quit");
        break; }
    case RC_MSG_GAME:
        if (p->state != RC_PS_EST) return;
        if (s->ninbox == RC_INBOX){ memmove(&s->inbox[0], &s->inbox[1], sizeof s->inbox[0] * (RC_INBOX - 1)); s->ninbox--; s->rx_game_drop++; }
        s->inbox[s->ninbox].from = m.h.sender; s->inbox[s->ninbox].len = m.u.game.len;
        memcpy(s->inbox[s->ninbox].d, m.u.game.data, m.u.game.len); s->ninbox++;
        break;
    }
}
int rc_send_game(rc_session *s, uint32_t id, const uint8_t *data, int len){
    if (!id || !data || len < 1 || len > RC_GAME_MAX) return -1;
    for (int i = 0; i < RC_TABLE; i++){
        rc_peer *p = &s->peer[i];
        if (p->state != RC_PS_EST || p->id != id) continue;
        rc_msg m; hdr(s, &m, RC_MSG_GAME);
        m.u.game.len = (uint8_t)len; memcpy(m.u.game.data, data, (size_t)len);
        tx(s, p, &m);
        return 0;
    }
    return -1;
}
int rc_recv_game(rc_session *s, uint32_t *from, uint8_t *out, int cap){
    if (!s->ninbox) return 0;
    int n = s->inbox[0].len < cap ? s->inbox[0].len : cap;
    if (n > 0) memcpy(out, s->inbox[0].d, (size_t)n);
    if (from) *from = s->inbox[0].from;
    memmove(&s->inbox[0], &s->inbox[1], sizeof s->inbox[0] * (RC_INBOX - 1)); s->ninbox--;
    return n > 0 ? n : 0;
}
void rc_tick(rc_session *s, int64_t now){
    if (s->closed) return;
    int hub = hub_mode(s, now);
    if (s->conf.hub_ip){
        if (!s->hub_since) s->hub_since = now ? now : 1;
        if (now - s->hub_hello_tx >= T_HUB_HELLO){ rc_msg m; fill_hello(s, &m, 0); tx_to(s, s->conf.hub_ip, s->conf.hub_port, &m); s->hub_hello_tx = now; }
        /* out of the world (a garage, a load) the game sends no states: our last one to the hub now and then, so its
         * position stays younger than the hub's stale_ms and our links (and the peers' slots for us) hold */
        if (s->status != RC_ST_ROAM && s->nlast && now - s->hub_snap_tx >= 4 * T_HUB_SNAP){
            rc_msg m; hdr(s, &m, RC_MSG_SNAP);
            m.u.snap.status = s->status; m.u.snap.epoch = s->epoch; m.u.snap.clock = s->clock; m.u.snap.cfg_rev = s->cfg_rev;
            m.u.snap.n = 1; m.u.snap.s[0] = s->last[0];
            tx_to(s, s->conf.hub_ip, s->conf.hub_port, &m);
            /* linked peers too: else they mark us lost after 3 s and our map blip (and slot) vanish */
            for (int i = 0; i < RC_TABLE; i++) if (s->peer[i].state == RC_PS_EST) tx(s, &s->peer[i], &m);
            s->hub_snap_tx = now;
        }
        int st = !hub ? 0 : s->hub_heard_us ? 2 : 1;
        if (st != s->hub_was && st != 1)
            note(s, RC_EV_NOTE, -1, st == 2 ? "the server's free-roam hub answers" : "no free-roam hub answers", "", "");
        s->hub_was = st;
    }
    if (s->conf.discovery && !hub && now - s->last_beacon >= T_BEACON && s->io.broadcast){
        rc_msg m; fill_hello(s, &m, 0);
        uint8_t b[RC_MAX_DGRAM]; int n = rc_encode(&m, b, sizeof b);
        if (n > 0) s->io.broadcast(s->io.user, s->conf.port, b, n);
        s->last_beacon = now;
    }
    for (int i = 0; i < RC_TABLE; i++){
        rc_peer *p = &s->peer[i];
        if (p->state == RC_PS_FREE) continue;
        if (p->last_rx_us && now - p->last_rx_us > T_DROP){
            /* rate-limited datagrams also leave last_rx_us behind: tell a flood from silence */
            char why[64];
            snprintf(why, sizeof why, "timeout (%u datagrams dropped by the rate limit)", (unsigned)p->rx_drop);
            drop_peer(s, i, p->rx_drop ? why : "timeout"); continue; }
        if (hub && s->hub_heard_us && !p->configured && s->hub_heard_us - p->hub_listed_us > T_HUB_GRACE){
            int live = p->state == RC_PS_EST && p->last_rx_us && now - p->last_rx_us <= T_LOST;
            if (!live || s->hub_heard_us - p->hub_listed_us > T_HUB_GRACE_LIVE){ drop_peer(s, i, "out of range"); continue; }
        }
        /* the hub lists it, its snapshots do not come: which step is missing (state 1 nothing heard, 2 heard but it
         * does not hear us, 3 linked; hello/snap: its datagrams in) */
        if (hub && p->hub_first_us && !p->rx_snap && !p->said_nosnap && now - p->hub_first_us > T_NOSNAP){
            char t[120];
            snprintf(t, sizeof t, "%08x: listed by the hub %d s, no snapshot (state %d, %s, datagrams in %u)",
                     (unsigned)p->id, (int)((now - p->hub_first_us) / 1000000), p->state,
                     p->hears_us ? "hears us" : "does not hear us", (unsigned)p->rx);
            note(s, RC_EV_NOTE, i, p->name[0] ? p->name : "peer ", p->name[0] ? " " : "", t);
            p->said_nosnap = 1;
        }
        if (!p->last_rx_us && !p->configured && now - p->added_us > T_GOSSIP_FORGET){ peer_clear(p, 0); continue; }
        if (p->state == RC_PS_EST && now - p->last_rx_us > T_LOST && !p->lost){ p->lost = 1; note(s, RC_EV_NOTE, i, p->name, ": no packets for 3 s", ""); }
        /* a far peer's HELLOs and pings as much rarer as its SNAPs */
        int slow = p->state == RC_PS_EST && p->far > 1 && now - p->last_snap_tx < T_FAR_FED ? p->far : 1;
        int64_t every = p->state == RC_PS_EST ? T_HELLO_EST * slow : T_HELLO_CAND;
        if (now - p->last_hello_tx >= every) send_hello(s, p, now);
        if (p->state == RC_PS_EST && now - p->last_ping_tx >= T_PING * slow){
            rc_msg m; hdr(s, &m, RC_MSG_PING); m.u.ping.t0 = (uint32_t)now; tx(s, p, &m); p->last_ping_tx = now; }
        if (p->state == RC_PS_EST) want_cfg(s, p, p->cfg_rev_seen, now);
    }
    if (!hub && (s->peers_dirty || now - s->last_peers_tx >= T_PEERS)){
        /* gossip (no hub): our established peers to each of them */
        rc_msg m; hdr(s, &m, RC_MSG_PEERS);
        int n = 0;
        for (int i = 0; i < RC_TABLE && n < RC_MAX_PEERS; i++){
            const rc_peer *p = &s->peer[i];
            if (p->state != RC_PS_EST) continue;
            m.u.peers.e[n].ip = p->ip; m.u.peers.e[n].port = p->port; m.u.peers.e[n].id = p->id; n++;
        }
        m.u.peers.n = (uint8_t)n;
        for (int i = 0; i < RC_TABLE && n; i++) if (s->peer[i].state == RC_PS_EST) tx(s, &s->peer[i], &m);
        s->peers_dirty = 0; s->last_peers_tx = now;
    }
}
void rc_set_status(rc_session *s, uint8_t st){
    if (st >= RC_ST__N || st == s->status) return;
    s->status = st;
    for (int i = 0; i < RC_TABLE; i++) if (s->peer[i].state == RC_PS_EST){ rc_msg m; fill_hello(s, &m, &s->peer[i]); tx(s, &s->peer[i], &m); }
    if (s->conf.hub_ip){ rc_msg m; fill_hello(s, &m, 0); tx_to(s, s->conf.hub_ip, s->conf.hub_port, &m); }
}
void rc_set_links(rc_session *s, int links){
    s->conf.links = links < 0 ? 0 : links > RC_MAX_PEERS ? RC_MAX_PEERS : links;
    s->hub_hello_tx = INT64_MIN / 4;                            /* the hub hears it at the next tick */
}
/* Another room (MC3: the city): each peer gets our HELLO with it and drops us, we drop every peer and the far list;
 * the hub hears it at the next tick and links us within the new room only. */
void rc_set_room(rc_session *s, uint32_t room){
    if (room == s->conf.room) return;
    s->conf.room = room;
    for (int i = 0; i < RC_TABLE; i++){
        if (s->peer[i].state == RC_PS_FREE) continue;
        if (s->peer[i].state == RC_PS_EST){ rc_msg m; fill_hello(s, &m, &s->peer[i]); tx(s, &s->peer[i], &m); }
        drop_peer(s, i, "other room");
    }
    s->nfar = 0; s->far_got = 0;
    s->hub_hello_tx = INT64_MIN / 4;
}
void rc_bump_epoch(rc_session *s){ s->epoch++; s->nlast = 0; s->hub_snap_tx = INT64_MIN / 4; }
void rc_bump_clock(rc_session *s){ s->clock++; s->nlast = 0; }
void rc_set_cfg(rc_session *s, const uint8_t *rec, int len, int64_t now){
    (void)now;
    if (len <= 0 || len > RC_CFG_MAX) return;
    if (len == s->cfg_len && !memcmp(rec, s->cfg, (size_t)len)) return;
    memcpy(s->cfg, rec, (size_t)len); s->cfg_len = len;
    s->cfg_rev++; if (!s->cfg_rev) s->cfg_rev = 1;
    for (int i = 0; i < RC_TABLE; i++) if (s->peer[i].state == RC_PS_EST) send_cfg(s, &s->peer[i], 0);
}
/* every how many snapshots peer p gets one: its newest state's distance from ours (3D) */
static int snap_every(const rc_peer *p, const rc_state *st){
    if (!p->mo.n) return 1;
    const float *q = p->mo.s[p->mo.n - 1].p;
    float dx = q[0] - st->p[0], dy = q[1] - st->p[1], dz = q[2] - st->p[2], d2 = dx * dx + dy * dy + dz * dz;
    return d2 > RC_VERYFAR_M * RC_VERYFAR_M ? 10 : d2 > RC_FAR_M * RC_FAR_M ? 4 : 1;
}
#define SNAP_MAX (RC_HDR + 6 + RC_SNAP_STATES * (RC_STATE_BYTES + RC_STATEX_BYTES) + RC_CRC)
int rc_push_state(rc_session *s, const rc_state *st, int64_t now){
    int64_t iv = (int32_t)1000000 / (int32_t)s->conf.rate_hz;
    if (s->closed || now < s->next_snap_us) return 0;
    s->next_snap_us = (now - s->next_snap_us > iv) ? now + iv : s->next_snap_us + iv;
    if (!(s->nlast && s->last[0].t_us == st->t_us && s->status == RC_ST_PAUSED)){
        memmove(&s->last[1], &s->last[0], sizeof s->last[0] * (RC_SNAP_STATES - 1));
        s->last[0] = *st; if (s->nlast < RC_SNAP_STATES) s->nlast++;
    }
    int to_hub = s->conf.hub_ip && now - s->hub_snap_tx >= T_HUB_SNAP;
    if (!rc_established(s) && !to_hub) return 1;
    s->seq++;
    rc_msg m; hdr(s, &m, RC_MSG_SNAP);
    m.u.snap.status = s->status; m.u.snap.epoch = s->epoch; m.u.snap.clock = s->clock; m.u.snap.cfg_rev = s->cfg_rev;
    m.u.snap.n = (uint8_t)(s->nlast < s->conf.snap_states ? s->nlast : s->conf.snap_states);
    for (int k = 0; k < m.u.snap.n; k++) m.u.snap.s[k] = s->last[k];
    uint8_t b[RC_MAX_DGRAM]; int n = rc_encode(&m, b, sizeof b);
    if (n <= 0) return 0;
    /* variants made when one needs them: bv[0] near and extended, bv[1 + 2x + j] far (x: extended, j: every 10th) */
    int ext = 0;
    for (int k = 0; k < m.u.snap.n; k++) ext |= m.u.snap.s[k].ext_len != 0;
    uint8_t bv[5][SNAP_MAX]; int nv[5] = { 0, 0, 0, 0, 0 };
    for (int i = 0; i < RC_TABLE; i++){
        rc_peer *p = &s->peer[i];
        if (p->state != RC_PS_EST || !s->io.send) continue;
        int e = snap_every(p, &s->last[0]), x = ext && p->statex;
        if (e == 1 && p->far > 1) p->last_ping_tx = INT64_MIN / 4;   /* come near: its round trip now */
        p->far = (uint8_t)e;
        if (e > 1 && (uint16_t)(s->seq + i) % (uint16_t)e) continue;
        const uint8_t *d = b; int dn = n;
        if (e > 1 || x){
            int v = e == 1 ? 0 : 1 + 2 * x + (e == 4 ? 0 : 1);
            if (!nv[v]){
                m.h.flags = (uint16_t)((e > 1 ? e : 0) | (x ? RC_HF_STATEX : 0));
                nv[v] = rc_encode(&m, bv[v], SNAP_MAX); m.h.flags = 0;
            }
            if (nv[v] <= 0) continue;
            d = bv[v]; dn = nv[v];
        }
        s->io.send(s->io.user, p->ip, p->port, d, dn); s->tx++; s->tx_bytes += (uint32_t)dn;
        p->last_snap_tx = now;
    }
    if (to_hub && s->io.send){
        s->io.send(s->io.user, s->conf.hub_ip, s->conf.hub_port, b, n); s->tx++; s->tx_bytes += (uint32_t)n;
        s->hub_snap_tx = now;
    }
    return 2;
}
void rc_shutdown(rc_session *s, uint8_t reason){
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < RC_TABLE; i++){
            rc_peer *p = &s->peer[i];
            if (p->state != RC_PS_EST && p->state != RC_PS_HEARD) continue;
            rc_msg m; hdr(s, &m, RC_MSG_BYE); m.u.bye.reason = reason; tx(s, p, &m);
        }
    if (s->conf.hub_ip){ rc_msg m; hdr(s, &m, RC_MSG_BYE); m.u.bye.reason = reason; tx_to(s, s->conf.hub_ip, s->conf.hub_port, &m); }
    s->closed = 1;
}
