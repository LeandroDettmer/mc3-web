/* Free roam (O3): remote players as cars in the Online cruise.
 *
 * Included by recomp_manual.c after the Online (O1) section (it uses
 * s_online and online_ram); the overrides stay in recomp_manual.c, where the
 * lifter looks for them. The network is O2's free roam
 * (xboxrecomp/src/net/roam/roam_api.h): this file starts it, hands it our
 * car each frame (roam_set_local) and takes the nearest peers back
 * (roam_peers). Native builds link its stub (roam_start fails, no peers).
 *
 * The game's own 8 player slots carry the cars. The player manager
 * ([0x558814], built by sub_002BE2C0 -> ctor sub_002BE100) makes one
 * player (0x2EC B, ctor sub_002BFE50) per race-setup entry below the count
 * ([0x53F2E0]: +0x4C count, entries at +0x114 + i*0x2C). Entry fields:
 * +0x0C CarCfg*, +0x10 pad (-1 none; sub_00269450), +0x14 viewport (-1 none;
 * sub_0026B270 counts them into +0x50, sub_002BD8A0 gives those a camera),
 * +0x18 controller (-1: no rumble object at player+0x78), +0x26 byte 1 = no
 * car (the garage). Outside a network race (+0x108 = 0) every entry below
 * the count is built, so roam_build() turns the cruise's single entry into
 * 1 + n: entries 1..n copy entry 0 (the same CarCfg, so the car model is
 * already loaded) with pad, viewport and controller -1, as the network race
 * makes its remote entries. They have no camera; the player update
 * (sub_002C1520) still runs for them -- minimap blip and position
 * (player+0x30) included -- and just before it roam_player() drives them
 * as System Link drives a remote player (roam_remote): the peer's pose
 * goes into the player's net block and its inputs (steering, pedals, gear,
 * buttons: 12 bytes, sent as the roam state's extension) into the car's
 * controller, and the game's own remote steps run on them (sub_002C0540
 * pose correction, sub_002C9570 inputs into the car) with the net flag
 * and a stand-in session set for those two calls only. The physics then
 * moves them like any car for the rest of the frame, collisions included,
 * wheels, engine and lights following the inputs. Their cars have that
 * controller only because the car builder's controller step (sub_002CB320)
 * runs for them as in a network race (roam_build_step): outside one, a car
 * without a pad gets none ([veh+4] = 0) and the inputs were dropped (no
 * steering, wheels still, the car dragged along). SetMatrix sub_002A35A0
 * (ecx = [[veh+8]+0x68], a 3x4 matrix: rows right, up, forward, then the
 * position; y up) and SetVel sub_003432B0 (ecx = [that+0x10], velocity at
 * +0x44) only park a slot and snap a car onto a new peer.
 *
 * Cars are built only while the cruise loads: a peer joining later takes a
 * slot built then (the player manager is made once per load, no slot can be
 * added mid-cruise). With free roam running, the peers at load + ROAM_SPARE
 * are built, at most 7 (MC3_ROAM_SLOTS=N: always N): each slot is a whole
 * player car on the game's heap, and 7 of them after a garage visit overran
 * it ('Heap overrun', sub_001B4740). A slot without
 * a peer is parked 30 km off and 500 m under our car with no speed and
 * its drawn flag off (no collisions, no car, no minimap blip). The player object has no name field found: names are
 * not shown.
 *
 * Each player's own car (O4): a car's look is its CarCfg (0x108 B, vtable
 * 0x4483DC at +0, +4 the car (-1 none; sub_0026FE80), plain fields after).
 * Every setup entry owns one on the heap (entry ctor sub_002766C0; the
 * setup reset sub_0026B510 copies the garage's, 0x541700, into each).
 * Ours goes to the peers with the vtable word zeroed (roam_set_cfg): at
 * Free Roam's start (the garage's car, roam_begin from sub_00180160) and at
 * each cruise load (entry 0's: the car the game builds, so a garage visit
 * is sent with the next load). At load, each remote entry keeps its own
 * CarCfg and gets a driving peer's through the game's operator=
 * (sub_00272010, as the network race's MCNET_PLAYER_CAR_CONFIG receive
 * sub_00026370 fills the same entries), our vtable put back; with no peer
 * yet it gets ours.
 *
 * Mid-session car swap (roam_pool.c's queue): when a slot's peer has a CarCfg other
 * than the one the slot was built with (a later join, a new peer on the
 * slot, a garage visit), the slot's car is rebuilt in the running cruise,
 * one slot at a time:
 * 1. no read ahead: the async read (sub_00105CA0, called only by
 *    sub_00145560) opens loose files only and the models are in
 *    assets_x.dat_2 (0xC0000034 in the port). The garage's showroom loads
 *    its car (sub_001ED2F0 -> sub_001FB040) with the builder's own cache
 *    acquire (sub_00105B80, key byte 1; cache [0x50A08C], 16 slots of
 *    0x10 B: +0 car index, +4 data, +8 key byte, +9 refs, +0xA pending,
 *    +0xC stream), which reads "$/resources/vehicle/<n>/<n>" synchronously
 *    out of the packs (sub_00105AA0 -> sub_002126C0, the file system
 *    [0x5348B0] vt+0x24) and pages it in (sub_00105A40 -> sub_00105830); the
 *    car builder in step 2 does the same with key byte 0. A read already
 *    pending is waited for (sub_001A1AD0 on its stream's event). The page-in
 *    relocates through globals ([0x500F10] context stack, [0x50A088] shader
 *    model) and the shader factory [0x5077B8]: mid-cruise that is the
 *    cruise's, and the car's shaders failed ("Invalid type %d in
 *    rmcShaderFactoryStandard::ResourcePageIn", KeBugCheck). The garage
 *    builds a car (sub_0013C8E0) with [0x507668], [0x5077B8], [0x5078D8]
 *    set from the resource context [[0x52B494]] +0x14/+0x18/+0x1C and put
 *    back after; step 2 does the same, with no other guest thread inside
 *    (xbox_gil_atomic), and waits while [0x500F10] shows another page-in
 *    in flight;
 * 2. the old vehicle leaves the world as a System Link peer's
 *    does when it quits (sub_000FFB40: physics level, drawing, vehicle
 *    registry) and is deleted as the player dtor sub_002C8480 does
 *    (vehicle, cache release sub_001057B0, model instance); the entry's
 *    CarCfg gets the peer's (operator=), and the new car is built as the
 *    player ctor sub_002BFE50 does (builder sub_002CB270 on the stack,
 *    sub_002CBC40, whose cache acquire loads the model;
 *    sub_001240F0; builder dtor), then snapped onto the peer's pose.
 * This runs after the physics step (roam_pool_job, sub_002BDEC0's hook),
 * the new car parked and hidden until the slot's next update snaps it.
 * Not redone: the per-class ability object (sub_002BF920, player +0xC8)
 * keeps the slot's first car class. MC3_ROAM_SWAP=0 turns swaps off.
 *
 * Who drives which slot, the swap queue, the heap check and the nearest
 * rule are the remote cars pool's (roam_pool.c), slots and standalone cars
 * alike.
 *
 * Pose correction: the game's (sub_002C0540) with its smoothing settings
 * changed from 0.02*d per frame and a 20 m snap to an exponential decay
 * (tau ROAM_TAU) toward the roam pose, which roam_peers already
 * interpolates ~100 ms in the past, and a ROAM_SNAP m snap.
 *
 * MC3_ROAM_FAKE=N (1-7): N fake remote players replaying our own car's path
 * 1-3 s late, 3 m to the side, without the network. MC3_ROAM_NAME: our name
 * in free roam (default "MC3"). */
#include <math.h>
#include "net/roam/roam_api.h"
#include "kernel/xbox_memory_layout.h"          /* xbox_ContiguousAlloc: the cars heap */

#define ROAM_SETUP_VA   0x0053F2E0u     /* the current race setup */
#define ROAM_ENTRY      0x114u          /* entry 0 */
#define ROAM_ENTRY_SIZE 0x2Cu
#define ROAM_SLOTS      7               /* the 8 player slots, one is ours */
#define ROAM_SPARE      2               /* slots built beyond the peers at load */
#define ROAM_HEAP_VA    0x0052B488u     /* the game's current heap (allocator sub_001B4740) */
#define ROAM_CAR_BYTES  (1000u << 10)   /* a car's share of that heap, rounded up (measured 780-850 KB) */
#define ROAM_HEAP_KEEP  (3072u << 10)   /* left free after the cruise's cars (1259 KB was fatal, 4815 KB played) */
#define ROAM_MAIN_HEAP_VA 0x005684A8u   /* the main heap object (sub_003670F0) */
#define ROAM_CHEAP_MB   48u             /* the cars heap at a 128 MB window (MC3_ROAM_HEAP_MB) */
#define ROAM_CHEAP_LEFT (4u << 20)      /* window bytes left free above it */
#define ROAM_CHEAP_KEEP (2048u << 10)   /* cars heap bytes kept free after one more car */
#define ROAM_HIST       256u            /* our car's path: ~8 s at 30 fps */
#define ROAM_CFG_SIZE   0x108u          /* CarCfg */
#define ROAM_CFG_VT     0x004483DCu     /* its vtable */
#define ROAM_CFG_CAR    4u              /* +4: the car */
#define ROAM_GARAGE_CFG 0x00541700u     /* the garage's car */

#define ROAM_CTL        12u             /* a car's inputs as System Link sends them (controller +0x6C) */
#define ROAM_NET_VA     0x00557EFCu     /* the System Link session object (none in the cruise) */
#define ROAM_NET_VT     0x00449100u     /* its vtable: +0x18 GetPeer(slot) = [[this+0xC] + 4*slot] */

#define ROAM_CACHE_VA   0x0050A08Cu     /* the car resource cache (16 slots of 0x10 B) */
#define ROAM_PAGEIN_VA  0x00500F10u     /* the page-in context stack's top (sub_00105A40 pushes; 0: none) */
#define ROAM_FACTORY_VA 0x0050A088u     /* the model instance the next page-in takes (sub_00105830 clears it) */
#define ROAM_RESCTX_VA  0x0052B494u     /* the resource context the garage builds cars under (sub_0013C8E0) */
#define ROAM_CARTAB_VA  0x00557EB0u     /* the car table, 40 B a car: +8 class (sub_00269CC0: 5/6/7/0xA a bike) */
#define ROAM_CFG_NAME   0xE3u           /* CarCfg +0xE3: the car's name (sub_00269410) */
#define ROAM_HOLD       3.0             /* s a slot stays its peer's after the peer drops out */
#define ROAM_TAU        0.1f            /* s, pose error decay */
#define ROAM_SNAP       8.0f            /* m, snap beyond (horizontal) */

