/* Free roam: the remote cars pool (2026-11-11, untested in game). One table
 * owns every remote car and the peer it shows: the player slots' cars
 * (roam_game.c's roam_build adds them to the cruise's race setup; System
 * Link's remote path drives them, roam_pool_slot) and the standalone cars
 * beyond them (the player builder, no player; driven here after the physics
 * step). One assignment, one build/rebuild queue, one heap check, one park,
 * one "nearest gets a car" rule. Replaces the slots' and roam_cars.c's twin
 * state (roam_pick_slot, roam_swap, roam_cars_net, roam_nearest).
 *
 * Included by recomp_manual.c after roam_cars.c (its physics-level and
 * registry helpers) and before roam_map.c / roam_tags.c (they read s_rc).
 *
 * Entries: s_rc[k], k = 1 .. s_roam_slots, is player slot k (its car is the
 * player's +0x14); k = s_roam_slots + 1 .. s_nrc - 1 a standalone car (veh
 * 0: dropped, reused before a new entry). Entry 0 is ours, unused.
 *
 * Assignment (roam_pool_net, from roam_net in our player's update): a peer
 * keeps its car while linked and ROAM_HOLD s after it drops out (parked
 * meanwhile); a newcomer (nearest first) takes a free car with its exact look,
 * else one with its car, else any free one (rebuilt with its look), else the
 * longest-unseen held one; a slot before a standalone car on a tie (System
 * Link's pose correction drives it). None: a new standalone car is queued.
 * Nearest rule: while no car can be built (count or heap: s_pool_full), the
 * nearest driving peer without a car takes the car of the farthest holder (an
 * away one -- garage, loading, AI race -- counts as farthest) once it has been
 * ROAM_NEAR_GAIN m nearer for ROAM_NEAR_HOLD s. '[ROAM] nearest: ...'.
 *
 * Queue (roam_pool_job, after the physics step): one build or rebuild per
 * ROAM_CARS_GAP s, none while a state request is queued (s_pool_hold) or
 * another thread is inside a resource page-in: first a car whose peer sent
 * another look (a slot as roam_swap did: old car out and deleted as the
 * player dtor, new one built as the player ctor; a standalone one dropped and
 * built), else a new standalone car for a peer without one. Every build:
 * roam_pool_look (the car list; no motorbike mid-cruise, roam_car_bike),
 * roam_pool_room (the heap) and roam_pool_fit (a car cache entry, a vehicle
 * registry place; '[ROAM] cars: capacity: ...' after each build). A rebuild that cannot
 * have the peer's car keeps the old one; a new car takes ours instead.
 *
 * Heap (one check, roam_pool_fits): free + cost(the car being rebuilt) >=
 * keep + cost(new car); keep ROAM_CHEAP_KEEP in the cars heap, ROAM_HEAP_KEEP
 * in the main one. cost(model): the heap bytes its builds took (free before
 * and after, the largest seen, kept for the session); a model not measured
 * yet costs the largest measured (the load's average per car included), at
 * least ROAM_CAR_BYTES. The load's slot count (roam_build) uses it too.
 *
 * Teardown: a state request queued in the cruise (the state pass
 * sub_00020EC0, roam_pool_pass) may end the cruise inside that pass, and the
 * player manager's delete (sub_002BD7F0, no override) frees the world with
 * our cars in it: standalone cars are dropped first and the slots put back in
 * the physics level (the game's delete removes them from it). Pause and
 * resume requests (1..4: pause menu, full map) never end the cruise and are
 * left alone (roam_pool_pause_only, 2026-11-12). If the cruise goes on after
 * another request, the standalone cars come back one per ROAM_CARS_GAP s from
 * 1 s later, each build a short freeze: kept, the cheaper way to never leave
 * a car the game's teardown cannot see.
 *
 * Car radius (2026-11-12, untested in game): a peer gets a car only within
 * ROAM_RADIUS m of our car (3D; MC3_ROAM_RADIUS m, 0 off; off in a group
 * race) and gives it back once beyond ROAM_RADIUS + ROAM_RADIUS_OUT m for
 * ROAM_RADIUS_HOLD s (parked, free for the next peer coming in; a slot keeps
 * its slot): a full city needs only the cars of the players around us, the
 * rest are bright map blips (roam_map.c). The caps, the nearest rule and the
 * nearest-first order work inside it. '[ROAM] radius: ...' on change.
 *
 * '[ROAM] pool: N peers, C cars (S slots + T standalone), D dimmed, heap F KB'
 * every 30 s at most, when the counts change. */
#define ROAM_RC_MAX    (ROAM_K_MAX + 1)
#define ROAM_NEAR_GAIN 50.0f            /* m nearer */
#define ROAM_NEAR_HOLD 3.0              /* s, held */
#define ROAM_RADIUS      250.0f         /* m: a peer gets a car within (MC3_ROAM_RADIUS, 0 off) */
#define ROAM_RADIUS_OUT  50.0f          /* m past it: the car goes back ... */
#define ROAM_RADIUS_HOLD 3.0            /* ... after this long beyond (no churn at the edge) */
#define ROAM_COSTS     64               /* models whose cost is kept */

typedef struct roam_rc {
    uint32_t veh, res;                  /* standalone: the vehicle (0 none) and its car cache reference */
    uint32_t bslot;                     /* the builder slot it was built under (a slot: its own) */
    uint32_t pveh;                      /* the vehicle phys was read for */
    int32_t  phys;                      /* 1 in the physics level, 0 out, -1 unknown (left as built) */
    int32_t  car;                       /* its model (car index), -1 unknown */
    uint8_t  look[ROAM_CFG_SIZE];       /* the CarCfg it was built with (vtable 0) */
    uint8_t  fixed;                     /* a slot without a CarCfg of its own: never rebuilt */
    uint32_t id, gen;                   /* its peer (0 none), the peer's cfg_gen compared */
    char     name[ROAM_NAME_MAX + 1];
    float    m[12], vel[3];
    uint8_t  ctl[ROAM_CTL], has_ctl;    /* has_ctl 0: the peer sends no inputs */
    uint8_t  here, away, live, want;    /* linked now; in its garage/loading/race; posed; rebuild with the peer's look */
    double   lost;                      /* when its peer dropped out */
    double   wait;                      /* no rebuild before (one refused for heap room) */
    double   out;                       /* its peer beyond the car radius since (0: within) */
    uint32_t said;                      /* RC_SAY_* logged */
    roam_tyre tyre;
} roam_rc;

enum { RC_SAY_BADREC = 1, RC_SAY_LOOK = 2, RC_SAY_READ = 4, RC_SAY_OFF = 8, RC_SAY_ROOM = 16 };
#define ROAM_ROOM_RETRY 10.0            /* s: a rebuild refused for heap room waits; new cars and the nearest rule go first */

static roam_rc s_rc[ROAM_RC_MAX];
static uint32_t s_rc_cfg[ROAM_RC_MAX];  /* each standalone entry's CarCfg (contiguous window, made once) */
static int s_nrc = 1;                   /* entries [1, s_nrc) in use */
static int s_pool_hold, s_pool_full;    /* a state request queued: no builds; the last new car was refused */
static uint32_t s_pool_need;            /* a linked peer without a car (the queue builds one) */
static double s_pool_next;              /* no build before */
static struct { int32_t car; uint32_t bytes; } s_cost[ROAM_COSTS];
static int s_ncost;
static uint32_t s_cost_max;             /* the largest measured, 0 none */

static int roam_rc_slot(int k) { return k >= 1 && k <= s_roam_slots; }

/* The car radius in m, 0 off (MC3_ROAM_RADIUS; a group race shows its racers wherever they are). */
static float roam_pool_radius(void)
{
    static float r = -1.0f;
    if (r < 0.0f) {
        const char *e = getenv("MC3_ROAM_RADIUS");
        r = e && *e ? (float)atof(e) : ROAM_RADIUS;
        if (!(r > 0.0f))
            r = 0.0f;
        fprintf(stderr, r > 0.0f ? "[ROAM] radius: remote cars for players within %.0f m\n"
                                 : "[ROAM] radius: off (%.0f), every linked player may get a car\n", r);
    }
    return roam_group_race() ? 0.0f : r;
}

/* A peer dist m away is outside the car radius: no car for it, a bright blip without one. */
static int roam_pool_beyond(float dist)
{
    float r = roam_pool_radius();
    return r > 0.0f && dist > r;
}

static int roam_rc_say(roam_rc *c, uint32_t why)
{
    if (c->said & why)
        return 0;
    c->said |= why;
    return 1;
}

static uint32_t roam_rc_player(int k)           /* slot k's player, 0 none */
{
    uint32_t pm = MEM32(ROAM_PM_VA), p;
    if (!roam_rc_slot(k) || !online_ram(pm) || !online_ram(p = MEM32(pm + 8u + 4u * (uint32_t)k)) ||
        MEM32(p + 0x5Cu) != (uint32_t)k)
        return 0;
    return p;
}

static uint32_t roam_rc_veh(int k)              /* entry k's vehicle, 0 none */
{
    uint32_t p;
    if (!roam_rc_slot(k))
        return k > 0 && k < s_nrc ? s_rc[k].veh : 0;
    p = roam_rc_player(k);
    return p && online_ram(MEM32(p + 0x14u)) ? MEM32(p + 0x14u) : 0;
}

