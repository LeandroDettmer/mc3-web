/* police.c -- the mod pack "police": police cars in the Online Free Roam
 * cruise that chase our car only, and a fine instead of the arrest's end.
 *
 * Included by recomp_manual.c last of the roam files (uses s_online,
 * s_mods, online_ram, online_money_va, online_garage_left, s_on_money,
 * op_dollars, roam_now, ROAM_PM_VA, ROAM_SETUP_VA, roam_free_cruise).
 *
 * The game's police are race AI opponents whose CarType is a police car
 * (aiCop brain, vtable 0x46BFF8; created sub_001ED2F0 -> sub_001EF8B0, which
 * adds the brain to the cop list 0x534088 [16], count 0x534110). A race
 * takes min(its .rac's opponents, 8, setup +0x40) of them, none while setup
 * +0x10D is 0 (.rac reader sub_001D3AE0, 0x1D42DB-0x1D42FB; AI pool
 * sub_001E43C0). The Free Roam cruise runs with +0x40 = 0 (fr_no_opponents).
 *
 * Spawn: the pack (tools/police_pack.py) gives each city's cruise .rac
 * (tune/race/<city>/cruise/easy/<city>_cruise_easy_cruise_race01.rac) 3 police
 * cars as its only opponents; with the pack on, fr_no_opponents sets the
 * Online Free Roam cruise setups' +0x40 = police_count() (3 at a 128 MB
 * window, 2 at 64 MB: phones, low memory; MC3_POLICE=N, 0 off) and +0x10D =
 * 1. Each police car is a whole AI car built at the cruise's load, before the
 * player manager (sub_00029240: race init, AI pool, then sub_002BE2C0), so
 * the remote slots' heap check (roam_build) already sees what they took, and
 * roam_build gives the remote players at most 7 - police slots (1 + slots +
 * police <= 8, as rra_start keeps an AI race). Pack off, or not the Online
 * Free Roam cruise: nothing here runs (career, races, Arcade untouched).
 *
 * Who they chase: each brain update (sub_001EFCC0) lists the players
 * (GetPlayer sub_002BF570 for each index; [[player+0x14]+8] vt+0x2C must
 * answer al != 0, call returning to 0x001EFF03) and the traffic within its
 * scan radius (+0x218: 50 m states 0/1, 70 m 2/3; sub_001F1150), rates each
 * with the heat test sub_001E4A70 (0 unless over 60/100 x 0x449990 m/s,
 * 0x4B2AB4/0x4B2AB8, or the wrong way / on a road of type 3 within 10 m; a
 * player +1) and, while it has no target, one hotter than +0x210 becomes the
 * target (brain +0x20C, level +0x210) and goes on the wanted list 0x534120
 * ({who, level} x16, count 0x534114). That indirect call answers 0 for any
 * player but ours (police_icall, via recomp_lookup_manual on its return
 * address, no relift), and for ours too until our car hits a police car:
 * police cars never pick us on their own (they did right after the cruise
 * started). Each frame a police car after a remote player or after us with
 * no hit gives it up the game's way (sub_001EFB00).
 *
 * State machine sub_001F0790 (brain +0x1B4; 8/9 off): with a target, states
 * 0-3 (patrol) go to 4 (closing in) beyond 225 m, else 6 (chase); 4 -> 6
 * under 150 m, gives up past 1000 m or 20 s; 5 likewise (300 m and 10 s);
 * 6 gives up past 300 m (radio 0x2A), goes to 7 (close pursuit, formation
 * sub_001E4CE0) within 30 m or not facing us; 7 back to 6 beyond 30 m or not
 * facing us, at our speed under 1 m/s within 30 m behaviour 6/0x24 (box in).
 * Timer +0x214 counts 1/60 a brain update while it has a target; after 5 s
 * in 6 it calls backup (sub_001F00F0 -> sub_001E4970, up to [0x4B2AB0]
 * cars). Giving up = sub_001EFB00 (state 2, target 0, patrol). The game has
 * no arrest of its own in a cruise (the checkpoint race sub_001DED30 only
 * counts chasers for its HUD messages 6/7 and its finish flag rec+0x5D).
 *
 * Chase: our car within 6 m of a police car while either one's ground
 * velocity changes faster than 25 m/s^2 (a hit; drift.c's hit test is ours
 * only) -> that car and every patrolling one within 300 m take us as their
 * target (police_chase), '[POLICE] chase starts: ...'.
 * Bust: a police car after us in close pursuit (7) within 30 m, our car under
 * 2 m/s for 3 s (not in the first 2 s after the hit). Escape: no police car
 * after us for 1 s (the game's own give-up rules above), or every one farther
 * than 250 m for 10 s: they give up (sub_001EFB00), prompt 'ESCAPED'.
 *
 * Arrest: our bust rule, the racer-finished call (police_finish) for us while a
 * police car is after us, or our record's arrested byte (police_arrest). Fine POLICE_FINE from the Online money
 * ([[0x557F0C]+0xC]+0xACC; never below 0), the Online profile saved
 * (online_garage_left, as the race prizes), the prompt box says BUSTED and
 * the fine, the chase ends (targets, wanted list cleared) and play goes on.
 *
 * Log lines '[POLICE] ...'. Browser only (the Online session is). */