typedef struct roam_pose { double t; float m[12], vel[3]; uint8_t ctl[ROAM_CTL]; } roam_pose;

void sub_002A35A0(void);                        /* SetMatrix, thiscall (const Matrix34 *), ret 4 */
void sub_003432B0(void);                        /* SetVel, thiscall (const Vector3 *), ret 4 */
void sub_0008B970(void);                        /* pack a Matrix34 (quaternion + position), thiscall (const Matrix34 *), ret 4 */
void sub_001B3A80(void);                        /* a peer's net smoothing settings (0x54 B), ctor, thiscall */
void sub_002C0540(void);                        /* a remote player's update (pose correction), thiscall */
void sub_002C9570(void);                        /* a controller's net sync (remote: inputs into the car), thiscall */
void sub_00272010(void);                        /* CarCfg operator=, thiscall (const CarCfg *), ret 4 */
void sub_0026A6A0(void);                        /* car index by name, cdecl (const char *); -1 none */
void sub_00105CA0(void);                        /* car cache: request a car's async read, thiscall, 6 args */
void sub_001A1AD0(void);                        /* event signalled? (wait 0), cdecl (handle); al */
void sub_000FFB40(void);                        /* vehicle out of the world, thiscall */
void sub_001057B0(void);                        /* car cache release, thiscall (data), ret 4 */
void sub_002CB270(void);                        /* car builder ctor, thiscall, 6 args, ret 0x18 */
void sub_002CBC40(void);                        /* car builder: build a race car, thiscall; the vehicle */
void sub_002CB2C0(void);                        /* car builder dtor, thiscall */
void sub_001240F0(void);                        /* a car's profile flags, thiscall */
void sub_000F2C00(void);                        /* car model: preset paint into +0x1228 when +0x1270, thiscall */
void sub_00092EF0(void);                        /* a drawable in/out of the draw list, thiscall (bool), ret 4 */
void sub_00121970(void);                        /* drawn tyres sized, thiscall wheels (CarCfg, rim holder), ret 8 */
void sub_001B4600(void);                        /* heap ctor, thiscall: links it into the heap list [0x52B478] */
void sub_001B3DF0(void);                        /* heap init, thiscall (base, size, free lists, flag), ret 0x10 */
void xbox_gil_atomic(int d);                    /* xboxrecomp kernel_bridge.c: no other guest thread inside */

static int s_roam_fake = -1, s_roam_slots, s_roam_swap_on = -1;
static int s_roam_cruise;                       /* the Free Roam cruise is loaded (slots or not): roam_player runs */
static int s_roam_cfg_loaded;                   /* entry 0's CarCfg sent again once the cruise runs */
/* Online career (MC3_CAREER_ONLINE=1, browser only, read at each cruise load): the career's cruise (game mode 2,
 * race type 1) runs a free-roam session of its own, never s_online (that one skips the career's saves and swaps
 * its money and garage): our car and status go out, the city's players come in as standalone cars (no race-setup
 * slots: s_roam_slots 0, builder slot 1 borrowed, roam_car_build) and map blips. Its races, missions and garage:
 * status EVENT / SHOP, the standalone cars torn down as in Free Roam (roam_pool_pass), ROAM again at the next
 * cruise frame; a load outside the career closes the session. No street or group races (every invite answered NO
 * busy, roam_challenge.c), no AI-race prompts, no online money (all s_online). '[CAREER-ONLINE] ...' lines. */
static int s_career_roam;                       /* the career's session is on (from its first cruise load) */
static int s_career_ev;                         /* EVENT sent at a state request in the career cruise */
static int s_roam_building;                     /* inside roam_car_build (roam_build_step) */
static int roam_career_on(void)
{
#ifdef __EMSCRIPTEN__
    int32_t l = mc3_live(MC3_LIVE_CAREER);      /* the in-game Settings row (roam_career_live), else the env */
    const char *e = getenv("MC3_CAREER_ONLINE");
    if (l)
        return l == 2;
    return e && e[0] == '1';
#else
    return 0;
#endif
}
/* Builder slots a standalone car may name: the remote slots, else in the career cruise entry 1 (count stays 1). */
static int roam_bslots(void)
{
    return s_roam_slots ? s_roam_slots : s_career_roam && s_roam_cruise ? 1 : 0;
}
/* A group race (roam_challenge.c invites, roam_race_ai.c loads it): the players racing the same event, in join order
 * (id[me] = 0, us). While on, its race load gets remote slots for them alone (roam_build), no standalone cars, and
 * the race ends as the game rules (no phase-7 hold). Off at the way back to the cruise. */
#define ROAM_GROUP_MAX 8
static struct { int on, n, me; uint32_t id[ROAM_GROUP_MAX]; } s_roam_group;
static int roam_group_race(void)                /* a group race's load or race is running */
{
    uint32_t setup = MEM32(ROAM_SETUP_VA);
    return s_roam_group.on && online_ram(setup) && MEM32(setup + 0x18u) != 0;
}
static int roam_group_has(uint32_t id)          /* peer id races with us */
{
    int k;
    for (k = 0; id && k < s_roam_group.n; k++)
        if (k != s_roam_group.me && s_roam_group.id[k] == id)
            return 1;
    return 0;
}
static roam_pose s_roam_hist[ROAM_HIST];
static unsigned s_roam_nhist;
static double s_roam_tlast;
static float s_roam_dt = 1.0f / 30.0f;          /* the last frame's length, s */
static const roam_peer *s_roam_seen;            /* roam_net's last roam_peers list (map blips, roam_map.c) */
static int s_roam_nseen;
static double s_roam_seen_t;                    /* when (roam_now) */
static double s_rep_at;                         /* roam_cars.c: when the next slot report is due (0 none) */
static int s_rep_n;                             /* reports left after s_rep_at's */

/* A lifted function called from here: ecx = this, args pushed right to
 * left below top, a 0 return address; eax back, the caller's registers put
 * back. top: below the caller's esp and anything kept there. */
static uint32_t roam_call(recomp_func_t fn, uint32_t ecx, uint32_t top, int n, const uint32_t *args)
{
    uint32_t r[8] = { g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi }, eax;
    if (!fn)
        return 0;
    g_esp = top;
    while (n-- > 0) {
        g_esp -= 4;
        MEM32(g_esp) = args[n];
    }
    g_esp -= 4; MEM32(g_esp) = 0;
    g_ecx = ecx;
    fn();
    eax = g_eax;
    g_eax = r[0]; g_ecx = r[1]; g_edx = r[2]; g_ebx = r[3];
    g_esp = r[4]; g_ebp = r[5]; g_esi = r[6]; g_edi = r[7];
    return eax;
}

static void roam_vdelete(uint32_t obj, uint32_t top)  /* obj->vt[0](1): the deleting destructor */
{
    uint32_t one = 1;
    if (online_ram(obj) && online_ram(MEM32(obj)))
        roam_call(recomp_lookup(MEM32(MEM32(obj))), obj, top, 1, &one);
}

static double roam_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* The game's rows (right, up, forward) are the columns of the body -> world
 * rotation; roam_api's quaternion is x y z w. */
static void roam_mat_quat(const float m[12], float q[4])
{
    float r00 = m[0], r10 = m[1], r20 = m[2], r01 = m[3], r11 = m[4], r21 = m[5];
    float r02 = m[6], r12 = m[7], r22 = m[8], tr = r00 + r11 + r22, s;
    if (tr > 0.0f) {
        s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s; q[0] = (r21 - r12) / s; q[1] = (r02 - r20) / s; q[2] = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        s = sqrtf(1.0f + r00 - r11 - r22) * 2.0f;
        q[3] = (r21 - r12) / s; q[0] = 0.25f * s; q[1] = (r01 + r10) / s; q[2] = (r02 + r20) / s;
    } else if (r11 > r22) {
        s = sqrtf(1.0f + r11 - r00 - r22) * 2.0f;
        q[3] = (r02 - r20) / s; q[0] = (r01 + r10) / s; q[1] = 0.25f * s; q[2] = (r12 + r21) / s;
    } else {
        s = sqrtf(1.0f + r22 - r00 - r11) * 2.0f;
        q[3] = (r10 - r01) / s; q[0] = (r02 + r20) / s; q[1] = (r12 + r21) / s; q[2] = 0.25f * s;
    }
}

static void roam_quat_mat(const float q[4], float m[9])
{
    float x = q[0], y = q[1], z = q[2], w = q[3], n = x * x + y * y + z * z + w * w;
    float k = n > 0.0f ? 2.0f / n : 0.0f;
    m[0] = 1.0f - k * (y * y + z * z); m[1] = k * (x * y + z * w);        m[2] = k * (x * z - y * w);
    m[3] = k * (x * y - z * w);        m[4] = 1.0f - k * (x * x + z * z); m[5] = k * (y * z + x * w);
    m[6] = k * (x * z + y * w);        m[7] = k * (y * z - x * w);        m[8] = 1.0f - k * (x * x + y * y);
}

static int roam_fake(void)                      /* MC3_ROAM_FAKE, read once */
{
    if (s_roam_fake < 0) {
        const char *e = getenv("MC3_ROAM_FAKE");
        s_roam_fake = e ? atoi(e) : 0;
        if (s_roam_fake < 0) s_roam_fake = 0;
        if (s_roam_fake > ROAM_SLOTS) s_roam_fake = ROAM_SLOTS;
    }
    return s_roam_fake;
}

static const char *roam_cfg_name(const uint8_t blob[ROAM_CFG_SIZE]);
static uint32_t roam_heap_log(uint32_t h, const char *when);

/* A motorbike (the car table's class, as sub_00269CC0 tests it). Its build
 * adds the rider (sub_002CBF70 -> sub_00117880), which loops on Sleep(1)
 * (0x00117A00-0x00117A39) until the rider's resource is in and every
 * streamed resource in the tables [0x509E20..] is idle (sub_000F16F0):
 * true at a load, never in the running cruise, where the city streams and
 * our build holds the main thread (2026-11-11, iPhone: vp_ducati_ss1000_04
 * swapped into a slot, main frozen for good, guest lock free). So no bike
 * is built mid-cruise: the slot or car keeps a stand-in, the next load
 * builds the bike. */
