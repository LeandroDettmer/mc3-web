/* Free roam, many cars (docs/roam-many-cars.md steps 1-5 and 9): remote
 * players beyond the 7 player slots, as standalone cars.
 *
 * Included by recomp_manual.c after roam_game.c. The nearest peers take the
 * player slots (roam_game.c, System Link's remote path); every other peer the
 * hub links us with gets a standalone car: built with the game's player
 * builder (roam_car_build: the same calls as the player ctor) but owned by no
 * player, so nothing of the game updates it as a remote player. We do:
 * after the player manager's update (sub_002BDEC0, physics stepped) its pose
 * goes toward the peer's (tau ROAM_TAU, snap ROAM_SNAP) through SetMatrix /
 * SetVel (roam_set_pose), and its inputs into its controller as for a slot
 * (roam_remote without a player: wheels steer and spin). Unused cars are
 * parked 30 km off, under the map, hidden, out of the physics level.
 *
 * How many: MC3_ROAM_CARS=N remote cars at most in all (slots included;
 * 0 or <= 7: the slots only, as before). Unset (auto): 32 with a 128 MB
 * contiguous window (RECOMP_RAM_MB=128), 12 with 64 MB. That many links are
 * asked of the hub (roam_set_k; MC3_ROAM_K still overrides). Cars are built
 * at the cruise's load for the peers known then (+ ROAM_SPARE), later one at
 * a time as peers come (mid-cruise with the garage's resource context,
 * roam_build_ctx), and only while the game's heap keeps ROAM_CARS_RESERVE
 * free after one more car (its cost measured on the last one): a car that
 * cannot be built is skipped with a '[ROAM] cars:' line, never an error.
 *
 * The builder's slot: a standalone car is built under a slot number
 * 1..s_roam_slots (several cars share one), whose race-setup entry has
 * pad/viewport/controller -1 (roam_build): never 0 (ours) nor >= 8.
 * roam_build_step gives it a controller as for the slot's car.
 *
 * Assignment (as nfsu2-web's): a peer keeps its car while linked and
 * ROAM_HOLD s after; a newcomer takes a free car with its exact look, else
 * any free car (rebuilt with its look), else the longest-unseen held car,
 * else a new car. A peer's new car record rebuilds its car (one rebuild or
 * build per ROAM_CARS_GAP s).
 *
 * Collision: a car is in the physics level ([0x5580F0], add vt+0x70
 * sub_002ABD00(collider [car+0x68], 0), remove vt+0x14 sub_002AD320(handle
 * word [[veh+0x1C]+8])) only within ROAM_CAR_NEAR m of us, out beyond
 * ROAM_CAR_FAR: the level's pool stays small. Membership is read from the
 * level (handle -> entry +8 == collider); a car whose first check fails is
 * never touched (left as built).
 *
 * The cruise's end: the player manager's delete (sub_002BD7F0, from the
 * cruise teardown sub_00029690, a state exit) frees the world while our
 * cars are still in it and has no override. The state machine's pass
 * (sub_00020EC0, overridden) runs every state change: with a state request
 * queued ([obj+0x3C]) our cars are taken out and deleted first; if the
 * cruise goes on they are built again (hold until the queue is empty and the
 * state is the cruise's). A car left over anyway (end not seen) is taken out
 * of the vehicle registry at the next load.
 *
 * The vehicle registry ([0x5638E4], 0x98 B, vtable 0x454F04) is made once at
 * boot (sub_0001DC80 -> sub_000FF0D0 -> ctor sub_000FEDC0(10), base ctor
 * sub_0035D200) with capacity 10 at +0x7C. Every car the game builds is added
 * to it (sub_002CBC40 @0x2CBDBD -> vt+4 sub_000FEEA0, AI cars sub_001FB040),
 * and Add never checks the capacity: the 8 player cars use 8 of 10, a car
 * more past 10 writes beyond its arrays. Readers (via getter sub_003308C0:
 * sub_002BDEC0 walks it with vt+0x10 sub_000FEF80 / vt+0x14 sub_000FF0A0;
 * removes sub_000FEF10) loop by each list's count, never by the capacity.
 * Its 18 lists are {count, array} pairs; each array is capacity * k bytes
 * (k below, from the two ctors). Enlarging = a new array per list, the old
 * contents copied, the capacity raised. The arrays come from the contiguous
 * window (xbox_ContiguousAlloc), never freed: the registry lives as long as
 * the game; the old arrays stay in the game heap (~1.2 KB, not freed: a
 * Free from here could run with another heap current). */

#include "kernel/xbox_memory_layout.h"

extern uint32_t g_xbox_contig_size;             /* kernel.h: the contiguous window's size */

