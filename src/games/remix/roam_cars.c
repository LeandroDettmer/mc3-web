/* Free roam, many cars: the game-side room and helpers the remote cars pool
 * (roam_pool.c) builds on, and the cruise's own end held in Free Roam.
 *
 * Included by recomp_manual.c after roam_game.c, before roam_pool.c.
 *
 * How many: MC3_ROAM_CARS=N remote cars at most in all (slots included;
 * 0 or <= 7: the slots only). Unset (auto): 32 with a 128 MB contiguous
 * window (RECOMP_RAM_MB=128), 12 with 64 MB. That many links are asked of
 * the hub, 32 at least (roam_begin; MC3_ROAM_K still overrides).
 *
 * Collision: a standalone car is in the physics level ([0x5580F0], add
 * vt+0x70 sub_002ABD00(collider [car+0x68], 0), remove vt+0x14
 * sub_002AD320(handle word [[veh+0x1C]+8])) only within ROAM_CAR_NEAR m of
 * us, out beyond ROAM_CAR_FAR: the level's pool stays small. Membership is
 * read from the level (handle -> entry +8 == collider); a car whose first
 * check fails is never touched (left as built).
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
#define ROAM_CAR_NEAR 20.0f             /* m: into the physics level within */
#define ROAM_CAR_FAR  30.0f             /* m: out of it beyond */
#define ROAM_CARS_GAP 0.25              /* s between two builds/rebuilds */

void sub_003308C0(void);                        /* the vehicle registry, cdecl; eax */

static const struct { uint8_t arr, k; } s_roam_reg_lists[] = {   /* array field, bytes per capacity */
    { 0x08, 4 },  { 0x10, 4 },  { 0x18, 4 },  { 0x20, 4 },  { 0x28, 4 },  { 0x30, 4 },
    { 0x38, 8 },  { 0x40, 16 }, { 0x48, 32 }, { 0x50, 8 },  { 0x58, 4 },  { 0x60, 4 },
    { 0x68, 4 },  { 0x70, 4 },  { 0x78, 4 },                  /* base sub_0035D200 */
    { 0x84, 4 },  { 0x8C, 4 },  { 0x94, 4 },                  /* sub_000FEDC0 */
};

static uint32_t s_cars_said;                    /* reasons already logged */
enum { CARS_SAY_HEAP = 1, CARS_SAY_MAX = 2, CARS_SAY_POOL = 4, CARS_SAY_PHYS = 8, CARS_SAY_FAIL = 16,
       CARS_SAY_PAGEIN = 32, CARS_SAY_BIKE = 64 };

/* Remote cars shown at most, slots included: MC3_ROAM_CARS, else by the window.
 * The page's in-game Settings change it live (mc3_live MC3_LIVE_CARS, -1 auto). */
