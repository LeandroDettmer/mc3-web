/* roam_core.h -- free-roam multiplayer: the portable core.
 *
 * Ported from nfsu2-web (runtime/roam_core.h, MIT License, Copyright (c)
 * 2026 th0mk), cut down to what any game needs: its NFSU2 layer (duels,
 * race lobbies, quick chat, crews, world map, flags, one-way links, the
 * NFSU2 state extension and car record) is left out.  Its wire format is
 * kept for the messages that stay (magic "MC3R" in place of "N2FR"):
 *
 *   header (16 bytes, little-endian): magic[4] "MC3R", version u8 (1),
 *   type u8, length u16 (the whole datagram), sender u32 (random id per
 *   start, nonzero), seq u16 (snapshot sequence), flags u16; the payload;
 *   CRC-32 (IEEE, of everything before it) u32.  At most RC_MAX_DGRAM bytes,
 *   UDP port RC_DEFAULT_PORT.
 *
 *   HELLO   core_ver u16, platform u8, status u8, listen_port u16,
 *           cfg_rev u16, room u32, name (len u8 + bytes), n_seen u8 + ids
 *           u32; with RC_HF_CREW: n_crews u8 (0 here) [+ crews], links u8
 *           (to the hub: the players it should link us with)
 *   PEERS   n u8, then n x { ip (4 bytes, big-endian), port u16, id u32 }
 *           (a hub's crew marks after them are skipped)
 *   SNAP    status u8, epoch u8, cfg_rev u16, n u8, clock u8, n states
 *           (newest first): 52 bytes each (rc_state_put), with RC_HF_STATEX
 *           each followed by its extension (length u8 + that many bytes)
 *   CFG     cfg_rev u16, frag u8, nfrag u8, total u16, dlen u16, bytes
 *   CFG_REQ cfg_rev u16, have_mask u8
 *   PING    t0 u32;  PONG t0, t1, t2 u32;  BYE reason u8
 *   FAR     (hub -> player, MC3 only) n u8, then n x { id u32, status u8,
 *           p f32 x3, q i16 x4 (as in a SNAP), name (len u8 + bytes) }: the
 *           players beyond our links, nearest first; a core without it drops
 *           it as RC_E_TYPE
 *
 * Portable C99: no OS headers, no libm, no threads, no allocation.  Floats
 * use + - * / and sqrt only.  Every function that takes a session must be
 * called with the caller's session lock held.  Times are microseconds of a
 * monotonic clock.
 */
#ifndef ROAM_CORE_H
#define ROAM_CORE_H
#include <stdint.h>
#include <stddef.h>
#include "roam_api.h"

#define RC_PROTO_VER     1
#define RC_CORE_VER      16
#define RC_MAGIC0        0x4D       /* "MC3R" */
#define RC_MAGIC1        0x43
#define RC_MAGIC2        0x33
#define RC_MAGIC3        0x52
#define RC_DEFAULT_PORT  ROAM_PORT
#define RC_MAX_DGRAM     1200       /* never IP-fragmented (< 1280) */
#define RC_HDR           16
#define RC_CRC           4
#define RC_NAME_MAX      ROAM_NAME_MAX
#define RC_MAX_PLAYERS   65         /* this machine + 64 peers */
#define RC_MAX_PEERS     (RC_MAX_PLAYERS - 1)
#define RC_PEERS_V1      7          /* ids a HELLO to everyone names at most */
#define RC_LIST_MAX      96         /* entries of a PEERS list at most */
#define RC_TABLE         128        /* known addresses */
#define RC_SNAP_STATES   3          /* states per SNAP at most (newest first) */
#define RC_STATE_BYTES   52
#define RC_STATEX_BYTES  (1 + ROAM_EXT_MAX)
#define RC_BUF_STATES    32         /* jitter buffer per peer */
#define RC_OFFWIN        64         /* clock-offset samples per peer */
#define RC_CFG_MAX       ROAM_CFG_MAX
#define RC_CFG_FRAG      1000       /* bytes per CFG fragment */
#define RC_CFG_MAXFRAG   3