#define ROAM_REG_VA   0x005638E4u       /* the vehicle registry */
#define ROAM_REG_CAP  0x7Cu             /* its capacity */
#define ROAM_PHYS_VA  0x005580F0u       /* the physics level */
#define ROAM_PM_VA    0x00558814u       /* the player manager (0: no cruise) */
#define ROAM_CARS_MAX (ROAM_K_MAX - 1)  /* standalone cars at most (at least one slot) */
#define ROAM_CAR_NEAR 20.0f             /* m: into the physics level within */
#define ROAM_CAR_FAR  30.0f             /* m: out of it beyond */
#define ROAM_CARS_GAP 0.25              /* s between two builds/rebuilds */
#define ROAM_CARS_RESERVE (6u << 20)    /* heap bytes kept free (at least; heap / 8 if more) */
#define ROAM_CAR_COST0 (512u << 10)     /* a car's heap cost before one was measured */

void sub_00092EF0(void);                        /* a drawable in/out of the draw list, thiscall (bool), ret 4 */
void sub_003308C0(void);                        /* the vehicle registry, cdecl; eax */

static const struct { uint8_t arr, k; } s_roam_reg_lists[] = {   /* array field, bytes per capacity */
    { 0x08, 4 },  { 0x10, 4 },  { 0x18, 4 },  { 0x20, 4 },  { 0x28, 4 },  { 0x30, 4 },
    { 0x38, 8 },  { 0x40, 16 }, { 0x48, 32 }, { 0x50, 8 },  { 0x58, 4 },  { 0x60, 4 },
    { 0x68, 4 },  { 0x70, 4 },  { 0x78, 4 },                  /* base sub_0035D200 */
    { 0x84, 4 },  { 0x8C, 4 },  { 0x94, 4 },                  /* sub_000FEDC0 */
};

typedef struct roam_car {
    uint32_t veh, res;                          /* the vehicle (0: none built), its car cache reference */
    uint32_t slot;                              /* the builder slot it was built under */
    int32_t  phys;                              /* 1 in the physics level, 0 out, -1 unknown (left as built) */
    uint8_t  look[ROAM_CFG_SIZE];               /* the CarCfg it was built with (vtable 0) */
    uint32_t id, gen;                           /* its peer (0 none), the peer's cfg_gen compared */
    char     name[ROAM_NAME_MAX + 1];
    float    m[12], vel[3];
    uint8_t  ctl[ROAM_CTL], has_ctl;
    uint8_t  here, away, live, want;            /* want: rebuild with the peer's look */
    double   lost;                              /* when its peer dropped out */
} roam_car;

static roam_car s_car[ROAM_CARS_MAX];
static uint32_t s_car_cfg[ROAM_CARS_MAX];       /* each car's own CarCfg (contiguous window, made once) */
static int s_ncar;                              /* cars [0, s_ncar) used (veh 0: dropped, rebuilt on demand) */
static int s_cars_on;                           /* standalone cars this cruise */
static int s_cars_hold;                         /* 1: a state request was queued, no builds */
static uint32_t s_cars_need;                    /* a linked peer without a car (built next) */
static uint32_t s_car_cost;                     /* heap bytes the last car took */
static double s_cars_next;                      /* no build/rebuild before */
static uint32_t s_cars_said;                    /* reasons already logged */
enum { CARS_SAY_HEAP = 1, CARS_SAY_MAX = 2, CARS_SAY_POOL = 4, CARS_SAY_PHYS = 8, CARS_SAY_FAIL = 16,
       CARS_SAY_PAGEIN = 32 };

/* Remote cars shown at most, slots included: MC3_ROAM_CARS, else by the window. */
static int roam_cars_total(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("MC3_ROAM_CARS");
        n = e && *e && strcmp(e, "auto") ? atoi(e) : g_xbox_contig_size >= (128u << 20) ? 32 : 12;
        n = n < ROAM_SLOTS ? ROAM_SLOTS : n > ROAM_K_MAX ? ROAM_K_MAX : n;
    }
    return n;
}

/* Before the cruise's cars are built: registry capacity >= 8 players + the
 * standalone cars. Once per game (later calls find it big enough). */
