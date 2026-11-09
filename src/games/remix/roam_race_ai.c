/* roam_race_ai.c -- Free Roam feature 3: an AI race from a race start point,
 * then back to the cruise at the same spot (docs/roam-races.md, F3.1-F3.5).
 *
 * Included by recomp_manual.c after roam_races.c (uses roam_call,
 * roam_set_pose, roam_status, roam_now, online_ram, s_online, remix_driving,
 * the state machine's request override sub_00020E40).
 *
 * Start (the game's own path from a cruise into a race, as career
 * Progress() sub_002779D0 and the hookmen do): in the Free Roam cruise
 * (race setup [0x53F2E0] +0x18 == 0, no System Link game), within
 * RRA_RADIUS m of a race start point (roam_races_nearest) and slower than
 * RRA_SLOW m/s, White on pad 1 (keyboard Q/E/Shift, touch White) starts
 * it: our car's matrix is kept, the setup to load next [0x53F2E4] becomes
 * a copy of the current one (sub_00268E50, cdecl (dst, src)), SetEvent
 * sub_0026B8E0 (thiscall (city, event), ret 8) makes it that event (type,
 * name, time, weather; the AI opponents come from the event's
 * OpponentCount when the race loads), one local player, no custom/network
 * race, no hookmen; then Request(8) (end of the current race events) and
 * Request(6) (state 2 loads [0x53F2E4]). Our roam status becomes
 * RC_ST_EVENT: peers park and hide our car, slot kept (as the garage).
 * roam_build (roam_game.c) adds no remote slots to a race load (type != 0),
 * so the race has only its own racers, and with no slots the cruise-only
 * watchers (roam_flow_watch's phase-7 hold, roam_cars) stay out of it: the
 * race ends normally (RACE OVER).
 *
 * Back: an Arcade race leaves through the front end -- the race-over and
 * pause menus' quit, the race flow's end -- with state request 0x16 or 0x14
 * (both EnterState(7), table at 0x2108C). While our race runs, such a
 * request becomes DoQuitToCruise's (sub_0016A340): [0x53F2E4] = the city's
 * cruise (SetEvent(city, -1), the cruise's time and weather back), one
 * player, Request(6). The cruise load then gets its slots again; at its
 * first frame our car is put back where it was (SetMatrix/SetVel, zero
 * speed) and held there RRA_HOLD s while the city streams in, then
 * watched RRA_WATCH s: if it falls RRA_FALL m below the spot it is put
 * back (at most 3 times). roam_player's first cruise frame sends roam
 * status RC_ST_ROAM + a teleport, so peers show our car there.
 *
 * Prompt (browser): near a start point the line "Race: <name> ..." is
 * published in roam_prompt_box and web/pre.js shows it at the bottom of
 * the picture; native logs '[RACE] near ...' once per start point.
 *
 * MC3_ROAM_RACE=0: off; =2: Request(6) alone at the start (no Request(8),
 * if the career's pair misbehaves in the Arcade cruise). Log lines '[RACE]'. */

#include <ctype.h>
#include "input/xinput_xbox.h"

#define RRA_NEXT_VA   0x0053F2E4u       /* the race setup to load next */
#define RRA_SM_VA     0x00557ED0u       /* the state machine (request: vtable +4, sub_00020E40) */
#define RRA_TEXT      192               /* prompt box bytes (web/pre.js reads the same) */
#define RRA_RADIUS   30.0f             /* m: a start point this near gets the prompt */
#define RRA_SLOW      8.0f              /* m/s: slower than this to start */
#define RRA_HOLD      1.5               /* s: the car held on its spot after the way back */
#define RRA_WATCH     6.0               /* s: then watched for a fall through unstreamed ground */
#define RRA_FALL      10.0f             /* m below the spot = fallen */

void sub_00268E50(void);                        /* race setup copy, cdecl (dst, src) */
void sub_0026B8E0(void);                        /* SetEvent, thiscall (city, event; -1 = the city's cruise), ret 8 */
void sub_00020E40(void);                        /* state request (override in recomp_manual.c), thiscall (n), ret 4 */

/* The prompt: three lines joined by '\n' (title, details, action); in the action '[Q]' is a keycap
 * (web/pre.js). */