#define POL_COPS_VA     0x00534088u     /* the cop brains [16] */
#define POL_NCOPS_VA    0x00534110u
#define POL_WANTED_VA   0x00534120u     /* {who, level} x 16 */
#define POL_NWANTED_VA  0x00534114u
#define POL_CAND_RET    0x001EFF03u     /* the brain's player check: [[player+0x14]+8] vt+0x2C returns here */
#define POL_TARGET      0x20Cu          /* brain: target (player or traffic car), 0 none */
#define POL_LEVEL       0x210u          /* brain: the target's heat level */
#define POL_FINISH_VA   0x001E06E0u     /* racer finished (race vt+0x64, thiscall (racer index), ret 4; indirect only) */
#define POL_RULES_VA    0x00533E24u     /* [+4] = the race object */
#define POLICE_FINE     5000u
#define POL_NOTE_S      6.0             /* s the BUSTED line shows (big the first 2) */

#define POL_STATE       0x1B4u          /* brain: state; 0-3 patrol, 4/5 closing in, 6 chase, 7 close pursuit, 8/9 off */
#define POL_TIMER       0x214u          /* brain: s since the last state change (1/60 a brain update) */
#define POL_HIT_M       6.0f            /* car centres closer than this ... */
#define POL_HIT_ACC     25.0f           /* ... and either car's ground velocity changing faster (m/s^2): a hit */
#define POL_BUST_M      30.0f           /* bust: a police car in close pursuit (state 7) within 30 m (0x4B2B5C) ... */
#define POL_BUST_MS     2.0f            /* ... and our car under 2 m/s (the game's own stop test is 1.0, 0x4B2B60) ... */
#define POL_BUST_S      3.0             /* ... for 3 s */
#define POL_GRACE_S     2.0             /* no bust in the first 2 s after the hit (the hit itself stops us) */
#define POL_LOST_M      250.0f          /* escape, fallback: every police car after us farther than this ... */
#define POL_LOST_S      10.0            /* ... for 10 s (the game's own: a chasing car drops us past 300 m) */
void sub_001EFB00(void);                /* brain: give up the target, back to patrol (state 2, +0x20C/+0x210 = 0) */

static int s_pol_live;                  /* the Online Free Roam cruise with police: the filters run */
static uint32_t s_pol_va;               /* the indirect call's real target (police_icall) */
static double s_pol_note_t;             /* when the fine's line went up, 0 none */
static char s_pol_note[96];
static int s_pol_wanted;                /* our car hit a police car: the chase is on until a bust or an escape */
static double s_pol_hit_t;              /* when the hit was */