static void roam_cars_registry(void)
{
    uint32_t reg = MEM32(ROAM_REG_VA), cap, want, i, old, neu, k;
    if (roam_cars_total() <= ROAM_SLOTS || !online_ram(reg))
        return;
    cap = MEM32(reg + ROAM_REG_CAP);
    want = 8u + (uint32_t)roam_cars_total() + 2u;
    if (cap >= want || cap == 0 || cap > 256u)
        return;
    for (i = 0; i < sizeof s_roam_reg_lists / sizeof s_roam_reg_lists[0]; i++)
        if (!online_ram(MEM32(reg + s_roam_reg_lists[i].arr))) {
            fprintf(stderr, "[ROAM] cars: registry list +0x%X has no array (%08X): left at %u\n",
                    s_roam_reg_lists[i].arr, (unsigned)MEM32(reg + s_roam_reg_lists[i].arr), (unsigned)cap);
            return;
        }
    for (i = 0; i < sizeof s_roam_reg_lists / sizeof s_roam_reg_lists[0]; i++) {
        k = s_roam_reg_lists[i].k;
        old = MEM32(reg + s_roam_reg_lists[i].arr);
        if (!(neu = xbox_ContiguousAlloc(want * k, 16))) {
            fprintf(stderr, "[ROAM] cars: no memory for the registry (list %u): left at %u\n", (unsigned)i,
                    (unsigned)cap);
            want = 0;
            break;                              /* lists done so far keep their bigger arrays: still valid */
        }
        memset((uint8_t *)XBOX_PTR(neu), 0, want * k);
        memcpy((uint8_t *)XBOX_PTR(neu), (uint8_t *)XBOX_PTR(old), cap * k);
        MEM32(reg + s_roam_reg_lists[i].arr) = neu;
    }
    if (!want)
        return;
    MEM32(reg + ROAM_REG_CAP) = want;
    fprintf(stderr, "[ROAM] cars: vehicle registry %u -> %u (%d remote cars at most)\n", (unsigned)cap,
            (unsigned)want, roam_cars_total());
}

static int roam_cars_say(uint32_t why)
{
    if (s_cars_said & why)
        return 0;
    s_cars_said |= why;
    return 1;
}

static int roam_cars_owns(uint32_t id)
{
    int i;
    for (i = 0; id && i < s_ncar; i++)
        if (s_car[i].id == id && s_car[i].veh)
            return 1;
    return 0;
}

/* The car's collider is in the physics level: its handle's entry holds it. */
static int roam_car_inlevel(uint32_t veh)
{
    uint32_t lvl = MEM32(ROAM_PHYS_VA), car = MEM32(veh + 8u), inst = MEM32(veh + 0x1Cu), phys, map, ents, h, e;
    if (!online_ram(lvl) || !online_ram(car) || !online_ram(phys = MEM32(car + 0x68u)) || !online_ram(inst))
        return 0;
    h = MEM16(inst + 8u);
    map = MEM32(lvl + 0x24u);
    ents = MEM32(lvl + 0x40u);
    if (h == 0xFFFFu || !online_ram(map) || !online_ram(ents) || !online_ram(map + 2u * h))
        return 0;
    e = ents + MEM16(map + 2u * h) * 0x27Cu;
    return online_ram(e + 8u) && MEM32(e + 8u) == phys;
}

/* Car c in (on 1) or out of the physics level, if we manage it. */
static void roam_car_phys(roam_car *c, int on, uint32_t top)
{
    uint32_t lvl = MEM32(ROAM_PHYS_VA), vt, a[2];
    if (c->phys < 0 || c->phys == on || !online_ram(lvl) || !online_ram(vt = MEM32(lvl)))
        return;
    if (on) {
        a[0] = MEM32(MEM32(c->veh + 8u) + 0x68u);
        a[1] = 0;
        roam_call(recomp_lookup(MEM32(vt + 0x70u)), lvl, top, 2, a);
    } else {
        a[0] = MEM16(MEM32(c->veh + 0x1Cu) + 8u);
        roam_call(recomp_lookup(MEM32(vt + 0x14u)), lvl, top, 1, a);
    }
    c->phys = roam_car_inlevel(c->veh);
    if (c->phys != on && roam_cars_say(on ? CARS_SAY_POOL : CARS_SAY_PHYS))
        fprintf(stderr, "[ROAM] cars: physics level %s refused (car left %s; logged once)\n",
                on ? "add" : "remove", on ? "without collisions" : "in it");
}

/* Car c out of the world (physics level, drawing, vehicle registry) and
 * deleted; mid: with the build context (mid-cruise). */
static void roam_car_drop(roam_car *c, int mid)
{
    uint32_t top = (g_esp - 0x400u) & ~0xFu, veh = c->veh, reg;
    if (!veh)
        return;
    if (mid && !roam_build_ctx(1))
        mid = 0;
    if (c->phys != 0)                           /* in the level, or as the game built it: the game's own way */
        roam_call(sub_000FFB40, veh, top, 0, NULL);
    else {                                      /* sub_000FFB40 without the physics remove */
        uint32_t one = 0;
        roam_call(sub_00092EF0, MEM32(veh + 0xCu) + 8u, top, 1, &one);
        if (online_ram(reg = roam_call(sub_003308C0, 0, top, 0, NULL)))
            roam_call(recomp_lookup(MEM32(MEM32(reg) + 8u)), reg, top, 1, &veh);
        MEM8(veh + 0x60u) = 1;
    }
    roam_car_delete(veh, c->res, top);
    if (mid)
        roam_build_ctx(0);
    c->veh = c->res = 0;
    c->live = 0;
}