static void rra_text(char *out, size_t n, const roam_race *r, int slow)
{
    static const char *const kinds[RR_KINDS] = { "Sprint", "Circuit", "Unordered", "Autocross", "Track", "Frenzy" };
    static const char *const skip[] = { "autox", "autocross", "sprint", "circuit", "cp", "ordered", "unordered",
                                        "track", "frenzy", "race", "atlanta", "detroit", "tokyo", "sandiego", NULL };
    static const char *const diff[] = { "easy", "medium", "hard", NULL };
    const char *base = r->name, *p, *e;
    char title[96] = "", det[64] = "", *t = title;
    const char *level = NULL;
    int first = 1, ntok = 0, k;
    for (p = r->name; *p; p++)                  /* last part of 'autox\easy\sd_autox_easy_downtown' */
        if (*p == '\\' || *p == '/')
            base = p + 1;
    for (p = base; *p; p = *e ? e + 1 : e) {    /* words between '_' */
        size_t len;
        e = strchr(p, '_');
        e = e ? e : p + strlen(p);
        len = (size_t)(e - p);
        ntok++;
        if (!len)
            continue;
        if (first && ntok == 1 && len <= 3 && strchr(base, '_') && strchr(strchr(base, '_') + 1, '_'))
            continue;                           /* city prefix: sd, atl, det */
        for (k = 0; diff[k]; k++)
            if (strlen(diff[k]) == len && !strncasecmp(p, diff[k], len))
                level = diff[k];
        for (k = 0; skip[k] && !(strlen(skip[k]) == len && !strncasecmp(p, skip[k], len)); k++)
            ;
        if (skip[k])
            continue;
        for (k = 0; diff[k] && !(strlen(diff[k]) == len && !strncasecmp(p, diff[k], len)); k++)
            ;
        if (diff[k])
            continue;
        if (!first && t < title + sizeof title - 1)
            *t++ = ' ';
        for (k = 0; k < (int)len && t < title + sizeof title - 1; k++)
            *t++ = (char)(k ? tolower((unsigned char)p[k]) : toupper((unsigned char)p[k]));
        *t = 0;
        first = 0;
    }
    if (!title[0])
        snprintf(title, sizeof title, "%s", base);
    if (level)
        snprintf(det, sizeof det, "%c%s", toupper((unsigned char)level[0]), level + 1);
    if (r->laps > 1)
        snprintf(det + strlen(det), sizeof det - strlen(det), "%s%u laps", det[0] ? " \xC2\xB7 " : "", r->laps);
    snprintf(det + strlen(det), sizeof det - strlen(det), "%s%u opponent%s", det[0] ? " \xC2\xB7 " : "", r->opponents,
             r->opponents == 1 ? "" : "s");
    snprintf(out, n, "%s \xC2\xB7 %s\n%s\n%s", r->kind < RR_KINDS ? kinds[r->kind] : "Race", title, det,
             slow ? "Slow down to race" : "Press White [Q] to race");
}

static int s_rra_race;                         /* our AI race is loading or running */
static int s_rra_back;                          /* the cruise load back: put the car back */
static float s_rra_m[12];                       /* our car's matrix at the start */
static uint32_t s_rra_time, s_rra_weather;      /* the cruise's (+4, +8) */
static double s_rra_t0;                         /* the first cruise frame after the race (roam_now), 0 not yet */
static int s_rra_tries;
static char s_rra_name[96];

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* Read by the page (web/pre.js): seq +1 per update, then the text (empty: none). */
static struct { volatile int32_t seq; char text[RRA_TEXT]; } s_rra_prompt;
EMSCRIPTEN_KEEPALIVE uintptr_t roam_prompt_box(void) { return (uintptr_t)&s_rra_prompt; }
#endif

static void rra_prompt(const char *text)
{
#ifdef __EMSCRIPTEN__
    snprintf(s_rra_prompt.text, sizeof s_rra_prompt.text, "%s", text);
    __atomic_add_fetch(&s_rra_prompt.seq, 1, __ATOMIC_RELEASE);
#else
    (void)text;
#endif
}

static int rra_mode(void)                       /* 0 off, 1 on, 2 on without Request(8) */
{
    static int m = -1;
    if (m < 0) {
        const char *v = getenv("MC3_ROAM_RACE");
        m = v && v[0] ? atoi(v) : 1;
    }
    return m;
}

static void rra_request(uint32_t n, uint32_t top)
{
    uint32_t sm = MEM32(RRA_SM_VA);
    if (online_ram(sm))
        roam_call(sub_00020E40, sm, top, 1, &n);
}