static int roam_car_bike(int32_t car)
{
    uint32_t t = MEM32(ROAM_CARTAB_VA), c;
    if (car < 0 || !online_ram(t))
        return 0;
    c = MEM32(t + 40u * (uint32_t)car + 8u);
    return c == 5u || c == 6u || c == 7u || c == 0xAu;
}

static int32_t roam_car_index(const char *name)  /* the car list's index of a vp_ name (sub_0026A6A0), -1 none */
{
    uint32_t top = (g_esp - 0x400u) & ~0xFu, a = top;
    if (!name || strlen(name) >= 0x100u)
        return -1;
    memcpy((char *)XBOX_PTR(top), name, strlen(name) + 1);
    return (int32_t)roam_call(sub_0026A6A0, 0, top, 1, &a);
}

/* Our car to every peer: the CarCfg at cfg, its vtable word zeroed (each
 * receiver puts its own). The core sends only a changed record. */
static void roam_send_cfg(uint32_t cfg)
{
    static uint8_t sent[ROAM_CFG_SIZE];
    uint8_t blob[ROAM_CFG_SIZE];
    const char *name;
    if (!roam_running() || !online_ram(cfg) || MEM32(cfg) != ROAM_CFG_VT)
        return;
    memcpy(blob, (uint8_t *)XBOX_PTR(cfg), ROAM_CFG_SIZE);
    memset(blob, 0, 4);
    {   /* MC3_ROAM_TEST_CAR=<vp_ name> (test hook): send that car instead of ours,
         * its name (+0xE3) and index (+4), so the peers have to swap */
        const char *t = getenv("MC3_ROAM_TEST_CAR");
        if (t && *t && strlen(t) < ROAM_CFG_SIZE - ROAM_CFG_NAME) {
            uint32_t top = (g_esp - 0x400u) & ~0xFu, a = top;
            int32_t car;
            memcpy((char *)XBOX_PTR(top), t, strlen(t) + 1);
            car = (int32_t)roam_call(sub_0026A6A0, 0, top, 1, &a);
            if (car >= 0) {
                memset(blob + ROAM_CFG_NAME, 0, ROAM_CFG_SIZE - ROAM_CFG_NAME);
                memcpy(blob + ROAM_CFG_NAME, t, strlen(t));
                memcpy(blob + ROAM_CFG_CAR, &car, 4);
            } else if (memcmp(blob, sent, ROAM_CFG_SIZE))
                fprintf(stderr, "[roam] MC3_ROAM_TEST_CAR=%s: not in the car list, ours sent\n", t);
        }
    }
    if (memcmp(blob, sent, ROAM_CFG_SIZE)) {
        memcpy(sent, blob, ROAM_CFG_SIZE);
        name = roam_cfg_name(blob);
        fprintf(stderr, "[roam] cfg sent: %s\n", name ? name : "(no car name)");
    }
    roam_set_cfg(blob, ROAM_CFG_SIZE);
}

/* Our status to the peers and the hub: RC_ST_ROAM driving in the cruise,
 * RC_ST_SHOP in the garage (peers hide our car and keep its slot). */
static void roam_status(uint8_t st)
{
    static int said = -1;
    if (!roam_running())
        return;
    roam_set_status(st);
    if (said != st) {
        said = st;
        fprintf(stderr, "[roam] status %s\n", st == RC_ST_ROAM      ? "roam"
                                              : st == RC_ST_SHOP    ? "garage"
                                              : st == RC_ST_LOADING ? "loading"
                                              : st == RC_ST_EVENT   ? "race (AI race from Free Roam)"
                                                                    : "other");
    }
}

/* Each city is its own free-roam room: the hub links, and peers accept,
 * players of the same city only (room 0 = San Diego, what clients without
 * rooms send). The city is the current race setup's +0 ([0x53F2E0]), the
 * index into the city table [0x557EE4] (0x50 B entries, name at +0;
 * sub_002693F0, read by the cruise load for "$/resources/city/%s_traffic"):
 * 0 San Diego, 1 Atlanta, 2 Detroit, 3 Tokyo; 4-13 are dlc1-5 slots.
 * Arcade's setup screen picks it (every city unlocked Online, 0x53F2ED).
 * Called each frame and at state requests while Online (so the room
 * changes once the pick lands, before the load) and at the cruise load
 * (sub_002BE2C0, authoritative). 1: the room changed now (peers of the old
 * city dropped). */
static int roam_room(void)
{
    static const char *const k_city[] = { "San Diego", "Atlanta", "Detroit", "Tokyo" };
    static int said = -1;
    uint32_t setup = MEM32(ROAM_SETUP_VA), city;
    if ((!s_online && !s_career_roam) || !online_ram(setup))
        return 0;
    city = MEM32(setup);
    if (city >= 14u || (int)city == said)
        return 0;
    roam_set_room(city);
    if (city < 4u)
        fprintf(stderr, "[ROAM] room %s (city %u)\n", k_city[city], (unsigned)city);
    else
        fprintf(stderr, "[ROAM] room city %u\n", (unsigned)city);
    said = (int)city;
    return 1;
}

static int roam_cars_total(void);               /* remote cars shown at most (roam_cars.c) */

/* Free roam on (not with MC3_ROAM_FAKE), and our car (CarCfg at cfg) to
 * every peer. */
static void roam_begin(uint32_t cfg)
{
    if (roam_fake())
        return;
    if (!roam_running()) {
        const char *name = getenv("MC3_ROAM_NAME");
        rc_state st;
        if (!name || !*name)
            name = getenv("MC3_PLAYER_NAME");   /* the page's 'Your name' */
        /* The links asked of the hub (MC3_ROAM_K overrides): 32 at least, more
         * than the cars we show. The hub links a pair only while both are under
         * their K, so a phone's 7 (MC3_ROAM_CARS 7) left every player beyond its
         * 7 nearest without its car and it without theirs, dimmed FAR blips
         * both ways (2026-11-11, 10 players in a city); a linked peer we have
         * no car for still gets a live blip. */
        roam_set_k(roam_cars_total() > 32 ? roam_cars_total() : 32);
        roam_start(name && *name ? name : "Player", 0);
        /* "Not placed yet" (the origin, status menus): the hub links only
         * players with a position, and ours used to come with the cruise's
         * first frame, ~3 s after its cars were built ("0 peers driving at
         * load"). The core re-sends it every 2 s until the cruise runs, so
         * the peers, their poses and car records are here by the load. */
        memset(&st, 0, sizeof st);
        st.q[3] = 1.0f;
        roam_set_local(&st);
    }
    roam_send_cfg(cfg);
}

/* The garage opens (state 5, sub_00021A60): the cruise is gone, the session
 * stays (the network thread keeps HELLO/PING going, the core our hub
 * position); the peers hide our car until the next cruise load says roam
 * and sends the car bought here. */
static void roam_garage(void)
{
    if ((s_online || s_career_roam) && !roam_fake())
        roam_status(RC_ST_SHOP);
}

/* A state request (sub_00020E40) in the career's cruise, not pause/resume (1-4): a race, a mission, the garage,
 * the menus may follow, so the peers hide our car now (EVENT); roam_player shows it again if the cruise goes on. */
static void roam_career_request(uint32_t req)
{
    if (!s_career_roam || !s_roam_cruise || (req >= 1u && req <= 4u))
        return;
    roam_status(RC_ST_EVENT);
    s_career_ev = 1;
}

/* CarCfg dst = blob through the game's operator= (sub_00272010), with
 * blob copied below the caller's stack and our vtable put back. */
static void roam_cfg_set(uint32_t dst, const uint8_t blob[ROAM_CFG_SIZE])
{
    uint32_t r[8] = { g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi };
    uint32_t buf = (r[4] - 0x200u) & ~0xFu;
    memcpy((uint8_t *)XBOX_PTR(buf), blob, ROAM_CFG_SIZE);
    MEM32(buf) = ROAM_CFG_VT;
    g_esp = buf;
    g_esp -= 4; MEM32(g_esp) = buf;
    g_esp -= 4; MEM32(g_esp) = 0;               /* return address; thiscall, ret 4 */
    g_ecx = dst;
    sub_00272010();
    g_eax = r[0]; g_ecx = r[1]; g_edx = r[2]; g_ebx = r[3];
    g_esp = r[4]; g_ebp = r[5]; g_esi = r[6]; g_edi = r[7];
}

/* A CarCfg blob's car name (+0xE3), or NULL when it is not a string. */
static const char *roam_cfg_name(const uint8_t blob[ROAM_CFG_SIZE])
{
    const char *s = (const char *)blob + ROAM_CFG_NAME;
    return s[0] && memchr(s, 0, ROAM_CFG_SIZE - ROAM_CFG_NAME) ? s : NULL;
}

/* A peer's CarCfg into blob (exactly ROAM_CFG_SIZE bytes); 1 if it is one
 * with a car name. Only the name counts: the car builder, the model read and
 * sub_0026A6A0 all go by it, while the index at +4 is written only by the
 * garage's car change (sub_001440D0; -1 after the reset sub_0026FE80).
 * Requiring a valid +4 here (2026-11-09) is the likely reason a peer's
 * record read as "none": no slot built with its car at load and no swap
 * ever queued, both silently -- each browser saw the other in its own car. */
static int roam_peer_rec(uint32_t id, uint8_t blob[ROAM_CFG_SIZE + 4])
{
    return roam_peer_cfg(id, blob, ROAM_CFG_SIZE + 4) == (int)ROAM_CFG_SIZE && roam_cfg_name(blob);
}

/* Two CarCfgs give the same car (name, parts, paint): compared from +0xA.
 * Not +4 (the index, often -1) nor +8 (a 16-bit field the car's build
 * rewrites, 0x3FFF before, 0 after: a two-browser run, 2026-11-09, saw
 * every same-car peer as "differs" and queued useless swaps). */
static int roam_cfg_same(const uint8_t *a, const uint8_t *b)
{
    return !memcmp(a + 0xA, b + 0xA, ROAM_CFG_SIZE - 0xA);
}

