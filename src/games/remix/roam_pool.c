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
 * roam_pool_look (the car list; no motorbike mid-cruise, roam_car_bike; the
 * car cache's 16 models) and roam_pool_room (the heap). A rebuild that cannot
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
 * '[ROAM] pool: N peers, C cars (S slots + T standalone), D dimmed, heap F KB'
 * every 30 s at most, when the counts change. */
#define ROAM_RC_MAX    (ROAM_K_MAX + 1)
#define ROAM_NEAR_GAIN 50.0f            /* m nearer */
#define ROAM_NEAR_HOLD 3.0              /* s, held */
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

/* ── Looks ── */

/* The car cache (key byte 0, as the builder asks) can take car: it holds it,
 * a slot is unreferenced, or old's slot frees with that car's delete. */
static int roam_pool_cache_ok(int32_t car, int32_t old)
{
    uint32_t cache = MEM32(ROAM_CACHE_VA), s, i;
    if (!online_ram(cache))
        return 0;
    for (i = 0; i < 16; i++) {
        s = cache + 0x10u * i;
        if ((SMEM32(s) == car && !MEM8(s + 8u)) || !MEM8(s + 9u) ||
            (old >= 0 && SMEM32(s) == old && !MEM8(s + 8u) && MEM8(s + 9u) == 1u))
            return 1;
    }
    return 0;
}

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
 * peer's when its car is in the car list, the car cache can take it and,
 * mid-cruise, it is no motorbike (roam_car_bike); else ours (stand_in), or
 * none (a rebuild keeps its old car). 0: no look. */
static int roam_pool_look(uint32_t id, uint8_t blob[ROAM_CFG_SIZE + 4], int32_t *car, int32_t old, int stand_in,
                          int mid)
{
    uint32_t setup = MEM32(ROAM_SETUP_VA), cfg0 = online_ram(setup) ? MEM32(setup + ROAM_ENTRY + 0x0Cu) : 0;
    *car = id && roam_peer_rec(id, blob) ? roam_car_index(roam_cfg_name(blob)) : -1;
    if (*car >= 0 && !(mid && roam_car_bike(*car)) && roam_pool_cache_ok(*car, old))
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
    uint32_t top = (g_esp - 0x400u) & ~0xFu, veh = c->veh, reg, zero = 0;
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
    roam_car_delete(veh, c->res, top);
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
    veh = roam_car_build(cfg, c->bslot, pad, top, &res);
    if (!veh && p && roam_cfg_name(c->look)) {  /* the slot's old car is gone: built again, else it stays carless all cruise */
        roam_cfg_set(cfg, c->look);
        if ((veh = roam_car_build(cfg, c->bslot, pad, top, &res)))
            fprintf(stderr, "[ROAM] cars: slot %d: %s not built; its old car (%s) built again, it stands in\n", k,
                    roam_cfg_name(look) ? roam_cfg_name(look) : "?", roam_cfg_name(c->look));
        look = c->look;
        car = c->car;
    }
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
        if (roam_cars_say(CARS_SAY_FAIL))
            fprintf(stderr, "[ROAM] cars: %s %d not built (builder gave no vehicle; logged once)\n", p ? "slot" : "car", k);
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
    c->wait = 0;
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
        if (used[i])
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
    if (!roam_pool_look(c->id, blob, &car, c->car, 0, 1)) {
        if (roam_rc_say(c, RC_SAY_LOOK))
            fprintf(stderr, "[ROAM] look: %s %d peer %s wants %s (model %d) -> kept %s (not in the car list, a "
                            "motorbike (no rider mid-cruise) or the car cache full (16 models))\n",
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
        if (roam_cars_say(CARS_SAY_MAX))
            fprintf(stderr, "[ROAM] cars: all %d remote cars in use (Players shown %d, vehicle registry %u): the "
                            "nearest get them (logged once)\n", s_nrc - 1, roam_cars_total(), (unsigned)cap);
        s_pool_full = 1;
        s_pool_next = now + 2.0;
        return;
    }
    if (!roam_pool_look(s_pool_need, blob, &car, -1, 1, 1) || !roam_pool_room(car, 0, "a new car")) {
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

/* After the cruise's player cars are built (sub_002BE2C0): standalone cars
 * for the peers known now that no slot took, + ROAM_SPARE. */
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
    for (i = 0; i < np; i++)
        want += !roam_pool_of(peers[i].id);
    if (want)                                   /* spares only once the slots are full: a free slot takes a newcomer first */
        want += ROAM_SPARE;
    if (want > cap)
        want = cap;
    for (i = 0; built < want; built++) {
        while (i < np && roam_pool_of(peers[i].id))
            i++;
        if (!roam_pool_look(i < np ? peers[i].id : 0, blob, &car, -1, 1, 0) || !roam_pool_room(car, 0, "a car at load"))
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
            (s_roam_seen[i].status != RC_ST_ROAM || !roam_pool_shows(s_roam_seen[i].id)))
            cur[3]++;
    if (!memcmp(cur, said, sizeof cur) || now - said_t < 30.0)
        return;
    memcpy(said, cur, sizeof cur);
    said_t = now;
    fprintf(stderr, "[ROAM] pool: %d peers, %d cars (%d slots + %d standalone), %d dimmed, heap %u KB\n", cur[0],
            cur[1] + cur[2], cur[1], cur[2], cur[3], (unsigned)(roam_heap_log(roam_car_heap(), NULL) >> 10));
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
    if (!s_roam_cruise || !s_roam_nhist || !online_ram(MEM32(ROAM_PM_VA)))
        return;
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
        for (k = s_roam_slots + 1; k < s_nrc; k++)
            if (s_rc[k].veh) {
                roam_rc_drop(&s_rc[k], 1);
                n++;
            }
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
