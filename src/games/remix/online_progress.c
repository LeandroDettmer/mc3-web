/* online_progress.c -- Online progression: race prizes and wins unlock what
 * the Online session still leaves locked (design A, owner-approved
 * 2026-11-12).
 *
 * Included by recomp_manual.c after roam_game.c, before roam_race_ai.c
 * (uses s_online, s_on_wins, online_money_va, online_garage_left,
 * roam_now, roam_call, ROAM_SETUP_VA; rra_prompt shows op_note()).
 *
 * The Online session keeps $2,000,000 on a fresh profile and every car and
 * part unlocked (0x0053F2EE / 0x0053F2F2). Still locked there: the driving
 * abilities -- rule types 0xB-0x10 (names at 0x004B3A18: ZONEAGROROAR,
 * NITRO, SST, BO, 2WD, IAC), checked by sub_00289EB0 (0xB, cdecl (special),
 * ret 4) and sub_00289E10/E30/E50/E70/E90 (0xC..0x10, cdecl, ret), which
 * answer 1 when 0x0053F2F0 is set, else ask the rules (sub_00292CD0) against
 * the career's progress (none in the Online session: a beginner); the car
 * setup (sub_001240F0, sub_002BF920) asks them, so an ability counts from
 * the next car build (a cruise load, the garage) -- and the reward paints,
 * which the paint list (sub_0014C180) asks the GENERIC check sub_0028B4E0
 * (thiscall (name, 0), ret 8) by name ("Pearlescent_reward" 0x0045AFF8,
 * "ColorShift_reward" 0x0045AFE4), not 0x0053F2F2.
 *
 * Every race counts: a street race (roam_challenge.c rch_result: we
 * finished, and at least one other racer started it, finished or not) and
 * an AI race from a start point (roam_race_ai.c; our finish seen at the
 * race flow's finish state sub_001C2B00 -> sub_001CFC50, the career's prize
 * routine, wrapped: in Arcade it does nothing, game mode != 2; place =
 * racer record (race + 0x98 + i * 0xB8, i = sub_002694D0) + 0x14, 1-based).
 * Prize by place: 1st $25,000, 2nd $12,000, 3rd $6,000 (an AI race's event
 * prize, [ev + 0x28] via the career's sub_0027F030, is career-only); a
 * street race pays that x racers / 2. A 1st place is a win. Wins unlock, in
 * order (OP_TIERS): the checks above answer 1 for what the wins reached,
 * only while the Online session is on (the career never sees it; leaving
 * Online is leaving the override). Money goes to [[0x557F0C]+0xC]+0xACC and
 * the Online profile is saved at once (online_garage_left: money, garage,
 * car, wins; profile.bin version 3). The career's reward paths
 * (sub_001CFC50's body, ReportRaceResult sub_00277EE0) are never run.
 *
 * Shown in the prompt box (rra_prompt): the street race's result line, or
 * after an AI race in the cruise, OP_NOTE_S s (big the first 2). Log lines
 * '[ONLINE] progress: ...'. */

#define OP_PEARL   0x100                       /* not rule types: the reward paints */
#define OP_SHIFT   0x101
#define OP_NOTE_S  8.0                         /* s the line shows back in the cruise */

void sub_002694D0(void);                        /* the local player's racer index, cdecl; eax */

static const struct { uint32_t wins; int type; const char *name, *code; } s_op_tiers[] = {
    { 1, 0xC, "Nitro", "NITRO" },
    { 2, 0xD, "Slingshot", "SST" },
    { 3, 0xE, "Burnout", "BO" },
    { 4, OP_PEARL, "Pearlescent paint", "Pearlescent_reward" },
    { 5, 0xF, "Two-wheel driving", "2WD" },
    { 6, 0x10, "In-air control", "IAC" },
    { 8, 0xB, "Zone / Agro / Roar", "ZONEAGROROAR" },
    { 10, OP_SHIFT, "Color-shift paint", "ColorShift_reward" },
};
#define OP_TIERS ((int)(sizeof s_op_tiers / sizeof s_op_tiers[0]))

static char s_op_line[96];                     /* the last prize, two lines ("+$25,000\nUnlocked: Nitro") */
static char s_op_flat[96];                     /* the same on one line, for the street race's result */
static int s_op_note;                          /* s_op_line to show back in the cruise */
static double s_op_t;                          /* when it first showed there (0 not yet) */
static int s_op_paid;                          /* this AI race paid (cleared at its start) */

/* Wins reach this type's tier, in the Online session. */
static int op_has(int type)
{
    static uint32_t said;                      /* logged once a run per tier */
    int i;
    if (!s_online)
        return 0;
    for (i = 0; i < OP_TIERS; i++)
        if (s_op_tiers[i].type == type) {
            if (s_on_wins < s_op_tiers[i].wins)
                return 0;
            if (!(said & (1u << i))) {
                said |= 1u << i;
                fprintf(stderr, "[ONLINE] progress: %s unlocked by %u wins (rule %s)\n", s_op_tiers[i].name,
                        (unsigned)s_on_wins, s_op_tiers[i].code);
            }
            return 1;
        }
    return 0;
}

/* An ability check (sub_00289E10..EB0, cdecl, `args` dwords): 1 = answered "unlocked". */
static int op_ability(int type, uint32_t args)
{
    if (!op_has(type))
        return 0;
    g_eax = (g_eax & ~0xFFu) | 1u;
    g_esp += 4u + 4u * args;
    return 1;
}