/* Free Roam's cars heap: a second heap of the game's own type (ctor
 * sub_001B4600, which links it into the heap list [0x52B478]; init
 * sub_001B3DF0 as the main heap's, sub_00367310) over contiguous-window
 * memory of its own, made current around every remote car's build (the
 * player manager at load -- ours too --, slot swaps, standalone cars), so
 * those cars never touch the main heap. The window, not low RAM or the
 * extended VMA: a car's textures and vertex data sit in its heap blocks and
 * the GPU reads them at bus addresses, which only window memory has. Frees
 * need nothing: operator delete (sub_001B3C00, [] sub_001B3CD0) finds the
 * heap by address (sub_001B3D80 walks the list), whatever heap is current.
 * Made at the first cruise with remote cars, kept for the session (the car
 * cache may keep a model read during a build in it); the heap object sits
 * in the block's first page. MC3_ROAM_HEAP_MB=N (0: none, the cars in the
 * main heap as before); default 48 at a 128 MB window, where the main heap
 * is 63 MB for it (sub_002698C0 override), none at 64 MB; at most the
 * window's free top less ROAM_CHEAP_LEFT. Logs '[ROAM] cars heap ...'. */
static uint32_t s_cheap, s_cheap_prev;          /* the heap object (0: none); the current heap it replaced */
static int s_cheap_tried, s_cheap_depth;

static uint32_t roam_cheap(void)
{
    const char *e = getenv("MC3_ROAM_HEAP_MB");
    uint32_t want, room, size, base, a[4];
    if (s_cheap_tried)
        return s_cheap;
    s_cheap_tried = 1;
    want = e && *e ? (uint32_t)strtoul(e, NULL, 10) : (g_xbox_contig_size >> 20) >= 128u ? ROAM_CHEAP_MB : 0u;
    if (want > 1024u)
        want = 1024u;
    if (!want) {
        fprintf(stderr, "[ROAM] cars heap: none (%s): remote cars in the main heap\n",
                e && *e ? "MC3_ROAM_HEAP_MB=0" : "64 MB window");
        return 0;
    }
    room = g_xbox_contig_size - xbox_ContiguousAllocatedBytes();
    room = room > ROAM_CHEAP_LEFT ? (room - ROAM_CHEAP_LEFT) & ~0xFFFFFu : 0u;
    size = want << 20 < room ? want << 20 : room;
    if (size < (8u << 20) || !(base = xbox_ContiguousAlloc(size, 0x1000u))) {
        fprintf(stderr, "[ROAM] cars heap: %u MB asked, %u KB free in the window: none, remote cars in the main heap\n",
                (unsigned)want, (unsigned)((g_xbox_contig_size - xbox_ContiguousAllocatedBytes()) >> 10));
        return 0;
    }
    roam_call(sub_001B4600, base, (g_esp - 0x400u) & ~0xFu, 0, NULL);
    a[0] = base + 0x1000u;                      /* base, size, free lists on, 0: as the main heap */
    a[1] = size - 0x1000u;
    a[2] = 1;
    a[3] = 0;
    roam_call(sub_001B3DF0, base, (g_esp - 0x400u) & ~0xFu, 4, a);
    memcpy((char *)XBOX_PTR(base + 0x20u), "RoamCars", 9);  /* +0x20 name, 11 bytes */
    s_cheap = base;
    fprintf(stderr, "[ROAM] cars heap: %u KB at 0x%08X (heap 0x%08X; main heap 0x%08X untouched by remote cars; "
                    "window %u KB used of %u)\n", (unsigned)((size - 0x1000u) >> 10), (unsigned)(base + 0x1000u),
            (unsigned)base, (unsigned)MEM32(ROAM_MAIN_HEAP_VA), (unsigned)(xbox_ContiguousAllocatedBytes() >> 10),
            (unsigned)(g_xbox_contig_size >> 10));
    return s_cheap;
}

/* The cars heap current around a remote car's build (nests); off puts the
 * one it replaced back. No-ops without a cars heap. */
static void roam_cheap_on(void)
{
    if (s_cheap && !s_cheap_depth++) {
        s_cheap_prev = MEM32(ROAM_HEAP_VA);
        MEM32(ROAM_HEAP_VA) = s_cheap;
    }
}

static void roam_cheap_off(void)
{
    if (s_cheap && s_cheap_depth && !--s_cheap_depth)
        MEM32(ROAM_HEAP_VA) = s_cheap_prev;
}

/* Once a frame: the cars heap current outside our builds means another
 * guest thread saved it as "previous" during the load's build and put it
 * back after ours: the whole game would fill it. The main heap again. */
static void roam_cheap_check(void)
{
    static int said;
    if (!s_cheap || s_cheap_depth || MEM32(ROAM_HEAP_VA) != s_cheap)
        return;
    MEM32(ROAM_HEAP_VA) = MEM32(ROAM_MAIN_HEAP_VA);
    if (!said++)
        fprintf(stderr, "[ROAM] cars heap: left current outside a car build; the main heap 0x%08X again (logged once)\n",
                (unsigned)MEM32(ROAM_MAIN_HEAP_VA));
}

/* Around a car built or deleted mid-cruise (on 1, back on 0; not nested).
 * The garage's way (sub_0013C8E0, and sub_00150DE0 around its cars'
 * deletes): the resource context [[0x52B494]] +0x14/+0x18/+0x1C into
 * [0x507668], the shader factory [0x5077B8] and [0x5078D8] for the
 * build, the old ones back after. Without it the car's shaders paged in
 * through the cruise's factory: "Invalid type %d in rmcShaderFactory
 * Standard::ResourcePageIn" (0x4537D8), KeBugCheck (2026-11-09).
 * No other guest thread inside: the page-in relocates through the global
 * context [0x500F10], takes its model from [0x50A088], and the factory
 * globals set here are the process's; another thread's page-in run at one
 * of our kernel calls would use them. The game's own loads of this kind
 * (cruise load, garage) have no city streaming beside them. 0: no resource
 * context (nothing changed). */
static int roam_build_ctx(int on)
{
    static const uint32_t ctxva[3] = { 0x00507668u, 0x005077B8u, 0x005078D8u };
    static uint32_t keep[3], factory;
    uint32_t rc = MEM32(ROAM_RESCTX_VA), i;
    if (on) {
        if (!online_ram(rc))
            return 0;
        factory = MEM32(ROAM_FACTORY_VA);
        for (i = 0; i < 3; i++) {
            keep[i] = MEM32(ctxva[i]);
            MEM32(ctxva[i]) = MEM32(rc + 0x14u + 4u * i);
        }
        xbox_gil_atomic(1);
        return 1;
    }
    for (i = 0; i < 3; i++)
        MEM32(ctxva[i]) = keep[i];
    MEM32(ROAM_FACTORY_VA) = factory;           /* another thread's, if it had set one */
    xbox_gil_atomic(-1);
    return 1;
}

/* A vehicle already out of the world deleted as the player dtor sub_002C8480
 * does: the vehicle, its car cache reference res, its model instance. */
static void roam_car_delete(uint32_t veh, uint32_t res, uint32_t top)
{
    uint32_t model = MEM32(veh + 0x68u), a[1] = { res };
    roam_vdelete(veh, top);
    roam_call(sub_001057B0, MEM32(ROAM_CACHE_VA), top, 1, a);
    roam_vdelete(model, top);
}

/* A car built as the player ctor sub_002BFE50 builds it: builder sub_002CB270
 * at top (0xF0 B, the calls' stack below), sub_002CBC40, sub_001240F0, the
 * builder's dtor. The vehicle (0 none); *res its car cache reference. */
static uint32_t roam_car_build(uint32_t cfg, uint32_t slot, uint32_t pad, uint32_t top, uint32_t *res)
{
    uint32_t setup = MEM32(ROAM_SETUP_VA), type = MEM32(setup + 0x18u), veh, a[6], i;
    uint32_t e = setup + ROAM_ENTRY + slot * ROAM_ENTRY_SIZE, keep[3];
    /* no remote slots (the career cruise): entry `slot` (past the count) is borrowed for the build with pad,
     * viewport and controller -1, as a remote slot's, and put back after */
    int borrow = !s_roam_slots && slot >= 1u && slot <= ROAM_SLOTS;
    for (i = 0; borrow && i < 3; i++) {
        keep[i] = MEM32(e + 0x10u + 4u * i);
        MEM32(e + 0x10u + 4u * i) = 0xFFFFFFFFu;
    }
    s_roam_building = 1;
    a[0] = cfg + ROAM_CFG_NAME;                 /* name, 0, slot, CarCfg, flag, pad */
    a[1] = 0;
    a[2] = slot;
    a[3] = cfg;
    a[4] = type != 0x15u && type != 0x12u;
    a[5] = pad;
    roam_cheap_on();                            /* the car's blocks in the cars heap */
    roam_call(sub_002CB270, top, top, 6, a);
    veh = roam_call(sub_002CBC40, top, top, 0, NULL);
    *res = MEM32(top + 0x4Cu);
    if (!MEM8(setup + 0x108u) && online_ram(veh))
        roam_call(sub_001240F0, MEM32(veh + 8u), top, 0, NULL);
    /* A preset paint (CarCfg +0x40 != 0xFF) reaches the model's paint
     * (+0x1228) only in the model update (sub_000F5E10 -> sub_000F2C00),
     * while +0x1270 is set; the draw (0x000FE09C) applies +0x1228 and clears
     * +0x1270. A car built mid-frame (after the physics step) was drawn
     * first: the car's own colour, never the peer's. Load it now. */
    if (online_ram(veh) && online_ram(MEM32(veh + 0x68u)))
        roam_call(sub_000F2C00, MEM32(veh + 0x68u), top, 0, NULL);
    roam_call(sub_002CB2C0, top, top, 0, NULL);
    roam_cheap_off();
    s_roam_building = 0;
    for (i = 0; borrow && i < 3; i++)
        MEM32(e + 0x10u + 4u * i) = keep[i];
    return online_ram(veh) ? veh : 0;
}

/* A car-builder step (ecx = the builder, vtable 0x476940: +0xE8 slot, +0xEC
 * pad) run for one of our remote slots with the race setup saying network
 * race, our slot 0 local (+0x108 = 1, +0x3D8 = 0), as System Link builds a
 * remote player's car. Used for the controller step sub_002CB320: outside a
 * network race a car without a pad gets no controller ([veh+4] = 0), so the
 * peer's inputs had nowhere to go -- no steering, no throttle, wheels still,
 * the car dragged by the pose correction alone. Initial build and swaps
 * (roam_rebuild) both pass here. */