static int roam_rc_driven(const roam_rc *c) { return c->id && c->here && !c->away; }

static int roam_pool_of(uint32_t id)            /* the entry with a car for peer id, 0 none */
{
    int k;
    for (k = 1; id && k < s_nrc; k++)
        if (s_rc[k].id == id && roam_rc_veh(k))
            return k;
    return 0;
}

/* Peer id's car is on the road here (roam_map.c's bright blip, the pool line). */
static int roam_pool_shows(uint32_t id)
{
    int k = roam_pool_of(id);
    return k && roam_rc_driven(&s_rc[k]);
}

/* ── Heap ── */

static uint32_t roam_pool_cost(int32_t car)
{
    int i;
    for (i = 0; car >= 0 && i < s_ncost; i++)
        if (s_cost[i].car == car)
            return s_cost[i].bytes;
    return s_cost_max > ROAM_CAR_BYTES ? s_cost_max : ROAM_CAR_BYTES;   /* not measured: never below the old flat rate */
}

static void roam_pool_cost_note(int32_t car, uint32_t bytes)  /* car -1: an average (the load's) */
{
    int i;
    if (!bytes || bytes > (32u << 20))
        return;
    if (bytes > s_cost_max)
        s_cost_max = bytes;
    for (i = 0; car >= 0 && i < s_ncost && s_cost[i].car != car; i++)
        ;
    if (car < 0 || (i == s_ncost && s_ncost == ROAM_COSTS))
        return;
    if (i == s_ncost) {
        s_ncost++;
        s_cost[i].car = car;
        s_cost[i].bytes = 0;
    }
    if (bytes > s_cost[i].bytes)
        s_cost[i].bytes = bytes;
}

/* left: free bytes of the cars' heap; leave room for model car, credit freed first (a rebuild's old car) */
static int roam_pool_fits(uint32_t left, int32_t car, uint32_t credit)
{
    return (uint64_t)left + credit >= (uint64_t)(s_cheap ? ROAM_CHEAP_KEEP : ROAM_HEAP_KEEP) + roam_pool_cost(car);
}

static int roam_pool_room(int32_t car, uint32_t credit, const char *what)
{
    uint32_t heap = roam_car_heap(), left = roam_heap_log(heap, NULL), extra = 0;
    /* mid-cruise in the main heap the game keeps allocating (garage, HUD, streams): keep max(6 MB, heap / 8)
     * as before the pool, not the load's 3 MB -- for a new car only. A rebuild (credit: its old car, deleted
     * first) replaces a car the load counted, ~0 net: the load's keep. With 6 MB a phone (4.0-4.8 MB free
     * mid-cruise) refused every rebuild and showed each peer in our car (2026-11-12). */
    if (!s_cheap && heap && !credit) {
        uint32_t want = MEM32(heap + 0xCu) / 8u > (6u << 20) ? MEM32(heap + 0xCu) / 8u : (6u << 20);
        extra = want - ROAM_HEAP_KEEP;
    }
    if (left >= extra && roam_pool_fits(left - extra, car, credit))
        return 1;
    if (roam_cars_say(CARS_SAY_HEAP))
        fprintf(stderr, "[ROAM] cars: %s %u KB free, %u KB kept, ~%u KB a car: %s waits (logged once)\n",
                s_cheap ? "cars heap" : "heap", (unsigned)(left >> 10),
                (unsigned)(((s_cheap ? ROAM_CHEAP_KEEP : ROAM_HEAP_KEEP) + extra) >> 10), (unsigned)(roam_pool_cost(car) >> 10),
                what);
    return 0;
}

/* ── Car caches and the registry ──
 *
 * The game's car cache [0x50A08C] (ctor sub_00103B40: 16 entries of 0x10 B,
 * +0 car index (-1 free), +4 resource, +8 key, +9 references, +0xA read
 * pending, +0xC read handle) gives every build its own entry: its get
 * sub_00105B80 shares an entry only for a non-zero key, and the player
 * builder (sub_002CB2D0, builder vt+4) asks key 0, same model or not. With
 * no entry free the get returns 0 and the builder goes on with resource 0:
 * null vtables (sub_000E6D00 from the render init sub_000FBA90), a half-built
 * car in the vehicle registry (wild calls from its walk sub_0035DA80), the
 * main thread spinning (2026-11-12, 9 logs: always the 17th car of any kind
 * -- ours + slots + standalone cars in Free Roam, ours + 3 of the career's +
 * 12 standalone in the Online career). Standalone cars take their entries
 * from caches of our own, same layout, the game's get and release run on
 * them with [0x50A08C] swapped around the build and the delete; the game's
 * cache stays for its own cars and the slots'. */
#define ROAM_XCACHES ((ROAM_RC_MAX + 15) / 16)
#define CARS_SAY_CAP 128                /* roam_cars_say: a build refused for capacity */
static uint32_t s_xcache[ROAM_XCACHES];

static int roam_cache_used(uint32_t cache)
{
    int i, n = 0;
    for (i = 0; online_ram(cache) && i < 16; i++)
        n += SMEM32(cache + 0x10u * i) != -1;
    return n;
}

/* cache can take a build: an entry free, or res's (one reference) freed by the old car's delete first */
static int roam_cache_room(uint32_t cache, uint32_t res)
{
    uint32_t s, i;
    for (i = 0; online_ram(cache) && i < 16; i++) {
        s = cache + 0x10u * i;
        if (SMEM32(s) == -1 || (res && MEM32(s + 4u) == res && MEM8(s + 9u) == 1u))
            return 1;
    }
    return 0;
}

static uint32_t roam_xcache_of(uint32_t res)    /* our cache holding res, 0 none */
{
    int j, i;
    for (j = 0; res && j < ROAM_XCACHES; j++)
        for (i = 0; s_xcache[j] && i < 16; i++)
            if (MEM32(s_xcache[j] + 0x10u * (uint32_t)i + 4u) == res)
                return s_xcache[j];
    return 0;
}

/* One of our caches with an entry free (made on first need as sub_00103B40
 * makes the game's, never freed), 0 none. */
static uint32_t roam_xcache_pick(void)
{
    uint32_t c, i;
    int j;
    for (j = 0; j < ROAM_XCACHES; j++) {
        if (!s_xcache[j]) {
            if (!(c = xbox_ContiguousAlloc(0x200u, 16)))
                return 0;
            memset((uint8_t *)XBOX_PTR(c), 0, 0x200u);
            for (i = 0; i < 16; i++) {
                MEM32(c + 0x10u * i) = 0xFFFFFFFFu;
                MEM32(c + 0x10u * i + 0xCu) = c + 0x100u + 0x10u * i;  /* read handle: only async reads use it, ours never */
            }
            s_xcache[j] = c;
        }
        if (roam_cache_room(s_xcache[j], 0))
            return s_xcache[j];
    }
    return 0;
}

/* The cache entry k's next build takes from (a slot: the game's; a
 * standalone car: ours), 0 none free. */
static uint32_t roam_pool_cache_for(int k)
{
    uint32_t p, res, xc;
    if (roam_rc_slot(k)) {
        res = (p = roam_rc_player(k)) ? MEM32(p + 0x10u) : 0;
        return roam_cache_room(MEM32(ROAM_CACHE_VA), res) ? MEM32(ROAM_CACHE_VA) : 0;
    }
    res = k > 0 && k < s_nrc ? s_rc[k].res : 0;
    if ((xc = roam_xcache_of(res)) && roam_cache_room(xc, res))
        return xc;
    return roam_xcache_pick();
}

/* The vehicle registry's cars and *cap its capacity (0 unread). A list of k
 * bytes per capacity holds k/4 entries a car (base Add sub_0035D500: +0x40 one
 * per wheel, 4 a car), so each list's count is read in cars, rounded up, and
 * the fullest taken (its raw count, 12 for 3 cars, logged as 12/10). */
static uint32_t roam_reg_used(uint32_t *cap)
{
    uint32_t reg = MEM32(ROAM_REG_VA), n = 0, i, per, c;
    *cap = online_ram(reg) ? MEM32(reg + ROAM_REG_CAP) : 0;
    for (i = 0; *cap && i < sizeof s_roam_reg_lists / sizeof s_roam_reg_lists[0]; i++) {
        per = s_roam_reg_lists[i].k / 4u;
        c = (MEM32(reg + s_roam_reg_lists[i].arr - 4u) + per - 1u) / per;
        if (c > n)
            n = c;
    }
    return n;
}

/* The particle manager [0x4D6308] (made by sub_000337A0, 0x408 B): 256
 * emitter pointers at +4, their count at +0x404 -- right after the array.
 * Its adds (sub_00033A00, sub_000339A0, sub_00033980) never check the count:
 * the 257th overwrites the count with the emitter's pointer and every add
 * after writes an emitter pointer at mgr + 4 * pointer, anywhere. Each car's
 * effects (sub_001167A0, from the render init sub_000FBA90) add 9 (smoke,
 * exhaust, ...: +0x18 +0xC +0x10, two sub_00033AD0, +0x14 +0x2C +0x34 +0x38);
 * its delete removes them (sub_001139C0 -> sub_00033910). The game's own
 * load (8 cars + the world's) stays far below; 33 cars did not: a 25-player
 * Free Roam (2026-11-12) built the 27th car at count 256, its first emitter
 * read back from the wild index (ICALL 0x83F05E2C from 0x00113A88, the effects
 * reset sub_00113A60), then the wild writes broke the vehicle registry's
 * objects (hundreds of ICALLs from its walk sub_0035DA80 @0x0035DC7B) and
 * 'divide by zero'. A new car waits unless count + its cost + a margin for
 * the game's own runtime adds fit; cost measured on each build. */