/* The car cache can hold car (it is there, or a slot is unreferenced). */
static int roam_cars_cache_ok(int32_t car)
{
    uint32_t cache = MEM32(ROAM_CACHE_VA), s, i;
    if (!online_ram(cache))
        return 0;
    for (i = 0; i < 16; i++) {
        s = cache + 0x10u * i;
        if ((SMEM32(s) == car && !MEM8(s + 8u)) || !MEM8(s + 9u))
            return 1;
    }
    return 0;
}

/* Car i (re)built with look, or ours when its car cannot be had; mid: in
 * the running cruise. 0: not built (logged, skipped). */
static int roam_car_make(int i, const uint8_t look[ROAM_CFG_SIZE], int mid)
{
    roam_car *c = &s_car[i];
    uint32_t setup = MEM32(ROAM_SETUP_VA), cfg0 = MEM32(setup + ROAM_ENTRY + 0x0Cu), heap = MEM32(ROAM_HEAP_VA);
    uint32_t top = (g_esp - 0x400u) & ~0xFu, before, after, reserve, cfg, veh, res, a;
    const char *name;
    int32_t car = -1;
    double t0 = roam_now();                     /* the build's time: the frame stands still meanwhile */
    if (!online_ram(setup) || !online_ram(cfg0) || MEM32(cfg0) != ROAM_CFG_VT || !online_ram(heap) || !s_roam_slots)
        return 0;
    reserve = MEM32(heap + 0xCu) / 8u > ROAM_CARS_RESERVE ? MEM32(heap + 0xCu) / 8u : ROAM_CARS_RESERVE;
    before = roam_heap_log(NULL);
    if (before < reserve + (s_car_cost ? s_car_cost : ROAM_CAR_COST0)) {
        if (roam_cars_say(CARS_SAY_HEAP))
            fprintf(stderr, "[ROAM] cars: heap %u KB free, %u KB kept: no more cars (%d built; logged once)\n",
                    (unsigned)(before >> 10), (unsigned)(reserve >> 10), i);
        return 0;
    }
    if (!s_car_cfg[i] && !(s_car_cfg[i] = xbox_ContiguousAlloc(ROAM_CFG_SIZE, 16)))
        return 0;
    cfg = s_car_cfg[i];
    if (look && (name = roam_cfg_name(look)) && strlen(name) < 0x100u) {
        a = top;
        memcpy((char *)XBOX_PTR(top), name, strlen(name) + 1);
        car = (int32_t)roam_call(sub_0026A6A0, 0, top, 1, &a);
    }
    if (car >= 0 && roam_cars_cache_ok(car))
        memcpy((uint8_t *)XBOX_PTR(cfg), look, ROAM_CFG_SIZE);
    else                                        /* no look, unknown car or the model cache full: ours */
        memcpy((uint8_t *)XBOX_PTR(cfg), (uint8_t *)XBOX_PTR(cfg0), ROAM_CFG_SIZE);
    MEM32(cfg) = ROAM_CFG_VT;
    /* +8 is the CarCfg's dirty mask: a record carries the sender's, 0 once
     * its car was built and updated. operator= (sub_00271E20) sets 0x3FFF; so
     * do we. Bit 0x80 is the wheel step of the vehicle update (0x00100657:
     * sub_00121970 sizes the tyres from the rim and the tyre profile,
     * sub_00105450 the wheels' physics): without it the rims drew bare. */
    MEM32(cfg + 8u) = 0x3FFFu;
    if (mid && !roam_build_ctx(1))
        return 0;
    c->slot = 1u + (uint32_t)i % (uint32_t)s_roam_slots;
    veh = roam_car_build(cfg, c->slot, 0xFFFFFFFFu, top, &res);
    if (mid)
        roam_build_ctx(0);
    if (!veh) {
        if (roam_cars_say(CARS_SAY_FAIL))
            fprintf(stderr, "[ROAM] cars: car %d not built (builder gave no vehicle; logged once)\n", i);
        return 0;
    }
    after = roam_heap_log(NULL);
    if (after < before)
        s_car_cost = before - after;
    c->veh = veh;
    c->res = res;
    c->live = 0;
    c->want = 0;
    memcpy(c->look, (uint8_t *)XBOX_PTR(cfg), ROAM_CFG_SIZE);
    memset(c->look, 0, 4);
    c->phys = roam_car_inlevel(veh) ? 1 : -1;   /* not found in the level: never touched */
    if (c->phys < 0 && roam_cars_say(CARS_SAY_PHYS))
        fprintf(stderr, "[ROAM] cars: car %d not found in the physics level after its build: its collisions "
                        "left as built (logged once)\n", i);
    roam_show(veh, 0);                          /* shown once driven */
    fprintf(stderr, "[ROAM] cars: car %d built (%s, slot %u%s) in %.0f ms: %u KB, heap %u KB free\n", i,
            roam_cfg_name(c->look) ? roam_cfg_name(c->look) : "?", (unsigned)c->slot, mid ? ", in the cruise" : "",
            (roam_now() - t0) * 1e3, (unsigned)(s_car_cost >> 10), (unsigned)(after >> 10));
    return 1;
}