static void roam_build_step(recomp_func_t fn)
{
    uint32_t setup = MEM32(ROAM_SETUP_VA), slot, net, me;
    /* no remote slots (the career cruise): our standalone cars' builds only, never the game's own */
    int lim = s_roam_slots ? s_roam_slots : s_roam_building ? roam_bslots() : 0;
    if (!lim || !online_ram(g_ecx) || !online_ram(setup) || MEM8(setup + 0x108u) ||
        MEM32(setup + 0x4Cu) != 1u + (uint32_t)s_roam_slots ||
        (slot = MEM32(g_ecx + 0xE8u)) < 1u || slot > (uint32_t)lim) {
        fn();
        return;
    }
    net = MEM8(setup + 0x108u);
    me = MEM32(setup + 0x3D8u);
    MEM8(setup + 0x108u) = 1;
    MEM32(setup + 0x3D8u) = 0;
    fn();
    MEM8(setup + 0x108u) = (uint8_t)net;
    MEM32(setup + 0x3D8u) = me;
}

/* Free bytes of game heap h (the current one is [0x52B488], allocator
 * sub_001B4740: +8 base, +0xC size, +0x10 bump top, +0x20 name, +0x2B
 * free-list mode, +0x30 32 bins of free blocks: +4 size, +0x14 next); its
 * overrun is fatal ("Heap overrun"). Logged with when; returns top + free
 * blocks, 0 if unread. */
static uint32_t roam_heap_log(uint32_t h, const char *when)
{
    uint32_t cap, top, bins = 0, big = 0, b, i, n = 0;
    char name[16];
    if (!online_ram(h))
        return 0;
    cap = MEM32(h + 0xCu);
    top = MEM32(h + 0x10u);
    top = top < cap ? cap - top : 0;
    for (i = 0; MEM8(h + 0x2Bu) && i < 32; i++)
        for (b = MEM32(h + 0x30u + 4u * i); online_ram(b) && n < 65536u; b = MEM32(b + 0x14u), n++) {
            bins += MEM32(b + 4u);
            if (MEM32(b + 4u) > big)
                big = MEM32(b + 4u);
        }
    for (i = 0; i < sizeof name - 1 && (name[i] = (char)MEM8(h + 0x20u + i)) >= ' ' && name[i] < 127; i++)
        ;
    name[i] = 0;
    if (when)                                   /* NULL: just the count */
        fprintf(stderr, "[ROAM] heap '%s' %s: %u KB free (%u KB at the top, %u KB in %u free blocks, largest %u KB) of %u KB\n",
            name, when, (unsigned)((top + bins) >> 10), (unsigned)(top >> 10), (unsigned)(bins >> 10), (unsigned)n,
            (unsigned)(big >> 10), (unsigned)(cap >> 10));
    return top + bins;
}

static uint32_t s_roam_heap0;                   /* free bytes before the player manager was built */
static uint32_t s_roam_main0;                   /* the main heap's then, with a cars heap */

/* The heap remote cars are built in: the cars heap, else the current one. */
static uint32_t roam_car_heap(void)
{
    return s_cheap ? s_cheap : MEM32(ROAM_HEAP_VA);
}

static void roam_pool_cost_note(int32_t car, uint32_t bytes);   /* roam_pool.c */
/* After the player manager is built: the margin left, and what each car took. */
static void roam_built(void)
{
    uint32_t heap0 = s_roam_heap0, heap1, main0 = s_roam_main0, main1;
    roam_cheap_off();                           /* roam_build made the cars heap current */
    s_roam_heap0 = s_roam_main0 = 0;
    if (!heap0 || !s_roam_slots || !(heap1 = roam_heap_log(roam_car_heap(), "after the cruise's cars")))
        return;
    if (heap1 < heap0)
        fprintf(stderr, "[ROAM] the cruise's %d cars took %u KB, about %u KB each\n", 1 + s_roam_slots,
                (unsigned)((heap0 - heap1) >> 10), (unsigned)(((heap0 - heap1) / (1u + (uint32_t)s_roam_slots)) >> 10));
    if (heap1 < heap0)                          /* a model not measured yet costs the most measured */
        roam_pool_cost_note(-1, (heap0 - heap1) / (1u + (uint32_t)s_roam_slots));
    if (main0 && (main1 = roam_heap_log(MEM32(ROAM_MAIN_HEAP_VA), "after the cruise's cars (main)")))
        fprintf(stderr, "[ROAM] main heap: %d KB taken meanwhile (other threads; the cars are in the cars heap)\n",
                (int)(main0 - main1) / 1024);
}

/* Remote slots to build: the peers driving at load + ROAM_SPARE, at most
 * ROAM_SLOTS; MC3_ROAM_SLOTS=N (1-7) builds N whatever the peers. */
static int roam_nslots(int np)
{
    const char *e = getenv("MC3_ROAM_SLOTS");
    int n = e && *e ? atoi(e) : np + ROAM_SPARE;
    return n < 1 ? 1 : n > ROAM_SLOTS ? ROAM_SLOTS : n;
}

/* Before the player manager is built (sub_002BE2C0). */
static int s_roam_last_n;                        /* the 1 + N we wrote into the race setup */
static void roam_pool_reset(void);              /* roam_pool.c: the remote cars table */
static void roam_pool_slot_init(int k, const uint8_t *look, const roam_peer *pp);
static int roam_pool_fits(uint32_t left, int32_t car, uint32_t credit);
static uint32_t roam_pool_cost(int32_t car);

/* roam_build outside the Online session: Online career (s_career_roam). The career's cruise (game mode 2 [0x53F2D4],
 * one local player with a car, race type 1) starts or keeps the session and sends LOADING (roam_player: ROAM at its
 * first frame); any other career load (race, mission, garage) says EVENT / SHOP; a load outside the career closes
 * it. No remote slots: the standalone cars come after the load (roam_pool_load), in the cars heap when there is one. */
static int roam_career_setup(uint32_t setup)   /* the career's cruise: one local player with a car, race type 1 */
{
    uint32_t e0 = setup + ROAM_ENTRY;
    return MEM32(setup + 0x4Cu) == 1u && !MEM8(setup + 0x108u) && !MEM8(e0 + 0x26u) && SMEM32(e0 + 0x14u) >= 0 &&
           MEM32(setup + 0x18u) == 1u;
}

/* The career's cruise joins the city's free roam (its load, or the live switch mid-cruise): LOADING now, ROAM and
 * our car at its next frame (roam_player). */
static void roam_career_join(uint32_t setup, const char *when)
{
    uint32_t cfg0 = MEM32(setup + ROAM_ENTRY + 0x0Cu);
    s_career_roam = 1;
    s_career_ev = 0;
    if (!online_ram(cfg0) || MEM32(cfg0) != ROAM_CFG_VT)
        cfg0 = 0;
    roam_room();
    roam_begin(cfg0);
    s_roam_cruise = roam_running();
    s_roam_cfg_loaded = 0;
    roam_status(RC_ST_LOADING);
    if (s_roam_cruise)
        roam_cheap();                           /* standalone cars in the cars heap (none at 64 MB: the main heap's check) */
    fprintf(stderr, "[CAREER-ONLINE] %s (city %u): %s\n", when, (unsigned)MEM32(setup),
            s_roam_cruise ? "session running, other players as standalone cars and blips" : "no session (link down?)");
}

static void roam_career_load(uint32_t setup)
{
    uint32_t e0 = setup + ROAM_ENTRY;
    if (!roam_career_on() || MEM32(0x0053F2D4u) != 2u) {
        if (s_career_roam) {
            s_career_roam = 0;
            roam_stop();
            fprintf(stderr, "[CAREER-ONLINE] off: %s, session closed\n",
                    MEM32(0x0053F2D4u) != 2u ? "a load outside the career" : "switched off");
        }
        return;
    }
    if (!roam_career_setup(setup)) {
        if (s_career_roam) {
            roam_status(MEM8(e0 + 0x26u) ? RC_ST_SHOP : RC_ST_EVENT);
            fprintf(stderr, "[CAREER-ONLINE] career load (race type %u%s): our car hidden from the others\n",
                    (unsigned)MEM32(setup + 0x18u), MEM8(e0 + 0x26u) ? ", garage" : "");
        }
        return;
    }
    if (!s_career_roam)
        fprintf(stderr, "[CAREER-ONLINE] on: the career's cruise joins the city's free roam (no online money, "
                        "no street races; the career saves as always)\n");
    roam_career_join(setup, "cruise load");
}

