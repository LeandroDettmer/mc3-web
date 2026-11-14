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
 * answer al != 0, call returning to 0x001EFF03) and the traffic, rates each
 * with the heat test sub_001E4A70 (speed over the road's limit x tune
 * 0x4B2AB8 / 0x4B2AB4, ...) and the highest becomes the target (brain +0x20C,
 * level +0x210) and goes on the wanted list 0x534120 ({who, level} x16,
 * count 0x534114). The remote players' cars are players 1..7 in our game:
 * that indirect call answers 0 for any player but ours (police_icall, via
 * recomp_lookup_manual on its return address, no relift), so a remote car
 * is never a candidate. Each frame a brain whose target is another player
 * anyway is cleared, and such entries leave the wanted list.
 *
 * Arrest: the racer-finished call (police_finish) for us while a police car
 * is after us, or our record's arrested byte (police_arrest). Fine POLICE_FINE from the Online money
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

static int s_pol_live;                  /* the Online Free Roam cruise with police: the filters run */
static uint32_t s_pol_va;               /* the indirect call's real target (police_icall) */
static double s_pol_note_t;             /* when the fine's line went up, 0 none */
static char s_pol_note[96];

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
 * args): edi is the player. Ours: the game's own; another: al 0, skipped. */
static void police_icall(void)
{
    recomp_func_t fn;
    if (g_edi != police_p0()) {
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

/* The chase is over: no target for any police car, the wanted list empty. */
static void police_calm(void)
{
    uint32_t n = MEM32(POL_NCOPS_VA), i, b;
    for (i = 0; i < n && i < 16u; i++)
        if (online_ram(b = MEM32(POL_COPS_VA + 4u * i))) {
            MEM32(b + POL_TARGET) = 0;
            MEM32(b + POL_LEVEL) = 0;
        }
    for (i = 0; i < 32u; i++)
        MEM32(POL_WANTED_VA + 4u * i) = 0;
    MEM32(POL_NWANTED_VA) = 0;
}

/* Remote players out of the targets and the wanted list (a safety net: police_icall keeps them out). */
static void police_scrub(void)
{
    uint32_t n = MEM32(POL_NCOPS_VA), i, j, b, w;
    for (i = 0; i < n && i < 16u; i++)
        if (online_ram(b = MEM32(POL_COPS_VA + 4u * i)) && police_remote(MEM32(b + POL_TARGET))) {
            fprintf(stderr, "[POLICE] police car %u was after remote player %08X: target cleared\n", (unsigned)i,
                    (unsigned)MEM32(b + POL_TARGET));
            MEM32(b + POL_TARGET) = 0;
            MEM32(b + POL_LEVEL) = 0;
        }
    n = MEM32(POL_NWANTED_VA);
    for (i = j = 0; i < n && i < 16u; i++) {
        w = MEM32(POL_WANTED_VA + 8u * i);
        if (police_remote(w))
            continue;
        MEM32(POL_WANTED_VA + 8u * j) = w;
        MEM32(POL_WANTED_VA + 8u * j + 4u) = MEM32(POL_WANTED_VA + 8u * i + 4u);
        j++;
    }
    if (j != n && n <= 16u)
        MEM32(POL_NWANTED_VA) = j;
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

static uint32_t police_on_us(uint32_t p) /* police cars whose target is player p */
{
    uint32_t n = MEM32(POL_NCOPS_VA), i, b, k = 0;
    for (i = 0; i < n && i < 16u; i++)
        if (online_ram(b = MEM32(POL_COPS_VA + 4u * i)) && p && MEM32(b + POL_TARGET) == p)
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

/* Each player update (sub_002C1520), before the game's: player 0's only, once a frame. */
static void police_frame(uint32_t p)
{
    static uint32_t chased;                     /* police cars after us at the last frame (log on change) */
    uint32_t setup = MEM32(ROAM_SETUP_VA), n, us;
    if (!p || p != police_p0())
        return;
    s_pol_live = police_pack() && roam_free_cruise() && online_ram(setup) && MEM32(setup + 0x18u) == 0;
    if (!s_pol_live) {
        chased = 0;
        return;
    }
    police_scrub();
    n = MEM32(POL_NCOPS_VA);
    us = police_on_us(p);
    if (us != chased) {
        fprintf(stderr, "[POLICE] chase: %u of %u police car%s after us\n", (unsigned)us, (unsigned)n,
                n == 1 ? "" : "s");
        chased = us;
    }
    police_arrest();
}