/* Every car out (before the cruise may end, or leftovers). */
static void roam_cars_drop_all(const char *why, int mid)
{
    int i, n = 0;
    for (i = 0; i < s_ncar; i++)
        if (s_car[i].veh) {
            roam_car_drop(&s_car[i], mid);
            n++;
        }
    for (i = 0; i < s_ncar; i++)
        s_car[i].id = 0;
    if (n)
        fprintf(stderr, "[ROAM] cars: %d standalone car%s removed (%s)\n", n, n == 1 ? "" : "s", why);
}

/* After the cruise's player cars are built (sub_002BE2C0): standalone cars
 * for the peers known now that no slot took, + ROAM_SPARE. */
static void roam_cars_load(void)
{
    static roam_peer peers[ROAM_K_MAX];
    uint8_t blob[ROAM_CFG_SIZE + 4];
    uint32_t top = (g_esp - 0x400u) & ~0xFu, reg;
    int i, k, np, want = 0, cap = roam_cars_total() - s_roam_slots;
    for (i = 0; i < s_ncar; i++)                /* the last cruise's end was not seen: registry only */
        if (s_car[i].veh) {
            if (online_ram(reg = roam_call(sub_003308C0, 0, top, 0, NULL)))
                roam_call(recomp_lookup(MEM32(MEM32(reg) + 8u)), reg, top, 1, &s_car[i].veh);
            fprintf(stderr, "[ROAM] cars: car %d left over from the last cruise (its end not seen): out of the "
                            "registry\n", i);
        }
    memset(s_car, 0, sizeof s_car);
    s_ncar = 0;
    s_cars_on = 0;
    s_cars_hold = 0;
    s_cars_need = 0;
    s_cars_said = 0;
    s_cars_next = 0;
    if (!s_roam_slots || s_roam_fake || !roam_running() || cap <= 0)
        return;
    s_cars_on = 1;
    if (cap > ROAM_CARS_MAX)
        cap = ROAM_CARS_MAX;
    np = roam_peers(peers, ROAM_K_MAX);
    for (i = 0; i < np; i++) {
        for (k = 1; k <= s_roam_slots && s_roam_peer[k].id != peers[i].id; k++)
            ;
        if (k > s_roam_slots)
            want++;
    }
    if (want)                                   /* spares only once the slots are full: a free slot takes a newcomer first */
        want += ROAM_SPARE;
    if (want > cap)
        want = cap;
    for (i = 0, k = 0; s_ncar < want; s_ncar++) {
        int ok = 0;
        while (i < np && !ok) {                 /* the next peer without a slot, for its look */
            int j;
            for (j = 1; j <= s_roam_slots && s_roam_peer[j].id != peers[i].id; j++)
                ;
            ok = j > s_roam_slots && roam_peer_rec(peers[i].id, blob);
            i++;
        }
        if (!roam_car_make(s_ncar, ok ? blob : NULL, 0))
            break;
        k++;
    }
    fprintf(stderr, "[ROAM] cars: %d standalone car%s at load (%d remote cars at most, %d slots; %d peers known)\n",
            k, k == 1 ? "" : "s", roam_cars_total(), s_roam_slots, np);
}

/* A peer's interpolated pose and newest inputs into car c. */
static void roam_car_take(roam_car *c, const roam_peer *pp)
{
    uint8_t away = pp->status == RC_ST_SHOP || pp->status == RC_ST_LOADING || pp->status == RC_ST_MENUS ||
                   pp->status == RC_ST_EVENT;   /* in an AI race (roam_race_ai.c) */
    if (away != c->away) {
        c->away = away;
        c->live = 0;                            /* snapped onto its pose when it drives again */
    }
    roam_quat_mat(pp->q, c->m);
    memcpy(c->m + 9, pp->p, sizeof pp->p);
    memcpy(c->vel, pp->v, sizeof pp->v);
    c->has_ctl = pp->ext_len == ROAM_CTL;
    if (c->has_ctl)
        memcpy(c->ctl, pp->ext, ROAM_CTL);
    if (pp->cfg_gen && pp->cfg_gen != c->gen) {  /* a new car record: rebuild if another look */
        uint8_t blob[ROAM_CFG_SIZE + 4];
        if (roam_peer_rec(pp->id, blob)) {
            c->gen = pp->cfg_gen;
            c->want = !roam_cfg_same(blob, c->look);
        }
    }
}

/* In our player's update (roam_net), the peers no slot took: each keeps its
 * car; newcomers take one (exact look > free > longest unseen held), or
 * wait for a new one (s_cars_need, built after the physics step). */
