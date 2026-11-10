/* roam_api.h -- free-roam multiplayer: the C API a game layer calls.
 *
 * Ported from nfsu2-web's free roam (runtime/roam_core.*, roam_plat.h,
 * runtime/web/roam_web.c; MIT License, Copyright (c) 2026 th0mk), without
 * its NFSU2 game layer.  The core (roam_core.h) is game-agnostic: a car is a
 * rc_state (position, velocity, orientation, controls) plus an opaque
 * extension the game defines; its look is an opaque record (MC3: the 264-byte
 * CarCfg).  The server's free-roam hub (web/server/crates/roam) links each
 * player with its K nearest (default ROAM_K_DEFAULT); snapshots then go
 * player to player at 20 Hz over the virtual LAN (UDP port ROAM_PORT on the
 * player's 10.77.x.x address).
 *
 * Threading: every function here takes the roam lock itself; call them from
 * the game thread.  A network thread (web: roam_web.c) receives and ticks.
 * Native builds have no platform yet: roam_start returns -1 there and the
 * rest are no-ops (roam_null.c).
 *
 * Coordinates: metres, any right-handed world frame the game uses
 * consistently (every axis within +-20 km); q is the body -> world rotation
 * (x y z w).  Times: microseconds.
 */
#ifndef ROAM_API_H
#define ROAM_API_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROAM_PORT        47210      /* UDP port of the roam endpoint (and of the hub) */
#define ROAM_HUB_VIP     0x0A4D00FDu /* 10.77.0.253: the server's free-roam hub */
#define ROAM_K_DEFAULT   7          /* players linked by the hub (the nearest K) */
#define ROAM_K_MAX       64
#define ROAM_NAME_MAX    24
#define ROAM_CFG_MAX     2048       /* the car-look record at most (MC3 CarCfg: 264) */
#define ROAM_EXT_MAX     29         /* a state's game-defined extension at most, bytes */
#define ROAM_MSG_MAX     96         /* a game message (roam_send_msg) at most, bytes */

/* car state bits (rc_state.bits); the game may define its own in the rest */
#define RC_BIT_BRAKE     0x01
#define RC_BIT_REVERSE   0x02
#define RC_BIT_NOS       0x04
#define RC_BIT_HANDBRAKE 0x08
#define RC_BIT_DARK      0x10
#define RC_BIT_HORN      0x20

/* player status (roam_set_status) */
enum { RC_ST_MENUS = 0, RC_ST_LOADING = 1, RC_ST_ROAM = 2, RC_ST_EVENT = 3, RC_ST_SHOP = 4, RC_ST_PAUSED = 5, RC_ST__N };

/* One car state (52 bytes on the wire, then the extension when ext_len > 0). */
typedef struct rc_state {
    uint32_t t_us;                 /* sender's clock (us, wraps); roam_set_local stamps it when 0 */
    float    p[3], v[3];           /* m, m/s */
    float    q[4];                 /* body -> world, x y z w */
    float    w[3];                 /* angular velocity, rad/s (world) */
    float    steer;                /* -1 .. 1 */
    float    throttle, brake;      /* 0 .. 1 */
    float    rpm, speed;           /* rpm (0 .. 65535), m/s forward */
    int8_t   gear;
    uint8_t  bits;                 /* RC_BIT_* */
    uint8_t  ext_len;              /* 0: no extension */
    uint8_t  ext[ROAM_EXT_MAX];    /* game-defined, opaque to the core; the newest state's is shown */
} rc_state;

/* A remote player as roam_peers gives it: interpolated for this frame. */
enum { ROAM_MODE_NONE = 0, ROAM_MODE_INTERP = 1, ROAM_MODE_EXTRAP = 2, ROAM_MODE_HOLD = 3 };
typedef struct roam_peer {
    uint32_t id;                   /* the player's session id (nonzero, random per start) */
    char     name[ROAM_NAME_MAX + 1];
    uint8_t  status;               /* RC_ST_* */
    int      mode;                 /* ROAM_MODE_* */
    float    dist;                 /* m from our last local state */
    float    p[3], q[4], v[3], w[3];
    float    steer, throttle, brake, rpm, speed;
    int8_t   gear;
    uint8_t  bits;
    uint8_t  ext_len;
    uint8_t  ext[ROAM_EXT_MAX];
    uint32_t cfg_gen;              /* changes with each new car-look record (roam_peer_cfg); 0: none yet */
} roam_peer;

/* A player beyond our links, as the hub's FAR list gives it (every ~1 s): a
 * map marker only, no car.  Place and orientation as of its last SNAP to the hub. */
#define ROAM_FAR_MAX     256        /* players of a FAR list (a city) kept at most */
typedef struct roam_far_peer {
    uint32_t id;
    char     name[ROAM_NAME_MAX + 1];
    uint8_t  status;               /* RC_ST_* */
    float    dist;                 /* m from our last local state */
    float    p[3], q[4];
} roam_far_peer;

/* Starts free roam as `name` (printable ASCII, cut to ROAM_NAME_MAX).  vip:
 * our virtual address (host order), 0 = the one the page's switch link holds
 * (web).  0 on success, -1 when there is no platform, no link or it runs. */
int  roam_start(const char *name, uint32_t vip);
void roam_stop(void);                       /* BYE to every peer, network thread joined */
int  roam_running(void);
/* the players the hub links us with (1 .. ROAM_K_MAX); default ROAM_K_DEFAULT,
 * MC3_ROAM_K overrides; the hub hears it with our next HELLO */
void roam_set_k(int k);
/* our room (MC3: the city index; 0, the default, is the one room older clients
 * are in): the hub links, and peers accept, players of the same room only.
 * Kept across roam_stop/roam_start; a change drops every peer of the old room. */
void roam_set_room(uint32_t room);
void roam_set_status(uint8_t status);       /* RC_ST_* */
/* our car's state, every frame (a SNAP goes out at 20 Hz when due) */
void roam_set_local(const rc_state *s);
void roam_teleport(void);                   /* our car jumped: receivers snap instead of blending */
/* the remote players to draw this frame: the established peers, nearest first,
 * at most max, interpolated at now minus the render delay (MC3_ROAM_DELAY ms,
 * default 100); the count */
int  roam_peers(roam_peer *out, int max);
/* our car-look record (opaque; MC3: CarCfg, 264 bytes); a change goes to every peer */
void roam_set_cfg(const void *blob, int len);
/* the peer's car-look record: bytes copied (<= cap), 0 none yet, -1 no such peer */
int  roam_peer_cfg(uint32_t id, void *out, int cap);
/* the players the hub knows beyond our links (none drawn as a car by roam_peers),
 * nearest first, at most max; 0 when the hub's last FAR list is older than 6 s */
int  roam_far(roam_far_peer *out, int max);
/* A game-defined message (1 .. ROAM_MSG_MAX bytes, opaque to the core) to one
 * linked peer, straight (not through the hub), unreliable like a SNAP: resend
 * what matters.  0 sent, -1 not running, no such linked peer or a bad length.
 * A peer with an older core drops it. */
int  roam_send_msg(uint32_t to, const void *data, int len);
/* the oldest game message received (at most 8 wait; the oldest goes): its
 * length (copied up to cap), *from its sender's id; 0 none */
int  roam_recv_msg(uint32_t *from, void *out, int cap);

#ifdef __cplusplus
}
#endif
#endif