/* Our car's physics collider (velocity at +0x44), 0 none. */
static uint32_t rra_coll(uint32_t veh)
{
    uint32_t car = MEM32(veh + 8u), phys;
    if (!online_ram(car) || !online_ram(phys = MEM32(car + 0x68u)) || !online_ram(MEM32(phys + 0x10u)))
        return 0;
    return MEM32(phys + 0x10u);
}

static void rra_start(const roam_race *r, uint32_t mtx, uint32_t top)
{
    uint32_t cur = MEM32(ROAM_SETUP_VA), next = MEM32(RRA_NEXT_VA), a[2], k;
    if (!online_ram(cur) || !online_ram(next))
        return;
    for (k = 0; k < 12; k++)
        s_rra_m[k] = MEMF(mtx + 0x10u + 4u * k);
    s_rra_m[10] += 0.3f;                        /* a little above the ground it stood on */
    s_rra_time = MEM32(cur + 4u);
    s_rra_weather = MEM32(cur + 8u);
    if (next != cur) {
        a[0] = next;
        a[1] = cur;
        roam_call(sub_00268E50, 0, top, 2, a);
    }
    a[0] = r->city;
    a[1] = r->index;
    roam_call(sub_0026B8E0, next, top, 2, a);
    MEM32(next + 0x4Cu) = 1;                    /* us alone: the opponents are the event's AI */
    MEM8(next + 0x48u) = 0;                     /* an event race, not a custom one */
    MEM8(next + 0x108u) = 0;                    /* not a network race */
    MEM8(next + 0x10Du) = 0;                    /* no hookmen */
    snprintf(s_rra_name, sizeof s_rra_name, "%s", r->name);
    s_rra_race = 1;
    s_rra_back = 0;
    rra_prompt("");
    roam_status(RC_ST_EVENT);                   /* peers hide our car, slot kept */
    fprintf(stderr, "[RACE] start %s (%s, type %u, %u lap%s, %u opponents; city %u event %u) from (%.1f, %.1f, %.1f)%s\n",
            r->name, roam_races_kind_name(r->kind), r->type, r->laps, r->laps == 1 ? "" : "s", r->opponents,
            r->city, r->index, s_rra_m[9], s_rra_m[10], s_rra_m[11],
            rra_mode() == 2 ? ", Request(6) alone (MC3_ROAM_RACE=2)" : "");
    if (rra_mode() != 2)
        rra_request(8, top);
    rra_request(6, top);
}

/* A state request (sub_00020E40, esp at its return address, n at +4)
 * before the game's: while our race runs, the front end (0x14, 0x16)
 * becomes the way back to the Free Roam cruise. */
static void rra_state_request(uint32_t esp)
{
    uint32_t n = MEM32(esp + 4u), next = MEM32(RRA_NEXT_VA), cur = MEM32(ROAM_SETUP_VA), a[2];
    if (!s_online) {
        s_rra_race = s_rra_back = 0;
        return;
    }
    if (!s_rra_race || (n != 0x14u && n != 0x16u) || !online_ram(next))
        return;
    if (!online_ram(cur) || MEM32(cur + 0x18u) == 0) {  /* a cruise is running: ours is not (stale flag) */
        fprintf(stderr, "[RACE] request 0x%X in a cruise: to the menus as the game does (AI race flag cleared)\n",
                (unsigned)n);
        s_rra_race = 0;
        return;
    }
    a[0] = online_ram(cur) ? MEM32(cur) : MEM32(next);   /* the race's city */
    a[1] = 0xFFFFFFFFu;                         /* -1: the city's cruise event (sub_0026D9E0) */
    roam_call(sub_0026B8E0, next, (esp - 0x100u) & ~0xFu, 2, a);
    MEM32(next + 4u) = s_rra_time;
    MEM32(next + 8u) = s_rra_weather;
    MEM32(next + 0x4Cu) = 1;                    /* roam_build adds the slots again */
    MEM8(next + 0x108u) = 0;
    MEM32(esp + 4u) = 6;                        /* load [0x53F2E4], not the front end */
    s_rra_race = 0;
    s_rra_back = 1;
    s_rra_t0 = 0;
    s_rra_tries = 0;
    fprintf(stderr, "[RACE] %s over: request 0x%X (front end) -> back to the Free Roam cruise (city %u, type %u)\n",
            s_rra_name, (unsigned)n, (unsigned)MEM32(next), (unsigned)MEM32(next + 0x18u));
}