static void roam_build(void)
{
    uint32_t setup = MEM32(ROAM_SETUP_VA), e0 = setup + ROAM_ENTRY, cfg0, i;
    uint8_t blob[ROAM_CFG_SIZE + 4];
    roam_peer peers[ROAM_SLOTS];
    int n, np = 0, k = 0, nrec = 0;
    roam_fake();
    roam_pool_reset();                          /* before s_roam_slots: a left-over standalone car is found by it */
    s_roam_slots = 0;
    s_roam_cruise = 0;
    s_roam_nhist = 0;
    if (!online_ram(setup))
        return;
    if (!s_online) {
        roam_career_load(setup);
        return;
    }
    s_career_roam = 0;                          /* Free Roam's session from here on */
    /* The game keeps the race setup between cruises: a second Free Roam found
     * our own 1 + N here, failed the check below, and the game built N
     * undriven players on top of each other (hang). Undo our count first. */
    if (s_roam_last_n && MEM32(setup + 0x4Cu) == 1u + (uint32_t)s_roam_last_n)
        MEM32(setup + 0x4Cu) = 1u;
    s_roam_last_n = 0;
    /* One local player with a car, no network race: the cruise, not the garage. */
    if (MEM32(setup + 0x4Cu) != 1u || MEM8(setup + 0x108u) || MEM8(e0 + 0x26u) || SMEM32(e0 + 0x14u) < 0)
        return;
    /* A race (Free Roam's AI race, roam_race_ai.c) gets no remote slots: the
     * race would count them as N undriven racers. Only the cruise (type 0),
     * and a group race: a slot for each player racing with us, the race's
     * own racers (the game places the 1 + N local players side by side at
     * its start, sub_001CBCA0; the AI on their own grid). */
    if (MEM32(setup + 0x18u) != 0 && !roam_group_race()) {
        fprintf(stderr, "[ROAM] race load: no slots (race type %u)\n", (unsigned)MEM32(setup + 0x18u));
        return;
    }
    cfg0 = MEM32(e0 + 0x0Cu);
    if (!online_ram(cfg0) || MEM32(cfg0) != ROAM_CFG_VT)
        cfg0 = 0;
    /* the city this load builds; a change only now (not seen in the menus)
     * leaves no peers known at load: the spare slots take the city's */
    if (roam_room() && roam_running())
        fprintf(stderr, "[ROAM] the city changed at the load: no peers of it known yet\n");
    roam_begin(cfg0);                           /* the car this load builds for us */
    /* The Free Roam cruise, slots or not: roam_player sends our state from its
     * first frame. With 0 slots (a low-heap phone) it used to return before
     * roam_net: no pose and no status ever went out, the others never saw us. */
    s_roam_cruise = s_roam_fake || roam_running();
    s_roam_cfg_loaded = 0;
    roam_status(RC_ST_LOADING);                 /* peers keep our car hidden until the cruise runs (roam_player) */
    if (s_roam_fake)
        n = s_roam_fake;
    else if (roam_running()) {
        /* the peers known now (any status: one still in its menus or loading
         * gets its car too, hidden until it drives), nearest first; not
         * waited for -- the session started at Free Roam's pick, ~10 s ago */
        np = roam_peers(peers, ROAM_SLOTS);
        if (roam_group_race()) {                /* the players racing with us, a slot each, no spares */
            int m = 0;
            for (i = 0; i < (uint32_t)np; i++)
                if (roam_group_has(peers[i].id))
                    peers[m++] = peers[i];
            np = m;
            n = s_roam_group.n - 1 < ROAM_SLOTS ? s_roam_group.n - 1 : ROAM_SLOTS;
            fprintf(stderr, "[RACE] group load: %d slot%s for the players racing with us (%d linked now)\n", n,
                    n == 1 ? "" : "s", np);
        }
        for (i = 0; i < (uint32_t)np; i++)
            nrec += roam_peer_rec(peers[i].id, blob);
        if (!roam_group_race())
            n = roam_nslots(np);
    } else
        n = 0;
    if (!n)
        return;
    if (roam_cheap())                           /* the player manager (all its cars) built in the cars heap */
        s_roam_main0 = roam_heap_log(MEM32(ROAM_MAIN_HEAP_VA), "before the cruise's cars (main)");
    s_roam_heap0 = roam_heap_log(roam_car_heap(), "before the cruise's cars");
    /* A game heap is fixed and its overrun is fatal: a second cruise with 6
     * remote cars left 1259 KB of the 47 MB main heap and died on an 84 KB
     * request ("Heap overrun", 2026-11-10); 2 cars left 4815 KB and played.
     * Build only what fits by the pool's one check (roam_pool_fits: each
     * model's measured cost, ROAM_HEAP_KEEP left over, ROAM_CHEAP_KEEP in the
     * cars heap, where only cars live), ours first. The nearest peers keep
     * their slots; MC3_ROAM_SLOTS skips it. */
    if (s_roam_heap0 && !s_roam_fake && !getenv("MC3_ROAM_SLOTS")) {
        int32_t car0 = cfg0 ? roam_car_index(roam_cfg_name((const uint8_t *)XBOX_PTR(cfg0))) : -1, car;
        uint32_t left = s_roam_heap0, c0 = roam_pool_cost(car0);
        int room;
        left = left > c0 ? left - c0 : 0;       /* our own car comes out of it too */
        for (room = 0; room < n; room++) {
            car = room < np && roam_peer_rec(peers[room].id, blob) ? roam_car_index(roam_cfg_name(blob)) : car0;
            if (!roam_pool_fits(left, car, 0))
                break;
            left -= roam_pool_cost(car);
        }
        if (n > room) {
            fprintf(stderr, "[ROAM] heap: %d remote car%s fit (of %d asked), %u KB free, %u KB kept\n", room,
                    room == 1 ? "" : "s", n, (unsigned)(s_roam_heap0 >> 10),
                    (unsigned)((s_cheap ? ROAM_CHEAP_KEEP : ROAM_HEAP_KEEP) >> 10));
            n = room;
            if (np > n)
                np = n;
        }
        if (!n) {
            s_roam_heap0 = 0;
            return;
        }
    }
    for (i = 1; i <= (uint32_t)n; i++) {
        uint32_t e = e0 + i * ROAM_ENTRY_SIZE, cfg = MEM32(e + 0x0Cu);
        memcpy((uint8_t *)XBOX_PTR(e), (uint8_t *)XBOX_PTR(e0), ROAM_ENTRY_SIZE);
        MEM32(e + 0x10u) = MEM32(e + 0x14u) = MEM32(e + 0x18u) = 0xFFFFFFFFu;
        /* the entry keeps its own CarCfg (a shared one would be freed twice) */
        if (cfg0 && cfg != cfg0 && online_ram(cfg) && MEM32(cfg) == ROAM_CFG_VT) {
            const roam_peer *pp = NULL;
            MEM32(e + 0x0Cu) = cfg;
            while (k < np && !roam_peer_rec(peers[k].id, blob)) {
                fprintf(stderr, "[ROAM] at load: %s (id %08X) has no usable car record yet (gen %u)\n",
                        peers[k].name, (unsigned)peers[k].id, (unsigned)peers[k].cfg_gen);
                k++;
            }
            if (k < np) {                       /* the slot is this peer's from the start */
                pp = &peers[k++];
                fprintf(stderr, "[ROAM] slot %u: %s car %s (built) id %08X\n", (unsigned)i, pp->name,
                        roam_cfg_name(blob), (unsigned)pp->id);
            } else
                memcpy(blob, (uint8_t *)XBOX_PTR(cfg0), ROAM_CFG_SIZE);
            roam_cfg_set(cfg, blob);
            roam_pool_slot_init((int)i, blob, pp);
        } else {
            fprintf(stderr, "[ROAM] slot %u: no CarCfg of its own (%08X, ours %08X): our car, no swaps\n",
                    (unsigned)i, (unsigned)cfg, (unsigned)cfg0);
            roam_pool_slot_init((int)i, NULL, NULL);
        }
    }
    MEM32(setup + 0x4Cu) = 1u + (uint32_t)n;
    s_roam_slots = n;
    s_roam_last_n = n;
    fprintf(stderr, "[ROAM] cruise with %d remote car%s (slots 1-%d, %s; %d peer%s known at load, %d with a car record)\n",
            n, n == 1 ? "" : "s", n, s_roam_fake ? "MC3_ROAM_FAKE: our path, 1-3 s late" : "free roam peers", np,
            np == 1 ? "" : "s", nrec);
    roam_cheap_on();                            /* until roam_built, after sub_002BE2C0_gen */
}

/* Pose at time t from our car's path, between two samples (inputs: the
 * earlier one's); 0 if the path does not reach back that far yet. */
static int roam_hist_at(double t, float m[12], float vel[3], uint8_t ctl[ROAM_CTL])
{
    unsigned n = s_roam_nhist, lo = n > ROAM_HIST ? n - ROAM_HIST : 0, i, k;
    const roam_pose *a, *b;
    float f;
    if (!n || s_roam_hist[lo % ROAM_HIST].t > t)
        return 0;
    for (i = n - 1; s_roam_hist[i % ROAM_HIST].t > t; i--)
        ;
    a = &s_roam_hist[i % ROAM_HIST];
    b = i + 1 < n ? &s_roam_hist[(i + 1) % ROAM_HIST] : a;
    f = b->t > a->t ? (float)((t - a->t) / (b->t - a->t)) : 0.0f;
    for (k = 0; k < 12; k++)
        m[k] = a->m[k] + (b->m[k] - a->m[k]) * f;
    for (k = 0; k < 3; k++)
        vel[k] = a->vel[k] + (b->vel[k] - a->vel[k]) * f;
    memcpy(ctl, a->ctl, ROAM_CTL);
    return 1;
}

/* A peer's interpolated pose and newest inputs into slot k; in its garage
 * (RC_ST_SHOP) the slot stays its own but the car is parked and hidden. */
static uint8_t roam_away(uint8_t st)            /* a peer whose car is parked and hidden */
{
    return st == RC_ST_SHOP || st == RC_ST_LOADING || st == RC_ST_MENUS || st == RC_ST_EVENT;
}

/* Our controller's inputs packed as System Link's sender packs them
 * (sub_002C9570, local branch) into controller +0x6C: +0x28's button bits
 * (24; bit 0x10 nitro, its index + 1 in the top byte), the 0..1 axes
 * +0x0C/+0x10/+0x14 x255, steering +0x18 x127, the byte +0x2C, the axes
 * +0x20/+0x24 x127, the gear +0x54. Bits of the frame before are kept too:
 * a press shorter than a send (20 Hz) still reaches the peers. */
static void roam_ctl_pack(uint32_t c, uint8_t out[ROAM_CTL])
{
    static uint32_t prev;
    uint32_t now = MEM32(c + 0x28u) & 0xFFFFFFu, bits = now | prev, n = MEM32(c + 0x50u), nos;
    prev = now;
    if ((bits & 0x10u) && online_ram(n) && online_ram(nos = MEM32(n + 0x44u)))
        bits |= (MEM32(nos + 0x3Cu) + 1u) << 24;
    memcpy(out, &bits, 4);
    out[4] = (uint8_t)(int)(MEMF(c + 0x0Cu) * 255.0f);
    out[5] = (uint8_t)(int)(MEMF(c + 0x10u) * 255.0f);
    out[6] = (uint8_t)(int)(MEMF(c + 0x14u) * 255.0f);
    out[7] = (uint8_t)(int)(MEMF(c + 0x18u) * 127.0f);
    out[8] = MEM8(c + 0x2Cu);
    out[9] = (uint8_t)(int)(MEMF(c + 0x20u) * 127.0f);
    out[10] = (uint8_t)(int)(MEMF(c + 0x24u) * 127.0f);
    out[11] = MEM8(c + 0x54u);
}

/* Our car to free roam, and the linked peers to the remote cars pool
 * (roam_pool.c). Our state goes out first, whatever cars we have. */