#define ROAM_EMIT_VA   0x004D6308u
#define ROAM_EMIT_MAX  256u
#define ROAM_EMIT_KEEP 24u              /* emitters left for the game's own adds (traffic, world effects) */
static uint32_t s_emit_car = 9u;        /* emitters a car adds (measured, the largest seen) */

static uint32_t roam_emit_count(void)   /* 0 unread */
{
    uint32_t m = MEM32(ROAM_EMIT_VA);
    return online_ram(m) ? MEM32(m + 0x404u) : 0;
}

/* After a batch of deletes: the emitters they gave back (9 a car; the delete's
 * render-object dtor sub_000FEB10 -> sub_001139C0 -> sub_00033910 removes them).
 * 21 crash logs of 2026-11-12 overflowed at the 28th/29th live vehicle whatever
 * the session's build/delete history (2WBJ4F: ~120 builds, ~67 deletes first):
 * no leak; this line keeps checking it. */
static void roam_emit_said(uint32_t before, int n)
{
    uint32_t now = roam_emit_count();
    fprintf(stderr, "[ROAM] cars: particle emitters %u/%u after delete (%d car%s, %d given back)\n", (unsigned)now,
            ROAM_EMIT_MAX, n, n == 1 ? "" : "s", (int)before - (int)now);
}

static void roam_pool_capacity(const char *when)
{
    uint32_t cap, used = roam_reg_used(&cap);
    int j, xu = 0;
    for (j = 0; j < ROAM_XCACHES; j++)
        xu += roam_cache_used(s_xcache[j]);
    fprintf(stderr, "[ROAM] cars: capacity: car cache %d/16, remote car caches %d/%d, vehicle registry %u/%u, particle "
                    "emitters %u/%u (%u a car) (%s)\n", roam_cache_used(MEM32(ROAM_CACHE_VA)), xu, ROAM_XCACHES * 16,
            (unsigned)used, (unsigned)cap, (unsigned)roam_emit_count(), ROAM_EMIT_MAX, (unsigned)s_emit_car, when);
}

/* Entry k can be built: a car cache entry and (a new car; a rebuild deletes
 * its old one first, its emitters too) a registry place and particle
 * emitters. Refused: logged once a cruise. */
static int roam_pool_fit(int k, const char *what)
{
    uint32_t cap, used = roam_reg_used(&cap);
    if (roam_pool_cache_for(k) && (roam_rc_veh(k) ||
        ((!cap || used < cap) && roam_emit_count() + s_emit_car + ROAM_EMIT_KEEP <= ROAM_EMIT_MAX)))
        return 1;
    if (roam_cars_say(CARS_SAY_CAP)) {
        fprintf(stderr, "[ROAM] cars: %s (entry %d) waits: no car cache entry or vehicle registry place (logged once)\n",
                what, k);
        roam_pool_capacity("refused");
    }
    return 0;
}

/* ── Looks ── */

/* A read of car pending in the cache, not done yet (the garage's async
 * reads; none of ours): the build would take it half read. */
static int roam_pool_reading(int32_t car)
{
    uint32_t cache = MEM32(ROAM_CACHE_VA), s, h, ev, i;
    for (i = 0; online_ram(cache) && i < 16; i++) {
        s = cache + 0x10u * i;
        if (SMEM32(s) != car || MEM8(s + 8u) || !MEM8(s + 0xAu))
            continue;
        h = MEM32(s + 0xCu);
        ev = online_ram(h) ? MEM32(h + 0xCu) : 0;
        return ev && !(roam_call(sub_001A1AD0, 0, (g_esp - 0x100u) & ~0xFu, 1, &ev) & 0xFFu);
    }
    return 0;
}

/* The look a car for peer id is built with, into blob (*car its model): the
 * peer's when its car is in the car list and, mid-cruise, it is no motorbike
 * (roam_car_bike); else ours (stand_in), or none (a rebuild keeps its old
 * car). 0: no look. (Any model takes one car cache entry: roam_pool_fit.) */
static int roam_pool_look(uint32_t id, uint8_t blob[ROAM_CFG_SIZE + 4], int32_t *car, int stand_in, int mid)
{
    uint32_t setup = MEM32(ROAM_SETUP_VA), cfg0 = online_ram(setup) ? MEM32(setup + ROAM_ENTRY + 0x0Cu) : 0;
    *car = id && roam_peer_rec(id, blob) ? roam_car_index(roam_cfg_name(blob)) : -1;
    if (*car >= 0 && !(mid && roam_car_bike(*car)))
        return 1;
    if (!stand_in || !online_ram(cfg0) || MEM32(cfg0) != ROAM_CFG_VT)
        return 0;
    memcpy(blob, (uint8_t *)XBOX_PTR(cfg0), ROAM_CFG_SIZE);
    *car = roam_car_index(roam_cfg_name(blob));
    if (mid && roam_car_bike(*car)) {
        if (roam_cars_say(CARS_SAY_BIKE))
            fprintf(stderr, "[ROAM] cars: no motorbike is built in the running cruise (its rider waits for the "
                            "city's streaming to stop) and ours is one: no stand-in car (logged once)\n");
        return 0;
    }
    return 1;
}

/* ── Build, drop, park ── */

/* Entry k's car in (on 1) or out of the physics level. Never out while a
 * state request is queued: the cruise may end in that pass, and the game's
 * player delete takes the car out of the level itself (sub_000FFB40). */
static void roam_rc_phys(roam_rc *c, uint32_t veh, int on)
{
    uint32_t sm = MEM32(0x00557ED0u);
    if (!online_ram(veh))
        return;
    if (veh != c->pveh) {                       /* built or rebuilt: in the level as the game left it */
        c->pveh = veh;
        c->phys = roam_car_inlevel(veh) ? 1 : -1;
    }
    if (!on && online_ram(sm) && MEM32(sm + 0x3Cu))
        return;
    roam_veh_phys(veh, &c->phys, on, (g_esp - 0x400u) & ~0xFu);
}

/* A standalone car out of the world (physics level, drawing, vehicle
 * registry) and deleted; mid: with the build context (mid-cruise). */
static void roam_rc_drop(roam_rc *c, int mid)
{
    uint32_t top = (g_esp - 0x400u) & ~0xFu, veh = c->veh, reg, zero = 0, xc = roam_xcache_of(c->res);
    uint32_t keepc = MEM32(ROAM_CACHE_VA);
    if (!veh)
        return;
    if (mid && !roam_build_ctx(1))
        mid = 0;
    if (veh != c->pveh) {
        c->pveh = veh;
        c->phys = roam_car_inlevel(veh) ? 1 : -1;
    }
    if (c->phys != 0)                           /* in the level, or as the game built it: the game's own way */
        roam_call(sub_000FFB40, veh, top, 0, NULL);
    else {                                      /* sub_000FFB40 without the physics remove */
        roam_call(sub_00092EF0, MEM32(veh + 0xCu) + 8u, top, 1, &zero);
        if (online_ram(reg = roam_call(sub_003308C0, 0, top, 0, NULL)))
            roam_call(recomp_lookup(MEM32(MEM32(reg) + 8u)), reg, top, 1, &veh);
        MEM8(veh + 0x60u) = 1;
    }
    if (xc)                                     /* its entry is in our cache: the game's release runs on that */
        MEM32(ROAM_CACHE_VA) = xc;
    roam_car_delete(veh, c->res, top);
    MEM32(ROAM_CACHE_VA) = keepc;
    if (mid)
        roam_build_ctx(0);
    c->veh = c->res = c->pveh = 0;
    c->live = 0;
}

/* Entry k's car parked: hidden, out of the physics level, 30 km off and
 * 500 m under our car (apart per entry), its drawn matrix too (out of the
 * level nothing else moves it). The minimap pins a blip beyond its radius to
 * its rim (sub_000B95B0) and skips an undrawn car ([[veh+0xC]+0x12D] 0), so
 * no ghost blip; out of the level, a parked car on the start spot no longer
 * blocks ours unseen (2026-11-10). Before our history exists: our car's
 * matrix (player 0). */
static void roam_rc_park(int k, uint32_t veh)
{
    roam_rc *c = &s_rc[k];
    uint32_t p0 = MEM32(MEM32(ROAM_PM_VA) + 8u), v0, m0, i;
    float m[12], vel[3] = { 0.0f, 0.0f, 0.0f };
    if (!online_ram(veh))
        return;
    roam_show(veh, 0);
    roam_rc_phys(c, veh, 0);
    c->live = 0;
    if (s_roam_nhist)
        memcpy(m, s_roam_hist[(s_roam_nhist - 1) % ROAM_HIST].m, sizeof m);
    else if (online_ram(p0) && online_ram(v0 = MEM32(p0 + 0x14u)) && online_ram(m0 = MEM32(v0 + 0x1Cu)))
        for (i = 0; i < 12; i++)
            m[i] = MEMF(m0 + 0x10u + 4u * i);
    else
        return;
    m[9] += 30000.0f + 50.0f * (float)k;
    m[10] -= 500.0f;
    roam_set_pose(veh, m, vel);
    roam_park_inst(veh, k);
}