enum { RC_MSG_HELLO = 1, RC_MSG_PEERS = 2, RC_MSG_SNAP = 3, RC_MSG_CFG = 4, RC_MSG_CFG_REQ = 5,
       RC_MSG_PING = 6, RC_MSG_PONG = 7, RC_MSG_BYE = 8, RC_MSG_FAR = 9 };
enum { RC_PLAT_WEB = 4 };
/* header flags: SNAP every n-th (far peers), HELLO to everyone, state extensions, crews (here: the links byte) */
#define RC_HF_EVERY      0x000F
#define RC_HF_ALL        0x0020
#define RC_HF_STATEX     0x0040
#define RC_HF_CREW       0x0200
#define RC_FAR_M         300.f      /* past this a peer gets every 4th snapshot */
#define RC_VERYFAR_M     1000.f     /* and past this every 10th */
enum { RC_OK = 0, RC_E_SHORT = -1, RC_E_MAGIC = -2, RC_E_VERSION = -3, RC_E_LENGTH = -4,
       RC_E_CRC = -5, RC_E_TYPE = -6, RC_E_FIELD = -7, RC_E_TRAIL = -8, RC_E_RANGE = -9 };

/* ---- byte helpers ---- */
typedef struct { uint8_t *p; uint32_t n, cap; int err; } rc_w;
typedef struct { const uint8_t *p; uint32_t n, len; int err; } rc_r;
void     rc_w_init(rc_w *w, uint8_t *buf, uint32_t cap);
void     rc_put8(rc_w *w, uint32_t v);
void     rc_put16(rc_w *w, uint32_t v);
void     rc_put32(rc_w *w, uint32_t v);
void     rc_putf(rc_w *w, float f);
void     rc_putb(rc_w *w, const void *src, uint32_t n);
void     rc_r_init(rc_r *r, const uint8_t *buf, uint32_t len);
uint32_t rc_get8(rc_r *r);
uint32_t rc_get16(rc_r *r);
uint32_t rc_get32(rc_r *r);
float    rc_getf(rc_r *r);
void     rc_getb(rc_r *r, void *dst, uint32_t n);
uint32_t rc_crc32(const uint8_t *p, uint32_t n);
uint32_t rc_fnv1a(const char *s);           /* 0 for "" */
uint32_t rc_fbits(float f);
float    rc_bitsf(uint32_t u);
float    rc_sqrtf(float x);
/* printable ASCII, at most RC_NAME_MAX bytes, never empty ("Player") */
void     rc_name_clean(char *dst, const char *src, uint32_t srclen);

/* ---- messages ---- */
void rc_state_put(rc_w *w, const rc_state *s, int ext);
int  rc_state_get(rc_r *r, rc_state *s, int ext);

typedef struct { uint8_t ver, type; uint16_t len; uint32_t sender; uint16_t seq, flags; } rc_hdr;
typedef struct {
    rc_hdr h;
    union {
        struct { uint16_t core_ver; uint8_t platform, status; uint16_t listen_port, cfg_rev;
                 uint32_t room; char name[RC_NAME_MAX + 1]; uint8_t n_seen; uint32_t seen[RC_MAX_PEERS];
                 uint8_t links; } hello;
        struct { uint8_t n; struct { uint32_t ip; uint16_t port; uint32_t id; } e[RC_LIST_MAX]; } peers;
        struct { uint8_t status, epoch; uint16_t cfg_rev; uint8_t n, clock; rc_state s[RC_SNAP_STATES]; } snap;
        struct { uint16_t cfg_rev; uint8_t frag, nfrag; uint16_t total, dlen; const uint8_t *data; } cfg;
        struct { uint16_t cfg_rev; uint8_t have_mask; } cfgreq;
        struct { uint32_t t0, t1, t2; } ping;
        struct { uint8_t reason; } bye;
        struct { uint8_t n; roam_far_peer e[ROAM_FAR_MAX]; } far;   /* dist unused on the wire */
    } u;
} rc_msg;
/* one datagram into out (RC_MAX_DGRAM at least): its length, or -1 */
int rc_encode(const rc_msg *m, uint8_t *out, int cap);
/* RC_OK or RC_E_*; on RC_OK every field is in range (finite floats, every
 * axis within +-20 km, |v| < 200 m/s, a normalised quaternion, printable
 * names); m->u.cfg.data points into buf */