/* The pack is on: MC3_MODS named it and its files were found (mods.c). */
static int police_pack(void)
{
    static int on = -1;
#ifndef __EMSCRIPTEN__
    return 0;                                   /* browser only: the native Online session stays as it was */
#endif
    if (on < 0) {
        const char *e = getenv("MC3_POLICE");
        on = 0;
        for (int i = 0; i < s_mods_n && !on; i++)
            on = !strncmp(s_mods[i].path, "police/", 7);
        if (on && e && *e && atoi(e) <= 0)
            on = 0;
        fprintf(stderr, "[POLICE] pack %s\n", on ? "on: police cars in the Online Free Roam cruise"
                                                 : e && *e && atoi(e) <= 0 ? "off (MC3_POLICE=0)" : "off");
    }
    return on;
}

/* Police cars for a Free Roam cruise: 3 at a 128 MB window, 2 at 64 MB (phones, low memory). */
static uint32_t police_count(void)
{
    const char *e = getenv("MC3_POLICE");
    int n = e && *e ? atoi(e) : (g_xbox_contig_size >> 20) >= 128u ? 3 : 2;
    return n < 0 ? 0u : n > 3 ? 3u : (uint32_t)n;
}

/* fr_no_opponents (a state request, Online Free Roam): a cruise setup (type 0) gets its police; 1 = done. */
static int police_setup(uint32_t s, uint32_t va)
{
    uint32_t n;
    if (!police_pack())
        return 0;
    n = police_count();
    if (MEM32(s + 0x40u) != n || MEM8(s + 0x10Du) != 1u)
        fprintf(stderr, "[POLICE] cruise setup [%08X]: %u police cars (opponents %u -> %u, AI on)\n", (unsigned)va,
                (unsigned)n, (unsigned)MEM32(s + 0x40u), (unsigned)n);
    MEM32(s + 0x40u) = n;
    MEM8(s + 0x10Du) = 1;
    return 1;
}

/* roam_build: the remote slots this cruise can have beside its police (1 + slots + police <= 8). */
static int police_slots(uint32_t setup, int n)
{
    int cops = police_pack() && MEM32(setup + 0x18u) == 0 ? (int)MEM32(setup + 0x40u) : 0, room = 7 - cops;
    if (cops <= 0 || n <= room)
        return n;
    fprintf(stderr, "[POLICE] %d remote slot%s (of %d) beside %d police cars; more players come as standalone cars\n",
            room, room == 1 ? "" : "s", n, cops);
    return room;
}

static uint32_t police_p0(void)         /* our player (player 0), 0 none */
{
    uint32_t pm = MEM32(ROAM_PM_VA), p0;
    return online_ram(pm) && online_ram(p0 = MEM32(pm + 8u)) ? p0 : 0;
}

/* A player of ours other than player 0 (a remote slot), else 0. */
static int police_remote(uint32_t who)
{
    uint32_t pm = MEM32(ROAM_PM_VA), i;
    if (!who || !online_ram(pm) || who == police_p0())
        return 0;
    for (i = 1; i < 8u; i++)
        if (MEM32(pm + 8u + 4u * i) == who)
            return 1;
    return 0;
}

/* In place of the brain's player check (thiscall on [[player+0x14]+8], no
 * args): edi is the player. Ours while wanted: the game's own (a police car
 * we pass speeding joins the chase); ours not wanted, or another: al 0,
 * skipped, so no police car picks us by its own heat rules. */
static void police_icall(void)
{
    recomp_func_t fn;
    if (g_edi != police_p0() || !s_pol_wanted) {
        static unsigned said;
        if (said++ < 4u)
            fprintf(stderr, "[POLICE] remote player %08X left out of a police car's suspects\n", (unsigned)g_edi);
        g_eax &= ~0xFFu;
        g_esp += 4u;
        return;
    }
    fn = recomp_lookup(s_pol_va);
    if (fn)
        fn();
    else {
        g_eax &= ~0xFFu;
        g_esp += 4u;
    }
}