/* SetLabel (sub_00348D70, thiscall (item, column, text), esp at its return
 * address), before the game's: in our AI race the pause and race-over
 * menus' (builder sub_0016ADD0) "GE_EXIT_TO_MC3FE" item reads "Back to Free
 * Roam". Its action 0x1C runs sub_001621E0(0): Request(0x16), which
 * rra_state_request turns into the cruise. SetLabel copies the text, so it
 * sits below the call's frame. */
static void rra_label(uint32_t esp)
{
    static const char txt[] = "Back to Free Roam";
    uint32_t cur = MEM32(ROAM_SETUP_VA), n, b, t, buf = (esp - 0x400u) & ~0xFu, i;
    if (!s_online || !s_rra_race || !online_ram(cur) || MEM32(cur + 0x18u) == 0 || !str_table(&n, &b) ||
        !(t = str_text(n, b, "GE_EXIT_TO_MC3FE")) || MEM32(esp + 0xCu) != t)
        return;
    for (i = 0; txt[i]; i++)
        MEM16(buf + 2u * i) = (uint8_t)txt[i];
    MEM16(buf + 2u * i) = 0;
    MEM32(esp + 0xCu) = buf;
}

/* Each player's update (sub_002C1520), before roam_player: slot 0 only. */
static void rra_player(uint32_t p)
{
    static int white_was;
    static const roam_race *near_said;
    XBOX_INPUT_STATE st;
    uint32_t setup = MEM32(ROAM_SETUP_VA), veh, mtx, coll, top = (g_esp - 0x400u) & ~0xFu;
    const roam_race *r;
    float x, y, z, v[3] = { 0 }, speed, d;
    int white;
    char text[RRA_TEXT];
    if (!s_online || !rra_mode() || !online_ram(p) || MEM32(p + 0x5Cu) != 0 || !online_ram(setup) ||
        !online_ram(veh = MEM32(p + 0x14u)) || !online_ram(mtx = MEM32(veh + 0x1Cu)))
        return;
    x = MEMF(mtx + 0x34u);
    y = MEMF(mtx + 0x38u);
    z = MEMF(mtx + 0x3Cu);
    if (s_rra_back && MEM32(setup + 0x18u) == 0) {          /* the cruise again: our car back on its spot */
        double now = roam_now(), dt;
        if (!s_rra_t0) {
            s_rra_t0 = now;
            fprintf(stderr, "[RACE] back in Free Roam: car put back at (%.1f, %.1f, %.1f), held %.1f s\n",
                    s_rra_m[9], s_rra_m[10], s_rra_m[11], RRA_HOLD);
        }
        dt = now - s_rra_t0;
        if (dt < RRA_HOLD) {
            roam_set_pose(veh, s_rra_m, v);
            if (roam_running())
                roam_teleport();                /* peers snap to it */
        } else if (y < s_rra_m[10] - RRA_FALL && s_rra_tries < 3) {
            s_rra_tries++;
            s_rra_t0 = now;
            fprintf(stderr, "[RACE] the car fell %.1f m below its spot (ground not streamed in yet?): put back (%d of 3)\n",
                    s_rra_m[10] - y, s_rra_tries);
        } else if (dt > RRA_WATCH)
            s_rra_back = 0;
        rra_prompt("");
        return;
    }
    white = xbox_InputGetState(0, &st) == 0 && st.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] > XBOX_ANALOG_BUTTON_THRESHOLD;
    r = NULL;
    if (!s_rra_race && MEM32(setup + 0x18u) == 0 && !MEM8(setup + 0x108u) && MEM32(0x0050F14Cu) == 0 &&
        remix_driving())
        r = roam_races_nearest(MEM32(setup), x, z, RRA_RADIUS, &d);
    if (!r) {
        white_was = white;
        rra_prompt("");
        return;
    }
    if ((coll = rra_coll(veh)) != 0)
        for (int k = 0; k < 3; k++)
            v[k] = MEMF(coll + 0x44u + 4u * k);
    speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (r != near_said) {
        near_said = r;
        fprintf(stderr, "[RACE] near %s (%s, %u opponents), %.0f m\n", r->name, roam_races_kind_name(r->kind),
                r->opponents, d);
    }
    rra_text(text, sizeof text, r, speed > RRA_SLOW);
    rra_prompt(text);
    if (white && !white_was && speed <= RRA_SLOW)
        rra_start(r, mtx, top);
    white_was = white;
}