static int roam_cars_total(void)
{
    static int n = -1;
    int32_t l = mc3_live(MC3_LIVE_CARS);
    int v;
    if (n < 0) {
        const char *e = getenv("MC3_ROAM_CARS");
        n = e && *e && strcmp(e, "auto") ? atoi(e) : g_xbox_contig_size >= (128u << 20) ? 32 : 12;
        n = n < ROAM_SLOTS ? ROAM_SLOTS : n > ROAM_K_MAX ? ROAM_K_MAX : n;
    }
    if (!l)
        return n;
    v = l > 0 ? l : g_xbox_contig_size >= (128u << 20) ? 32 : 12;
    return v < ROAM_SLOTS ? ROAM_SLOTS : v > ROAM_K_MAX ? ROAM_K_MAX : v;
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

/* Car veh in (on 1) or out of the physics level, if we manage it (*st: 1 in,
 * 0 out, -1 unknown). */
static void roam_veh_phys(uint32_t veh, int32_t *st, int on, uint32_t top)
{
    uint32_t lvl = MEM32(ROAM_PHYS_VA), vt, a[2];
    if (*st < 0 || *st == on || !online_ram(lvl) || !online_ram(vt = MEM32(lvl)))
        return;
    if (on) {
        a[0] = MEM32(MEM32(veh + 8u) + 0x68u);
        a[1] = 0;
        roam_call(recomp_lookup(MEM32(vt + 0x70u)), lvl, top, 2, a);
    } else {
        a[0] = MEM16(MEM32(veh + 0x1Cu) + 8u);
        roam_call(recomp_lookup(MEM32(vt + 0x14u)), lvl, top, 1, a);
    }
    *st = roam_car_inlevel(veh);
    if (*st != on && roam_cars_say(on ? CARS_SAY_POOL : CARS_SAY_PHYS))
        fprintf(stderr, "[ROAM] cars: physics level %s refused (car left %s; logged once)\n",
                on ? "add" : "remove", on ? "without collisions" : "in it");
}

/* A parked car's drawn matrix 30 km off and 500 m under our car (idx apart per
 * car). Out of the physics level (roam_rc_phys, roam_pool.c) nothing syncs what is drawn
 * from the body any more, and the game rewrites the drawn flag [[veh+0xC]+0x12D]
 * itself (sub_001D86A0, sub_001DAD40, sub_001ED2F0): a car parked by SetMatrix
 * and the flag alone stayed on its grid spot, drawn (2026-11-10). */
static void roam_park_inst(uint32_t veh, int idx)
{
    uint32_t mtx;
    const float *me;
    if (!s_roam_nhist || !online_ram(veh) || !online_ram(mtx = MEM32(veh + 0x1Cu)))
        return;
    me = s_roam_hist[(s_roam_nhist - 1) % ROAM_HIST].m;
    MEMF(mtx + 0x34u) = me[9] + 30000.0f + 50.0f * (float)idx;
    MEMF(mtx + 0x38u) = me[10] - 500.0f;
    MEMF(mtx + 0x3Cu) = me[11];
}

/* One line per slot and standalone car: its peer, driven or parked, drawn flag,
 * physics level, drawn position. "NOT PARKED" = no driving peer, yet drawn or near. */
static void roam_car_line(const char *what, int k, uint32_t id, int driven, int live, uint32_t veh, int phys)
{
    uint32_t mtx = online_ram(veh) ? MEM32(veh + 0x1Cu) : 0, r = online_ram(veh) ? MEM32(veh + 0xCu) : 0;
    const float *me = s_roam_nhist ? s_roam_hist[(s_roam_nhist - 1) % ROAM_HIST].m : NULL;
    float x = online_ram(mtx) ? MEMF(mtx + 0x34u) : 0.0f, z = online_ram(mtx) ? MEMF(mtx + 0x3Cu) : 0.0f;
    int drawn = online_ram(r) ? MEM8(r + 0x12Du) : -1, listed = online_ram(veh) ? roam_listed(veh) : -1;
    float rx = online_ram(r) ? MEMF(r + 0xC4u) : 0.0f, rz = online_ram(r) ? MEMF(r + 0xCCu) : 0.0f;
    char peer[16];
    if (id)
        snprintf(peer, sizeof peer, "%08X", (unsigned)id);
    else
        snprintf(peer, sizeof peer, "none");
    /* "parked" = out of the draw list (the drawn flag is the rules' each frame, sub_001D86A0) */
    fprintf(stderr, "[ROAM] report %s %d: peer %s, %s, drawn %d, in draw list %d, phys %d, at %.0f %.0f, render object "
                    "at %.0f %.0f (%.0f m from us)\n", what, k, peer,
            driven ? (live ? "driven" : "driven, NOT LIVE (never posed)") : listed == 0 ? "parked" : "NOT PARKED", drawn,
            listed, phys, x, z, rx, rz, me ? sqrtf((rx - me[9]) * (rx - me[9]) + (rz - me[11]) * (rz - me[11])) : -1.0f);
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
    return s_online && MEM32(0x0050F14Cu) == 0 && s_roam_cruise && online_ram(MEM32(ROAM_PM_VA));
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