/* The GENERIC check sub_0028B4E0 (thiscall (name, x), ret 8): the reward paints. */
static int op_paint(void)
{
    uint32_t name = MEM32(g_esp + 4u);
    const char *s;
    int type;
    if (!s_online || !online_ram(name))
        return 0;
    s = (const char *)XBOX_PTR(name);
    type = !strncmp(s, "Pearlescent_reward", 19) ? OP_PEARL : !strncmp(s, "ColorShift_reward", 18) ? OP_SHIFT : 0;
    if (!type || !op_has(type))
        return 0;
    g_eax = (g_eax & ~0xFFu) | 1u;
    g_esp += 12u;
    return 1;
}

static void op_dollars(char *out, size_t n, uint32_t v)   /* 25000 -> "25,000" */
{
    if (v >= 1000000u)
        snprintf(out, n, "%u,%03u,%03u", v / 1000000u, v / 1000u % 1000u, v % 1000u);
    else if (v >= 1000u)
        snprintf(out, n, "%u,%03u", v / 1000u, v % 1000u);
    else
        snprintf(out, n, "%u", v);
}

/* A finish at `place` (1-based) of `racers`: prize, win, unlock, save. `what` names the race for the log. */
static void op_award(int place, int racers, int street, int note, const char *what)
{
    static const uint32_t prize[3] = { 25000u, 12000u, 6000u };
    uint32_t money, m = online_money_va();
    char d[24], next[64] = "";
    const char *unlocked = NULL;
    int i;
#ifndef __EMSCRIPTEN__
    (void)prize; (void)racers; (void)street; (void)note; (void)what; (void)m; (void)d; (void)next; (void)unlocked;
    (void)i;
    return;                                     /* browser only: the native Online session stays as it was */
#endif
    if (!s_online || place < 1)
        return;
    if (place > 3) {                            /* no prize, no win: nothing to show or save */
        fprintf(stderr, "[ONLINE] progress: %s, place %d: no prize (1st-3rd pay)\n", what, place);
        return;
    }
    money = street ? prize[place - 1] * (uint32_t)racers / 2u : prize[place - 1];
    if (place == 1) {
        s_on_wins++;
        for (i = 0; i < OP_TIERS; i++)
            if (s_op_tiers[i].wins == s_on_wins)
                unlocked = s_op_tiers[i].name;
    }
    for (i = 0; i < OP_TIERS && s_op_tiers[i].wins <= s_on_wins; i++)
        ;
    if (i < OP_TIERS)
        snprintf(next, sizeof next, "%u win%s \xC2\xB7 next: %s at %u", (unsigned)s_on_wins, s_on_wins == 1 ? "" : "s",
                 s_op_tiers[i].name, (unsigned)s_op_tiers[i].wins);
    else
        snprintf(next, sizeof next, "%u wins \xC2\xB7 everything unlocked", (unsigned)s_on_wins);
    if (money && m) {
        MEM32(m) += money;
        s_on_money = MEM32(m);
    }
    op_dollars(d, sizeof d, money);
    snprintf(s_op_line, sizeof s_op_line, "+$%s\n%s%s", d, unlocked ? "Unlocked: " : "", unlocked ? unlocked : next);
    snprintf(s_op_flat, sizeof s_op_flat, "+$%s \xC2\xB7 %s%s", d, unlocked ? "unlocked: " : "", unlocked ? unlocked : next);
    s_op_note = note;
    s_op_t = 0;
    fprintf(stderr, "[ONLINE] progress: %s, place %d (racers %d, 0 = AI race): +$%u (money %u%s), wins %u%s%s\n",
            what, place, racers,
            (unsigned)money, m ? (unsigned)MEM32(m) : 0u, m ? "" : ", no progress object: not paid",
            (unsigned)s_on_wins, unlocked ? ", unlocked " : "", unlocked ? unlocked : "");
    online_garage_left();                       /* the profile saved now (money, garage, car, wins) */
}

/* The street race's result (rch_result): we finished at `place` of `racers`. */
static void op_street(int place, int racers, int finished)
{
    s_op_flat[0] = 0;
    if (!finished || racers < 2) {              /* no prize alone or unfinished */
        fprintf(stderr, "[ONLINE] progress: street race %s: no prize\n", finished ? "with no other racer" : "not finished");
        return;
    }
    op_award(place, racers, 1, 0, "street race");
}

/* The race flow's finish (sub_001CFC50 wrapper, ecx = race), our AI race only. */
static void op_ai_finish(uint32_t race, uint32_t top)
{
    uint32_t idx, place;
    if (s_op_paid || !online_ram(race) || MEM8(race + 0xEu))
        return;
    idx = roam_call(sub_002694D0, 0, top, 0, NULL);
    if (idx > 7u)
        return;
    place = MEM32(race + 0x98u + idx * 0xB8u + 0x14u);
    s_op_paid = 1;
    if (place < 1u || place > 8u) {
        fprintf(stderr, "[ONLINE] progress: AI race finish, place %u unreadable: no prize\n", (unsigned)place);
        return;
    }
    op_award((int)place, 0, 0, 1, "AI race");
}

/* The line for an empty prompt: the AI race's prize, back in the cruise. */
__attribute__((unused)) static const char *op_note(void)   /* browser only (rra_prompt) */
{
    static char out[100];
    uint32_t setup = MEM32(ROAM_SETUP_VA);
    double now;
    if (!s_op_note || !s_online || !online_ram(setup) || MEM32(setup + 0x18u) != 0)
        return "";
    now = roam_now();
    if (!s_op_t)
        s_op_t = now;
    if (now - s_op_t > OP_NOTE_S) {
        s_op_note = 0;
        return "";
    }
    snprintf(out, sizeof out, "%s%s", now - s_op_t < 2.0 ? "\x01" : "", s_op_line);
    return out;
}