/* Entry k (re)built with look (car its model; vtable word ignored): a slot's
 * old car out as a quitting peer's (sub_000FFB40) and deleted as the player
 * dtor does, the entry's CarCfg = look (operator=); a standalone's dropped,
 * its own CarCfg = look. Built as the player ctor builds (roam_car_build),
 * hidden. mid: in the running cruise (the garage's build context). 0: not
 * built. */
static int roam_rc_make(int k, const uint8_t look[ROAM_CFG_SIZE], int32_t car, int mid)
{
    roam_rc *c = &s_rc[k];
    uint32_t setup = MEM32(ROAM_SETUP_VA), e = setup + ROAM_ENTRY + (uint32_t)k * ROAM_ENTRY_SIZE;
    uint32_t top = (g_esp - 0x400u) & ~0xFu, heap = roam_car_heap(), p = 0, pad = 0xFFFFFFFFu, cfg, veh, res, before, after;
    uint32_t xc, keepc, em;
    double t0 = roam_now();
    if (!online_ram(setup) || !online_ram(heap) || !roam_bslots())
        return 0;
    if (roam_rc_slot(k)) {
        cfg = MEM32(e + 0x0Cu);
        if (!(p = roam_rc_player(k)) || !online_ram(cfg) || MEM32(cfg) != ROAM_CFG_VT ||
            cfg == MEM32(setup + ROAM_ENTRY + 0x0Cu) || !online_ram(MEM32(p + 0x14u)))
            return 0;
        pad = MEM32(e + 0x10u);
    } else if (!(cfg = s_rc_cfg[k]) && !(cfg = s_rc_cfg[k] = xbox_ContiguousAlloc(ROAM_CFG_SIZE, 16)))
        return 0;
    /* no cache entry: nothing touched (the builder would build on resource 0, a half car the game keeps) */
    if (!(xc = roam_pool_cache_for(k)))
        return 0;
    if (mid && !roam_build_ctx(1))
        return 0;
    if (p) {
        veh = MEM32(p + 0x14u);
        roam_rc_phys(c, veh, 1);                /* the game's removal expects it in the level */
        roam_call(sub_000FFB40, veh, top, 0, NULL);
        roam_car_delete(veh, MEM32(p + 0x10u), top);
        MEM32(p + 0x14u) = MEM32(p + 0x10u) = 0;
        roam_cfg_set(cfg, look);
        c->bslot = (uint32_t)k;
    } else {
        roam_rc_drop(c, 0);
        memcpy((uint8_t *)XBOX_PTR(cfg), look, ROAM_CFG_SIZE);
        MEM32(cfg) = ROAM_CFG_VT;
        /* +8 is the CarCfg's dirty mask: a record carries the sender's, 0 once
         * its car was built. operator= (sub_00271E20) sets 0x3FFF; so do we
         * (bit 0x80: the wheel step, else bare rims). */
        MEM32(cfg + 8u) = 0x3FFFu;
        /* a builder slot 1..roam_bslots() (several cars share one): its entry has pad/viewport/controller -1 (the
         * career cruise, no slots: entry 1, borrowed by roam_car_build) */
        c->bslot = 1u + (uint32_t)(k - s_roam_slots - 1) % (uint32_t)roam_bslots();
    }
    before = roam_heap_log(heap, NULL);
    keepc = MEM32(ROAM_CACHE_VA);
    MEM32(ROAM_CACHE_VA) = xc;                  /* a standalone car's entry from our cache (a slot's: the game's) */
    em = roam_emit_count();                     /* the old car's emitters gone: what this build adds */
    veh = roam_car_build(cfg, c->bslot, pad, top, &res);
    if (veh && em && roam_emit_count() > em && roam_emit_count() - em > s_emit_car && roam_emit_count() - em < 64u)
        s_emit_car = roam_emit_count() - em;
    if (!veh && p && roam_cfg_name(c->look)) {  /* the slot's old car is gone: built again, else it stays carless all cruise */
        roam_cfg_set(cfg, c->look);
        if ((veh = roam_car_build(cfg, c->bslot, pad, top, &res)))
            fprintf(stderr, "[ROAM] cars: slot %d: %s not built; its old car (%s) built again, it stands in\n", k,
                    roam_cfg_name(look) ? roam_cfg_name(look) : "?", roam_cfg_name(c->look));
        look = c->look;
        car = c->car;
    }
    MEM32(ROAM_CACHE_VA) = keepc;
    after = roam_heap_log(heap, NULL);
    if (mid)
        roam_build_ctx(0);
    if (p) {
        MEM32(p + 0x14u) = veh;
        MEM32(p + 0x10u) = res;
    } else {
        c->veh = veh;
        c->res = res;
    }
    if (!veh) {
        if (roam_cars_say(CARS_SAY_FAIL)) {
            fprintf(stderr, "[ROAM] cars: %s %d not built (builder gave no vehicle; logged once)\n", p ? "slot" : "car", k);
            roam_pool_capacity("after the failed build");
        }
        return 0;
    }
    if (after < before)
        roam_pool_cost_note(car, before - after);
    c->car = car;
    if (look != c->look) {
        memcpy(c->look, look, ROAM_CFG_SIZE);
        c->gen = 0;                             /* its peer's record compared again with the new car */
    }                                           /* old car back: gen kept, no rebuild until the peer's next record */
    memset(c->look, 0, 4);
    c->want = 0;
    c->said = 0;
    c->pveh = veh;
    c->phys = roam_car_inlevel(veh) ? 1 : -1;   /* not found in the level: never touched */
    if (c->phys < 0 && roam_cars_say(CARS_SAY_PHYS))
        fprintf(stderr, "[ROAM] cars: %s %d not found in the physics level after its build: its collisions left as "
                        "built (logged once)\n", p ? "slot" : "car", k);
    if (mid)
        roam_rc_park(k, veh);                   /* shown once driven */
    else
        roam_show(veh, 0);
    fprintf(stderr, "[ROAM] cars: %s %d built (%s, model %d, builder slot %u%s) in %.0f ms: %u KB, %s %u KB free, "
                    "main heap %u KB free\n", p ? "slot" : "car", k, roam_cfg_name(c->look) ? roam_cfg_name(c->look) : "?",
            (int)car, (unsigned)c->bslot, mid ? ", in the cruise" : "", (roam_now() - t0) * 1e3,
            (unsigned)(after < before ? (before - after) >> 10 : 0), s_cheap ? "cars heap" : "heap", (unsigned)(after >> 10),
            (unsigned)(roam_heap_log(MEM32(ROAM_MAIN_HEAP_VA), NULL) >> 10));
    roam_pool_capacity(p ? "slot built" : "car built");
    return 1;
}

/* ── Peers ── */

/* Peer pp's interpolated pose, newest inputs and status into entry k; a new
 * car record against the look k was built with (another asks a rebuild). */
static void roam_rc_take(int k, const roam_peer *pp)
{
    roam_rc *c = &s_rc[k];
    uint8_t blob[ROAM_CFG_SIZE + 4], away = roam_away(pp->status);
    c->here = 1;
    if (away != c->away) {
        c->away = away;
        c->live = 0;                            /* snapped onto its pose when it drives again */
        fprintf(stderr, "[ROAM] %s %d: %s %s\n", roam_rc_slot(k) ? "slot" : "car", k, pp->name,
                !away ? "drives again: car shown"
                : pp->status == RC_ST_SHOP    ? "is in the garage: car hidden, kept"
                : pp->status == RC_ST_LOADING ? "is loading: car hidden, kept"
                : pp->status == RC_ST_EVENT   ? "is in an AI race: car hidden, kept"
                                              : "is in the menus (not placed yet): car hidden, kept");
    }
    roam_quat_mat(pp->q, c->m);
    memcpy(c->m + 9, pp->p, sizeof pp->p);
    memcpy(c->vel, pp->v, sizeof pp->v);
    c->has_ctl = pp->ext_len == ROAM_CTL;
    if (c->has_ctl)
        memcpy(c->ctl, pp->ext, ROAM_CTL);
    if (!pp->cfg_gen || pp->cfg_gen == c->gen || c->fixed)
        return;
    if (!roam_peer_rec(pp->id, blob)) {         /* not marked compared: looked at again next frame */
        if (roam_rc_say(c, RC_SAY_BADREC))
            fprintf(stderr, "[ROAM] %s %d: %s's car record unusable (%d B, want %u, or no car name): no rebuild\n",
                    roam_rc_slot(k) ? "slot" : "car", k, pp->name, roam_peer_cfg(pp->id, blob, ROAM_CFG_SIZE + 4),
                    (unsigned)ROAM_CFG_SIZE);
        return;
    }
    c->gen = pp->cfg_gen;
    c->said &= ~(uint32_t)RC_SAY_BADREC;
    c->want = !roam_cfg_same(blob, c->look);
    fprintf(stderr, "[ROAM] %s %d: %s's car record %u: %s, built with %s: %s\n", roam_rc_slot(k) ? "slot" : "car", k,
            pp->name, (unsigned)pp->cfg_gen, roam_cfg_name(blob),
            roam_cfg_name(c->look) ? roam_cfg_name(c->look) : "?", c->want ? "differs, rebuild queued" : "the same");
}