/* recomp_lookup_manual, every indirect call: the brain's player check, 0 the others. */
static void police_finish(void);
static recomp_func_t police_lookup(uint32_t va)
{
    if (!s_pol_live)
        return NULL;
    if (va == POL_FINISH_VA)
        return police_finish;
    if (MEM32(g_esp) != POL_CAND_RET)
        return NULL;
    s_pol_va = va;
    return police_icall;
}

/* A police car gives up its target the game's way (sub_001EFB00: state 2,
 * target and level 0, patrol behaviour). Zeroing the target alone left a
 * car in state 4-7 with none, and the brain update (sub_001EFCC0, states 4-7)
 * reads the target's vtable. */
static void police_drop(uint32_t b)
{
    roam_call(sub_001EFB00, b, (g_esp - 0x400u) & ~0xFu, 0, NULL);
}

static uint32_t police_cop(uint32_t i)  /* cop brain i, 0 none or off (states 8/9) */
{
    uint32_t b = i < 16u && i < MEM32(POL_NCOPS_VA) ? MEM32(POL_COPS_VA + 4u * i) : 0;
    return online_ram(b) && MEM32(b + POL_STATE) <= 7u ? b : 0;
}

/* The entries for which out() says 1 leave the wanted list ({who, level} x16). */
static void police_unwant(int (*out)(uint32_t))
{
    uint32_t n = MEM32(POL_NWANTED_VA), i, j, w;
    for (i = j = 0; i < n && i < 16u; i++) {
        w = MEM32(POL_WANTED_VA + 8u * i);
        if (out(w))
            continue;
        MEM32(POL_WANTED_VA + 8u * j) = w;
        MEM32(POL_WANTED_VA + 8u * j + 4u) = MEM32(POL_WANTED_VA + 8u * i + 4u);
        j++;
    }
    if (j != n && n <= 16u)
        MEM32(POL_NWANTED_VA) = j;
}

static int police_is_p0(uint32_t w) { return w && w == police_p0(); }

/* The chase is over: every police car after us back to patrol, we leave the wanted list. */
static void police_calm(void)
{
    uint32_t p = police_p0(), i, b;
    for (i = 0; i < 16u; i++)
        if ((b = police_cop(i)) && p && MEM32(b + POL_TARGET) == p)
            police_drop(b);
    police_unwant(police_is_p0);
    s_pol_wanted = 0;
}

/* Remote players out of the targets and the wanted list (a safety net: police_icall keeps them out). */
static void police_scrub(void)
{
    uint32_t i, b;
    for (i = 0; i < 16u; i++)
        if ((b = police_cop(i)) && police_remote(MEM32(b + POL_TARGET))) {
            fprintf(stderr, "[POLICE] police car %u was after remote player %08X: back to patrol\n", (unsigned)i,
                    (unsigned)MEM32(b + POL_TARGET));
            police_drop(b);
        }
    police_unwant(police_remote);
}

/* We were arrested: the fine, the profile saved, the line, the chase over. */
static void police_fine(const char *how)
{
    uint32_t m = online_money_va(), fine = 0;
    char d[24];
    if (m) {
        fine = MEM32(m) < POLICE_FINE ? MEM32(m) : POLICE_FINE;
        MEM32(m) -= fine;
        s_on_money = MEM32(m);
    }
    op_dollars(d, sizeof d, fine);
    snprintf(s_pol_note, sizeof s_pol_note, "BUSTED\nFine: -$%s", d);
    s_pol_note_t = roam_now();
    police_calm();
    fprintf(stderr, "[POLICE] busted (%s): fine $%u, money %u%s; the chase is over, the cruise goes on\n", how,
            (unsigned)fine, m ? (unsigned)MEM32(m) : 0u, m ? "" : " (no progress object: not paid)");
    online_garage_left();                       /* the profile saved now, as the race prizes */
}

static uint32_t police_race(void)        /* the race object (the cruise's mcRaceBase), 0 none */
{
    uint32_t h = MEM32(POL_RULES_VA);
    return online_ram(h) && online_ram(MEM32(h + 4u)) ? MEM32(h + 4u) : 0;
}