static void roam_pool_net(const roam_peer *peers, int n, double now);

static void roam_send_local(const roam_pose *me)
{
    rc_state st;
    memset(&st, 0, sizeof st);
    memcpy(st.p, me->m + 9, sizeof st.p);
    memcpy(st.v, me->vel, sizeof st.v);
    roam_mat_quat(me->m, st.q);
    st.speed = st.v[0] * me->m[6] + st.v[1] * me->m[7] + st.v[2] * me->m[8];
    st.steer = (float)(int8_t)me->ctl[7] / 127.0f;
    st.gear = (int8_t)me->ctl[11];
    st.ext_len = ROAM_CTL;                      /* the receiver applies these through the game */
    memcpy(st.ext, me->ctl, ROAM_CTL);
    roam_set_local(&st);
}

static void roam_net(const roam_pose *me)
{
    static roam_peer peers[ROAM_K_MAX];         /* all the hub links us with, nearest first */
    int n;
    roam_send_local(me);
    n = roam_peers(peers, ROAM_K_MAX);
    s_roam_seen = peers;                        /* a second roam_peers a frame would upset its blend clock */
    s_roam_nseen = n;
    s_roam_seen_t = me->t;
    if (s_roam_fake)
        return;
    if (roam_group_race()) {                    /* a group race: its slots show the players racing with us only */
        static roam_peer mates[ROAM_GROUP_MAX];
        int i, m = 0;
        for (i = 0; i < n && m < ROAM_GROUP_MAX; i++)
            if (roam_group_has(peers[i].id))
                mates[m++] = peers[i];
        roam_pool_net(mates, m, me->t);
        return;
    }
    roam_pool_net(peers, n, me->t);             /* who drives which remote car (roam_pool.c) */
}

/* SetMatrix and SetVel on a player's car, as sub_002C0540 does. */
static void roam_set_pose(uint32_t veh, const float m[12], const float vel[3])
{
    uint32_t car = MEM32(veh + 8u), phys, coll, buf, i;
    uint32_t r[8] = { g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi };
    if (!online_ram(car) || !online_ram(phys = MEM32(car + 0x68u)) || !online_ram(coll = MEM32(phys + 0x10u)))
        return;
    buf = (r[4] - 0x100u) & ~0xFu;              /* below the caller's stack */
    for (i = 0; i < 12; i++)
        MEMF(buf + 4u * i) = m[i];
    for (i = 0; i < 3; i++)
        MEMF(buf + 0x30u + 4u * i) = vel[i];
    g_esp = buf;
    g_esp -= 4; MEM32(g_esp) = buf;
    g_esp -= 4; MEM32(g_esp) = 0;               /* return address; thiscall, ret 4 */
    g_ecx = phys;
    sub_002A35A0();
    g_esp -= 4; MEM32(g_esp) = buf + 0x30u;
    g_esp -= 4; MEM32(g_esp) = 0;
    g_ecx = coll;
    sub_003432B0();
    g_eax = r[0]; g_ecx = r[1]; g_edx = r[2]; g_ebx = r[3];
    g_esp = r[4]; g_ebp = r[5]; g_esi = r[6]; g_edi = r[7];
}

/* A remote car driven the System Link way. The network race's receiver
 * (msg 0x42, sub_00026D00) leaves the pose in the player's net block, as
 * sub_002BF2B0 copies it: +0x90 the matrix packed by sub_0008B970
 * (quaternion, position), +0xAC velocity, +0xB8 contact bits (one per slot),
 * +0xBC = 1; and the inputs at controller ([veh+4]) +0x6C, +0x78 = 1, as
 * sub_002C9810 copies them. Each frame the game then runs sub_002C0540 for
 * that player (blends the car toward the pose; snaps beyond 20 m or
 * 20 m/s) and sub_002C9570 for its controller (the inputs into the car:
 * pedals, steering, gear, buttons, nitro), and the car's own physics,
 * wheels, engine sound and lights follow the inputs. Both only run in a
 * network race and ask the session for the peer's smoothing settings: for
 * these two calls the race setup says network (+0x108 = 1, our slot +0x3D8
 * = 0) and [0x557EFC] is a stand-in session below the caller's stack whose
 * every peer has the settings' defaults (ctor sub_001B3A80). Contact bits
 * stay 0: slot numbers differ between machines. */
static void roam_remote(uint32_t p, uint32_t c, const float m[12], const float vel[3], const uint8_t *ctl)
{
    uint32_t setup = MEM32(ROAM_SETUP_VA), r[8] = { g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi };
    uint32_t buf = (r[4] - 0x400u) & ~0xFu, prm = buf + 0x40u, peer = buf + 0xA0u, arr = buf + 0x110u,
             ses = buf + 0x130u, i;
    uint32_t net = MEM8(setup + 0x108u), me = MEM32(setup + 0x3D8u), old = MEM32(ROAM_NET_VA);
    g_esp = buf;
    if (p) {                                    /* p 0: a standalone car (roam_cars.c), inputs only */
        for (i = 0; i < 12; i++)
            MEMF(buf + 4u * i) = m[i];
        g_esp -= 4; MEM32(g_esp) = buf;
        g_esp -= 4; MEM32(g_esp) = 0;           /* return address; thiscall, ret 4 */
        g_ecx = p + 0x90u;
        sub_0008B970();
        for (i = 0; i < 3; i++)
            MEMF(p + 0xACu + 4u * i) = vel[i];
        MEM16(p + 0xB8u) = 0;
        MEM8(p + 0xBCu) = 1;
    }
    g_esp -= 4; MEM32(g_esp) = 0;
    g_ecx = prm;
    sub_001B3A80();
    /* Smoothing (position +0x0C.., velocity +0x24.., rotation +0x3C..: on,
     * snap on, snap above, blend above, factor a + b*e + c*e^2 per frame of
     * the error e): the defaults pull 0.02*e per frame (slow when close,
     * rubber-banding) and snap at 20 m. Ours: a constant 1 - exp(-dt/tau),
     * an exponential decay, and the position snap at ROAM_SNAP. */
    {
        float f = 1.0f - expf(-s_roam_dt / ROAM_TAU);
        MEMF(prm + 0x10u) = ROAM_SNAP;
        MEMF(prm + 0x14u) = 0.0f;
        for (i = 0x18u; i <= 0x48u; i += 0x18u) {
            MEMF(prm + i) = f;
            MEMF(prm + i + 4u) = 0.0f;
            MEMF(prm + i + 8u) = 0.0f;
        }
    }
    MEM32(peer + 0x60u) = prm;
    for (i = 0; i <= ROAM_SLOTS; i++)
        MEM32(arr + 4u * i) = peer;
    MEM32(ses) = ROAM_NET_VT;
    MEM32(ses + 0xCu) = arr;
    MEM8(setup + 0x108u) = 1;
    MEM32(setup + 0x3D8u) = 0;
    MEM32(ROAM_NET_VA) = ses;
    if (p) {
        g_esp -= 4; MEM32(g_esp) = 0;
        g_ecx = p;
        sub_002C0540();
    }
    if (ctl) {
        memcpy((uint8_t *)XBOX_PTR(c + 0x6Cu), ctl, ROAM_CTL);
        MEM8(c + 0x78u) = 1;
        g_esp -= 4; MEM32(g_esp) = 0;
        g_ecx = c;
        sub_002C9570();
    }
    MEM8(setup + 0x108u) = (uint8_t)net;
    MEM32(setup + 0x3D8u) = me;
    MEM32(ROAM_NET_VA) = old;
    g_eax = r[0]; g_ecx = r[1]; g_edx = r[2]; g_ebx = r[3];
    g_esp = r[4]; g_ebp = r[5]; g_esi = r[6]; g_edi = r[7];
}

/* MC3_ROAM_TRACE=1: once a second per remote slot, before the correction:
 * the target's speed against the car's (m/s, world velocity, the frame both
 * ends write), the car's gap to the target (horizontal, and along the
 * target's forward row: < 0 behind), the peer's pedals and steering, and
 * what the car's input block ([controller+0x34] +0x70/+0x74/+0x6C, last
 * written by sub_002C9570) holds; "no controller" is the old bug. */
static int s_roam_trace = -1;

static void roam_trace(uint32_t slot, uint32_t veh, uint32_t c, const float m[12], const float vel[3],
                       const uint8_t *ctl, double now)
{
    static double next[ROAM_SLOTS + 1];
    uint32_t car = MEM32(veh + 8u), mtx = MEM32(veh + 0x1Cu), phys, coll, in;
    float v[3] = { 0, 0, 0 }, d[3] = { 0, 0, 0 };
    char inputs[64];
    int k;
    if (s_roam_trace < 0) {
        const char *e = getenv("MC3_ROAM_TRACE");
        s_roam_trace = e && e[0] == '1';
    }
    if (!s_roam_trace || slot > ROAM_SLOTS || now < next[slot])
        return;
    next[slot] = now + 1.0;
    if (online_ram(car) && online_ram(phys = MEM32(car + 0x68u)) && online_ram(coll = MEM32(phys + 0x10u)))
        for (k = 0; k < 3; k++)
            v[k] = MEMF(coll + 0x44u + 4u * k);
    if (online_ram(mtx))
        for (k = 0; k < 3; k++)
            d[k] = MEMF(mtx + 0x34u + 4u * k) - m[9 + k];
    if (online_ram(c) && online_ram(in = MEM32(c + 0x34u)))
        snprintf(inputs, sizeof inputs, "car thr %.2f brk %.2f steer %+.2f", MEMF(in + 0x70u), MEMF(in + 0x74u),
                 MEMF(in + 0x6Cu));
    else
        snprintf(inputs, sizeof inputs, "no controller");
    fprintf(stderr,
            "[ROAM] trace slot %u: rx %.1f m/s, car %.1f m/s, gap %.2f m (along %+.2f), peer thr %.2f brk %.2f "
            "steer %+.2f, %s\n",
            (unsigned)slot, sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]),
            sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), sqrtf(d[0] * d[0] + d[2] * d[2]),
            d[0] * m[6] + d[1] * m[7] + d[2] * m[8], ctl ? ctl[4] / 255.0f : 0.0f, ctl ? ctl[5] / 255.0f : 0.0f,
            ctl ? (int8_t)ctl[7] / 127.0f : 0.0f, inputs);
}