int rc_decode(const uint8_t *buf, int len, rc_msg *m);

/* ---- interpolation ---- */
typedef struct {
    float   p[3], q[4], v[3], w[3];
    float   steer, throttle, brake, rpm, speed;
    int8_t  gear;
    uint8_t bits, ext_len, ext[ROAM_EXT_MAX];
    int     mode;                  /* ROAM_MODE_* */
} rc_pose;

typedef struct {
    int      n;
    int64_t  t[RC_BUF_STATES];     /* unwrapped sender time, us */
    rc_state s[RC_BUF_STATES];
    int64_t  t_last;  int t_init;
    uint8_t  epoch;   int epoch_init;   /* teleport epoch: a change snaps */
    uint8_t  clock;   int clock_init;   /* sender clock epoch: a change re-syncs, blended */
    uint8_t  status;
    struct { int64_t at, d; } od[RC_OFFWIN];
    int      odn, odh;
    int64_t  off_r;   int off_init;   /* render offset: host_recv - sender time, slewed */
    int      shown;
    int64_t  prev_rt;
    float    prev_p[3], prev_q[4];
    float    err_p[3], err_q[4];      /* visual correction, decays (tau 100 ms) */
    int      snap_pending;
    uint64_t n_eval, n_interp, n_extrap, n_hold, n_dup, n_old, n_snap, n_epoch, n_stale;
    float    err_max;
} rc_interp;

void rc_interp_reset(rc_interp *ip);
/* a received state; recv_us: host time of arrival (newest = 1 for a SNAP's newest only) */
void rc_interp_insert(rc_interp *ip, const rc_state *s, uint8_t epoch, uint8_t clock, uint8_t status,
                      int64_t recv_us, int newest);
/* the pose for this frame at host time now_us minus delay_us (Hermite between
 * states, straight-line extrapolation up to extrap_cap_us past the newest,
 * then hold; corrections decay); 1 when there is anything to show */
int  rc_interp_eval(rc_interp *ip, int64_t now_us, int64_t frame_dt_us, int64_t delay_us,
                    int64_t extrap_cap_us, rc_pose *out);
int  rc_interp_peek(const rc_interp *ip, int64_t render_t, int64_t extrap_cap_us, rc_pose *out);
float rc_expneg(float x);

/* ---- session ---- */
typedef struct {
    void *user;
    void (*send)(void *user, uint32_t ip, uint16_t port, const uint8_t *buf, int len);   /* ip host order */
    void (*broadcast)(void *user, uint16_t port, const uint8_t *buf, int len);           /* may be 0 */
    void (*event)(void *user, int ev, int peer, const char *text);                       /* may be 0 */
} rc_io;
enum { RC_EV_JOIN = 1, RC_EV_LEAVE = 2, RC_EV_NOTE = 3, RC_EV_CFG = 4, RC_EV_RESTART = 5 };

typedef struct {
    uint32_t self_id;              /* random, nonzero, per start */
    uint16_t port;                 /* our listen port */
    uint32_t room;                 /* 0: the one room */
    char     name[RC_NAME_MAX + 1];
    uint8_t  platform;
    int      discovery;            /* beacon through io.broadcast every 2 s while no hub answers */
    int      max_players;          /* 2 .. RC_MAX_PLAYERS */
    int      rate_hz;              /* snapshots per second (10 .. 60) */
    int      snap_states;          /* states per SNAP (1 .. RC_SNAP_STATES; 0 = all) */
    uint32_t hub_ip;  uint16_t hub_port;   /* the server's hub, or 0 */
    int      links;                /* the players the hub links us with (1 .. RC_MAX_PEERS) */
} rc_config;