static uint32_t police_on_us(uint32_t p) /* police cars (not off) whose target is player p */
{
    uint32_t i, b, k = 0;
    for (i = 0; i < 16u; i++)
        if ((b = police_cop(i)) && p && MEM32(b + POL_TARGET) == p)
            k++;
    return k;
}

/* In place of "racer finished" (sub_001E06E0) in the police cruise. It sets
 * the racer record's done flag (race + 0x98 + i x 0xB8 + 4), counts it
 * (race +0x1250, bit i of +0x1258), shows "hud_arrested" when the record's
 * +5 is set, and the cruise is over for that player. Nothing finishes in a
 * Free Roam cruise but an arrest, so ours (the local racer, sub_002694D0)
 * finishing with the arrested byte set or a police car after us is the
 * arrest: the game's is not run, the byte goes, a fine instead. Anyone
 * else's, or ours with neither: the game's. */
static void police_finish(void)
{
    uint32_t race = g_ecx, idx = MEM32(g_esp + 4u), from = MEM32(g_esp), me, rec, cops, arrested;
    recomp_func_t fn;
    me = roam_call(sub_002694D0, 0, (g_esp - 0x400u) & ~0xFu, 0, NULL);
    if (idx == me && idx < 8u && online_ram(race) && race == police_race()) {
        rec = race + 0x98u + idx * 0xB8u;
        arrested = MEM8(rec + 5u);
        cops = police_on_us(police_p0());
        if (arrested || cops) {
            fprintf(stderr, "[POLICE] arrest: our racer %u 'finished' (from %08X; arrested byte %u, done %u, %u police "
                            "car%s after us): the game's race end skipped\n", (unsigned)idx, (unsigned)from,
                    (unsigned)arrested, (unsigned)MEM8(rec + 4u), (unsigned)cops, cops == 1 ? "" : "s");
            MEM8(rec + 5u) = 0;
            police_fine("arrest");
            g_esp += 8u;                        /* ret 4 */
            return;
        }
        fprintf(stderr, "[POLICE] our racer %u finished (from %08X) with no police after us: the game's own\n",
                (unsigned)idx, (unsigned)from);
    }
    if ((fn = recomp_lookup(POL_FINISH_VA)) != NULL)
        fn();
    else
        g_esp += 8u;
}

/* A frame of ours: an arrested byte set without a finish (another path) is an arrest too. */
static void police_arrest(void)
{
    uint32_t race = police_race(), me, rec;
    if (!race)
        return;
    me = roam_call(sub_002694D0, 0, (g_esp - 0x400u) & ~0xFu, 0, NULL);
    if (me >= 8u)
        return;
    rec = race + 0x98u + me * 0xB8u + 5u;
    if (!MEM8(rec))
        return;
    fprintf(stderr, "[POLICE] arrest: our racer %u's arrested byte set (done %u)\n", (unsigned)me,
            (unsigned)MEM8(rec - 1u));
    MEM8(rec) = 0;
    police_fine("arrested byte");
}

/* The prompt box's line for an empty prompt (op_note): the fine, POL_NOTE_S s; "" none. */
static const char *police_note(void)
{
    static char out[112];
    double now = roam_now();
    if (!s_pol_note_t)
        return "";
    if (now - s_pol_note_t > POL_NOTE_S) {
        s_pol_note_t = 0;
        return "";
    }
    snprintf(out, sizeof out, "%s%s", now - s_pol_note_t < 2.0 ? "\x01" : "", s_pol_note);
    return out;
}

/* A vehicle's position ([veh+0x1C]+0x34, as the brain's sub_001E9340) and its
 * body's velocity (phInertialCS [[[veh+8]+0x68]+0x10] +0x44); 0 unreadable. */
static int police_car(uint32_t veh, float pos[3], float vel[3])
{
    uint32_t m, car, ph, ics;
    int k;
    if (!(online_ram(veh) && online_ram(m = MEM32(veh + 0x1Cu)) && online_ram(car = MEM32(veh + 8u)) &&
          online_ram(ph = MEM32(car + 0x68u)) && online_ram(ics = MEM32(ph + 0x10u))))
        return 0;
    for (k = 0; k < 3; k++) {
        pos[k] = MEMF(m + 0x34u + 4u * k);
        vel[k] = MEMF(ics + 0x44u + 4u * k);
    }
    return 1;
}