/* Entry k is pp's from now on (a newcomer, or a hand-over to a nearer peer). */
static void roam_rc_give(int k, const roam_peer *pp, const char *why)
{
    roam_rc *c = &s_rc[k];
    c->id = pp->id;
    c->live = c->away = c->want = 0;
    c->wait = c->out = 0;
    c->gen = 0;
    c->said = 0;
    snprintf(c->name, sizeof c->name, "%s", pp->name);
    fprintf(stderr, "[ROAM] %s %d: %s (id %08X)%s\n", roam_rc_slot(k) ? "slot" : "car", k, c->name, (unsigned)c->id,
            why);
    roam_rc_take(k, pp);
}

/* The nearest rule (s_pool_full: no car can be built): peers[cand], the
 * nearest driving peer without a car, takes the farthest holder's car once
 * ROAM_NEAR_GAIN m nearer for ROAM_NEAR_HOLD s; no flips at the edge. The
 * old peer goes the newcomer way next frame. */
static void roam_pool_nearest(const roam_peer *peers, int n, int cand, double now)
{
    static double since;
    float far = -1.0f, d;
    int i, k, from = -1, to = 0;
    for (k = 1; cand >= 0 && k < s_nrc; k++) {
        if (!s_rc[k].id || !s_rc[k].here || !roam_rc_veh(k))
            continue;
        for (i = 0; i < n && peers[i].id != s_rc[k].id; i++)
            ;
        if (i < n && (d = roam_away(peers[i].status) ? 1e9f : peers[i].dist) > far) {
            far = d;
            from = i;
            to = k;
        }
    }
    if (cand < 0 || from < 0 || far < peers[cand].dist + ROAM_NEAR_GAIN) {
        since = 0;
        return;
    }
    if (!since)
        since = now;
    if (now - since < ROAM_NEAR_HOLD)
        return;
    since = 0;
    fprintf(stderr, "[ROAM] nearest: %s %d goes from %s (%.0f m%s) to %s (%.0f m)\n", roam_rc_slot(to) ? "slot" : "car",
            to, peers[from].name, peers[from].dist, roam_away(peers[from].status) ? ", away" : "", peers[cand].name,
            peers[cand].dist);
    roam_rc_give(to, &peers[cand], " (nearer)");
}

/* In our player's update (roam_net), the linked peers (nearest first): each
 * keeps its car; newcomers take one; the rest wait for a new one or the
 * nearest rule. */
static void roam_pool_net(const roam_peer *peers, int n, double now)
{
    static int k_was;                           /* the hub's K follows a live Players shown (roam_begin's rule) */
    uint8_t used[ROAM_K_MAX] = { 0 }, blob[ROAM_CFG_SIZE + 4];
    const char *pname;
    int at[ROAM_RC_MAX];                        /* entry k's peer index this frame */
    int i, k, best, bs, sc, ok, cand = -1, held = 0, lim = roam_cars_total(), kk = lim > 32 ? lim : 32;
    float rad = roam_pool_radius();
    if (k_was && kk != k_was && roam_running())
        roam_set_k(kk);
    k_was = kk;
    if (n > ROAM_K_MAX)
        n = ROAM_K_MAX;
    s_pool_need = 0;
    for (k = 1; k < s_nrc; k++) {
        roam_rc *c = &s_rc[k];
        if (!c->id)
            continue;
        for (i = 0; i < n && (used[i] || peers[i].id != c->id); i++)
            ;
        if (i < n && rad > 0.0f && peers[i].dist > rad + ROAM_RADIUS_OUT) {
            if (!c->out)
                c->out = now;
            else if (now - c->out >= ROAM_RADIUS_HOLD) {    /* beyond the radius: its car parked, free for another */
                fprintf(stderr, "[ROAM] %s %d: %s parked (%.0f m, beyond the %.0f m car radius)\n",
                        roam_rc_slot(k) ? "slot" : "car", k, c->name, peers[i].dist, rad);
                c->id = 0;
                c->here = c->live = 0;
                c->out = 0;
                continue;
            }
        } else
            c->out = 0;
        if (i < n) {
            used[i] = 1;
            at[k] = i;
            held++;
            roam_rc_take(k, &peers[i]);
        } else if (c->here) {                   /* dropped out: parked, held a while */
            c->here = 0;
            c->lost = now;
            c->live = 0;
        } else if (now - c->lost > ROAM_HOLD)
            c->id = 0;
    }
    /* A live lower Players shown (in-game Settings): the farthest holders over it lose their car (parked, free
     * for a nearer one); no newcomer gets a car while lim are held, the nearest rule swaps instead. */
    while (held > lim) {
        for (k = 1, best = 0, bs = -1; k < s_nrc; k++)
            if (s_rc[k].id && s_rc[k].here && at[k] > bs) {
                bs = at[k];
                best = k;
            }
        fprintf(stderr, "[ROAM] %s %d: %s parked (Players shown %d)\n", roam_rc_slot(best) ? "slot" : "car", best,
                s_rc[best].name, lim);
        s_rc[best].id = 0;
        s_rc[best].here = s_rc[best].live = 0;
        held--;
    }
    for (i = 0; i < n; i++) {
        if (used[i] || (rad > 0.0f && peers[i].dist > rad))  /* beyond the car radius: a blip only */
            continue;
        if (held >= lim) {
            if (cand < 0 && peers[i].status == RC_ST_ROAM)
                cand = i;
            continue;
        }
        for (k = 1, best = 0, bs = 0, ok = -1; k < s_nrc; k++) {
            roam_rc *c = &s_rc[k];
            if ((c->id && c->here) || !roam_rc_veh(k))
                continue;
            if (c->id)                          /* held for a peer that dropped out */
                sc = 2;
            else {
                if (ok < 0)
                    ok = roam_peer_rec(peers[i].id, blob);
                pname = ok ? roam_cfg_name(blob) : NULL;
                sc = !pname ? 4 : roam_cfg_same(blob, c->look) ? 8
                   : roam_cfg_name(c->look) && !strcmp(pname, roam_cfg_name(c->look)) ? 6 : 4;
            }
            sc += roam_rc_slot(k);
            if (sc > bs || (sc == bs && c->id && c->lost < s_rc[best].lost)) {
                bs = sc;
                best = k;
            }
        }
        if (best) {
            used[i] = 1;
            held++;
            roam_rc_give(best, &peers[i], "");
            continue;
        }
        if (!s_pool_need)
            s_pool_need = peers[i].id;
        if (cand < 0 && peers[i].status == RC_ST_ROAM)
            cand = i;
    }
    roam_pool_nearest(peers, n, s_pool_full || held >= lim ? cand : -1, now);
}

/* ── The queue ── */

static int roam_pool_pagein(double now)        /* another thread inside a page-in: wait (3 s at most) */
{
    static double since;
    if (!MEM32(ROAM_PAGEIN_VA)) {
        since = 0;
        return 0;
    }
    if (!since)
        since = now;
    if (now - since >= 3.0)
        return 0;
    if (roam_cars_say(CARS_SAY_PAGEIN))
        fprintf(stderr, "[ROAM] cars: build waits: a resource page-in is in flight ([0x500F10] %08X; logged once)\n",
                (unsigned)MEM32(ROAM_PAGEIN_VA));
    return 1;
}

static void roam_pool_rebuild(int k, double now)
{
    roam_rc *c = &s_rc[k];
    uint8_t blob[ROAM_CFG_SIZE + 4];
    int32_t car;
    const char *old, *got;
    char want[32];
    if (s_roam_swap_on < 0) {
        const char *e = getenv("MC3_ROAM_SWAP");
        s_roam_swap_on = !(e && e[0] == '0');
    }
    if (!s_roam_swap_on) {
        if (roam_rc_say(c, RC_SAY_OFF))
            fprintf(stderr, "[ROAM] %s %d: other car, but rebuilds are off (MC3_ROAM_SWAP=0)\n",
                    roam_rc_slot(k) ? "slot" : "car", k);
        c->want = 0;
        return;
    }
    s_pool_next = now + ROAM_CARS_GAP;
    old = roam_cfg_name(c->look) ? roam_cfg_name(c->look) : "?";
    if (!roam_pool_look(c->id, blob, &car, 0, 1)) {
        if (roam_rc_say(c, RC_SAY_LOOK))
            fprintf(stderr, "[ROAM] look: %s %d peer %s wants %s (model %d) -> kept %s (not in the car list or a "
                            "motorbike (no rider mid-cruise))\n",
                    roam_rc_slot(k) ? "slot" : "car", k, c->name,
                    roam_peer_rec(c->id, blob) && roam_cfg_name(blob) ? roam_cfg_name(blob) : "?", (int)car, old);
        c->want = 0;                            /* until the peer's next car record */
        return;
    }
    if (roam_pool_reading(car)) {
        if (roam_rc_say(c, RC_SAY_READ))
            fprintf(stderr, "[ROAM] %s %d: rebuild waits for model %d's pending read\n", roam_rc_slot(k) ? "slot" : "car",
                    k, (int)car);
        return;
    }
    if (!roam_pool_room(car, roam_pool_cost(c->car), "a rebuild")) {
        /* the old car stands in; new cars and the nearest rule are not held behind it (it was retried every 2 s
         * before them, so a full heap gave no new car and never set s_pool_full) */
        if (roam_rc_say(c, RC_SAY_ROOM))
            fprintf(stderr, "[ROAM] look: %s %d peer %s wants %s (cost %u KB) -> kept %s (heap %u KB free + %u KB "
                            "freed < %u KB kept + cost; tried again every %.0f s)\n", roam_rc_slot(k) ? "slot" : "car",
                    k, c->name, roam_cfg_name(blob) ? roam_cfg_name(blob) : "?", (unsigned)(roam_pool_cost(car) >> 10),
                    old, (unsigned)(roam_heap_log(roam_car_heap(), NULL) >> 10),
                    (unsigned)(roam_pool_cost(c->car) >> 10),
                    (unsigned)((s_cheap ? ROAM_CHEAP_KEEP : ROAM_HEAP_KEEP) >> 10), ROAM_ROOM_RETRY);
        c->wait = now + ROAM_ROOM_RETRY;
        s_pool_full = 1;
        s_pool_next = now + ROAM_CARS_GAP;
        return;
    }
    if (!roam_pool_fit(k, "a rebuild")) {       /* the old car stands in, as for heap room */
        c->wait = now + ROAM_ROOM_RETRY;
        s_pool_full = 1;
        return;
    }
    c->said &= ~(uint32_t)RC_SAY_ROOM;
    snprintf(want, sizeof want, "%s", roam_cfg_name(blob) ? roam_cfg_name(blob) : "?");
    if (!roam_rc_make(k, blob, car, 1)) {
        fprintf(stderr, "[ROAM] look: %s %d peer %s wants %s (cost %u KB) -> not rebuilt (build failed)\n",
                roam_rc_slot(k) ? "slot" : "car", k, c->name, want, (unsigned)(roam_pool_cost(car) >> 10));
        c->want = 0;
        c->id = 0;                              /* its peer waits for a car like a newcomer */
        return;
    }
    got = roam_cfg_name(c->look) ? roam_cfg_name(c->look) : "?";
    fprintf(stderr, "[ROAM] look: %s %d peer %s wants %s (cost %u KB) -> %s %s (%s)\n", roam_rc_slot(k) ? "slot" : "car",
            k, c->name, want, (unsigned)(roam_pool_cost(car) >> 10), strcmp(got, want) ? "kept" : "built", got,
            strcmp(got, want) ? "its build failed, the old car built again" : "rebuilt");
}