enum { RC_PS_FREE = 0, RC_PS_CANDIDATE = 1, RC_PS_HEARD = 2, RC_PS_EST = 3 };
typedef struct {
    int      state, configured;
    uint32_t ip;  uint16_t port;
    uint32_t id;
    int64_t  added_us, last_rx_us, last_hello_tx, last_ping_tx, last_cfgreq_tx, last_snap_rx, last_snap_tx;
    uint8_t  far;                  /* gets every how many of our SNAPs (1, 4, 10) */
    int64_t  cfg_answer_us;
    int64_t  hub_listed_us;
    char     name[RC_NAME_MAX + 1];
    uint8_t  platform, status, statex;
    uint16_t core_ver, cfg_rev_seen;
    int      hears_us, lost, hears_miss;
    int64_t  rl_t0;  int rl_n, rl_bytes;
    uint32_t rx, rx_snap, rx_drop, seq_lost;  uint16_t last_seq;  int seq_init;
    int32_t  rtt_us;
    uint16_t cfg_rev;  int cfg_len;  uint8_t cfg[RC_CFG_MAX];  uint32_t cfg_gen;
    uint16_t asm_rev, asm_total;  uint8_t asm_mask, asm_nfrag;  uint8_t asm_buf[RC_CFG_MAX];
    rc_interp mo;
} rc_peer;

typedef struct {
    rc_config conf;
    rc_io     io;
    rc_peer   peer[RC_TABLE];
    uint16_t  seq;
    uint8_t   status, epoch, clock;
    uint16_t  cfg_rev;  int cfg_len;  uint8_t cfg[RC_CFG_MAX];
    rc_state  last[RC_SNAP_STATES];  int nlast;
    int64_t   next_snap_us, last_beacon, last_peers_tx;  int peers_dirty;
    uint32_t  tx, tx_bytes, rx, rx_bad, rx_policy, rx_ratelimit, rx_unknown_full, rx_version, rx_room, rx_dup_id, rx_stray;
    int       closed;
    struct { uint32_t id; int64_t at; } bye[4];  int nbye;
    uint32_t  rx_after_bye;
    int64_t   hub_since, hub_heard_us, hub_hello_tx, hub_snap_tx;  int hub_was;
    uint32_t  hub_lists, rx_unlisted;
    roam_far_peer far[ROAM_FAR_MAX];  int nfar;  int64_t far_us;   /* the hub's last FAR list, when */
} rc_session;

void rc_session_init(rc_session *s, const rc_config *cfg, const rc_io *io);
int  rc_add_peer(rc_session *s, uint32_t ip, uint16_t port, int configured, int64_t now_us);
int  rc_addr_private(uint32_t ip);
void rc_receive(rc_session *s, uint32_t ip, uint16_t port, const uint8_t *buf, int len, int64_t now_us);
void rc_tick(rc_session *s, int64_t now_us);
void rc_set_status(rc_session *s, uint8_t status);
void rc_set_links(rc_session *s, int links);
void rc_set_room(rc_session *s, uint32_t room);   /* conf.room; the old room's peers dropped */
void rc_set_cfg(rc_session *s, const uint8_t *rec, int len, int64_t now_us);
int  rc_push_state(rc_session *s, const rc_state *st, int64_t now_us);
void rc_bump_epoch(rc_session *s);
void rc_bump_clock(rc_session *s);
void rc_shutdown(rc_session *s, uint8_t reason);
int  rc_established(const rc_session *s);
const char *rc_status_name(int st);

#endif