static void roam_cars_net(const roam_peer *peers, int n, const uint8_t *used, double now)
{
    uint8_t blob[ROAM_CFG_SIZE + 4], seen[ROAM_CARS_MAX] = { 0 };
    int i, k, best, score, sc, ok;
    if (!s_cars_on)
        return;
    s_cars_need = 0;
    for (i = 0; i < n; i++) {
        if (used[i])
            continue;
        for (k = 0; k < s_ncar && s_car[k].id != peers[i].id; k++)
            ;
        if (k < s_ncar) {
            seen[k] = 1;
            s_car[k].here = 1;
            roam_car_take(&s_car[k], &peers[i]);
        }
    }
    for (k = 0; k < s_ncar; k++)
        if (s_car[k].id && !seen[k] && s_car[k].here) {
            s_car[k].here = 0;
            s_car[k].lost = now;
            s_car[k].live = 0;
        }
    for (i = 0; i < n; i++) {
        if (used[i] || roam_cars_owns(peers[i].id))
            continue;
        ok = roam_peer_rec(peers[i].id, blob);
        for (k = 0, best = -1, score = 0; k < s_ncar; k++) {
            roam_car *c = &s_car[k];
            if (!c->veh || c->here)
                continue;
            if (!c->id || now - c->lost > ROAM_HOLD)
                sc = ok && roam_cfg_same(blob, c->look) ? 3 : 2;
            else                                /* held for its peer: the longest unseen */
                sc = 1;
            if (sc > score || (sc == 1 && score == 1 && c->lost < s_car[best].lost)) {
                score = sc;
                best = k;
            }
        }
        if (best < 0) {
            if (!s_cars_need)
                s_cars_need = peers[i].id;
            continue;
        }
        {
            roam_car *c = &s_car[best];
            c->id = peers[i].id;
            c->here = 1;
            c->live = 0;
            c->gen = 0;
            snprintf(c->name, sizeof c->name, "%s", peers[i].name);
            roam_car_take(c, &peers[i]);
            fprintf(stderr, "[ROAM] cars: car %d: %s (id %08X)%s\n", best, c->name, (unsigned)c->id,
                    c->want ? ", other car: rebuild queued" : "");
        }
    }
}

/* ── The cruise's own end (RACE OVER) in Free Roam ──
 *
 * RACE OVER is sub_00023C80 (this = [0x4CBAE0]): [this+0x38] = 1, the
 * MenuRaceOver screen (sub_00154980, "EndReplay"), state request 0xF
 * (sub_00020E40 from 0x00023CA5). Three ways reach it: sub_00023D00 (from
 * 0x00023D2B: the replay's playback ended, sub_0002DAC0 -> sub_0020EB90),
 * sub_00023E30 (from 0x00023E5B) and the race flow's phase 7 (from
 * 0x001C3830). The race flow ([0x557EEC], vtable 0x4691B8; request vt+4
 * sub_001C22D0, update vt+8 sub_001C3610, enter vt+0x18 sub_001C3B70: phase
 * at +4 and +0x48, timer at +8) enters phase 7 when the race rules say the
 * race is over (a racer finished or was eliminated: sub_001C5C30,
 * sub_001D6680 the cruise's challenge end, sub_001DA070, sub_001DD600,
 * sub_001E1580; each walks the racers [0x558838] (count +0x28) and the
 * rules' per-racer records [[[0x533E24]+4]+0x65C]); phase 7's update, with
 * no System Link session ([[0x557EFC]+0x10] < 0) and the state machine in 3,
 * opens RACE OVER once its timer passes [0x469230]. Phase 6 is the cruise
 * driving (enter: timer 0). The cruise's rules are vtable 0x46A7D8 (ctor
 * 0x001D6E2F, update vt+0x34 sub_001D8F20).
 *
 * In Free Roam the cruise ends only when the player picks it (pause menu):
 * a race flow found in phase 7 goes back to the phase it was in (written,
 * not requested: phase 6's enter only zeroes the timer), its timer 0, and
 * the racers' records are logged ('[ROAM] race over held'). A RACE OVER
 * request that still comes is logged with its way (roam_race_over).
 * MC3_ROAM_RACEOVER=1 lets the game end it (logs only). */
#define ROAM_FLOW_VA   0x00557EECu      /* the race flow */
#define ROAM_RULES_VA  0x00533E24u      /* +4: the race rules */
#define ROAM_RACERS_VA 0x00558838u      /* the racers, count +0x28 */

static int roam_free_cruise(void)
{
    return s_online && MEM32(0x0050F14Cu) == 0 && s_roam_slots > 0 && online_ram(MEM32(ROAM_PM_VA));
}