static uint32_t police_veh(uint32_t b)  /* a cop brain's vehicle (sub_001E93B0: [[brain+0xC]+0x20]) */
{
    uint32_t c = MEM32(b + 0xCu);
    return online_ram(c) ? MEM32(c + 0x20u) : 0;
}

static float police_dist(const float a[3], const float b[3])
{
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(x * x + y * y + z * z);
}

static float police_acc(const float v[3], const float v0[3], double dt)    /* ground-plane dv/dt (world y up) */
{
    float x = v[0] - v0[0], z = v[2] - v0[2];
    return (float)(sqrt((double)(x * x + z * z)) / dt);
}

/* We hit police car `hit`: it, and every patrolling one (states 0-3) within
 * 300 m, takes us as its target the way the brain's own pick does (+0x20C =
 * us, level 4 = a speeding player's heat, sub_001E4A70; the state machine
 * sub_001F0790 then goes to 4, closing in, beyond 225 m or 6, chase); we go
 * on the wanted list (the backup call sub_001F00F0 reads it). */
static void police_chase(uint32_t p, uint32_t hit, const float up[3], const char *why)
{
    uint32_t i, b, n, k = 0;
    float cp[3], cv[3], d;
    s_pol_wanted = 1;
    s_pol_hit_t = roam_now();
    for (i = 0; i < 16u; i++) {
        if (!(b = police_cop(i)) || !police_car(police_veh(b), cp, cv))
            continue;
        d = police_dist(cp, up);
        if (i != hit && (d >= 300.0f || MEM32(b + POL_STATE) > 3u))
            continue;
        if (MEM32(b + POL_STATE) > 3u && MEM32(b + POL_TARGET) != p)
            police_drop(b);                     /* it was after a traffic car */
        MEM32(b + POL_TARGET) = p;
        MEM32(b + POL_LEVEL) = 4;
        MEMF(b + POL_TIMER) = 0;
        k++;
    }
    n = MEM32(POL_NWANTED_VA);
    for (i = 0; i < n && i < 16u && MEM32(POL_WANTED_VA + 8u * i) != p; i++)
        ;
    if (i == n && n < 16u) {
        MEM32(POL_WANTED_VA + 8u * n) = p;
        MEM32(POL_WANTED_VA + 8u * n + 4u) = 4;
        MEM32(POL_NWANTED_VA) = n + 1u;
    }
    fprintf(stderr, "[POLICE] chase starts: our car hit police car %u (%s); %u police car%s after us\n", (unsigned)hit,
            why, (unsigned)k, k == 1 ? "" : "s");
}

static void police_escaped(const char *why)
{
    police_calm();
    snprintf(s_pol_note, sizeof s_pol_note, "ESCAPED");
    s_pol_note_t = roam_now();
    fprintf(stderr, "[POLICE] escaped (%s): the chase is over, the police back on patrol\n", why);
}

/* Each player update (sub_002C1520), before the game's: player 0's only, once a frame.
 * Chase only after our car hits a police car (police_icall keeps us out of
 * their own picks until then); bust = pulled over next to one; escape = the
 * game's own give-up for every car after us, or all of them far for long. */