/* A new standalone car for s_pool_need: a dropped entry first, else a new one. */
static void roam_pool_grow(double now)
{
    static double said = -1e9;                  /* the last failed build logged */
    uint8_t blob[ROAM_CFG_SIZE + 4];
    int32_t car;
    int k;
    uint32_t reg = MEM32(ROAM_REG_VA), cap = online_ram(reg) ? MEM32(reg + ROAM_REG_CAP) : 0;
    s_pool_next = now + ROAM_CARS_GAP;
    for (k = s_roam_slots + 1; k < s_nrc && s_rc[k].veh; k++)
        ;
    /* a new entry also needs room in the vehicle registry, sized at the cruise's load (roam_cars_registry) for
     * the Players shown of then: a live raise beyond it waits for the next load */
    if (!roam_bslots() || (k == s_nrc && (s_nrc - 1 >= roam_cars_total() || s_nrc >= ROAM_RC_MAX
                                         || 8u + (uint32_t)s_nrc + 2u > cap))) {
        if (roam_cars_say(CARS_SAY_MAX)) {
            fprintf(stderr, "[ROAM] cars: all %d remote cars in use (Players shown %d, vehicle registry %u): the "
                            "nearest get them (logged once)\n", s_nrc - 1, roam_cars_total(), (unsigned)cap);
            roam_pool_capacity("all in use");
        }
        s_pool_full = 1;
        s_pool_next = now + 2.0;
        return;
    }
    if (!roam_pool_look(s_pool_need, blob, &car, 1, 1) || !roam_pool_room(car, 0, "a new car") ||
        !roam_pool_fit(k, "a new car")) {
        s_pool_full = 1;
        s_pool_next = now + 2.0;
        return;
    }
    memset(&s_rc[k], 0, sizeof s_rc[k]);
    s_rc[k].car = -1;
    if (k == s_nrc)
        s_nrc++;
    if (roam_rc_make(k, blob, car, 1)) {
        s_pool_full = 0;                        /* the newcomer pick gives it to its peer next frame */
        /* one car per ask: the next net pass asks again if still needed. While no pass runs (pause menu: the
         * player update stops, this queue does not) a stale ask built a car every 0.25 s for nobody (2026-11-12:
         * 29 unassigned cars in 8 s, 33 in all, the particle manager overflowed) */
        s_pool_need = 0;
        return;
    }
    /* failed: the nearest rule takes over; no full build again every 0.25 s */
    if (now - said >= 30.0) {                   /* 30 s apart at most */
        said = now;
        fprintf(stderr, "[ROAM] cars: a new car for peer %08X (%s) not built: the nearest get the cars, tried again "
                        "in 2 s\n", (unsigned)s_pool_need, roam_cfg_name(blob) ? roam_cfg_name(blob) : "?");
    }
    s_pool_full = 1;
    s_pool_next = now + 2.0;
}

static void roam_pool_job(double now)
{
    int k;
    if (s_pool_hold || s_roam_fake || now < s_pool_next || roam_pool_pagein(now))
        return;
    for (k = 1; k < s_nrc && !(s_rc[k].want && now >= s_rc[k].wait && s_rc[k].id && s_rc[k].here && roam_rc_veh(k)); k++)
        ;
    if (k < s_nrc)
        roam_pool_rebuild(k, now);
    else if (s_pool_need)
        roam_pool_grow(now);
}

/* ── Load, frame, teardown ── */

/* Before the player manager is built (roam_build): the last cruise's table
 * gone; a standalone car left over (its end not seen) out of the registry. */
static void roam_pool_reset(void)
{
    uint32_t top = (g_esp - 0x400u) & ~0xFu, reg;
    int k;
    for (k = 1; k < s_nrc; k++)
        if (!roam_rc_slot(k) && s_rc[k].veh) {
            if (online_ram(reg = roam_call(sub_003308C0, 0, top, 0, NULL)))
                roam_call(recomp_lookup(MEM32(MEM32(reg) + 8u)), reg, top, 1, &s_rc[k].veh);
            fprintf(stderr, "[ROAM] cars: car %d left over from the last cruise (its end not seen): out of the "
                            "registry\n", k);
        }
    /* every standalone car is gone (dropped, or left over above): an entry still in our caches is orphaned; freed
     * here as empty (its model's bytes not freed: the left-over car may still point at them) */
    for (k = 0; k < ROAM_XCACHES * 16; k++) {
        uint32_t e = s_xcache[k / 16] ? s_xcache[k / 16] + 0x10u * (uint32_t)(k % 16) : 0;
        if (!e || SMEM32(e) == -1)
            continue;
        fprintf(stderr, "[ROAM] cars: remote car cache entry %d (model %d) left from the last cruise: emptied\n", k,
                (int)SMEM32(e));
        MEM32(e) = 0xFFFFFFFFu;
        MEM32(e + 4u) = 0;
        MEM8(e + 8u) = MEM8(e + 9u) = MEM8(e + 0xAu) = 0;
    }
    memset(s_rc, 0, sizeof s_rc);
    for (k = 0; k < ROAM_RC_MAX; k++)
        s_rc[k].car = -1;
    s_nrc = 1;
    s_pool_hold = s_pool_full = 0;
    s_pool_need = 0;
    s_pool_next = 0;
    s_cars_said = 0;
}

/* roam_build: slot k is built with look (NULL: ours, no CarCfg of its own,
 * never rebuilt), pp's from the start (NULL: free). */
static void roam_pool_slot_init(int k, const uint8_t *look, const roam_peer *pp)
{
    roam_rc *c = &s_rc[k];
    c->bslot = (uint32_t)k;
    c->fixed = !look;
    if (look) {
        memcpy(c->look, look, ROAM_CFG_SIZE);
        memset(c->look, 0, 4);
        c->car = roam_car_index(roam_cfg_name(look));
    }
    if (!pp)
        return;
    c->id = pp->id;
    c->here = 1;
    c->gen = pp->cfg_gen;
    snprintf(c->name, sizeof c->name, "%s", pp->name);
    roam_quat_mat(pp->q, c->m);
    memcpy(c->m + 9, pp->p, sizeof pp->p);
    memcpy(c->vel, pp->v, sizeof pp->v);
}

/* ── Race ghosts (s_roam_ghosts, roam_game.c) ──
 *
 * At the race's load only: the ROAM_GHOSTS nearest linked players (Players
 * shown fewer) with a car record, each a standalone car with its own look,
 * taken out of the physics level at once (still in it: dropped, no ghost),
 * fixed to its peer for the race (no hand-over, no rebuild, no build later: a
 * player without one stays a blip). Builds stop at the heap or capacity
 * check, a failed build, or ROAM_GHOST_LOAD s of builds (the load's freeze). */
#define ROAM_GHOSTS     6               /* ghosts at most in a race */
#define ROAM_GHOST_LOAD 2.0             /* s of ghost builds at a race load at most */