static void roam_racers_log(void)
{
    uint32_t hold = MEM32(ROAM_RULES_VA), rules = online_ram(hold) ? MEM32(hold + 4u) : 0, racers = MEM32(ROAM_RACERS_VA);
    uint32_t arr = online_ram(rules) ? MEM32(rules + 0x65Cu) : 0, n = online_ram(racers) ? MEM32(racers + 0x28u) : 0, i, e;
    fprintf(stderr, "[ROAM] racers: rules %08X (vtable %08X), %u racers, setup count %u:", (unsigned)rules,
            online_ram(rules) ? (unsigned)MEM32(rules) : 0u, (unsigned)n,
            online_ram(MEM32(ROAM_SETUP_VA)) ? (unsigned)MEM32(MEM32(ROAM_SETUP_VA) + 0x4Cu) : 0u);
    for (i = 0; i < n && i < 16 && online_ram(arr); i++) {
        e = MEM32(arr + 4u * i);
        if (online_ram(e))
            fprintf(stderr, " [%u] %02X %02X %02X %02X", (unsigned)i, MEM8(e + 4u), MEM8(e + 5u), MEM8(e + 6u), MEM8(e + 7u));
    }
    fprintf(stderr, " (bytes +4 done, +7 out)\n");
}

/* Each frame, after the physics step: the race flow kept out of phase 7. */
static void roam_flow_watch(void)
{
    static int let = -1, said;
    static uint32_t last, held;
    uint32_t flow = MEM32(ROAM_FLOW_VA), sm = MEM32(0x00557ED0u), ph;
    if (!roam_free_cruise() || !online_ram(flow)) {
        last = 0;
        return;
    }
    if (let < 0) {
        const char *e = getenv("MC3_ROAM_RACEOVER");
        let = e && e[0] == '1';
    }
    ph = MEM32(flow + 4u);
    if (ph == last)
        return;
    if (said < 20) {
        said++;
        fprintf(stderr, "[ROAM] race flow: phase %u -> %u (state %u)\n", (unsigned)last, (unsigned)ph,
                online_ram(sm) ? (unsigned)MEM32(sm + 4u) : 0u);
        if (ph == 7u)
            roam_racers_log();
    }
    if (ph == 7u && last && !let) {             /* the race is not over in Free Roam */
        MEM32(flow + 4u) = last;
        MEM32(flow + 0x48u) = last;
        MEMF(flow + 8u) = 0.0f;
        if (++held <= 5u || held % 300u == 0)
            fprintf(stderr, "[ROAM] race over held (%u): the race flow back in phase %u (MC3_ROAM_RACEOVER=1 lets "
                            "it end)\n", (unsigned)held, (unsigned)last);
        return;
    }
    last = ph;
}

/* The state machine's request (sub_00020E40, thiscall (n), ret 4; esp at its
 * return address): RACE OVER's (0xF from 0x00023CA5) says which way came. */
static void roam_race_over(uint32_t esp)
{
    uint32_t from, flow = MEM32(ROAM_FLOW_VA);
    if (MEM32(esp) != 0x00023CA5u || MEM32(esp + 4u) != 0xFu || !roam_free_cruise())
        return;
    from = MEM32(esp + 0x10u);                  /* sub_00023C80's caller (it pushed esi, edi) */
    fprintf(stderr, "[ROAM] RACE OVER asked from %08X (%s), race flow phase %u timer %.2f, race type %u\n",
            (unsigned)from,
            from == 0x00023D2Bu ? "sub_00023D00: the replay check" :
            from == 0x00023E5Bu ? "sub_00023E30" :
            from == 0x001C3830u ? "race flow phase 7" : "?",
            online_ram(flow) ? (unsigned)MEM32(flow + 4u) : 0u, online_ram(flow) ? MEMF(flow + 8u) : 0.0f,
            online_ram(MEM32(ROAM_SETUP_VA)) ? (unsigned)MEM32(MEM32(ROAM_SETUP_VA) + 0x18u) : 0u);
    roam_racers_log();
}

/* Another thread inside a resource page-in ([0x500F10] set): a car built
 * now would page in through its context ("Invalid type in
 * rmcShaderFactory", as roam_swap waits). 1: wait; 3 s at most, in case the
 * top is left set. */
static int roam_cars_pagein(double now)
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

/* After the physics step (sub_002BDEC0): at most one build or rebuild, then
 * every car's pose, inputs, collisions, or its parking spot. */