/* A car's "drawn" flag, which also gates its minimap blip and HUD icons, and
 * the car in or out of the game's draw list (node [veh+0xC]+8, sub_00092EF0:
 * idempotent both ways, as sub_000FFB40 takes a quitting peer's car out).
 * The flag alone did not hide a parked slot: the cruise's rules update
 * (sub_001D8F20 -> sub_001D86A0) sets it to 1 for every racer's car each
 * frame, and moving [veh+0x1C] out of the physics level (be33760) left the
 * car drawn on its start-grid spot (the render object keeps its own place,
 * +0xC4): the spare slots stood there as nameless copies of our car
 * (2026-11-10, owner). The slot report logs both places and the list. */
static void roam_show(uint32_t veh, int on)
{
    uint32_t o = MEM32(veh + 0xCu), a = (uint32_t)on;
    if (!online_ram(o))
        return;
    MEM8(o + 0x12Du) = (uint8_t)on;
    roam_call(sub_00092EF0, o + 8u, (g_esp - 0x400u) & ~0xFu, 1, &a);
}

/* The car's drawable is in the draw list (sub_00092EF0's own test). */
static int roam_listed(uint32_t veh)
{
    uint32_t o = MEM32(veh + 0xCu), n = o + 8u, l;
    if (!online_ram(o) || !online_ram(l = MEM32(n + 0x10u)))
        return -1;
    return MEM32(n + 4u) || MEM32(n + 8u) || MEM32(l + 0x1Cu) == n;
}

/* A remote car's tyres (2026-11-10: a peer's Jetta drawn without tyres after
 * 39336b5; 2026-11-11: still rims only on every peer's car). The drawn tyres
 * are sized only in the vehicle update's wheel step (0x00100654, while the
 * CarCfg [veh+0x64] has dirty bit 0x80): sub_00121970 on the wheels
 * [veh+0x50] (+0x54 front, +0x58 rear) with (CarCfg, rim holder
 * [[veh+0xC]+0x2F0]); sub_000F11E0 reads the rim geometry at
 * [[holder+0x1800]+0x18]+0x3818 / +0x381C (missing: a -0.3 m rim). A remote
 * car never gets there: the update returns first for a network player's car
 * ([veh+0x48] set, player [veh+0x5D] +0xF4 == 3 and +0xF8 == 0): 0x00100645
 * jumps to the return at 0x001007E8, or, with the session's +0x10 < 0, the
 * 0x001005E7 path sets 0x80 again and returns. Re-arming the bit (be2029f)
 * therefore sized nothing. So the sizing is called here, once per vehicle,
 * as soon as the rim geometry is loaded; the bit stays armed for a car whose
 * update does reach the step. One log line per car. */
typedef struct {
    uint32_t veh;                               /* the vehicle this state is for (a rebuild starts over) */
    uint8_t done, said;                         /* said: "no rim geometry" logged */
    double t0;
} roam_tyre;

static void roam_tyres(roam_tyre *t, uint32_t veh, const char *who, int idx)
{
    uint32_t cfg = MEM32(veh + 0x64u), r = MEM32(veh + 0xCu), w = MEM32(veh + 0x50u), h, g = 0, a[2];
    if (t->veh != veh) {
        memset(t, 0, sizeof *t);
        t->veh = veh;
        t->t0 = roam_now();
    }
    if (t->done || !online_ram(cfg) || !online_ram(r) || !online_ram(w) || !online_ram(MEM32(w + 0x54u)) ||
        !online_ram(MEM32(w + 0x58u)))
        return;
    MEM32(cfg + 8u) |= 0x80u;
    h = MEM32(r + 0x2F0u);
    if (online_ram(h) && online_ram(MEM32(h + 0x1800u)))
        g = MEM32(MEM32(h + 0x1800u) + 0x18u);
    if (!online_ram(g) || !online_ram(MEM32(g + 0x3818u)) || !online_ram(MEM32(g + 0x381Cu))) {
        if (!t->said && roam_now() - t->t0 > 10.0) {
            t->said = 1;
            fprintf(stderr, "[ROAM] tyres: %s %d no rim geometry after 10 s (holder 0x%08X)\n", who, idx,
                    (unsigned)h);
        }
        return;                                 /* not loaded yet: next frame */
    }
    a[0] = cfg;
    a[1] = h;
    roam_call(sub_00121970, w, (g_esp - 0x400u) & ~0xFu, 2, a);
    t->done = 1;
    fprintf(stderr, "[ROAM] tyres: %s %d sized after %.1f s (radius front %.3f rear %.3f, rim %.3f)\n", who, idx,
            roam_now() - t->t0, MEMF(MEM32(w + 0x54u) + 4u), MEMF(MEM32(w + 0x58u) + 4u),
            MEMF(MEM32(g + 0x3818u) + 0x14u));
}

/* Free Roam starts every player on the same spot: the city cruise's start
 * (or the garage's door after a visit). A peer still there had our car
 * built inside its own, collisions on both (2026-11-10). At our first
 * cruise frame, once roam_net has the peers' poses, our car steps along its
 * forward row (ahead, then behind, farther each try) to the first spot with
 * no driving peer within ROAM_CLEAR m (else the emptiest); lifted a little
 * for slopes. Not after an AI race: roam_race_ai.c puts the car back there.
 * 1: moved (s updated). */
#define ROAM_CLEAR 7.0f                         /* m, horizontal, centre to centre (a car is ~4.5 m) */
static int s_rra_back;                          /* roam_race_ai.c */

static int roam_spawn_clear(uint32_t veh, roam_pose *s)
{
    static const float step[] = { 0, 10, -10, 20, -20, 30, -30, 40, -40 };
    float m[12], best_d = -1.0f;
    int i, j, best = 0;
    if (s_rra_back || !s_roam_seen || roam_group_race())   /* a group race: the game's grid (roam_race_ai.c) */
        return 0;
    for (i = 0; i < (int)(sizeof step / sizeof step[0]); i++) {
        float x = s->m[9] + s->m[6] * step[i], z = s->m[11] + s->m[8] * step[i], d = 1e9f;
        for (j = 0; j < s_roam_nseen; j++)
            if (!roam_away(s_roam_seen[j].status)) {
                float dx = s_roam_seen[j].p[0] - x, dz = s_roam_seen[j].p[2] - z, dd = sqrtf(dx * dx + dz * dz);
                d = dd < d ? dd : d;
            }
        if (d > best_d) {
            best_d = d;
            best = i;
        }
        if (d >= ROAM_CLEAR)
            break;
    }
    if (!best)
        return 0;
    memcpy(m, s->m, sizeof m);
    m[9] += m[6] * step[best];
    m[10] += m[7] * step[best] + 0.5f + 0.1f * fabsf(step[best]);
    m[11] += m[8] * step[best];
    roam_set_pose(veh, m, s->vel);
    fprintf(stderr, "[ROAM] spawn: a player within %.1f m of the start spot: moved %+.0f m along the road "
            "(nearest player now %.1f m)\n", ROAM_CLEAR, step[best], best_d);
    memcpy(s->m, m, sizeof m);
    return 1;
}

/* Each player's update (sub_002C1520, thiscall), before the game's own:
 * slot 0 (ours, updated first) is recorded and sent, in every Free Roam
 * cruise (no remote slots too); slots 1..n are the pool's (roam_pool_slot). */
static void roam_pool_slot(uint32_t p, int k, double now);
static void roam_player(uint32_t p)
{
    uint32_t slot, veh, mtx, car, phys, coll, c, k;
    double now;
    if (!s_roam_cruise || !online_ram(p))
        return;
    slot = MEM32(p + 0x5Cu);
    veh = MEM32(p + 0x14u);
    if (slot == 0)
        roam_cheap_check();                     /* once a frame */
    if (slot > (uint32_t)s_roam_slots || !online_ram(veh))
        return;
    now = roam_now();
    if (slot == 0) {
        roam_pose *s = &s_roam_hist[s_roam_nhist % ROAM_HIST];
        if (s_roam_tlast > 0.0) {
            double dt = now - s_roam_tlast;
            s_roam_dt = (float)(dt < 1.0 / 120 ? 1.0 / 120 : dt > 0.25 ? 0.25 : dt);
        }
        s_roam_tlast = now;
        mtx = MEM32(veh + 0x1Cu);
        car = MEM32(veh + 8u);
        if (!online_ram(mtx) || !online_ram(car) || !online_ram(phys = MEM32(car + 0x68u)) ||
            !online_ram(coll = MEM32(phys + 0x10u)))
            return;
        s->t = now;
        for (k = 0; k < 12; k++)
            s->m[k] = MEMF(mtx + 0x10u + 4u * k);
        for (k = 0; k < 3; k++)
            s->vel[k] = MEMF(coll + 0x44u + 4u * k);
        if (online_ram(c = MEM32(veh + 4u)))
            roam_ctl_pack(c, s->ctl);
        else
            memset(s->ctl, 0, ROAM_CTL);
        s_roam_nhist++;
        if (!s_roam_fake) {
            int first = !s_roam_cfg_loaded;
            if (first) {                        /* the cruise runs: our car (a garage buy included), shown again */
                uint32_t setup = MEM32(ROAM_SETUP_VA);
                s_roam_cfg_loaded = 1;
                if (online_ram(setup))
                    roam_send_cfg(MEM32(setup + ROAM_ENTRY + 0x0Cu));
            }
            roam_net(s);                        /* still RC_ST_LOADING on a first frame: peers keep our car hidden */
            if (s_career_ev && !first && s_state_obj && MEM32(s_state_obj + 4u) == 2u && !MEM32(s_state_obj + 0x3Cu)) {
                s_career_ev = 0;                /* the career's request left the cruise running: shown again */
                roam_status(RC_ST_ROAM);
            }
            if (first) {
                s_career_ev = 0;
                if (!s_career_roam && roam_spawn_clear(veh, s))   /* off a peer parked on the start spot (not the career's) */
                    roam_send_local(s);
                s_rep_at = now + 0.5;           /* slot reports: once parked, then 5 s later (roam_cars.c) */
                s_rep_n = 2;
                roam_teleport();                /* a new spot (the garage's door): peers snap, no blend */
                roam_status(RC_ST_ROAM);
            }
        }
        return;
    }
    roam_pool_slot(p, (int)slot, now);          /* driven or parked (roam_pool.c) */
}