static void roam_ghost_load(const roam_peer *peers, int np)
{
    uint8_t blob[ROAM_CFG_SIZE + 4];
    uint32_t top = (g_esp - 0x400u) & ~0xFu;
    int32_t car;
    int i, built = 0, norec = 0, cap = roam_cars_total() < ROAM_GHOSTS ? roam_cars_total() : ROAM_GHOSTS;
    double t0 = roam_now();
    const char *stop = "every linked player";
    for (i = 0; i < np; i++) {
        roam_rc *c = &s_rc[s_nrc];
        if (built >= cap) {
            stop = "the cap";
            break;
        }
        if (roam_now() - t0 >= ROAM_GHOST_LOAD) {
            stop = "the build time cap";
            break;
        }
        if (peers[i].status == RC_ST_MENUS || peers[i].status == RC_ST_SHOP || peers[i].status >= RC_ST__N)
            continue;                           /* not placed, or back in another car: a blip */
        if (!roam_pool_look(peers[i].id, blob, &car, 0, 0)) {
            norec++;                            /* no car record (yet): a blip, never ours stood in */
            continue;
        }
        if (!roam_pool_room(car, 0, "a race ghost")) {
            stop = "the heap";
            break;
        }
        if (!roam_pool_fit(s_nrc, "a race ghost")) {
            stop = "capacity";
            break;
        }
        if (!roam_rc_make(s_nrc, blob, car, 0)) {
            stop = "a failed build";
            break;
        }
        roam_veh_phys(c->veh, &c->phys, 0, top);
        if (c->phys != 0) {                     /* a car others could hit: none */
            roam_rc_drop(c, 0);
            stop = "the physics level (car not taken out)";
            break;
        }
        roam_rc_give(s_nrc, &peers[i], " (race ghost)");
        s_nrc++;
        built++;
    }
    fprintf(stderr, "[ROAM] race ghosts: %d built at load (cap %d, %s %u KB free) in %.0f ms; %d players linked, %d "
                    "without a car record; stopped at %s\n", built, cap, s_cheap ? "cars heap" : "heap",
            (unsigned)(roam_heap_log(roam_car_heap(), NULL) >> 10), (roam_now() - t0) * 1e3, np, norec, stop);
}

/* veh is one of our race's ghosts (sub_000A5890's water rule: a parked one sits 500 m under us, "in the sea", and
 * the vehicle update would open the pause menu for its builder slot in our race). */
static int roam_ghost_veh(uint32_t veh)
{
    int k;
    for (k = 1; s_roam_ghosts && veh && k < s_nrc; k++)
        if (s_rc[k].veh == veh)
            return 1;
    return 0;
}

/* In our player's update (roam_net) during a race: each ghost follows its own peer, parked while it is not linked. */
static void roam_ghost_net(const roam_peer *peers, int n, double now)
{
    int i, k;
    for (k = 1; k < s_nrc; k++) {
        roam_rc *c = &s_rc[k];
        if (!c->id || !c->veh)
            continue;
        for (i = 0; i < n && peers[i].id != c->id; i++)
            ;
        if (i < n)
            roam_rc_take(k, &peers[i]);
        else if (c->here) {
            c->here = c->live = 0;
            c->lost = now;
        }
    }
}

/* After the physics step: each driven ghost on its peer's pose, body and drawn matrix both (out of the physics level
 * nothing syncs them), its inputs into its controller (wheels, steering); never into the physics level. */
static void roam_ghost_frame(void)
{
    uint32_t veh, mtx, cc, i;
    int k;
    for (k = 1; k < s_nrc; k++) {
        roam_rc *c = &s_rc[k];
        if (!(veh = c->veh))
            continue;
        roam_tyres(&c->tyre, veh, "ghost", k);
        if (!roam_rc_driven(c)) {
            roam_rc_park(k, veh);
            continue;
        }
        roam_set_pose(veh, c->m, c->vel);
        if (online_ram(mtx = MEM32(veh + 0x1Cu)))
            for (i = 0; i < 12; i++)
                MEMF(mtx + 0x10u + 4u * i) = c->m[i];
        if (!c->live) {
            roam_show(veh, 1);
            c->live = 1;
        }
        cc = MEM32(veh + 4u);
        if (c->has_ctl && online_ram(cc) && MEM32(cc + 0x60u) == c->bslot)
            roam_remote(0, cc, c->m, c->vel, c->ctl);
    }
}

/* After the cruise's player cars are built (sub_002BE2C0): standalone cars
 * for the peers known now that no slot took, + ROAM_SPARE. A race: its
 * ghosts instead. */
static void roam_pool_load(void)
{
    static roam_peer peers[ROAM_K_MAX];
    uint8_t blob[ROAM_CFG_SIZE + 4];
    int32_t car;
    int i = 0, np, want = 0, cap = roam_cars_total() - s_roam_slots, built = 0;
    s_nrc = 1 + s_roam_slots;
    if (!roam_bslots() || s_roam_fake || !roam_running() || cap <= 0)
        return;
    if (cap > ROAM_RC_MAX - s_nrc)
        cap = ROAM_RC_MAX - s_nrc;
    np = roam_peers(peers, ROAM_K_MAX);
    if (s_roam_race) {
        if (s_roam_ghosts)
            roam_ghost_load(peers, np);
        return;
    }
    for (i = 0; i < np; i++)
        want += !roam_pool_of(peers[i].id);
    if (want)                                   /* spares only once the slots are full: a free slot takes a newcomer first */
        want += ROAM_SPARE;
    if (want > cap)
        want = cap;
    for (i = 0; built < want; built++) {
        while (i < np && roam_pool_of(peers[i].id))
            i++;
        if (!roam_pool_look(i < np ? peers[i].id : 0, blob, &car, 1, 0) || !roam_pool_room(car, 0, "a car at load") ||
            !roam_pool_fit(s_nrc, "a car at load"))
            break;
        i++;
        if (!roam_rc_make(s_nrc, blob, car, 0))
            break;
        s_nrc++;
    }
    fprintf(stderr, "[ROAM] cars: %d standalone car%s at load (%d remote cars at most, %d slots; %d peers known)\n",
            built, built == 1 ? "" : "s", roam_cars_total(), s_roam_slots, np);
}

/* Each player's update (roam_player), slot k: driven the System Link way
 * (roam_remote), snapped onto a new pose first; parked without a driving
 * peer. MC3_ROAM_FAKE: our own path 1-3 s late, 3 m to the side. */
static void roam_pool_slot(uint32_t p, int k, double now)
{
    roam_rc *c = &s_rc[k];
    uint32_t veh = MEM32(p + 0x14u), cc, i;
    float m[12], vel[3];
    uint8_t ctl[ROAM_CTL], has_ctl = 1;
    if (!online_ram(veh))
        return;
    roam_tyres(&c->tyre, veh, "slot", k);
    if (s_roam_fake) {
        float side = (k & 1) ? 3.0f : -3.0f;
        double delay = 1.0 + (s_roam_fake > 1 ? 2.0 * (k - 1) / (s_roam_fake - 1) : 0.0);
        if (!roam_hist_at(now - delay, m, vel, ctl))
            return;                             /* stays on its starting spot until then */
        for (i = 0; i < 3; i++)
            m[9 + i] += m[i] * side;            /* right row */
    } else if (roam_rc_driven(c)) {
        memcpy(m, c->m, sizeof m);
        memcpy(vel, c->vel, sizeof vel);
        memcpy(ctl, c->ctl, ROAM_CTL);
        has_ctl = c->has_ctl;
    } else {
        roam_rc_park(k, veh);
        return;
    }
    cc = MEM32(veh + 4u);
    if (!c->live) {
        roam_set_pose(veh, m, vel);
        roam_rc_phys(c, veh, 1);
        roam_show(veh, 1);
        c->live = 1;
    }
    roam_trace((uint32_t)k, veh, cc, m, vel, has_ctl ? ctl : NULL, now);
    roam_remote(p, cc, m, vel, has_ctl && online_ram(cc) && MEM32(cc + 0x60u) == (uint32_t)k ? ctl : NULL);
}

static void roam_pool_report(const char *when)
{
    int k;
    fprintf(stderr, "[ROAM] report (%s): %d slots, %d standalone cars\n", when, s_roam_slots,
            s_nrc - 1 > s_roam_slots ? s_nrc - 1 - s_roam_slots : 0);
    for (k = 1; k < s_nrc; k++)
        if (roam_rc_slot(k) || s_rc[k].veh)
            roam_car_line(roam_rc_slot(k) ? "slot" : "car", k, s_rc[k].id, roam_rc_driven(&s_rc[k]), s_rc[k].live,
                          roam_rc_veh(k), s_rc[k].phys);
}

/* Once a second: the pool line when its counts changed, 30 s apart at most. */
static void roam_pool_log(double now)
{
    static double next, said_t = -1e9;
    static int said[4] = { -1, -1, -1, -1 };
    int cur[4] = { 0, 0, 0, 0 }, i, k;          /* peers, slots driven, standalone driven, dimmed */
    if (now < next)
        return;
    next = now + 1.0;
    if (s_roam_seen && now - s_roam_seen_t < 2.0)
        cur[0] = s_roam_nseen;
    for (k = 1; k < s_nrc; k++)
        if (roam_rc_driven(&s_rc[k]) && roam_rc_veh(k))
            cur[roam_rc_slot(k) ? 1 : 2]++;
    for (i = 0; i < cur[0]; i++)
        if (s_roam_seen[i].status < RC_ST__N &&
            (s_roam_seen[i].status != RC_ST_ROAM ||
             (!roam_pool_shows(s_roam_seen[i].id) && !roam_pool_beyond(s_roam_seen[i].dist))))
            cur[3]++;
    if (roam_pool_radius() > 0.0f) {            /* the car radius line: on change, 10 s apart at most */
        static double rad_t = -1e9;
        static int rad_said[2] = { -1, -1 };
        int far = 0, in;
        for (i = 0; i < cur[0]; i++)
            far += roam_pool_beyond(s_roam_seen[i].dist);
        in = cur[0] - far;
        if ((in != rad_said[0] || far != rad_said[1]) && now - rad_t >= 10.0) {
            rad_said[0] = in;
            rad_said[1] = far;
            rad_t = now;
            fprintf(stderr, "[ROAM] radius: %d peers in %.0f m, %d cars, %d far\n", in, roam_pool_radius(),
                    cur[1] + cur[2], far);
        }
    }
    if (!memcmp(cur, said, sizeof cur) || now - said_t < 30.0)
        return;
    memcpy(said, cur, sizeof cur);
    said_t = now;
    fprintf(stderr, "[ROAM] pool: %d peers, %d cars (%d slots + %d standalone), %d dimmed, heap %u KB\n", cur[0],
            cur[1] + cur[2], cur[1], cur[2], cur[3], (unsigned)(roam_heap_log(roam_car_heap(), NULL) >> 10));
}