static void roam_cars_frame(void)
{
    uint32_t top = (g_esp - 0x400u) & ~0xFu, mtx, c;
    float me[12], m[12], vel[3], d[3], dist, f;
    double now;
    int i, k;
    roam_flow_watch();
    if (!s_cars_on || !s_roam_nhist || !online_ram(MEM32(ROAM_PM_VA)))
        return;
    now = roam_now();
    memcpy(me, s_roam_hist[(s_roam_nhist - 1) % ROAM_HIST].m, sizeof me);
    if (!s_cars_hold && now >= s_cars_next && !roam_cars_pagein(now)) {
        uint8_t blob[ROAM_CFG_SIZE + 4];
        for (i = 0; i < s_ncar && !(s_car[i].veh && s_car[i].want && s_car[i].here); i++)
            ;
        if (i < s_ncar) {                       /* a peer's other car */
            s_cars_next = now + ROAM_CARS_GAP;
            s_car[i].want = 0;
            if (roam_peer_rec(s_car[i].id, blob)) {
                roam_car_drop(&s_car[i], 1);
                if (!roam_car_make(i, blob, 1))
                    s_car[i].id = 0;            /* its peer waits for a car like a newcomer */
            }
        } else if (s_cars_need) {               /* a peer without a car: a dropped one first, else a new one */
            for (i = 0; i < s_ncar && s_car[i].veh; i++)
                ;
            if (i < s_ncar || s_ncar < roam_cars_total() - s_roam_slots) {
                int ok = roam_peer_rec(s_cars_need, blob);
                s_cars_next = now + ROAM_CARS_GAP;
                memset(&s_car[i], 0, sizeof s_car[i]);
                if (roam_car_make(i, ok ? blob : NULL, 1) && i == s_ncar)
                    s_ncar++;
            } else if (roam_cars_say(CARS_SAY_MAX))
                fprintf(stderr, "[ROAM] cars: all %d remote cars in use: farther players not shown (logged once)\n",
                        roam_cars_total());
        }
    }
    f = 1.0f - expf(-s_roam_dt / ROAM_TAU);
    for (i = 0; i < s_ncar; i++) {
        roam_car *cr = &s_car[i];
        static roam_tyre tyre[ROAM_CARS_MAX];
        if (!cr->veh)
            continue;
        roam_tyres(&tyre[i], cr->veh, "car", i);  /* roam_game.c: the wheel step once with the rim there */
        if (cr->id && cr->here && !cr->away) {
            memcpy(m, cr->m, sizeof m);
            memcpy(vel, cr->vel, sizeof vel);
            mtx = MEM32(cr->veh + 0x1Cu);
            if (cr->live && online_ram(mtx)) {  /* blend the position from where the step left it */
                for (k = 0; k < 3; k++)
                    d[k] = m[9 + k] - MEMF(mtx + 0x34u + 4u * k);
                if (d[0] * d[0] + d[2] * d[2] < ROAM_SNAP * ROAM_SNAP)
                    for (k = 0; k < 3; k++)
                        m[9 + k] -= d[k] * (1.0f - f);
            }
            roam_set_pose(cr->veh, m, vel);
            if (!cr->live) {
                roam_show(cr->veh, 1);
                cr->live = 1;
            }
            c = MEM32(cr->veh + 4u);
            if (cr->has_ctl && online_ram(c) && MEM32(c + 0x60u) == cr->slot)
                roam_remote(0, c, m, vel, cr->ctl);
            for (k = 0; k < 3; k++)
                d[k] = m[9 + k] - me[9 + k];
            dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (dist < ROAM_CAR_NEAR)
                roam_car_phys(cr, 1, top);
            else if (dist > ROAM_CAR_FAR)
                roam_car_phys(cr, 0, top);
            continue;
        }
        roam_car_phys(cr, 0, top);              /* parked far away, under the map, hidden */
        roam_show(cr->veh, 0);
        memcpy(m, me, sizeof m);
        m[9] += 30000.0f + 50.0f * (float)(8 + i);
        m[10] -= 500.0f;
        vel[0] = vel[1] = vel[2] = 0.0f;
        cr->live = 0;
        roam_set_pose(cr->veh, m, vel);
    }
}

/* The state machine's pass (sub_00020EC0; after = 0 before the game's,
 * 1 after): a queued state request may end the cruise inside this pass,
 * and the world then goes with our cars in it: they go first. Builds wait
 * until the queue is empty and the state is the cruise's again. */
static void roam_cars_pass(uint32_t obj, int after)
{
    static uint32_t state;
    if (!s_cars_on || !online_ram(obj))
        return;
    if (!after) {
        if (MEM32(obj + 0x3Cu) && online_ram(MEM32(ROAM_PM_VA))) {
            int i, n = 0;
            for (i = 0; i < s_ncar; i++)
                n += s_car[i].veh != 0;
            if (n) {
                state = MEM32(obj + 4u);
                roam_cars_drop_all("a state request: the cruise may end", 1);
            }
            s_cars_hold = 1;
        }
        return;
    }
    if (s_cars_hold && !MEM32(obj + 0x3Cu) && online_ram(MEM32(ROAM_PM_VA)) && (!state || MEM32(obj + 4u) == state)) {
        s_cars_hold = 0;
        s_cars_next = roam_now() + 1.0;         /* the cruise goes on: cars built again */
    }
}