static void police_frame(uint32_t p)
{
    static uint32_t chased;                     /* police cars after us at the last frame (log on change) */
    static uint32_t seen[16];                   /* brain whose velocity cv[i] holds */
    static float cv0[16][3], uv0[3];
    static double last_t, bust_t, gone_t, far_t;
    static unsigned stray;
    uint32_t setup = MEM32(ROAM_SETUP_VA), n, us, i, b, hit = 16u;
    float up[3], uv[3], cp[3], cv[3], d, a_us = 0, a_cop = 0, hd = 0, sp;
    double now, dt;
    int have, ok, near = 0, all_far = 1;
    char why[160];
    if (!p || p != police_p0())
        return;
    s_pol_live = police_pack() && roam_free_cruise() && online_ram(setup) && MEM32(setup + 0x18u) == 0;
    if (!s_pol_live) {
        chased = 0;
        s_pol_wanted = 0;
        last_t = 0;
        return;
    }
    police_scrub();
    now = roam_now();
    dt = last_t ? now - last_t : 0;
    last_t = now;
    ok = dt > 0.002 && dt < 0.2;                /* not after a pause or a load */
    have = police_car(MEM32(p + 0x14u), up, uv);
    for (i = 0; i < 16u; i++) {
        if (!(b = police_cop(i)) || !police_car(police_veh(b), cp, cv)) {
            seen[i] = 0;
            continue;
        }
        d = have ? police_dist(cp, up) : 1e9f;
        if (!s_pol_wanted && MEM32(b + POL_TARGET) == p) {
            if (stray++ < 8u)
                fprintf(stderr, "[POLICE] police car %u went after us with no hit (state %u, its own pick): back to "
                                "patrol\n", (unsigned)i, (unsigned)MEM32(b + POL_STATE));
            police_drop(b);
        } else if (!s_pol_wanted && have && ok && seen[i] == b && hit == 16u && d < POL_HIT_M) {
            float au = police_acc(uv, uv0, dt), ac = police_acc(cv, cv0[i], dt);
            if (au > POL_HIT_ACC || ac > POL_HIT_ACC) {
                hit = i;
                hd = d;
                a_us = au;
                a_cop = ac;
            }
        } else if (s_pol_wanted && MEM32(b + POL_TARGET) == p) {
            if (MEM32(b + POL_STATE) == 7u && d < POL_BUST_M)
                near = 1;
            if (d <= POL_LOST_M)
                all_far = 0;
        }
        seen[i] = b;
        memcpy(cv0[i], cv, sizeof cv);
    }
    if (have)
        memcpy(uv0, uv, sizeof uv);
    if (hit < 16u) {
        snprintf(why, sizeof why, "%.1f m apart, our speed changed %.0f m/s^2, its %.0f m/s^2, we at %.0f km/h",
                 (double)hd, (double)a_us, (double)a_cop,
                 (double)sqrtf(uv[0] * uv[0] + uv[1] * uv[1] + uv[2] * uv[2]) * 3.6);
        police_chase(p, hit, up, why);
        bust_t = gone_t = far_t = 0;
    }
    n = MEM32(POL_NCOPS_VA);
    us = police_on_us(p);
    if (us != chased) {
        fprintf(stderr, "[POLICE] chase: %u of %u police car%s after us\n", (unsigned)us, (unsigned)n,
                n == 1 ? "" : "s");
        chased = us;
    }
    if (s_pol_wanted) {
        sp = have ? sqrtf(uv[0] * uv[0] + uv[1] * uv[1] + uv[2] * uv[2]) : 1e9f;
        if (near && sp < POL_BUST_MS && now - s_pol_hit_t > POL_GRACE_S) {
            if (!bust_t)
                bust_t = now;
            else if (now - bust_t >= POL_BUST_S) {
                snprintf(why, sizeof why, "pulled over: a police car in close pursuit within %.0f m, our car under "
                         "%.0f m/s for %.0f s", (double)POL_BUST_M, (double)POL_BUST_MS, POL_BUST_S);
                police_fine(why);
                bust_t = 0;
            }
        } else
            bust_t = 0;
    }
    if (s_pol_wanted) {
        gone_t = us ? 0 : gone_t ? gone_t : now;
        far_t = us && all_far ? (far_t ? far_t : now) : 0;
        if (gone_t && now - gone_t >= 1.0)
            police_escaped("every police car gave us up: the game's own rule, past 300 m in a chase, 1000 m or 20 s "
                           "closing in");
        else if (far_t && now - far_t >= POL_LOST_S)
            police_escaped("every police car after us farther than 250 m for 10 s");
    }
    police_arrest();
}