/* Online career switched in game (mc3_live MC3_LIVE_CAREER, the in-game Settings row), after the physics step:
 * only in the career's running cruise (game mode 2, state 2, no request queued), never Free Roam's (s_online).
 * Off: the standalone cars dropped as at a teardown (roam_pool_pass), the session closed (its BYE: the page closes
 * the city's socket; blips, name tags and the players panel go with s_career_roam); the career untouched. On: the
 * session as at the cruise load (roam_career_join), the pool's queue then builds the cars one at a time with its
 * heap and capacity checks (roam_pool_job). Anywhere else the next cruise load reads the setting. */
static void roam_career_live(void)
{
    uint32_t setup = MEM32(ROAM_SETUP_VA);
    int on = roam_career_on(), k, n = 0;
    uint32_t em;
    if (s_online || on == s_career_roam || MEM32(0x0053F2D4u) != 2u || !s_state_obj ||
        MEM32(s_state_obj + 4u) != 2u || MEM32(s_state_obj + 0x3Cu) || !online_ram(MEM32(ROAM_PM_VA)) ||
        !online_ram(setup) || !roam_career_setup(setup))
        return;
    if (on) {
        s_roam_nhist = 0;                       /* roam_player records from its next frame */
        s_roam_tlast = 0.0;
        fprintf(stderr, "[CAREER-ONLINE] live on\n");
        roam_career_join(setup, "live on");
        return;
    }
    em = roam_emit_count();
    for (k = s_roam_slots + 1; k < s_nrc; k++)
        if (s_rc[k].veh) {
            roam_rc_drop(&s_rc[k], 1);
            n++;
        }
    if (n)
        roam_emit_said(em, n);
    roam_pool_reset();
    s_career_roam = s_career_ev = s_roam_cruise = 0;
    s_roam_nhist = 0;
    s_roam_seen = NULL;
    s_roam_nseen = 0;
    s_rep_at = 0.0;
    roam_stop();
    fprintf(stderr, "[CAREER-ONLINE] live off: %d standalone car%s removed, session closed (the career goes on)\n", n,
            n == 1 ? "" : "s");
}

/* After the physics step (sub_002BDEC0): reports, the queue, every undriven
 * car parked (again: the game's update may have moved it), each standalone
 * car driven: its pose toward the peer's (tau ROAM_TAU, snap ROAM_SNAP),
 * inputs into its controller, in the physics level within ROAM_CAR_NEAR m. */
static void roam_pool_frame(void)
{
    float me[12], m[12], vel[3], d[3], dist, f;
    uint32_t veh, mtx, cc;
    double now;
    int i, k;
    roam_flow_watch();
    roam_career_live();
    if (!s_roam_cruise || !s_roam_nhist || !online_ram(MEM32(ROAM_PM_VA)))
        return;
    if (s_roam_race) {                          /* a race: its ghosts only (built at load), no queue */
        if (s_roam_ghosts)
            roam_ghost_frame();
        return;
    }
    now = roam_now();
    if (s_rep_at && now >= s_rep_at) {
        roam_pool_report(s_rep_n == 2 ? "load + 0.5 s" : "load + 5 s");
        s_rep_at = --s_rep_n > 0 ? now + 4.5 : 0.0;
    }
    roam_pool_job(now);
    roam_pool_log(now);
    memcpy(me, s_roam_hist[(s_roam_nhist - 1) % ROAM_HIST].m, sizeof me);
    f = 1.0f - expf(-s_roam_dt / ROAM_TAU);
    for (k = 1; k < s_nrc; k++) {
        roam_rc *c = &s_rc[k];
        if (!(veh = roam_rc_veh(k)) || (s_roam_fake && roam_rc_slot(k)))
            continue;
        if (!roam_rc_slot(k))
            roam_tyres(&c->tyre, veh, "car", k);
        if (!roam_rc_driven(c)) {
            roam_rc_park(k, veh);
            continue;
        }
        if (roam_rc_slot(k))                    /* driven in its player's update */
            continue;
        memcpy(m, c->m, sizeof m);
        memcpy(vel, c->vel, sizeof vel);
        mtx = MEM32(veh + 0x1Cu);
        if (c->live && online_ram(mtx)) {       /* blend the position from where the step left it */
            for (i = 0; i < 3; i++)
                d[i] = m[9 + i] - MEMF(mtx + 0x34u + 4u * i);
            if (d[0] * d[0] + d[2] * d[2] < ROAM_SNAP * ROAM_SNAP)
                for (i = 0; i < 3; i++)
                    m[9 + i] -= d[i] * (1.0f - f);
        }
        roam_set_pose(veh, m, vel);
        if (!c->live) {
            roam_show(veh, 1);
            c->live = 1;
        }
        cc = MEM32(veh + 4u);
        if (c->has_ctl && online_ram(cc) && MEM32(cc + 0x60u) == c->bslot)
            roam_remote(0, cc, m, vel, c->ctl);
        for (i = 0; i < 3; i++)
            d[i] = m[9 + i] - me[9 + i];
        dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dist < ROAM_CAR_NEAR)
            roam_rc_phys(c, veh, 1);
        else if (dist > ROAM_CAR_FAR)
            roam_rc_phys(c, veh, 0);
    }
}

/* The state machine's pass (sub_00020EC0; after 0 before the game's, 1
 * after): a queued state request may end the cruise inside it (see the head
 * comment). Builds wait until the queue is empty and the state is the
 * cruise's again. */
/* Every queued request (ring of 10 at obj+0xC, read index +0x38, count +0x3C;
 * sub_00020E40) is 1..4: pause and resume. Their handlers (sub_00020AF0 /
 * sub_00020B70 / sub_00020BE0, request 4 inline) only set the pause flags
 * (+8/+9/+0xA) and call each player's pause methods and sub_002BDB10; none
 * deletes the player manager, so the cruise goes on with our cars in it. */
static int roam_pool_pause_only(uint32_t obj)
{
    uint32_t n = MEM32(obj + 0x3Cu), head = MEM32(obj + 0x38u), i, r;
    for (i = 0; i < n && i < 10u; i++)
        if ((r = MEM32(obj + 0xCu + 4u * ((head + 1u + i) % 10u))) < 1u || r > 4u)
            return 0;
    return n <= 10u;
}

static void roam_pool_pass(uint32_t obj, int after)
{
    static uint32_t state;
    uint32_t em;
    int k, n = 0;
    if (!online_ram(obj) || s_nrc <= 1)
        return;
    if (!after) {
        if (!MEM32(obj + 0x3Cu) || !online_ram(MEM32(ROAM_PM_VA)))
            return;
        if (roam_pool_pause_only(obj)) {        /* pause menu, full map: cars kept, no rebuild after the resume */
            static int said;
            if (!said++)
                fprintf(stderr, "[ROAM] cars: a pause/resume request (%u): remote cars kept (logged once)\n",
                        (unsigned)MEM32(obj + 0xCu + 4u * ((MEM32(obj + 0x38u) + 1u) % 10u)));
            return;
        }
        for (k = 1; k <= s_roam_slots; k++)     /* parked slots back in: the game's delete expects them there */
            roam_rc_phys(&s_rc[k], roam_rc_veh(k), 1);
        em = roam_emit_count();
        for (k = s_roam_slots + 1; k < s_nrc; k++)
            if (s_rc[k].veh) {
                roam_rc_drop(&s_rc[k], 1);
                n++;
            }
        if (n)
            roam_emit_said(em, n);
        for (k = s_roam_slots + 1; k < s_nrc; k++)
            s_rc[k].id = 0;
        if (n) {
            state = MEM32(obj + 4u);
            fprintf(stderr, "[ROAM] cars: %d standalone car%s removed (state request %u: the cruise may end; their "
                            "peers have no car until the rebuilds)\n", n, n == 1 ? "" : "s",
                    (unsigned)MEM32(obj + 0xCu + 4u * ((MEM32(obj + 0x38u) + 1u) % 10u)));
        }
        s_pool_hold = 1;
        return;
    }
    if (s_pool_hold && !MEM32(obj + 0x3Cu) && online_ram(MEM32(ROAM_PM_VA)) && (!state || MEM32(obj + 4u) == state)) {
        s_pool_hold = 0;
        s_pool_next = roam_now() + 1.0;         /* the cruise goes on: cars built again */
    }
}
