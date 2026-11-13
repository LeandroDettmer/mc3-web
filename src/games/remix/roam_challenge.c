/* roam_challenge.c -- Free Roam feature 2: a street race of 2 to RCH_RACERS
 * players inside the cruise (docs/roam-races.md, F2).  No load, no menu: the
 * racers stay in the Free Roam cruise, everyone else just sees cars driving.
 *
 * Included by recomp_manual.c after roam_tags.c (uses roam_call, roam_set_pose,
 * roam_races_*, rra_prompt, rra_coll, s_rra_race/s_rra_back, the map helpers
 * rmap_car_mat/rmap_f2u, tags_proj's camera, s_roam_seen).
 *
 * Flow (every player in the Free Roam cruise, driving):
 *  - Invite: while we drive slower than RCH_SLOW within RCH_NEAR m of other
 *    driving players (status roam, height within 10 m), the prompt box offers
 *    "Street race? N nearby players will be invited"; White (Q; the touch
 *    pad's Flash button) confirms.  White is also the game's headlight toggle
 *    (the press toggles them too).  While that prompt shows, White is ours, never
 *    the AI race prompt's.  We build a course (no course near: a short note)
 *    and send INVITE (race id, course, players) every RCH_RESEND s to the
 *    up to RCH_RACERS - 1 nearest of them for RCH_ASK_S s.
 *  - Each invitee sees "<host> invites you to a race (N players)" with the
 *    course on its maps; White joins (slower than RCH_SLOW) within RCH_ASK_S s:
 *    JOIN to the host every RCH_RESEND s.  A busy invitee answers NO; one more
 *    than RCH_AWAY m from the host drops out.
 *  - The window ends after RCH_ASK_S s, when every invitee still in has joined,
 *    or when the host presses White again: with no one joined, "nobody joined";
 *    else the host sends GO (milliseconds left to the start, the racers' ids in
 *    grid order, the receiver's slot) to each joined racer during the
 *    RCH_COUNT_S s countdown, and NO (late) to invitees who did not join.  Every
 *    racer is put on the event's own start grid (slot 0, the host: the .rac's
 *    PlayerPosDir; slot k: its k-th Opponent PosDir; each facing the .rac's
 *    direction) and held there; the game's own 3-2-1-GO (HUD message queue +
 *    countdown sound) where the HUD has it, else in the prompt box.
 *  - The race: the game's own checkpoints (below, "Native checkpoints"): the
 *    flare on the next one (a passed one fades out), the HUD's next-checkpoint
 *    arrow, the game's pass test and checkpoint sound; the course is on the
 *    maps (the race start icons are hidden meanwhile); the prompt box shows
 *    place, checkpoint, distance and time.  Each racer sends PROG (checkpoints,
 *    its finish time) to every other racer every RCH_RESEND s.  Without native
 *    checkpoints (another race object, MC3_ROAM_CHALLENGE=3) the old page
 *    marker and a RCH_CP_R m test stand in.
 *  - Places from the finish times (each on its own clock from its GO; the GO
 *    messages carry the time left, so the clocks start within one trip), then
 *    checkpoints.  Decided when every racer still in has finished, or when one
 *    is left racing RCH_LATE s after the latest finish (it is last), or at
 *    RCH_MAX_S s (the unfinished do not finish).  Shown RCH_RESULT_S s, then
 *    Free Roam goes on.  A racer gone or not roaming for RCH_GONE_S s, silent
 *    as long or saying NO is out (did not finish); all others out: cancelled
 *    (our result if we finished).  Our garage or a race load: we leave (NO).
 *    Unfinished and RCH_OFF_M m farther from our next checkpoint than our
 *    closest since the last one, or RCH_STALL_S s without a checkpoint: we
 *    gave up (NO decline): a racer who drove off kept its flag and the others
 *    waiting up to RCH_MAX_S s.
 *
 * Course: the game's own checkpoints of the city's nearest sprint or circuit
 * (roam_races list, start beacon within RCH_EVENT_M m).  The race's file is
 * tune/race/<city>/<event>.rac, read as the race loader does (sub_001CDB60:
 * the file system [0x5348B0] vt+0x20 open(dir, name, "rac", 0, 1), the text
 * tokenizer vtable 0x46D868 with ctor sub_0020EE80(name, stream); the loader
 * sub_001D4F20 tries each section keyword with sub_0020F3C0 and else skips a
 * token (vt+4); "numcheckpoints" N (vt+0x10) then N x "checkpoint" x y z
 * (vt+0x24) f f f f [Arrow f i] [Type Normal|Different] [RespawnAllowed i],
 * sub_001CDC80).  Only events whose .rac has "PlayerPosDir" x y z dx dy dz
 * (just before numcheckpoints in 1567 of 1576 files) count.  The course runs
 * from the event's checkpoint 0 to its last, as the game's race does (one lap
 * of a circuit); each side reads it from its own .rac.  Only if no file reads:
 * a chain of the city's race start beacons ahead of the host (on roads too),
 * RCH_COURSE_M m or RCH_MSG_CP checkpoints, sent in the INVITE.
 *
 * Each checkpoint keeps the .rac's four sizes (after x y z: +0x10C, +0x110
 * radius, +0x108 height, +0).  The start grid is the race's own: the player
 * slot (the host's) at PlayerPosDir, slot k at the k-th "Opponent { ... PosDir
 * x y z dx dy dz }" after the checkpoints (the first 3.5 m to the +(up x dir)
 * side in 241 of 270 files; a PosDir farther than RCH_OPP_M from the player
 * slot is skipped).  A slot the file has no opponent for stands in rows of
 * two behind PlayerPosDir: odd slots RCH_OPP_W m to that side, every second
 * slot RCH_ROW_M m further back.  dir is the direction of travel (the grid's back rows lie behind
 * it; dir points at checkpoint 0 in 739 of 787 files).  The loader
 * sub_001D4F20 stores the heading h = atan2(-dx, -dz), and the racer placement
 * (sub_001CBCA0 -> sub_002C8860, rotation sub_0008C2C0: local +z goes to
 * (sin h, 0, cos h)) turns the car so that its local +z is -dir: the car's
 * matrix row +0x28 points backwards (the engine's cars face -z), and
 * rch_grid_pose writes -dir there (until 2026-11-12 it wrote dir: both cars
 * stood turned around).  A slot whose dir points away from checkpoint 0
 * (dot < 0) is turned to face it.  Start beacons only: slots RCH_GRID_W m to
 * each side of where the host stood (rows RCH_ROW_M m apart), facing the first
 * beacon.
 *
 * Native checkpoints (every racer, each in its own world): the Free Roam
 * cruise's race object [[0x533E24]+4] (vtable 0x46B608) is an mcRaceBase with
 * no checkpoints.  The player's render (sub_002BE8E0) calls its vt+0x110
 * (sub_001CF3B0) each frame, which runs the flare (sub_001C8DB0) of every
 * checkpoint in race+0x24 (count) / +0x28 (array of 0x120 B, delete[]d by the
 * race's unload sub_001D2B50).  The cruise's own .rac has numcheckpoints 0,
 * and its reader new[]s 4 + 0x120 n bytes anyway: +0x28 is an empty array
 * (count 0 at -4), kept aside while ours is in and put back after (ours freed,
 * sub_001B3CD0; until 2026-11-11 that empty array made us refuse).  We give it
 * the course: an array made with the
 * game's new[] (sub_001B3C60) and checkpoint ctor (sub_001CA0A0), filled as
 * the .rac reader sub_001CDC80 does (+0xFC position, matrix at +4 with
 * sub_001C8B20, the sizes, +0xB8 self, +0x3C next), visual reset
 * sub_001C7690.  Racer record 0 (race+0x98, 0xB8 B): +0x34 target (word,
 * -1 none), +0x18 laps left (1: the last one is the finish flare), +0x48
 * passed, +0x4C passed bits, +4 finished.  The flare of the target shows
 * (state 3 hidden -> 2); passing (the game's gate test sub_001D1510: radius
 * +0x110, height +0x108) does as the checkpoint races (sub_001DFC00): state
 * 1, +0x48, the bit, audio event 0x15 (sub_000233C0 on [0x4CBAD8]).  The
 * flare's per-frame tick is the race's vt+0x11C (sub_001CCA20 -> each
 * checkpoint's sub_001C8D50 -> sub_001C7740: state 1 passed fades out to 3
 * hidden, 2 appearing fades in to 0 shown; scale sub_001C77C0); the checkpoint
 * races call it from their update (sub_001D8F20), the cruise never does, so
 * we call it every frame while ours are in (until 2026-11-12 a passed flare
 * stayed up, frozen at full size).  The HUD's arrow asks race vt+0xE8 for the target: the cruise's is a stub
 * (sub_00129FA0), so the cruise vtable's slot points at the ordered races'
 * sub_001DF290 meanwhile (false with no checkpoints).  Countdown: the HUD
 * message queue [[player+0x58]+0x50] sub_000B6330(id: 2 "3", 1 "2", 0 "1",
 * 3 "GO") and audio event 0xC, as the race flow's phase 3 (sub_001C20E0).
 * Afterwards race+0x24 = 0 and the record back.  The game's map draws race
 * checkpoints only for race types it knows (sub_000C2CE0: none for the
 * cruise), so ours draw them in its colours (map tune [0x507508] +0x350 next,
 * +0x360 the one after, +0x370 finish).
 *
 * Messages (roam_send_msg, peer to peer, ROAM_MSG_MAX bytes): 'C', version 5
 * (v4 builds drop ours, we drop theirs), op, arg, race id u32, then by op:
 * INVITE (arg = n checkpoints) start line (x, y, z) f32 (PlayerPosDir's
 * position), city u8, event u16, players u8 (invited + host), then only for
 * event 0xFFFF (start beacons) n x (x, y, z) f32; with an event the invitee
 * reads the course, sizes and grid slots from its own .rac and declines (NO
 * busy) when that gives another count or start line; JOIN; NO (arg = reason:
 * decline, busy, cancel, late); GO (arg = the receiver's slot) u16 ms to the
 * start, racers u8, pad u8, racers x id u32 in slot order (slot 0, the host:
 * 0, the sender); PROG (arg = checkpoints passed) finished u8, 3 pad, finish ms
 * u32.  Anything else, or another race id, is dropped (an INVITE while busy is
 * answered NO busy; two hosts inviting each other: the lower race id goes on).
 *
 * Group race (2026-11-12, untested in game): at a race start point (within
 * RRA_RADIUS m, slower than RRA_SLOW, an event with opponents) with players
 * near, the prompt offers the event itself as a group race: the same INVITE
 * with 0 checkpoints and the event (v5 builds before it answer NO busy: they
 * take 0 checkpoints as no course), JOIN, GO; at GO every racer loads the
 * event with its AI (roam_race_ai.c rra_group_start: racers in join order,
 * each sees the others' cars in its race). Nobody joins, or every other
 * racer drops out before GO: the host races the AI alone. '[RACE] group'
 * lines.
 *
 * MC3_ROAM_CHALLENGE=0 turns it off, =2 native without the HUD/audio calls,
 * =3 without native checkpoints.  Log lines '[CHALLENGE]'. */

#define RCH_NEAR       200.0f           /* m: a player this near can be invited (the grid moves everyone to the start) */
#define RCH_AWAY       250.0f           /* m: before the start, farther apart than this = declined (RCH_NEAR + margin: both move in the window) */
#define RCH_JOIN_SLOW  25.0f            /* m/s: an invitee joins slower than this (a cruising player can accept) */
#define RCH_DY         30.0f            /* m: height difference limit of an invite */
#define RCH_SLOW       12.0f            /* m/s: slower to invite (the host) or join (an invitee) */
#define RCH_ASK_S      15.0             /* s an invite waits (was 10, 2026-11-12: owner) */
#define RCH_QUIT_S     1.0              /* s Black is held to cancel, leave or give up a race */
#define RCH_COUNT_S    3.0              /* s of countdown */
#define RCH_CP_R       25.0f            /* m: a checkpoint is reached (horizontal) */
#define RCH_CP_DY      15.0f            /* m: and this near in height */
#define RCH_GONE_S     5.0              /* s a racer may be silent or away before it is out */
#define RCH_MAX_S      600.0            /* s: the race's time limit (a whole course: 14-29 checkpoints seen) */
#define RCH_LATE       1.0              /* s past the latest finish with one racer left: decided */
#define RCH_OFF_M      1000.0f          /* m farther from our next checkpoint than our closest since the last: we left the race */
#define RCH_STALL_S    120.0            /* s without a checkpoint: we left the race */
#define RCH_RESULT_S   8.0              /* s the result stays */
#define RCH_NOTE_S     3.5              /* s a notice stays */
#define RCH_RESEND     0.3              /* s between repeats */
#define RCH_RACERS     8                /* racers at most: the host + 7 (the game's 8 player slots, 7 remote cars) */
#define RCH_ROW_M      8.0f             /* m between grid rows a .rac gives no opponent slot for */
#define RCH_MSG_CP     6                /* start beacons only: checkpoints an INVITE carries (24 + 12 n <= ROAM_MSG_MAX) */
#define RCH_COURSE_M   2000.0f          /* start beacons only: m of course (straight lines) */
#define RCH_EVENT_M    1500.0f          /* m: an event's start this near gives its course */
#define RCH_RAC_MAX    96               /* checkpoints read from a .rac at most */
#define RCH_TOKENS     8000             /* tokens skipped at most looking for numcheckpoints */
#define RCH_GRID_W     2.5f             /* m: start beacons only: each grid slot off the start line's middle */
#define RCH_OPP_W      3.5f             /* m: an odd slot beside PlayerPosDir when the .rac has no opponent for it */
#define RCH_OPP_M      40.0f            /* m: an Opponent PosDir farther from PlayerPosDir is not on the grid */
#define RCH_DIMS       4                /* sizes a checkpoint keeps (see above) */

#define RCH_RACEP_VA   0x00533E24u      /* [+4] = the race object */
#define RCH_CRUISE_VT  0x0046B608u      /* the cruise race's vtable */
#define RCH_ARROW_STUB 0x00129FA0u      /* its vt+0xE8: no arrow target */
#define RCH_ARROW_FN   0x001DF290u      /* the ordered races' vt+0xE8: the target checkpoint */
#define RCH_AUDIO_VA   0x004CBAD8u      /* the audio event manager (sub_000233C0) */
#define RCH_MAPTUNE_VA 0x00507508u      /* the map's tune: checkpoint colours */
#define RCH_CP_SIZE    0x120u
#define RCH_BIG        "\x01"           /* a prompt starting with this: big and centred (web/pre.js) */

#define RCH_FS_VA      0x005348B0u      /* the file system: vt+0x20 open(dir, name, ext, 0, 1) */
#define RCH_CITY_VA    0x00557EE4u      /* city table, +0 the city's name (char *) */
#define RCH_TOK_VT     0x0046D868u      /* the text tokenizer's vtable */
#define RCH_S_RAC      0x00469B68u      /* "rac" */
#define RCH_S_NUMCP    0x0046A278u      /* "numcheckpoints" */
#define RCH_S_CP       0x00469BDCu      /* "checkpoint" */
#define RCH_S_ARROW    0x00469BD4u      /* "Arrow" */
#define RCH_S_TYPE     0x0044B868u      /* "Type" */
#define RCH_S_RESP     0x00469B80u      /* "RespawnAllowed" */

void sub_0020EE80(void);                        /* tokenizer ctor, thiscall (name, stream), ret 8 */
void sub_0020F3C0(void);                        /* tokenizer: next token is this? (consumed when 1), thiscall (str, 1), ret 8; al */
void sub_0020F2E0(void);                        /* tokenizer: take this keyword, thiscall (str), ret 4 */
void sub_001A2630(void);                        /* stream close, thiscall */
void sub_001B3C60(void);                        /* new[] / malloc, cdecl (size) */
void sub_001B3CD0(void);                        /* delete[] / free, cdecl (ptr) */
void sub_001CA0A0(void);                        /* checkpoint ctor, thiscall */
void sub_001C8B20(void);                        /* checkpoint matrix update, thiscall (cp + 4) */
void sub_001C7690(void);                        /* checkpoint visual reset, thiscall (cp + 4) */
void sub_001D1510(void);                        /* race: passes checkpoint? thiscall (index, pos *, 1, float *d2), ret 0x10; al */
void sub_000233C0(void);                        /* audio event, thiscall (id, player, 0, 0), ret 0x10; float */
void sub_000B6330(void);                        /* HUD message queue: show, thiscall (id, 0, 0) */
void sub_001CCA20(void);                        /* race vt+0x11C: each checkpoint's flare tick (sub_001C8D50), thiscall */

static const float rch_dims_def[RCH_DIMS] = { 12.0f, 12.0f, 8.0f, 0.0f };

enum { RCH_OP_INVITE = 1, RCH_OP_JOIN = 2, RCH_OP_NO = 3, RCH_OP_GO = 4, RCH_OP_PROG = 5 };
enum { RCH_NO_DECLINE = 1, RCH_NO_BUSY = 2, RCH_NO_CANCEL = 3, RCH_NO_LATE = 4 };
enum { RCH_IDLE, RCH_INVITING, RCH_INVITED, RCH_JOINED, RCH_COUNT, RCH_RACE, RCH_RESULT };

typedef struct {                                /* a racer, by grid slot */
    uint32_t id;                                /* its roam id; 0 = us */
    char name[ROAM_NAME_MAX + 1];
    int cps;                                    /* checkpoints passed */
    double fin;                                 /* finish time, s from its GO (0 not yet) */
    double heard, away;                         /* its last message; since when it is away (0 not) */
    int out;                                    /* gone, silent or said NO: does not finish */
} rch_racer;

static struct {
    int st;
    uint32_t race, peer;                        /* race id; invited/joined: the host's roam id */
    int host;                                   /* 1: we invited (we send GO) */
    char name[ROAM_NAME_MAX + 1];               /* invited/joined: the host's name */
    int players;                                /* invited/joined: players the host invited, the host included */
    int accepted;                               /* invited/joined: invitees who joined so far (the host's INVITE tail) */
    struct { uint32_t id; char name[ROAM_NAME_MAX + 1]; int joined, out; } inv[RCH_RACERS - 1];  /* inviting: nearest first */
    int ninv;
    rch_racer rc[RCH_RACERS];                   /* count/race/result: the racers in slot order */
    int nrc, me;                                /* their count, our slot */
    float cp[RCH_RAC_MAX][3];
    float dims[RCH_RAC_MAX][RCH_DIMS];
    float grid[3];                              /* the start line (PlayerPosDir, or where the host stood) */
    float slot[RCH_RACERS][3], sdir[RCH_RACERS][2];  /* grid slots (0 the host): position, forward x/z */
    unsigned ev_city, ev_index;                 /* the event the course is from (ev_index 0xFFFF none) */
    int ncp;
    float km;
    int said;                                   /* countdown: the last number shown (4 none) */
    int native;                                 /* the course is the game's checkpoints now */
    double t0;                                  /* invite: when sent/heard; COUNT/RACE: the start (GO) */
    double tx, heard, away;                     /* last send; invited/joined: the host's last message, away since */
    float hold[12];                             /* our car's matrix during the countdown */
    int held;
    int next;                                   /* our next checkpoint */
    double cp_t;                                /* racing: GO or our last checkpoint */
    float cp_best;                              /* racing: our closest to the next checkpoint since cp_t */
    double until;                             /* RESULT ends */
    int white_was;
    char note[RRA_TEXT];
    double note_until;
    struct { uint32_t to[RCH_RACERS], race; int n, left; uint8_t why; double tx; } no;   /* NOs still to repeat */
    int ai;                                     /* a group race: the event's own race with its AI (air), loaded at GO */
    const roam_race *air;
    double black_t;                             /* Black held since (0 not held, or held before a quit was offered) */
    int black_was;
} s_rch;
static float s_rch_pos[3];                      /* our car's position at the last frame (the Race panel's live places) */

/* What quitting does now (Black held RCH_QUIT_S s, or the page's button): RCH_Q_*. */
enum { RCH_Q_NONE, RCH_Q_CANCEL, RCH_Q_LEAVE, RCH_Q_GIVEUP, RCH_Q_DECLINE, RCH_Q_ACCEPT };   /* ACCEPT: the page's Accept beside Decline = White */

#ifdef __EMSCRIPTEN__
/* The prompt box's button (web/pre.js): act = RCH_Q_* offered now (written with each prompt, 0 none); click =
 * the act the player clicked (Module.mc3RaceButton), taken here only while that act is still the one offered. */
static struct { volatile int32_t act, click; } s_rch_btn;
EMSCRIPTEN_KEEPALIVE uintptr_t roam_race_button_box(void) { return (uintptr_t)&s_rch_btn; }
#endif

static int rch_mode(void)                       /* 0 off, 1 native, 2 native without HUD/audio calls, 3 no native */
{
    static int m = -1;
    if (m < 0) {
        const char *v = getenv("MC3_ROAM_CHALLENGE");
        m = v && v[0] ? atoi(v) : 1;
        if (m < 0 || m > 3)
            m = 1;
    }
    return m;
}

static int rch_on(void) { return rch_mode() != 0; }
static void rch_nat_off(void);

static float rch_tokf(uint32_t buf)             /* a token the tokenizer copied to buf, as a float */
{
    char s[32];
    int i;
    for (i = 0; i < 31 && (s[i] = (char)MEM8(buf + (uint32_t)i)) != 0; i++)
        ;
    s[i] = 0;
    return strtof(s, NULL);
}

static float rch_hd(const float *a, const float *b)  /* horizontal distance */
{
    float dx = a[0] - b[0], dz = a[2] - b[2];
    return sqrtf(dx * dx + dz * dz);
}

static void rch_fmt_time(char *out, size_t n, double s)
{
    int ms = (int)(s * 1000.0 + 0.5);
    snprintf(out, n, "%d:%02d.%d", ms / 60000, ms / 1000 % 60, ms / 100 % 10);
}

/* ---- the course ---- */

/* A race's .rac as read: checkpoints (x y z + the RCH_DIMS sizes), PlayerPosDir and the Opponents' PosDirs within
 * RCH_OPP_M m of it, in file order (x y z dx dy dz; has_ppd 0 none). */
typedef struct {
    uint16_t index; uint8_t city; int n;
    float cp[RCH_RAC_MAX][3 + RCH_DIMS];
    int has_ppd, nopd;
    float ppd[6], opd[RCH_RACERS - 1][6];
} rch_rac;

/* Reads "x y z dx dy dz" (two vectors, tokenizer vt+0x24) into pd; 1 when sane (dir not vertical). */
static int rch_posdir(uint32_t tok, uint32_t vt, uint32_t base, uint32_t vec, float pd[6])
{
    uint32_t a[1], h, i;
    for (h = 0; h < 2; h++) {
        MEMF(vec) = MEMF(vec + 4u) = MEMF(vec + 8u) = 0.0f;
        a[0] = vec;
        roam_call(recomp_lookup(MEM32(vt + 0x24u)), tok, base, 1, a);
        for (i = 0; i < 3; i++)
            pd[3 * h + i] = MEMF(vec + 4u * i);
    }
    for (i = 0; i < 6; i++)
        if (!isfinite(pd[i]) || fabsf(pd[i]) > 20000.0f)
            return 0;
    return pd[3] * pd[3] + pd[5] * pd[5] > 0.01f;
}

/* Race r's .rac (the race loader's way) into out: checkpoints, PlayerPosDir, the Opponents' PosDirs. */
static void rch_load_rac(const roam_race *r, rch_rac *out, uint32_t esp)
{
    uint32_t base = (esp - 0x1000u) & ~0xFu, dir = base, tok = base + 0x40u, buf = base + 0x100u, vec = base + 0x180u,
             ename = base + 0x200u, s_ppd = base + 0x300u, s_pd = base + 0x320u;
    uint32_t fs = MEM32(RCH_FS_VA), tbl = MEM32(RCH_CITY_VA), cname, stream, vt, a[5], i;
    int n = 0, want, k, t, max = RCH_RAC_MAX;
    float (*cp)[3 + RCH_DIMS] = out->cp;
    char dname[64];
    const char *pre = "tune/race/", *kw_ppd = "PlayerPosDir", *kw_pd = "PosDir";
    out->n = out->has_ppd = out->nopd = 0;
    for (i = 0; kw_ppd[i]; i++)
        MEM8(s_ppd + i) = (uint8_t)kw_ppd[i];
    MEM8(s_ppd + i) = 0;
    for (i = 0; kw_pd[i]; i++)
        MEM8(s_pd + i) = (uint8_t)kw_pd[i];
    MEM8(s_pd + i) = 0;
    if (!online_ram(fs) || !online_ram(MEM32(fs)) || !online_ram(tbl) || !online_ram(cname = MEM32(tbl + r->city * 0x50u)))
        return;
    for (i = 0; pre[i]; i++)
        MEM8(dir + i) = (uint8_t)pre[i];
    for (k = 0; MEM8(cname + k) && i < 0x3Fu; k++, i++)
        MEM8(dir + i) = MEM8(cname + k);
    MEM8(dir + i) = 0;
    for (i = 0; i < sizeof dname - 1 && MEM8(dir + i); i++)
        dname[i] = (char)MEM8(dir + i);
    dname[i] = 0;
    for (i = 0; r->name[i] && i < 0x7Fu; i++)
        MEM8(ename + i) = (uint8_t)r->name[i];
    MEM8(ename + i) = 0;
    a[0] = dir; a[1] = ename; a[2] = RCH_S_RAC; a[3] = 0; a[4] = 1;
    stream = roam_call(recomp_lookup(MEM32(MEM32(fs) + 0x20u)), fs, base, 5, a);
    if (!online_ram(stream)) {
        fprintf(stderr, "[CHALLENGE] course: %s/%s.rac did not open\n", dname, r->name);
        return;
    }
    for (i = 0; i < 0xA0u; i += 4)
        MEM32(tok + i) = 0;
    MEM32(tok) = RCH_TOK_VT;
    a[0] = RCH_S_RAC; a[1] = stream;            /* the name is only for its messages */
    roam_call(sub_0020EE80, tok, base, 2, a);
    vt = MEM32(tok);
    for (t = 0; t < RCH_TOKENS; t++) {
        a[0] = RCH_S_NUMCP; a[1] = 1;
        if (roam_call(sub_0020F3C0, tok, base, 2, a) & 0xFFu)
            break;
        a[0] = s_ppd; a[1] = 1;                 /* the player's grid slot, just before numcheckpoints */
        if (roam_call(sub_0020F3C0, tok, base, 2, a) & 0xFFu) {
            out->has_ppd = rch_posdir(tok, vt, base, vec, out->ppd);
            continue;
        }
        a[0] = buf; a[1] = 0x80u;
        if (!roam_call(recomp_lookup(MEM32(vt + 4u)), tok, base, 2, a))
            break;                              /* the end of the file */
    }
    if (t < RCH_TOKENS) {
        want = (int)roam_call(recomp_lookup(MEM32(vt + 0x10u)), tok, base, 0, NULL);
        for (k = 0; k < want && k < RCH_RAC_MAX; k++) {
            a[0] = RCH_S_CP;
            roam_call(sub_0020F2E0, tok, base, 1, a);
            MEMF(vec) = MEMF(vec + 4u) = MEMF(vec + 8u) = 0.0f;
            a[0] = vec;
            roam_call(recomp_lookup(MEM32(vt + 0x24u)), tok, base, 1, a);
            float d[RCH_DIMS];
            for (i = 0; i < RCH_DIMS; i++) {    /* the four sizes (sub_001CDC80 reads them with vt+0x14) */
                MEM8(buf) = 0;
                a[0] = buf; a[1] = 0x80u;
                roam_call(recomp_lookup(MEM32(vt + 4u)), tok, base, 2, a);
                d[i] = rch_tokf(buf);
                if (!isfinite(d[i]) || fabsf(d[i]) > 1000.0f)
                    d[i] = rch_dims_def[i];
            }
            if (n < max && isfinite(MEMF(vec)) && isfinite(MEMF(vec + 8u)) && fabsf(MEMF(vec)) < 20000.0f &&
                fabsf(MEMF(vec + 4u)) < 20000.0f && fabsf(MEMF(vec + 8u)) < 20000.0f) {
                cp[n][0] = MEMF(vec);
                cp[n][1] = MEMF(vec + 4u);
                cp[n][2] = MEMF(vec + 8u);
                memcpy(&cp[n][3], d, sizeof d);
                if (n == 0 || r->kind == RR_CIRCUIT)
                    fprintf(stderr, "[CHALLENGE] course: %s checkpoint %d sizes %.1f %.1f %.1f %.1f\n", r->name, n,
                            d[0], d[1], d[2], d[3]);
                n++;
            }
            a[0] = RCH_S_ARROW; a[1] = 1;
            if (roam_call(sub_0020F3C0, tok, base, 2, a) & 0xFFu)
                for (i = 0; i < 2; i++) {       /* float, int */
                    a[0] = buf; a[1] = 0x80u;
                    roam_call(recomp_lookup(MEM32(vt + 4u)), tok, base, 2, a);
                }
            a[0] = RCH_S_TYPE; a[1] = 1;
            if (roam_call(sub_0020F3C0, tok, base, 2, a) & 0xFFu) {
                a[0] = buf; a[1] = 0x80u;
                roam_call(recomp_lookup(MEM32(vt + 4u)), tok, base, 2, a);
            }
            a[0] = RCH_S_RESP; a[1] = 1;
            if (roam_call(sub_0020F3C0, tok, base, 2, a) & 0xFFu) {
                a[0] = buf; a[1] = 0x80u;
                roam_call(recomp_lookup(MEM32(vt + 4u)), tok, base, 2, a);
            }
        }
        for (t = 0, k = 0; t < RCH_TOKENS && out->nopd < RCH_RACERS - 1; t++) {  /* the PosDirs after them: */
            a[0] = s_pd; a[1] = 1;                                          /* the Opponents' grid slots */
            if (roam_call(sub_0020F3C0, tok, base, 2, a) & 0xFFu) {
                float *o = out->opd[out->nopd];
                k++;
                if (rch_posdir(tok, vt, base, vec, o) && out->has_ppd && rch_hd(o, out->ppd) < RCH_OPP_M &&
                    fabsf(o[1] - out->ppd[1]) < 10.0f)
                    out->nopd++;
                continue;
            }
            a[0] = buf; a[1] = 0x80u;
            if (!roam_call(recomp_lookup(MEM32(vt + 4u)), tok, base, 2, a))
                break;
        }
        fprintf(stderr, "[CHALLENGE] course: %s/%s.rac: %d of %d checkpoints read; PlayerPosDir %s, %d of %d opponent "
                "PosDirs on the grid\n", dname, r->name, n, want, out->has_ppd ? "yes" : "no", out->nopd, k);
    } else
        fprintf(stderr, "[CHALLENGE] course: %s/%s.rac has no numcheckpoints\n", dname, r->name);
    roam_call(sub_001A2630, stream, base, 0, NULL);
    out->n = n;
}

/* s_rch.cp from the race's checkpoints from its first, as the game's race runs, the start line its PlayerPosDir;
 * its count. */
static int rch_take(const rch_rac *rac)
{
    int i;
    float sum = 0.0f;
    const float *prev = rac->ppd;
    memcpy(s_rch.grid, rac->ppd, sizeof s_rch.grid);
    s_rch.ncp = 0;
    for (i = 0; i < rac->n; i++) {             /* the whole course, as the game's race */
        sum += rch_hd(rac->cp[i], prev);
        prev = rac->cp[i];
        memcpy(s_rch.cp[s_rch.ncp], rac->cp[i], sizeof s_rch.cp[0]);
        memcpy(s_rch.dims[s_rch.ncp++], &rac->cp[i][3], sizeof s_rch.dims[0]);
    }
    s_rch.km = sum / 1000.0f;
    return s_rch.ncp;
}

/* Every grid slot: the race's own (rac's PlayerPosDir at s_rch.grid: slot 0 there, slot k at the k-th Opponent
 * PosDir; a slot without one in rows of two behind PlayerPosDir: odd slots RCH_OPP_W m to the +(up x dir) side, as
 * the game's opponent 0 stands, every second slot RCH_ROW_M m further back), else the start beacons' (RCH_GRID_W m
 * to each side of s_rch.grid facing the first checkpoint, slot 0 at -, rows RCH_ROW_M m apart). */
static void rch_slots(const rch_rac *rac)
{
    float f[2], l, side, back;
    int k;
    if (rac && rac->has_ppd && rch_hd(rac->ppd, s_rch.grid) < 1.0f && fabsf(rac->ppd[1] - s_rch.grid[1]) < 1.0f) {
        l = sqrtf(rac->ppd[3] * rac->ppd[3] + rac->ppd[5] * rac->ppd[5]);
        f[0] = rac->ppd[3] / l;
        f[1] = rac->ppd[5] / l;
        for (k = 0; k < RCH_RACERS; k++) {
            const float *pd = k >= 1 && k - 1 < rac->nopd ? rac->opd[k - 1] : rac->ppd;
            l = sqrtf(pd[3] * pd[3] + pd[5] * pd[5]);
            s_rch.sdir[k][0] = pd[3] / l;
            s_rch.sdir[k][1] = pd[5] / l;
            memcpy(s_rch.slot[k], pd, sizeof s_rch.slot[0]);
            if (k >= 1 && k - 1 >= rac->nopd) {
                side = k & 1 ? RCH_OPP_W : 0.0f;
                back = (float)(k / 2) * RCH_ROW_M;
                s_rch.slot[k][0] += side * f[1] - back * f[0];  /* up x dir = (dz, 0, -dx) */
                s_rch.slot[k][2] -= side * f[0] + back * f[1];
            }
        }
        for (k = 0; k < RCH_RACERS; k++)        /* guard: a slot facing away from checkpoint 0 is turned around */
            if (s_rch.ncp > 0 && s_rch.sdir[k][0] * (s_rch.cp[0][0] - s_rch.slot[k][0]) +
                                 s_rch.sdir[k][1] * (s_rch.cp[0][2] - s_rch.slot[k][2]) < 0.0f) {
                s_rch.sdir[k][0] = -s_rch.sdir[k][0];
                s_rch.sdir[k][1] = -s_rch.sdir[k][1];
                fprintf(stderr, "[CHALLENGE] grid: slot %d's dir pointed away from checkpoint 0: turned\n", k);
            }
        return;
    }
    f[0] = s_rch.cp[0][0] - s_rch.grid[0];
    f[1] = s_rch.cp[0][2] - s_rch.grid[2];
    if ((l = sqrtf(f[0] * f[0] + f[1] * f[1])) < 1.0f)
        f[0] = 0.0f, f[1] = 1.0f, l = 1.0f;
    f[0] /= l;
    f[1] /= l;
    for (k = 0; k < RCH_RACERS; k++) {
        side = k & 1 ? RCH_GRID_W : -RCH_GRID_W;
        back = (float)(k / 2) * RCH_ROW_M;
        s_rch.sdir[k][0] = f[0];
        s_rch.sdir[k][1] = f[1];
        s_rch.slot[k][0] = s_rch.grid[0] + side * f[1] - back * f[0];
        s_rch.slot[k][1] = s_rch.grid[1];
        s_rch.slot[k][2] = s_rch.grid[2] - side * f[0] - back * f[1];
    }
}

/* Only if no .rac reads: the city's race start beacons chained ahead (fwd) of us, 250-700 m a leg. */
static int rch_beacons(unsigned city, const float me[3], const float fwd[3])
{
    const float *prev = me;
    float dir[2] = { fwd[0], fwd[2] }, l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]), sum = 0.0f;
    int used[RCH_MSG_CP], i, k, best;
    if (l < 1e-3f)
        dir[0] = 1.0f, dir[1] = 0.0f, l = 1.0f;
    dir[0] /= l;
    dir[1] /= l;
    memcpy(s_rch.grid, me, sizeof s_rch.grid); /* the start line where we stand */
    s_rch.ncp = 0;
    while (s_rch.ncp < RCH_MSG_CP && sum < RCH_COURSE_M) {
        float bs = -2.0f, d, s, dx, dz;
        best = -1;
        for (i = 0; i < roam_races_count(); i++) {
            const roam_race *r = roam_races_get(i);
            if (r->city != city || (d = rch_hd(r->pos, prev)) < 250.0f || d > 700.0f)
                continue;
            for (k = 0; k < s_rch.ncp && used[k] != i; k++)
                ;
            if (k < s_rch.ncp)
                continue;
            dx = (r->pos[0] - prev[0]) / d;
            dz = (r->pos[2] - prev[2]) / d;
            if ((s = dx * dir[0] + dz * dir[1]) > bs) {
                bs = s;
                best = i;
            }
        }
        if (best < 0 || bs < 0.0f)
            break;
        used[s_rch.ncp] = best;
        sum += rch_hd(roam_races_get(best)->pos, prev);
        memcpy(s_rch.cp[s_rch.ncp], roam_races_get(best)->pos, sizeof s_rch.cp[0]);
        memcpy(s_rch.dims[s_rch.ncp], rch_dims_def, sizeof s_rch.dims[0]);
        dir[0] = (s_rch.cp[s_rch.ncp][0] - prev[0]) / rch_hd(s_rch.cp[s_rch.ncp], prev);
        dir[1] = (s_rch.cp[s_rch.ncp][2] - prev[2]) / rch_hd(s_rch.cp[s_rch.ncp], prev);
        prev = s_rch.cp[s_rch.ncp++];
    }
    s_rch.km = sum / 1000.0f;
    return s_rch.ncp;
}

/* Race r's .rac, read once (4 races kept). */
static rch_rac *rch_cache(const roam_race *r, uint32_t esp)
{
    static rch_rac cache[4];
    static int ncache;
    int c;
    for (c = 0; c < ncache && !(cache[c].index == r->index && cache[c].city == r->city); c++)
        ;
    if (c == ncache) {
        c = ncache < 4 ? ncache++ : (int)(roam_now() * 7.0) % 4;
        cache[c].index = (uint16_t)r->index;
        cache[c].city = (uint8_t)r->city;
        rch_load_rac(r, &cache[c], esp);
    }
    return &cache[c];
}

/* The challenged side: the event's .rac (the course is read from it, as the challenger did), NULL none. */
static const rch_rac *rch_rac_of(unsigned city, unsigned index, uint32_t esp)
{
    int i;
    for (i = 0; index != 0xFFFFu && i < roam_races_count(); i++)
        if (roam_races_get(i)->city == city && roam_races_get(i)->index == index)
            return rch_cache(roam_races_get(i), esp);
    return NULL;
}

/* The course for a race from where we stand; its checkpoint count (2 at least), 0 none. */
static int rch_course(unsigned city, const float me[3], const float fwd[3], uint32_t esp)
{
    const roam_race *cand[3] = { 0 }, *r;
    const rch_rac *rac;
    float cd[3] = { 0 }, d;
    int nc = 0, i, j, n;
    for (i = 0; i < roam_races_count(); i++) {
        r = roam_races_get(i);
        if (r->city != city || (r->kind != RR_SPRINT && r->kind != RR_CIRCUIT) || (d = rch_hd(r->pos, me)) > RCH_EVENT_M)
            continue;
        for (j = nc < 3 ? nc++ : 3; j > 0 && cd[j - 1] > d; j--)
            if (j < 3) {
                cand[j] = cand[j - 1];
                cd[j] = cd[j - 1];
            }
        if (j < 3) {
            cand[j] = r;
            cd[j] = d;
        }
    }
    for (i = 0; i < nc; i++) {
        r = cand[i];
        rac = rch_cache(r, esp);
        if (!rac->has_ppd) {
            fprintf(stderr, "[CHALLENGE] course: %s: no PlayerPosDir in its .rac\n", r->name);
            continue;
        }
        if (rac->n >= 2 && (n = rch_take(rac)) >= 2) {
            s_rch.ev_city = r->city;
            s_rch.ev_index = r->index;
            rch_slots(rac);
            fprintf(stderr, "[CHALLENGE] course: %s (%s, start %.0f m away): %d checkpoints from the first, %.1f km, "
                    "grid %.0f m from us\n", r->name, roam_races_kind_name(r->kind), cd[i], n, s_rch.km,
                    rch_hd(s_rch.grid, me));
            return n;
        }
    }
    s_rch.ev_city = city;
    s_rch.ev_index = 0xFFFFu;
    if ((n = rch_beacons(city, me, fwd)) >= 2) {
        rch_slots(NULL);
        fprintf(stderr, "[CHALLENGE] course: no race file near (%d tried): %d start beacons ahead, %.1f km\n", nc, n,
                s_rch.km);
        return n;
    }
    fprintf(stderr, "[CHALLENGE] course: none here (%d race files tried, no start beacons ahead)\n", nc);
    return 0;
}

/* ---- messages ---- */

static void rch_put32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }
static uint32_t rch_get32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }

/* One message to `to` (see the top); GO tells the receiver its slot `arg`. */
static void rch_send(uint32_t to, uint32_t race, int op, int arg, double now)
{
    uint8_t b[ROAM_MSG_MAX];
    int n = 8, k, i;
    float f;
    double left;
    memset(b, 0, sizeof b);
    b[0] = 'C'; b[1] = 5; b[2] = (uint8_t)op; b[3] = (uint8_t)arg;
    rch_put32(b + 4, race);
    if (op == RCH_OP_INVITE) {
        b[3] = (uint8_t)s_rch.ncp;
        for (i = 0; i < 3; i++) {               /* the start line */
            memcpy(b + n, &s_rch.grid[i], 4);
            n += 4;
        }
        b[n++] = (uint8_t)s_rch.ev_city;
        b[n++] = (uint8_t)s_rch.ev_index;
        b[n++] = (uint8_t)(s_rch.ev_index >> 8);
        b[n++] = (uint8_t)(s_rch.ninv + 1);     /* players: the invited + us */
        for (k = 0; s_rch.ev_index == 0xFFFFu && k < s_rch.ncp && k < RCH_MSG_CP; k++)  /* beacons: the checkpoints */
            for (i = 0; i < 3; i++) {
                f = s_rch.cp[k][i];
                memcpy(b + n, &f, 4);
                n += 4;
            }
        for (k = 0, i = 0; k < s_rch.ninv; k++)  /* tail byte (2026-11-13): invitees joined so far; older builds ignore it */
            i += s_rch.inv[k].joined && !s_rch.inv[k].out;
        if (n < ROAM_MSG_MAX)                   /* 6 start beacons fill the message: no count then */
            b[n++] = (uint8_t)i;
    } else if (op == RCH_OP_GO) {
        left = (s_rch.t0 - now) * 1000.0;
        k = left < 0.0 ? 0 : left > 60000.0 ? 60000 : (int)left;
        b[8] = (uint8_t)k; b[9] = (uint8_t)(k >> 8);
        b[10] = (uint8_t)s_rch.nrc;
        for (i = 0; i < s_rch.nrc; i++)         /* slot 0 (us, the host) goes as 0: the sender */
            rch_put32(b + 12 + 4 * i, s_rch.rc[i].id);
        n = 12 + 4 * s_rch.nrc;
    } else if (op == RCH_OP_PROG) {
        b[3] = (uint8_t)s_rch.next;
        b[8] = s_rch.rc[s_rch.me].fin > 0.0;
        rch_put32(b + 12, (uint32_t)(s_rch.rc[s_rch.me].fin * 1000.0 + 0.5));
        n = 16;
    }
    roam_send_msg(to, b, n);
}

static void rch_send_racers(int op, double now)  /* to every racer still in but us (GO: each its slot) */
{
    int k;
    for (k = 0; k < s_rch.nrc; k++)
        if (k != s_rch.me && !s_rch.rc[k].out)
            rch_send(s_rch.rc[k].id, s_rch.race, op, k, now);
}

static void rch_note(const char *text, double now)
{
    snprintf(s_rch.note, sizeof s_rch.note, "%s", text);
    s_rch.note_until = now + RCH_NOTE_S;
}

/* NO `why` to each of ids (sent three times, rch_player). */
static void rch_no_list(const uint32_t *ids, int n, int why)
{
    int k;
    s_rch.no.n = 0;
    for (k = 0; k < n && k < RCH_RACERS; k++)
        if (ids[k])
            s_rch.no.to[s_rch.no.n++] = ids[k];
    s_rch.no.race = s_rch.race;
    s_rch.no.why = (uint8_t)why;
    s_rch.no.left = s_rch.no.n ? 3 : 0;
    s_rch.no.tx = 0;
}

/* Out of the race (state back to idle), the others told `why` (0: nothing to tell). */
static void rch_end(int why, const char *text, double now)
{
    uint32_t ids[RCH_RACERS];
    int n = 0, k;
    if (why) {
        if (s_rch.st == RCH_INVITING) {
            for (k = 0; k < s_rch.ninv; k++)
                if (!s_rch.inv[k].out)
                    ids[n++] = s_rch.inv[k].id;
        } else if (s_rch.st == RCH_INVITED || s_rch.st == RCH_JOINED) {
            ids[n++] = s_rch.peer;
        } else if (s_rch.st == RCH_COUNT || s_rch.st == RCH_RACE) {
            for (k = 0; k < s_rch.nrc; k++)
                if (k != s_rch.me && !s_rch.rc[k].out)
                    ids[n++] = s_rch.rc[k].id;
        }
        rch_no_list(ids, n, why);
    }
    if (text) {
        rch_note(text, now);
        fprintf(stderr, "[CHALLENGE] %s\n", text);
    }
    s_rch.st = RCH_IDLE;
    s_rch.peer = 0;
    s_rch.held = 0;
    rch_nat_off();
}

static const roam_peer *rch_peer(uint32_t id)
{
    int i;
    if (!s_roam_seen || roam_now() - s_roam_seen_t > 0.5)
        return NULL;
    for (i = 0; i < s_roam_nseen; i++)
        if (s_roam_seen[i].id == id)
            return &s_roam_seen[i];
    return NULL;
}

static float rch_speed(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

static const char *rch_ord(int place)
{
    static const char *o[RCH_RACERS + 1] = { "-", "1st", "2nd", "3rd", "4th", "5th", "6th", "7th", "8th" };
    return place >= 1 && place <= RCH_RACERS ? o[place] : "-";
}

/* Racer k's distance to its next checkpoint (1e9 unknown). */
static float rch_left(int k, const float me[3])
{
    const rch_racer *r = &s_rch.rc[k];
    const roam_peer *pp;
    if (r->cps >= s_rch.ncp)
        return 0.0f;
    if (k == s_rch.me)
        return rch_hd(s_rch.cp[r->cps], me);
    return (pp = rch_peer(r->id)) ? rch_hd(s_rch.cp[r->cps], pp->p) : 1e9f;
}

/* Racer a ahead of racer b: finished first (by time), then still in, then checkpoints, then distance to the next. */
static int rch_ahead(int a, int b, const float me[3])
{
    const rch_racer *x = &s_rch.rc[a], *y = &s_rch.rc[b];
    if ((x->fin > 0.0) != (y->fin > 0.0))
        return x->fin > 0.0;
    if (x->fin > 0.0)
        return x->fin < y->fin;
    if (x->out != y->out)
        return !x->out;
    if (x->cps != y->cps)
        return x->cps > y->cps;
    return rch_left(a, me) < rch_left(b, me);
}

/* The racers in place order into order[]; our place (1-based). */
static int rch_rank(int order[RCH_RACERS], const float me[3])
{
    int i, j, t, place = 0;
    for (i = 0; i < s_rch.nrc; i++) {
        for (j = i, t = i; j > 0 && rch_ahead(t, order[j - 1], me); j--)
            order[j] = order[j - 1];
        order[j] = t;
    }
    for (i = 0; i < s_rch.nrc; i++)
        if (order[i] == s_rch.me)
            place = i + 1;
    return place;
}

/* The standings, "1 Name 1:23.4, 2 You DNF ..." (names cut to 8 characters). */
static void rch_standings(char *out, size_t cap, const int order[RCH_RACERS])
{
    char t[16];
    size_t o = 0;
    int i, w;
    out[0] = 0;
    for (i = 0; i < s_rch.nrc && o < cap; i++) {
        const rch_racer *r = &s_rch.rc[order[i]];
        rch_fmt_time(t, sizeof t, r->fin);
        w = snprintf(out + o, cap - o, "%s%d %.8s %s", i ? " \xC2\xB7 " : "", i + 1,
                     order[i] == s_rch.me ? "You" : r->name, r->fin > 0.0 ? t : "DNF");
        if (w < 0)
            break;
        o += (size_t)w;
    }
}

static void rch_result(const float me[3], double now)
{
    int order[RCH_RACERS], place;
    char t[160];
    rch_nat_off();
    s_rch.st = RCH_RESULT;
    s_rch.until = now + RCH_RESULT_S;
    place = rch_rank(order, me);
    rch_standings(t, sizeof t, order);
    fprintf(stderr, "[CHALLENGE] result: we are %s of %d: %s\n", rch_ord(place), s_rch.nrc, t);
    op_street(place, s_rch.nrc, s_rch.rc[s_rch.me].fin > 0.0);   /* Online prize and wins (online_progress.c) */
}

/* The countdown to t0 with nrc racers (ids in s_rch.rc[].id already, ours 0 at slot me). */
static void rch_go(int nrc, int me, double t0, double now)
{
    const roam_peer *pp;
    int k;
    s_rch.st = RCH_COUNT;
    s_rch.nrc = nrc;
    s_rch.me = me;
    s_rch.t0 = t0;
    s_rch.held = 0;
    s_rch.tx = 0;
    for (k = 0; k < nrc; k++) {
        rch_racer *r = &s_rch.rc[k];
        r->cps = 0;
        r->fin = 0.0;
        r->heard = now;
        r->away = 0.0;
        r->out = 0;
        if (k == me)
            snprintf(r->name, sizeof r->name, "You");
        else if ((pp = rch_peer(r->id)) != NULL)
            snprintf(r->name, sizeof r->name, "%s", pp->name);
        else if (!r->name[0])
            snprintf(r->name, sizeof r->name, "Player");
    }
}

/* The host: the window is over, the joined race (nearest first, slots 1..); the others hear NO late. */
static void rch_host_go(double now)
{
    uint32_t late[RCH_RACERS];
    int n = 1, nl = 0, k;
    s_rch.rc[0].id = 0;
    for (k = 0; k < s_rch.ninv; k++) {
        if (s_rch.inv[k].out)
            continue;
        if (s_rch.inv[k].joined && n < RCH_RACERS) {
            s_rch.rc[n].id = s_rch.inv[k].id;
            snprintf(s_rch.rc[n].name, sizeof s_rch.rc[n].name, "%s", s_rch.inv[k].name);
            n++;
        } else
            late[nl++] = s_rch.inv[k].id;
    }
    rch_no_list(late, nl, RCH_NO_LATE);
    rch_go(n, 0, now + RCH_COUNT_S, now);
    fprintf(stderr, "[CHALLENGE] %d joined of %d invited: countdown (%d racers)\n", n - 1, s_rch.ninv, n);
}

static int rch_racer_of(uint32_t id)            /* the slot of the racer with roam id `id` (not us), -1 none */
{
    int k;
    for (k = 0; k < s_rch.nrc; k++)
        if (k != s_rch.me && s_rch.rc[k].id == id)
            return k;
    return -1;
}

static void rch_recv(const float me[3], double now, uint32_t esp)
{
    uint8_t b[ROAM_MSG_MAX];
    uint32_t from, race;
    const roam_peer *pp;
    int n, op, k, i, ok;
    while ((n = roam_recv_msg(&from, b, sizeof b)) > 0) {
        if (n < 8 || b[0] != 'C' || b[1] != 5)  /* v5: invites of up to RCH_RACERS; v4 builds' messages are dropped */
            continue;
        op = b[2];
        race = rch_get32(b + 4);
        if (op == RCH_OP_INVITE) {
            if ((s_rch.st == RCH_INVITED || s_rch.st == RCH_JOINED) && s_rch.peer == from && s_rch.race == race) {
                s_rch.heard = now;
                k = b[3] == 0 || s_rch.ev_index != 0xFFFFu ? 24 : 24 + 12 * b[3];   /* past the checkpoints: joined */
                if (n > k)
                    s_rch.accepted = b[k];
                continue;
            }
            for (k = 0; s_rch.st == RCH_INVITING && k < s_rch.ninv && s_rch.inv[k].id != from; k++)
                ;
            /* two hosts inviting each other: the lower race id goes on (ours cancelled, our invitees told) */
            if (s_rch.st == RCH_INVITING && k < s_rch.ninv && race < s_rch.race)
                rch_end(RCH_NO_CANCEL, NULL, now);
            if (s_rch.st != RCH_IDLE) {
                if (!(s_rch.st == RCH_INVITING && k < s_rch.ninv))
                    rch_send(from, race, RCH_OP_NO, RCH_NO_BUSY, now);
                continue;
            }
            /* a group race: 0 checkpoints and an event (a v5 build before it answers NO busy: b[3] < 2) */
            if (b[3] == 0 && n >= 24 && (b[21] | b[22] << 8) != 0xFFFF) {
                const roam_race *gr = NULL;
                for (i = 0; i < roam_races_count() && !gr; i++)
                    if (roam_races_get(i)->city == b[20] && roam_races_get(i)->index == (unsigned)(b[21] | b[22] << 8))
                        gr = roam_races_get(i);
                if (!gr || !gr->opponents || !(pp = rch_peer(from)) || rch_hd(pp->p, me) > RCH_AWAY) {
                    fprintf(stderr, "[RACE] group: invite from %08X to event %u/%u declined (%s)\n", (unsigned)from,
                            b[20], (unsigned)(b[21] | b[22] << 8), !gr ? "not one of our start points" :
                            !gr->opponents ? "no opponents" : "too far");
                    rch_send(from, race, RCH_OP_NO, RCH_NO_BUSY, now);
                    continue;
                }
                s_rch.st = RCH_INVITED;
                s_rch.ai = 1;
                s_rch.air = gr;
                s_rch.ncp = 0;
                s_rch.km = 0.0f;
                s_rch.ev_city = gr->city;
                s_rch.ev_index = gr->index;
                s_rch.race = race;
                s_rch.peer = from;
                s_rch.host = 0;
                s_rch.players = b[23] < 2 ? 2 : b[23] > RCH_RACERS ? RCH_RACERS : b[23];
                s_rch.accepted = 0;
                snprintf(s_rch.name, sizeof s_rch.name, "%s", pp->name);
                s_rch.t0 = s_rch.heard = now;
                s_rch.tx = 0;
                s_rch.away = 0;
                fprintf(stderr, "[RACE] group: %s invites us to %s (race %08X, %d players)\n", s_rch.name, gr->name,
                        (unsigned)race, s_rch.players);
                continue;
            }
            s_rch.ai = 0;
            if (b[3] < 2 || n < 24 || !(pp = rch_peer(from)) || rch_hd(pp->p, me) > RCH_AWAY) {
                rch_send(from, race, RCH_OP_NO, RCH_NO_BUSY, now);
                continue;
            }
            /* start line (8), city (20), event (21), players (23); start beacons only (event 0xFFFF): the
             * checkpoints (24) */
            float g[3];
            const rch_rac *rac;
            ok = 1;
            for (i = 0; i < 3; i++) {
                memcpy(&g[i], b + 8 + 4 * i, 4);
                ok &= isfinite(g[i]) && fabsf(g[i]) < 20000.0f;
            }
            s_rch.ev_city = b[20];
            s_rch.ev_index = (unsigned)b[21] | (unsigned)b[22] << 8;
            if (ok && s_rch.ev_index != 0xFFFFu) {
                /* our own read of the event's .rac, as the host took it (both sides read the same disc) */
                rac = rch_rac_of(s_rch.ev_city, s_rch.ev_index, esp);
                ok = rac && rac->has_ppd && rch_take(rac) == b[3] && rch_hd(g, s_rch.grid) < 1.0f &&
                     fabsf(g[1] - s_rch.grid[1]) < 1.0f;
                if (!ok) {
                    fprintf(stderr, "[CHALLENGE] INVITE from %08X: event %u/%u: our .rac gives another course (%d of "
                            "%d checkpoints), declined\n", (unsigned)from, s_rch.ev_city, s_rch.ev_index,
                            rac ? rac->n : -1, b[3]);
                    rch_send(from, race, RCH_OP_NO, RCH_NO_BUSY, now);
                    continue;
                }
                rch_slots(rac);
            } else if (ok) {
                if (b[3] > RCH_MSG_CP || n < 24 + 12 * b[3]) {
                    rch_send(from, race, RCH_OP_NO, RCH_NO_BUSY, now);
                    continue;
                }
                for (k = 0; k < b[3]; k++) {
                    for (i = 0; i < 3; i++) {
                        memcpy(&s_rch.cp[k][i], b + 24 + 12 * k + 4 * i, 4);
                        ok &= isfinite(s_rch.cp[k][i]) && fabsf(s_rch.cp[k][i]) < 20000.0f;
                    }
                    memcpy(s_rch.dims[k], rch_dims_def, sizeof s_rch.dims[0]);
                }
                memcpy(s_rch.grid, g, sizeof g);
                s_rch.ncp = b[3];
                s_rch.km = rch_hd(s_rch.cp[0], s_rch.grid);
                for (k = 1; k < s_rch.ncp; k++)
                    s_rch.km += rch_hd(s_rch.cp[k], s_rch.cp[k - 1]);
                s_rch.km /= 1000.0f;
                rch_slots(NULL);
            }
            if (!ok)
                continue;
            s_rch.st = RCH_INVITED;
            s_rch.race = race;
            s_rch.peer = from;
            s_rch.host = 0;
            s_rch.players = b[23] < 2 ? 2 : b[23] > RCH_RACERS ? RCH_RACERS : b[23];
            s_rch.accepted = 0;
            snprintf(s_rch.name, sizeof s_rch.name, "%s", pp->name);
            s_rch.t0 = s_rch.heard = now;
            s_rch.tx = 0;
            s_rch.away = 0;
            fprintf(stderr, "[CHALLENGE] %s invites us (race %08X, %d players, %d checkpoints, %.1f km)\n", s_rch.name,
                    (unsigned)race, s_rch.players, s_rch.ncp, s_rch.km);
            continue;
        }
        if (s_rch.st == RCH_IDLE || race != s_rch.race)
            continue;
        if (s_rch.st == RCH_INVITING) {         /* an invitee's answer */
            for (k = 0; k < s_rch.ninv && s_rch.inv[k].id != from; k++)
                ;
            if (k == s_rch.ninv || s_rch.inv[k].out)
                continue;
            if (op == RCH_OP_JOIN && !s_rch.inv[k].joined) {
                s_rch.inv[k].joined = 1;
                fprintf(stderr, "[CHALLENGE] %s joins\n", s_rch.inv[k].name);
            } else if (op == RCH_OP_NO) {
                s_rch.inv[k].out = 1;
                fprintf(stderr, "[CHALLENGE] %s can't race (NO %d)\n", s_rch.inv[k].name, b[3]);
            }
            continue;
        }
        if (s_rch.st == RCH_INVITED || s_rch.st == RCH_JOINED) {    /* the host's */
            if (from != s_rch.peer)
                continue;
            s_rch.heard = now;
            if (op == RCH_OP_GO && s_rch.st == RCH_JOINED && n >= 12 && b[10] >= 2 && b[10] <= RCH_RACERS &&
                b[3] >= 1 && b[3] < b[10] && n >= 12 + 4 * b[10]) {
                for (k = 0; k < b[10]; k++) {
                    s_rch.rc[k].id = k == 0 ? from : k == b[3] ? 0 : rch_get32(b + 12 + 4 * k);
                    s_rch.rc[k].name[0] = 0;
                }
                snprintf(s_rch.rc[0].name, sizeof s_rch.rc[0].name, "%s", s_rch.name);
                rch_go(b[10], b[3], now + (b[8] | b[9] << 8) / 1000.0, now);
                fprintf(stderr, "[CHALLENGE] GO from %s: %d racers, our slot %d, start in %d ms\n", s_rch.name,
                        s_rch.nrc, s_rch.me, b[8] | b[9] << 8);
            } else if (op == RCH_OP_NO) {
                char t[RRA_TEXT];
                snprintf(t, sizeof t, b[3] == RCH_NO_BUSY ? "%s can't race now\n\nFree Roam goes on"
                         : b[3] == RCH_NO_LATE ? "%s's race started without you\n\nFree Roam goes on"
                                               : "%s cancelled the race\n\nFree Roam goes on", s_rch.name);
                rch_end(0, t, now);
            }
            continue;
        }
        /* counting down, racing, the result: a racer's */
        if ((k = rch_racer_of(from)) < 0) {
            if (op == RCH_OP_JOIN && s_rch.host && s_rch.st != RCH_RESULT)
                rch_send(from, race, RCH_OP_NO, RCH_NO_LATE, now);  /* an invitee's JOIN after the window */
            continue;
        }
        rch_racer *r = &s_rch.rc[k];
        r->heard = now;
        if (op == RCH_OP_PROG && n >= 16) {
            r->cps = b[3] > s_rch.ncp ? s_rch.ncp : b[3];
            if (b[8] && r->fin == 0.0) {
                r->fin = rch_get32(b + 12) / 1000.0;
                if (r->fin <= 0.0)
                    r->fin = 0.001;
                fprintf(stderr, "[CHALLENGE] %s finished in %.2f s\n", r->name, r->fin);
            }
        } else if (op == RCH_OP_NO && !r->out && r->fin == 0.0 && s_rch.st != RCH_RESULT) {
            r->out = 1;
            fprintf(stderr, "[CHALLENGE] %s left the race (NO %d)\n", r->name, b[3]);
        }
    }
}

/* ---- native checkpoints (the game's flare, arrow, pass test; see the top) ---- */

static struct {
    uint32_t race, arr, orig, cnt;              /* the race object we gave checkpoints, our array (count cnt at -4),
                                                   the array it had (the cruise .rac's numcheckpoints 0: new[] of 0) */
    int on, vt;                                 /* race+0x24 is ours; the arrow slot is pointed */
    uint32_t r18, r4c[4];                       /* racer record 0's fields as they were */
    uint16_t r34;
    uint8_t r04, r48;
} s_nat;

static uint32_t rch_race(void)                  /* the cruise's race object, 0 none */
{
    uint32_t g = MEM32(RCH_RACEP_VA), r;
    return online_ram(g) && online_ram(r = MEM32(g + 4u)) && MEM32(r) == RCH_CRUISE_VT ? r : 0;
}

/* A lifted call whose float return (x87) nobody pops: the stack top put back. */
static uint32_t rch_callf(recomp_func_t fn, uint32_t ecx, uint32_t top, int n, const uint32_t *args)
{
    int t = g_fp_top;
    uint32_t r = roam_call(fn, ecx, top, n, args);
    g_fp_top = t;
    return r;
}

static void rch_audio(uint32_t id, uint32_t top)        /* audio event (0xC countdown, 0x15 checkpoint) for player 0 */
{
    uint32_t a[4] = { id, 0, 0, 0 }, mgr = MEM32(RCH_AUDIO_VA);
    if (rch_mode() == 1 && online_ram(mgr))
        rch_callf(sub_000233C0, mgr, top, 4, a);
}

/* The HUD's countdown message id (2 "3", 1 "2", 0 "1", 3 "GO") for player p; 1 shown. */
static int rch_hud(uint32_t p, uint32_t id, uint32_t top)
{
    uint32_t hud = MEM32(p + 0x58u), q, a[3] = { id, 0, 0 };
    if (rch_mode() != 1 || !online_ram(hud) || !online_ram(q = MEM32(hud + 0x50u)) || !MEM8(q + 0x84u) ||
        !online_ram(MEM32(q + 0xCu + 4u * id)))
        return 0;
    rch_callf(sub_000B6330, q, top, 3, a);
    return 1;
}

static void rch_nat_off(void)
{
    uint32_t R = rch_race(), rec, a[1], top = (g_esp - 0x400u) & ~0xFu;
    if (s_nat.vt) {
        if (MEM32(RCH_CRUISE_VT + 0xE8u) == RCH_ARROW_FN)
            MEM32(RCH_CRUISE_VT + 0xE8u) = RCH_ARROW_STUB;
        s_nat.vt = 0;
    }
    if (!s_nat.on)
        return;
    s_nat.on = 0;
    s_rch.native = 0;
    if (!R || R != s_nat.race || MEM32(R + 0x28u) != s_nat.arr || MEM32(s_nat.arr - 4u) != s_nat.cnt)
        return;                                 /* that cruise is gone (its unload delete[]d ours) */
    MEM32(R + 0x24u) = 0;
    MEM32(R + 0x28u) = s_nat.orig;              /* its own (empty) array back: the unload frees that one */
    a[0] = s_nat.arr - 4u;
    roam_call(sub_001B3CD0, 0, top, 1, a);      /* delete[] ours (the checkpoint dtor sub_001E2D40 is empty) */
    rec = R + 0x98u;
    MEM32(rec + 0x18u) = s_nat.r18;
    MEM16(rec + 0x34u) = s_nat.r34;
    MEM8(rec + 4u) = s_nat.r04;
    MEM8(rec + 0x48u) = s_nat.r48;
    for (int i = 0; i < 4; i++)
        MEM32(rec + 0x4Cu + 4u * i) = s_nat.r4c[i];
    fprintf(stderr, "[CHALLENGE] native checkpoints off\n");
}

/* The course as the cruise race's checkpoints; 1 done. */
static int rch_nat_on(uint32_t top)
{
    uint32_t R = rch_race(), arr, orig, cp, rec, a[1], k, i;
    if (s_nat.on)
        rch_nat_off();
    if (rch_mode() == 3 || !R || s_rch.ncp < 2)
        return 0;
    /* The cruise's .rac reader (sub_001CDC80) new[]s 4 + 0x120 n bytes even for numcheckpoints 0: an empty
     * array (count 0 at -4) is the cruise's normal state, ours goes over it and it comes back at the end. */
    orig = MEM32(R + 0x28u);
    if (MEM32(R + 0x24u) || (orig && (!online_ram(orig - 4u) || MEM32(orig - 4u)))) {
        fprintf(stderr, "[CHALLENGE] native checkpoints: the cruise has %u of its own, page marker instead\n",
                (unsigned)MEM32(R + 0x24u));
        return 0;
    }
    a[0] = 4u + (uint32_t)s_rch.ncp * RCH_CP_SIZE;
    if (!online_ram(arr = roam_call(sub_001B3C60, 0, top, 1, a))) {
        fprintf(stderr, "[CHALLENGE] native checkpoints: no memory, page marker instead\n");
        return 0;
    }
    MEM32(arr) = (uint32_t)s_rch.ncp;           /* new[]'s count: the unload's delete[] walks it */
    arr += 4u;
    s_nat.race = R;
    s_nat.arr = arr;
    s_nat.cnt = (uint32_t)s_rch.ncp;
    s_nat.orig = orig;
    MEM32(R + 0x24u) = 0;
    MEM32(R + 0x28u) = arr;
    for (k = 0; k < (uint32_t)s_rch.ncp; k++) { /* as the .rac reader sub_001CDC80 */
        cp = arr + k * RCH_CP_SIZE;
        roam_call(sub_001CA0A0, cp, top, 0, NULL);
        for (i = 0; i < 3; i++) {
            MEMF(cp + 0xFCu + 4u * i) = s_rch.cp[k][i];
            MEMF(cp + 4u + 0x24u + 4u * i) = s_rch.cp[k][i];
        }
        roam_call(sub_001C8B20, cp + 4u, top, 0, NULL);
        MEMF(cp + 0x10Cu) = s_rch.dims[k][0];
        MEMF(cp + 0x110u) = MEMF(cp + 0x38u) = s_rch.dims[k][1];
        MEMF(cp + 0x108u) = MEMF(cp + 0x34u) = s_rch.dims[k][2];
        MEMF(cp) = s_rch.dims[k][3];
        MEM32(cp + 0xB8u) = cp;
        MEM8(cp + 0x11Cu) = 0;                  /* Type Normal */
        MEM8(cp + 0x11Du) = 1;                  /* RespawnAllowed */
    }
    for (k = 0; k + 1 < (uint32_t)s_rch.ncp; k++)
        MEM32(arr + k * RCH_CP_SIZE + 0x3Cu) = arr + (k + 1u) * RCH_CP_SIZE + 4u;
    MEM32(R + 0x24u) = (uint32_t)s_rch.ncp;
    for (k = 0; k < (uint32_t)s_rch.ncp; k++)
        roam_call(sub_001C7690, arr + k * RCH_CP_SIZE + 4u, top, 0, NULL);
    rec = R + 0x98u;
    s_nat.r18 = MEM32(rec + 0x18u);
    s_nat.r34 = MEM16(rec + 0x34u);
    s_nat.r04 = MEM8(rec + 4u);
    s_nat.r48 = MEM8(rec + 0x48u);
    for (i = 0; i < 4; i++) {
        s_nat.r4c[i] = MEM32(rec + 0x4Cu + 4u * i);
        MEM32(rec + 0x4Cu + 4u * i) = 0;
    }
    MEM32(rec + 0x18u) = 1;                     /* one lap: the last flare is the finish */
    MEM16(rec + 0x34u) = 0;                     /* the target */
    MEM8(rec + 4u) = 0;
    MEM8(rec + 0x48u) = 0;
    if (MEM32(RCH_CRUISE_VT + 0xE8u) == RCH_ARROW_STUB) {
        MEM32(RCH_CRUISE_VT + 0xE8u) = RCH_ARROW_FN;
        s_nat.vt = 1;
    }
    s_nat.on = 1;
    fprintf(stderr, "[CHALLENGE] native checkpoints on: %d in race %08X, arrow %s\n", s_rch.ncp, (unsigned)R,
            s_nat.vt ? "on" : "slot not the stub");
    return 1;
}

/* Our next checkpoint passed? The game's gate test (sub_001D1510) on our car's position. */
static int rch_nat_passed(uint32_t mtx, uint32_t top)
{
    uint32_t R = rch_race(), a[4];
    if (!s_nat.on || R != s_nat.race)
        return 0;
    a[0] = (uint32_t)s_rch.next;
    a[1] = mtx + 0x34u;
    a[2] = 1;
    a[3] = top + 0x20u;                         /* the squared distance: above the call's frame */
    return (roam_call(sub_001D1510, R, top, 4, a) & 0xFFu) != 0;
}

/* Checkpoint k passed at el s: as the checkpoint races do (sub_001DFC00), then the next target. */
static void rch_nat_pass(int k, double el, uint32_t top)
{
    uint32_t R = rch_race(), rec, cp;
    if (!s_nat.on || R != s_nat.race)
        return;
    rec = R + 0x98u;
    cp = s_nat.arr + (uint32_t)k * RCH_CP_SIZE;
    MEMF(rec + 0x30u) = (float)el;
    MEM8(rec + 0x48u) = (uint8_t)(k + 1);
    MEM32(rec + 0x4Cu + 4u * ((uint32_t)k >> 5)) |= 1u << (k & 31);
    if (MEM32(cp + 4u + 0x74u) != 3u) {         /* player 0's state: passed */
        MEM32(cp + 4u + 0x74u) = 1;
        MEMF(cp + 4u + 0x94u) = 0.0f;
    }
    MEM16(rec + 0x34u) = (uint16_t)(k + 1 < s_rch.ncp ? k + 1 : 0xFFFF);
    if (k + 1 >= s_rch.ncp)
        MEM8(rec + 4u) = 1;                     /* finished */
    rch_audio(0x15, top);
}

/* The flares' tick, as the checkpoint races' update (sub_001D8F20) runs it: race vt+0x11C (sub_001CCA20 -> each
 * checkpoint's sub_001C8D50 -> sub_001C7740): a passed flare (state 1) fades out to hidden (3), the next one fades
 * in (2 -> 0).  The cruise never calls it: a passed flare stayed up. */
static void rch_nat_tick(uint32_t top)
{
    uint32_t R = rch_race();
    if (s_nat.on && R == s_nat.race && MEM32(R + 0x28u) == s_nat.arr && MEM32(R + 0x24u) == s_nat.cnt)
        roam_call(sub_001CCA20, R, top, 0, NULL);
}

/* Our grid slot (rch_slots: slot s_rch.me) facing its dir: the car's local +z (row +0x28) is its back, so that row
 * is -dir, as the game's racer placement turns it (sub_0008C2C0 with h = atan2(-dx, -dz)); the car's own matrix
 * turned there (its handedness kept). */
static void rch_grid_pose(uint32_t mtx, float m[12])
{
    const float *f = s_rch.sdir[s_rch.me], *at = s_rch.slot[s_rch.me];
    float s, b[2] = { -f[0], -f[1] };          /* the +z row: backwards */
    int k;
    for (k = 0; k < 12; k++)
        m[k] = MEMF(mtx + 0x10u + 4u * k);
    s = m[0] * m[8] - m[2] * m[6] >= 0.0f ? 1.0f : -1.0f;  /* x row = s * (up x z row) */
    m[0] = s * b[1]; m[1] = 0.0f; m[2] = -s * b[0];
    m[3] = 0.0f;     m[4] = 1.0f; m[5] = 0.0f;
    m[6] = b[0];     m[7] = 0.0f; m[8] = b[1];
    m[9] = at[0];
    m[10] = at[1] + 0.5f;
    m[11] = at[2];
}

static const char *rch_ev_name(void)            /* the course's event, for the log */
{
    int i;
    for (i = 0; s_rch.ev_index != 0xFFFFu && i < roam_races_count(); i++)
        if (roam_races_get(i)->city == s_rch.ev_city && roam_races_get(i)->index == s_rch.ev_index)
            return roam_races_get(i)->name;
    return "start beacons";
}

/* The driving players we would invite: within RCH_NEAR m (height within 30 m), nearest first, at most
 * RCH_RACERS - 1, into s_rch.inv (not committed: s_rch.ninv is set by the caller); their count. */
static int rch_nearby(const float me[3], float reach)
{
    float dist[RCH_RACERS - 1], d;
    int n = 0, i, j;
    if (!s_roam_seen || roam_now() - s_roam_seen_t > 0.5)
        return 0;
    for (i = 0; i < s_roam_nseen; i++) {
        const roam_peer *pp = &s_roam_seen[i];
        if (pp->status != RC_ST_ROAM || (pp->bits & (ROAM_BIT_RACE | ROAM_BIT_STREET)) || (d = rch_hd(pp->p, me)) > reach ||
            fabsf(pp->p[1] - me[1]) > RCH_DY)
            continue;
        for (j = n < RCH_RACERS - 1 ? n++ : RCH_RACERS - 1; j > 0 && dist[j - 1] > d; j--)
            if (j < RCH_RACERS - 1) {
                dist[j] = dist[j - 1];
                s_rch.inv[j] = s_rch.inv[j - 1];
            }
        if (j < RCH_RACERS - 1) {
            dist[j] = d;
            s_rch.inv[j].id = pp->id;
            snprintf(s_rch.inv[j].name, sizeof s_rch.inv[j].name, "%s", pp->name);
            s_rch.inv[j].joined = s_rch.inv[j].out = 0;
        }
    }
    return n;
}

/* Late joiners: while the invite window is open, a driving player who came within RCH_NEAR m since White gets
 * appended to s_rch.inv (never one already listed: joined, out or NO stay as they are); max RCH_RACERS - 1. */
static void rch_late(const float me[3])
{
    int i, k;
    if (!s_roam_seen || roam_now() - s_roam_seen_t > 0.5)
        return;
    for (i = 0; i < s_roam_nseen && s_rch.ninv < RCH_RACERS - 1; i++) {
        const roam_peer *pp = &s_roam_seen[i];
        float d;
        if (pp->status != RC_ST_ROAM || (pp->bits & (ROAM_BIT_RACE | ROAM_BIT_STREET)) || (d = rch_hd(pp->p, me)) > RCH_NEAR ||
            fabsf(pp->p[1] - me[1]) > RCH_DY)
            continue;
        for (k = 0; k < s_rch.ninv && s_rch.inv[k].id != pp->id; k++)
            ;
        if (k < s_rch.ninv)
            continue;
        s_rch.inv[k].id = pp->id;
        snprintf(s_rch.inv[k].name, sizeof s_rch.inv[k].name, "%s", pp->name);
        s_rch.inv[k].joined = s_rch.inv[k].out = 0;
        s_rch.ninv++;
        fprintf(stderr, "[CHALLENGE] late invite: %s (%.0f m)\n", s_rch.inv[k].name, d);
    }
}

/* ---- group race (roam_race_ai.c): the same invite, then every racer loads the event with its AI ---- */

/* The event's first two prompt lines ("Sprint · Downtown", "Easy · 7 opponents") into d1, d2. */
static void rch_ai_desc(char *d1, size_t n, const char **d2)
{
    char *e;
    rra_text(d1, n, s_rch.air, 0);
    *d2 = "";
    if ((e = strchr(d1, '\n')) != NULL) {
        *e++ = 0;
        *d2 = e;
        if ((e = strchr(e, '\n')) != NULL)
            *e = 0;
    }
}

/* GO: every racer still in (join order) loads the event now; alone: just us, as the AI race prompt does. */
static void rch_ai_go(uint32_t mtx, uint32_t top, double now, int alone)
{
    uint32_t ids[RCH_RACERS];
    const roam_race *r = s_rch.air;
    int n = alone ? 1 : s_rch.nrc, me = alone ? 0 : s_rch.me, k;
    for (k = 0; k < n; k++)
        ids[k] = alone || s_rch.rc[k].out ? 0 : s_rch.rc[k].id;
    rch_end(alone && s_rch.st == RCH_INVITING ? RCH_NO_DECLINE : 0, NULL, now);
    fprintf(stderr, "[RACE] group: %s\n", alone ? "nobody else races: alone against the AI" : "GO: loading the race");
    rra_group_start(r, ids, n, me, mtx, top);
}

/* ---- quitting, markers ---- */

static int rch_quit_act(void)                   /* RCH_Q_* offered in the state we are in */
{
    switch (s_rch.st) {
    case RCH_INVITING: return RCH_Q_CANCEL;
    case RCH_INVITED:  return RCH_Q_DECLINE;
    case RCH_JOINED:   return RCH_Q_LEAVE;
    case RCH_COUNT:    return RCH_Q_GIVEUP;
    case RCH_RACE:     return s_rch.rc[s_rch.me].fin == 0.0 ? RCH_Q_GIVEUP : RCH_Q_NONE;
    }
    return RCH_Q_NONE;
}

/* Quit as act says: the others hear NO (the host's invitees "cancelled", the host "can't race", racers mark us out:
 * DNF in their result); our checkpoints go (rch_end -> rch_nat_off puts the cruise's empty array back). */
static void rch_quit(int act, int button, double now)
{
    char t[RRA_TEXT];
    const char *how = button ? "page button" : "Black held";
    if (act == RCH_Q_CANCEL) {
        fprintf(stderr, "[CHALLENGE] cancel: we cancel our race %08X (%s)\n", (unsigned)s_rch.race, how);
        rch_end(RCH_NO_CANCEL, "Race cancelled\n\nFree Roam goes on", now);
    } else if (act == RCH_Q_LEAVE || act == RCH_Q_DECLINE) {
        fprintf(stderr, "[CHALLENGE] cancel: we %s %s's race %08X (%s)\n", act == RCH_Q_LEAVE ? "leave" : "decline",
                s_rch.name, (unsigned)s_rch.race, how);
        snprintf(t, sizeof t, act == RCH_Q_LEAVE ? "You left %s's race\n\nFree Roam goes on"
                                                 : "You declined %s's race\n\nFree Roam goes on", s_rch.name);
        rch_end(RCH_NO_DECLINE, t, now);
    } else if (act == RCH_Q_GIVEUP) {
        fprintf(stderr, "[CHALLENGE] give up: race %08X, checkpoint %d of %d at %.1f s (%s)\n", (unsigned)s_rch.race,
                s_rch.next, s_rch.ncp, now - s_rch.t0, how);
        rch_end(RCH_NO_DECLINE, "You gave up the race\n\nFree Roam goes on", now);
    }
}

/* rra_prompt for the race states: the quit hint as a last line, and the page's button. */
static void rch_prompt(const char *text)
{
    static const char *what[] = { "", "cancel the race", "leave the race", "give up", "decline" };
    char t[RRA_TEXT];
    int act = rch_quit_act();
#ifdef __EMSCRIPTEN__
    s_rch_btn.act = act;
#endif
    if (!act) {
        rra_prompt(text);
        return;
    }
    snprintf(t, sizeof t, "%s\nHold Black [E] to %s", text, what[act]);
    rra_prompt(t);
}

/* Our rc_state.bits (roam_send_local): ROAM_BIT_STREET while we are in a street or group race of the cruise --
 * hosting one someone joined (a group race's host races anyway), joined, counting down, racing until we finish. */
static unsigned rch_bits(void)
{
    int k;
    if (!rch_on())
        return 0;
    switch (s_rch.st) {
    case RCH_INVITING:
        for (k = 0; !s_rch.ai && k < s_rch.ninv && !(s_rch.inv[k].joined && !s_rch.inv[k].out); k++)
            ;
        return s_rch.ai || k < s_rch.ninv ? ROAM_BIT_STREET : 0;
    case RCH_JOINED:
    case RCH_COUNT:
        return ROAM_BIT_STREET;
    case RCH_RACE:
        return s_rch.rc[s_rch.me].fin == 0.0 ? ROAM_BIT_STREET : 0;
    }
    return 0;
}

/* Player `id` is going to race or racing (its name tag says so, roam_tags.c): its own bits (any race, or joined
 * a street/group race), or what we know of our race (joined our invite, our host, a racer still in). */
static int rch_marked(uint32_t id)
{
    const roam_peer *pp = rch_peer(id);
    int k;
    if (pp && (pp->bits & (ROAM_BIT_RACE | ROAM_BIT_STREET)))
        return 1;
    switch (s_rch.st) {
    case RCH_INVITING:
        for (k = 0; k < s_rch.ninv; k++)
            if (s_rch.inv[k].id == id)
                return s_rch.inv[k].joined && !s_rch.inv[k].out;
        return 0;
    case RCH_JOINED:
        return id == s_rch.peer;
    case RCH_COUNT:
    case RCH_RACE:
        return (k = rch_racer_of(id)) >= 0 && !s_rch.rc[k].out && s_rch.rc[k].fin == 0.0;
    }
    return 0;
}

#ifdef __EMSCRIPTEN__
/* Read by the page (web/pre.js, the Race panel beside the full map): mode (0 hidden, 1 our race, 2 players racing near
 * us), then UTF-8 lines joined by '\n'. Mode 1: "Street race\t<event>\t<our status>", then one row per racer
 * "<place>\t<name>\t<k/N>\t<mark>" (place and k/N empty before the start; mark: host, joined, invited, out, DNF, a finish
 * time). Mode 2: "Racing now", then "<name>\t<race|street>". Refilled by each call of the getter; no wire data of its own
 * (the peers' rc_state bits 0x40/0x80 and our race's JOIN/PROG state). */
static struct { int32_t mode; char text[700]; } s_rch_box;
EMSCRIPTEN_KEEPALIVE uintptr_t roam_race_box(void)
{
    char *o = s_rch_box.text, *end = s_rch_box.text + sizeof s_rch_box.text - 80, t[16];
    const char *status = "";
    int order[RCH_RACERS], i, k, n = 0;
    o[0] = 0;
    s_rch_box.mode = 0;
    if (!rch_on() || (!s_online && !s_roam_race))   /* s_roam_race: also an Online career race */
        return (uintptr_t)&s_rch_box;
#define RB(...) (o += snprintf(o, (size_t)(s_rch_box.text + sizeof s_rch_box.text - o), __VA_ARGS__))
    switch (s_rch.st) {
    case RCH_INVITING: status = "Inviting"; break;
    case RCH_INVITED: status = "Invited"; break;
    case RCH_JOINED: status = "Joined"; break;
    case RCH_COUNT: status = "Starting"; break;
    case RCH_RACE: status = s_rch.rc[s_rch.me].fin > 0.0 ? "Finished" : "Racing"; break;
    case RCH_RESULT: status = "Result"; break;
    }
    if (s_rch.st != RCH_IDLE) {
        s_rch_box.mode = 1;
        RB("%s\t%s\t%s", s_rch.ai ? "Group race" : "Street race",
           s_rch.ai && s_rch.air ? s_rch.air->name : rch_ev_name(), status);
        if (s_rch.st == RCH_INVITING) {
            RB("\n\t%s\t\thost", "You");
            for (k = 0; k < s_rch.ninv && o < end; k++)
                RB("\n\t%s\t\t%s", s_rch.inv[k].name, s_rch.inv[k].out ? "out" : s_rch.inv[k].joined ? "joined" : "invited");
        } else if (s_rch.st == RCH_INVITED || s_rch.st == RCH_JOINED) {
            RB("\n\t%s\t\thost\n\t%s\t\t%s", s_rch.name, "You", s_rch.st == RCH_JOINED ? "joined" : "invited");
        } else {
            rch_rank(order, s_rch_pos);
            for (i = 0; i < s_rch.nrc && o < end; i++) {
                const rch_racer *r = &s_rch.rc[order[i]];
                if (r->fin > 0.0)
                    rch_fmt_time(t, sizeof t, r->fin);
                else
                    t[0] = 0;
                RB("\n%d\t%s\t%d/%d\t%s%s", i + 1, order[i] == s_rch.me ? "You" : r->name, r->cps, s_rch.ncp,
                   r->fin > 0.0 ? t : r->out || s_rch.st == RCH_RESULT ? "DNF" : "", order[i] == 0 ? " host" : "");
            }
        }
        return (uintptr_t)&s_rch_box;
    }
    if (s_roam_race || roam_group_race() || rch_bits()) {   /* we race: first, as '<nick> (you) · race' (the page knows the nick) */
        RB("Racing now\n\tme");
        n++;
    }
    if (!s_roam_seen || roam_now() - s_roam_seen_t > 0.5) {
        s_rch_box.mode = n ? 2 : 0;
        return (uintptr_t)&s_rch_box;
    }
    for (i = 0; i < s_roam_nseen && n < 8; i++) {
        const roam_peer *pp = &s_roam_seen[i];
        if (!(pp->bits & (ROAM_BIT_RACE | ROAM_BIT_STREET)))
            continue;
        if (!n)
            RB("Racing now");
        RB("\n%s\t%s", pp->name, pp->bits & ROAM_BIT_STREET ? "street" : "race");
        n++;
    }
    s_rch_box.mode = n ? 2 : 0;
#undef RB
    return (uintptr_t)&s_rch_box;
}
#endif

/* ---- the frame ---- */

/* Each player's update (sub_002C1520) for slot 0, before rra_player; 1: the street race owns the prompt and White
 * (rra_player then stays out). */
static int rch_player(uint32_t p)
{
    uint32_t setup = MEM32(ROAM_SETUP_VA), veh, mtx, coll, top = (g_esp - 0x400u) & ~0xFu;
    XBOX_INPUT_STATE st;
    const roam_peer *pp = NULL;
    float me[3], fwd[3], v[3] = { 0 }, speed;
    double now = roam_now(), el, last;
    int white, black, press, click, act, i, k, n, nj, nin, go, place, order[RCH_RACERS], got;
    char text[RRA_TEXT] = "", t1[16], t2[160], d1[RRA_TEXT];
    const char *big, *tail, *d2;
    const roam_race *gr;
    float gd;
    rch_racer *mine;
    if (!rch_on() || !online_ram(p) || MEM32(p + 0x5Cu) != 0)
        return 0;
    if (!s_online || !roam_running() || !online_ram(setup) || MEM32(setup + 0x18u) != 0 || s_rra_race || s_rra_back ||
        MEM8(setup + 0x108u) || MEM32(0x0050F14Cu) != 0 || !online_ram(veh = MEM32(p + 0x14u)) ||
        !online_ram(mtx = MEM32(veh + 0x1Cu))) {
#ifdef __EMSCRIPTEN__
        s_rch_btn.act = RCH_Q_NONE;
#endif
        if (s_rch.st != RCH_IDLE)               /* a race load, the garage, the menus: we are out */
            rch_end(RCH_NO_CANCEL, "cancelled: we left the cruise", now);
        /* Online career (no street or group races), or racing with the session on (roam_race_load): invites NO busy */
        if ((s_career_roam || s_roam_race) && roam_running()) {
            uint8_t b[ROAM_MSG_MAX];
            uint32_t from;
            while ((n = roam_recv_msg(&from, b, sizeof b)) > 0)
                if (n >= 8 && b[0] == 'C' && b[1] == 5 && b[2] == RCH_OP_INVITE)
                    rch_send(from, rch_get32(b + 4), RCH_OP_NO, RCH_NO_BUSY, now);
        }
        return 0;
    }
    for (i = 0; i < 3; i++) {
        me[i] = MEMF(mtx + 0x34u + 4u * i);
        fwd[i] = -MEMF(mtx + 0x28u + 4u * i);  /* row +0x28 is the car's back (rch_grid_pose) */
        s_rch_pos[i] = me[i];
    }
    if ((coll = rra_coll(veh)) != 0)
        for (i = 0; i < 3; i++)
            v[i] = MEMF(coll + 0x44u + 4u * i);
    speed = rch_speed(v);
    got = xbox_InputGetState(0, &st) == 0;
    white = got && st.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] > XBOX_ANALOG_BUTTON_THRESHOLD;
    black = got && st.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] > XBOX_ANALOG_BUTTON_THRESHOLD;
#ifdef __EMSCRIPTEN__
    click = __atomic_exchange_n(&s_rch_btn.click, 0, __ATOMIC_ACQ_REL);
#else
    click = 0;
#endif
    press = white && !s_rch.white_was;
    s_rch.white_was = white;
    if (click == RCH_Q_ACCEPT && s_rch.st == RCH_INVITED)
        press = 1;                              /* the page's Accept: as White */
    rch_recv(me, now, g_esp);
    if (s_rch.no.left > 0 && now - s_rch.no.tx >= RCH_RESEND / 3) {     /* each NO three times */
        for (i = 0; i < s_rch.no.n; i++)
            rch_send(s_rch.no.to[i], s_rch.no.race, RCH_OP_NO, s_rch.no.why, now);
        s_rch.no.left--;
        s_rch.no.tx = now;
    }
    rch_nat_tick(top);
    if (s_rch.st == RCH_INVITED || s_rch.st == RCH_JOINED) {    /* the host gone or away */
        pp = rch_peer(s_rch.peer);
        if (pp && pp->status == RC_ST_ROAM)
            s_rch.away = 0;
        else if (!s_rch.away)
            s_rch.away = now;
        if (s_rch.away && now - s_rch.away > RCH_GONE_S) {
            snprintf(text, sizeof text, "%s left\n\nFree Roam goes on", s_rch.name);
            rch_end(RCH_NO_CANCEL, text, now);
        }
    } else if (s_rch.st == RCH_COUNT || s_rch.st == RCH_RACE) {    /* racers gone, away or silent: out */
        for (k = 0, n = 0; k < s_rch.nrc; k++) {
            rch_racer *r = &s_rch.rc[k];
            if (k == s_rch.me || r->out)
                continue;
            pp = rch_peer(r->id);
            if (pp && pp->status == RC_ST_ROAM)
                r->away = 0;
            else if (!r->away)
                r->away = now;
            if (r->fin == 0.0 && ((r->away && now - r->away > RCH_GONE_S) || now - r->heard > RCH_GONE_S)) {
                r->out = 1;
                fprintf(stderr, "[CHALLENGE] %s is out (gone or silent %.0f s)\n", r->name, RCH_GONE_S);
            }
            n += !r->out;
        }
        if (!n) {                               /* every other racer out */
            if (s_rch.ai) {                     /* a group race: we race the AI alone */
                rch_ai_go(mtx, top, now, 1);
                return 1;
            }
            if (s_rch.st == RCH_RACE && s_rch.rc[s_rch.me].fin > 0.0)
                rch_result(me, now);
            else
                rch_end(0, "Everyone else left the race\n\nFree Roam goes on", now);
        }
    }
    /* cancel (the host, before GO), leave or decline (an invitee), give up (a racer): Black held, or the button */
    act = rch_quit_act();
    if (!black || !act)
        s_rch.black_t = 0;
    else if (!s_rch.black_t && !s_rch.black_was)
        s_rch.black_t = now;                    /* a press while a quit is offered: one held from before never counts */
    s_rch.black_was = black;
    if (act && ((s_rch.black_t && now - s_rch.black_t >= RCH_QUIT_S) || click == act)) {
        rch_quit(act, click == act, now);
        s_rch.black_t = 0;
        rch_prompt(s_rch.note);
        return 1;
    }
    switch (s_rch.st) {
    case RCH_IDLE:
#ifdef __EMSCRIPTEN__
        s_rch_btn.act = RCH_Q_NONE;
#endif
        if (now < s_rch.note_until) {
            rra_prompt(s_rch.note);
            return 1;
        }
        if (!remix_driving() || speed > RCH_SLOW)
            return 0;
        /* a map race start area beats the street race: no opponents (time trial, frenzy) = roam_race_ai.c's solo
         * prompt; with opponents the group race, inviting everyone in reach of the host (the invitee's own limit,
         * RCH_AWAY, is the same: the host's area decides, not the invitee's) */
        gr = roam_races_nearest(MEM32(setup), me[0], me[2], RRA_RADIUS, &gd);
        if (gr && !gr->opponents)
            return 0;
        if (!(n = rch_nearby(me, RCH_NEAR)))
            return 0;                           /* no one near: the AI race prompt may show */
        /* at a race start point with opponents: a group race of its event (alone if nobody joins) */
        if (gr) {
            s_rch.air = gr;
            if (!press || speed > RRA_SLOW) {
                rch_ai_desc(d1, sizeof d1, &d2);
                snprintf(text, sizeof text, "%s\n%s \xC2\xB7 %d nearby player%s invited\n%s", d1, d2, n, n > 1 ? "s" : "",
                         speed > RRA_SLOW ? "Slow down to race" : "Press White [Q] to race");
                rra_prompt(text);
                return 1;
            }
            s_rch.st = RCH_INVITING;
            s_rch.ai = 1;
            s_rch.ninv = n;
            s_rch.host = 1;
            s_rch.peer = 0;
            s_rch.ncp = 0;
            s_rch.km = 0.0f;
            s_rch.ev_city = gr->city;
            s_rch.ev_index = gr->index;
            memcpy(s_rch.grid, gr->pos, sizeof s_rch.grid);
            s_rch.race = (uint32_t)rand() ^ (uint32_t)(now * 1e6) ^ s_rch.inv[0].id;
            if (!s_rch.race)
                s_rch.race = 1;
            s_rch.t0 = now;
            s_rch.tx = 0;
            fprintf(stderr, "[CHALLENGE] invite: %d players within 200 m (of %d near)\n", n, s_roam_nseen);
            fprintf(stderr, "[RACE] group: we invite %d player%s to %s (race %08X):", n, n > 1 ? "s" : "", gr->name,
                    (unsigned)s_rch.race);
            for (k = 0; k < n; k++)
                fprintf(stderr, " %s", s_rch.inv[k].name);
            fprintf(stderr, "\n");
            return 1;
        }
        s_rch.ai = 0;
        if (!press) {
            snprintf(text, sizeof text, "Street race?\n%d nearby player%s will be invited\nPress White [Q] (Flash) to invite",
                     n, n > 1 ? "s" : "");
            rra_prompt(text);
            return 1;
        }
        if (!rch_course(MEM32(setup), me, fwd, g_esp)) {
            rch_note("Street race\nNo race course near here\nTry closer to a race start", now);
            rra_prompt(s_rch.note);
            return 1;
        }
        s_rch.st = RCH_INVITING;
        s_rch.ninv = n;
        s_rch.host = 1;
        s_rch.peer = 0;
        s_rch.race = (uint32_t)rand() ^ (uint32_t)(now * 1e6) ^ s_rch.inv[0].id;
        if (!s_rch.race)
            s_rch.race = 1;
        s_rch.t0 = now;
        s_rch.tx = 0;
        fprintf(stderr, "[CHALLENGE] invite: %d players within 200 m (of %d near)\n", n, s_roam_nseen);
        fprintf(stderr, "[CHALLENGE] we invite %d player%s (race %08X, %d checkpoints, %.1f km):", n, n > 1 ? "s" : "",
                (unsigned)s_rch.race, s_rch.ncp, s_rch.km);
        for (k = 0; k < n; k++)
            fprintf(stderr, " %s", s_rch.inv[k].name);
        fprintf(stderr, "\n");
        return 1;
    case RCH_INVITING:
        rch_late(me);
        for (k = 0, nj = 0, nin = 0; k < s_rch.ninv; k++) {
            if (!s_rch.inv[k].out && (pp = rch_peer(s_rch.inv[k].id)) != NULL &&
                (pp->status != RC_ST_ROAM || rch_hd(pp->p, me) > RCH_AWAY)) {
                s_rch.inv[k].out = 1;           /* left the cruise or drove off */
                fprintf(stderr, "[CHALLENGE] %s is out of the invite (away)\n", s_rch.inv[k].name);
            }
            nin += !s_rch.inv[k].out;
            nj += !s_rch.inv[k].out && s_rch.inv[k].joined;
        }
        if (s_rch.ai && (press || nj == nin || now - s_rch.t0 >= RCH_ASK_S)) {
            if (!nj) {                          /* nobody joined (or could): the race alone */
                rch_ai_go(mtx, top, now, 1);
                return 1;
            }
            rch_host_go(now);
            break;
        }
        if (s_rch.ai) {
            k = (int)(RCH_ASK_S - (now - s_rch.t0) + 0.99);
            rch_ai_desc(d1, sizeof d1, &d2);
            snprintf(text, sizeof text, "%s\nGroup race \xC2\xB7 %d of %d joined \xC2\xB7 %d s\nPress White [Q] to start now",
                     d1, nj, nin, k > 0 ? k : 0);
            if (now - s_rch.tx >= RCH_RESEND) {
                for (k = 0; k < s_rch.ninv; k++)
                    if (!s_rch.inv[k].out)
                        rch_send(s_rch.inv[k].id, s_rch.race, RCH_OP_INVITE, 0, now);
                s_rch.tx = now;
            }
            rch_prompt(text);
            return 1;
        }
        if (nj && (press || nj == nin || now - s_rch.t0 >= RCH_ASK_S)) {
            rch_host_go(now);
            break;                              /* the countdown from the next frame */
        }
        if (!nin || now - s_rch.t0 >= RCH_ASK_S) {
            rch_end(RCH_NO_DECLINE, nin ? "Nobody joined the race\n\nFree Roam goes on"
                                        : "Nobody can race now\n\nFree Roam goes on", now);
            return 1;
        }
        if (now - s_rch.tx >= RCH_RESEND) {
            for (k = 0; k < s_rch.ninv; k++)
                if (!s_rch.inv[k].out)
                    rch_send(s_rch.inv[k].id, s_rch.race, RCH_OP_INVITE, 0, now);
            s_rch.tx = now;
        }
        k = (int)(RCH_ASK_S - (now - s_rch.t0) + 0.99);
        if (nj)
            snprintf(text, sizeof text, "Street race \xC2\xB7 %d of %d joined\n%d checkpoints \xC2\xB7 %.1f km\nPress White [Q] to start now \xC2\xB7 %d s",
                     nj, nin, s_rch.ncp, s_rch.km, k > 0 ? k : 0);
        else
            snprintf(text, sizeof text, "Street race \xC2\xB7 %d invited\n%d checkpoints \xC2\xB7 %.1f km\nWaiting for players to join \xC2\xB7 %d s",
                     nin, s_rch.ncp, s_rch.km, k > 0 ? k : 0);
        rch_prompt(text);
        return 1;
    case RCH_INVITED:
    case RCH_JOINED:
        if (now - s_rch.t0 > RCH_ASK_S + (s_rch.st == RCH_JOINED ? 3.0 : 0.0)) {
            snprintf(text, sizeof text, s_rch.st == RCH_INVITED ? "Invite from %s expired\n\nFree Roam goes on"
                                                                : "%s's race did not start\n\nFree Roam goes on",
                     s_rch.name);
            rch_end(RCH_NO_DECLINE, text, now);
            return 1;
        }
        if (pp && rch_hd(pp->p, me) > RCH_AWAY) {
            snprintf(text, sizeof text, "Too far from %s's race\n\nFree Roam goes on", s_rch.name);
            rch_end(RCH_NO_DECLINE, text, now);
            return 1;
        }
        if (s_rch.st == RCH_JOINED && now - s_rch.tx >= RCH_RESEND) {
            rch_send(s_rch.peer, s_rch.race, RCH_OP_JOIN, 0, now);
            s_rch.tx = now;
        }
        k = (int)(RCH_ASK_S - (now - s_rch.t0) + 0.99);
        if (s_rch.ai) {                         /* a group race */
            rch_ai_desc(d1, sizeof d1, &d2);
            if (s_rch.st == RCH_JOINED)
                snprintf(text, sizeof text, "%s\nGroup race with %s (%d players, %d joined)\nJoined: waiting for the start", d1,
                         s_rch.name, s_rch.players, s_rch.accepted);
            else
                snprintf(text, sizeof text, "%s invites you: group race (%d players, %d joined)\n%s \xC2\xB7 %s\n%s \xC2\xB7 %d s",
                         s_rch.name, s_rch.players, s_rch.accepted, d1, d2, speed > RCH_JOIN_SLOW ? "Slow down to join" : "Press White [Q] to join",
                         k > 0 ? k : 0);
            if (s_rch.st == RCH_INVITED && press && speed <= RCH_JOIN_SLOW) {
                s_rch.st = RCH_JOINED;
                s_rch.tx = 0;
                fprintf(stderr, "[RACE] group: we join %s's race\n", s_rch.name);
            }
            rch_prompt(text);
            return 1;
        }
        if (s_rch.st == RCH_JOINED)
            snprintf(text, sizeof text, "Race with %s (%d players, %d joined)\n%d checkpoints \xC2\xB7 %.1f km\nJoined: waiting for the start",
                     s_rch.name, s_rch.players, s_rch.accepted, s_rch.ncp, s_rch.km);
        else if (speed > RCH_JOIN_SLOW)
            snprintf(text, sizeof text, "%s invites you to a race (%d players, %d joined)\n%d checkpoints \xC2\xB7 %.1f km\nSlow down to join \xC2\xB7 %d s",
                     s_rch.name, s_rch.players, s_rch.accepted, s_rch.ncp, s_rch.km, k > 0 ? k : 0);
        else {
            snprintf(text, sizeof text, "%s invites you to a race (%d players, %d joined)\n%d checkpoints \xC2\xB7 %.1f km\nPress White [Q] (Flash) to join \xC2\xB7 %d s",
                     s_rch.name, s_rch.players, s_rch.accepted, s_rch.ncp, s_rch.km, k > 0 ? k : 0);
            if (press) {
                s_rch.st = RCH_JOINED;
                s_rch.tx = 0;
                fprintf(stderr, "[CHALLENGE] we join %s's race\n", s_rch.name);
            }
        }
        rch_prompt(text);
        return 1;
    case RCH_COUNT:
        if (s_rch.ai) {                         /* a group race: the countdown to everyone's load */
            if (now - s_rch.tx >= RCH_RESEND) {
                rch_send_racers(s_rch.host ? RCH_OP_GO : RCH_OP_PROG, now);
                s_rch.tx = now;
            }
            if (now < s_rch.t0) {
                k = (int)(s_rch.t0 - now) + 1;
                rch_ai_desc(d1, sizeof d1, &d2);
                snprintf(text, sizeof text, RCH_BIG "%d\n%s\nGroup race \xC2\xB7 %d racers \xC2\xB7 loading", k > 3 ? 3 : k,
                         d1, s_rch.nrc);
                rch_prompt(text);
                return 1;
            }
            rch_ai_go(mtx, top, now, 0);
            return 1;
        }
        if (!s_rch.held) {
            rch_grid_pose(mtx, s_rch.hold);     /* our slot on the start line */
            s_rch.held = 1;
            s_rch.next = 0;
            s_rch.said = 4;
            s_rch.native = rch_nat_on(top);
            rch_audio(0xC, top);                /* the countdown sound */
            fprintf(stderr, "[CHALLENGE] grid: %s: %d racers, we are slot %d at (%.1f, %.1f, %.1f) heading %.0f deg "
                    "(the game's atan2(-dx, -dz)); %.0f m from where we stood; %s checkpoints\n", rch_ev_name(),
                    s_rch.nrc, s_rch.me, s_rch.slot[s_rch.me][0], s_rch.slot[s_rch.me][1], s_rch.slot[s_rch.me][2],
                    atan2f(-s_rch.sdir[s_rch.me][0], -s_rch.sdir[s_rch.me][1]) * 57.29578f, rch_hd(&s_rch.hold[9], me),
                    s_rch.native ? "the game's" : "page");
        }
        if (now - s_rch.tx >= RCH_RESEND) {     /* the host's GO, the others' PROG: everyone hears everyone */
            rch_send_racers(s_rch.host ? RCH_OP_GO : RCH_OP_PROG, now);
            s_rch.tx = now;
        }
        if (now < s_rch.t0) {
            float zero[3] = { 0 };
            int left = (int)(s_rch.t0 - now) + 1, hud = 0;
            roam_set_pose(veh, s_rch.hold, zero);   /* everyone waits on the grid */
            if (roam_running())
                roam_teleport();                /* peers snap to it */
            if (left > 3)
                left = 3;
            if (left < s_rch.said) {
                s_rch.said = left;
                hud = rch_hud(p, (uint32_t)(left - 1), top);   /* "3" is id 2 ... "1" id 0 */
                s_rch.native = s_rch.native | (hud << 1);
            }
            if (s_rch.native & 2)               /* the HUD counts: the box only says who */
                snprintf(text, sizeof text, "Street race \xC2\xB7 %d racers\n%d checkpoints \xC2\xB7 %.1f km\nGet ready",
                         s_rch.nrc, s_rch.ncp, s_rch.km);
            else
                snprintf(text, sizeof text, RCH_BIG "%d\nStreet race \xC2\xB7 %d racers \xC2\xB7 %d checkpoints\nGet ready",
                         left, s_rch.nrc, s_rch.ncp);
            rch_prompt(text);
            return 1;
        }
        s_rch.st = RCH_RACE;
        s_rch.tx = 0;
        s_rch.cp_t = now;
        s_rch.cp_best = 1e9f;
        rch_hud(p, 3, top);                     /* GO */
        fprintf(stderr, "[CHALLENGE] GO: %d racers (HUD countdown %s)\n", s_rch.nrc,
                s_rch.native & 2 ? "the game's" : "not there: prompt");
        /* fall through */
    case RCH_RACE:
        el = now - s_rch.t0;
        mine = &s_rch.rc[s_rch.me];
        if (mine->fin == 0.0 && s_rch.next < s_rch.ncp &&
            (s_nat.on ? rch_nat_passed(mtx, top)
                      : rch_hd(s_rch.cp[s_rch.next], me) < RCH_CP_R && fabsf(s_rch.cp[s_rch.next][1] - me[1]) < RCH_CP_DY)) {
            rch_nat_pass(s_rch.next, el, top);
            mine->cps = ++s_rch.next;
            s_rch.tx = 0;                       /* tell now */
            s_rch.cp_t = now;
            s_rch.cp_best = 1e9f;
            fprintf(stderr, "[CHALLENGE] checkpoint %d of %d at %.2f s\n", s_rch.next, s_rch.ncp, el);
            if (s_rch.next == s_rch.ncp) {
                mine->fin = el > 0.001 ? el : 0.001;
                rch_nat_off();                  /* the finish: flares and arrow gone */
                fprintf(stderr, "[CHALLENGE] we finished in %.2f s\n", mine->fin);
            }
        }
        /* driven off the course or stopped: we gave up (NO decline, as Black held), else we would race on -- flag,
         * 0x80 and the others' wait -- up to RCH_MAX_S s */
        if (mine->fin == 0.0 && s_rch.next < s_rch.ncp) {
            gd = rch_hd(s_rch.cp[s_rch.next], me);
            if (gd < s_rch.cp_best)
                s_rch.cp_best = gd;
            if (gd > s_rch.cp_best + RCH_OFF_M || now - s_rch.cp_t > RCH_STALL_S) {
                fprintf(stderr, "[CHALLENGE] give up: race %08X, checkpoint %d of %d at %.1f s (left the course: %.0f m "
                        "from it, closest %.0f m, %.0f s since the last)\n", (unsigned)s_rch.race, s_rch.next, s_rch.ncp,
                        el, gd, s_rch.cp_best, now - s_rch.cp_t);
                rch_end(RCH_NO_DECLINE, gd > s_rch.cp_best + RCH_OFF_M ? "You left the race: too far from the course\n\nFree Roam goes on"
                                                                       : "You left the race: no checkpoint for 2 minutes\n\nFree Roam goes on", now);
                rch_prompt(s_rch.note);
                return 1;
            }
        }
        if (now - s_rch.tx >= RCH_RESEND) {
            rch_send_racers(RCH_OP_PROG, now);
            s_rch.tx = now;
        }
        /* decided: every racer still in finished, or one left RCH_LATE s after the latest finish, or the time limit */
        for (k = 0, n = 0, last = 0.0; k < s_rch.nrc; k++) {
            if (s_rch.rc[k].fin > last)
                last = s_rch.rc[k].fin;
            n += s_rch.rc[k].fin == 0.0 && !s_rch.rc[k].out;
        }
        if (!n || (n == 1 && last > 0.0 && el > last + RCH_LATE) || el > RCH_MAX_S) {
            if (last == 0.0) {
                rch_end(RCH_NO_CANCEL, "Race over: time limit\n\nFree Roam goes on", now);
                return 1;
            }
            rch_result(me, now);
            break;
        }
        place = rch_rank(order, me);
        rch_fmt_time(t1, sizeof t1, el);
        go = el < 1.0 && !(s_rch.native & 2);   /* the HUD said GO itself */
        if (mine->fin > 0.0) {
            rch_fmt_time(t1, sizeof t1, mine->fin);
            snprintf(text, sizeof text, "Finished! %s of %d\n%s\nWaiting for %d racer%s", rch_ord(place), s_rch.nrc, t1, n,
                     n > 1 ? "s" : "");
        } else {
            if (go)
                snprintf(t2, sizeof t2, RCH_BIG "GO!");
            else
                snprintf(t2, sizeof t2, "%s of %d", rch_ord(place), s_rch.nrc);
            snprintf(text, sizeof text, "%s\n%s %d/%d \xC2\xB7 %.0f m\n%s", t2,
                     s_rch.next + 1 == s_rch.ncp ? "Finish" : "Checkpoint", s_rch.next + 1, s_rch.ncp,
                     rch_hd(s_rch.cp[s_rch.next], me), t1);
        }
        rch_prompt(text);
        return 1;
    case RCH_RESULT:
        break;
    }
    if (s_rch.st != RCH_RESULT)                 /* the host's countdown starts next frame */
        return 1;
    /* RESULT: keep telling our finish a while (late finishers learn it), then Free Roam */
    if (now - s_rch.tx >= RCH_RESEND) {
        rch_send_racers(RCH_OP_PROG, now);
        s_rch.tx = now;
    }
    if (now > s_rch.until) {
        s_rch.st = RCH_IDLE;
        s_rch.peer = 0;
        return 0;
    }
    mine = &s_rch.rc[s_rch.me];
    place = rch_rank(order, me);
    tail = s_op_flat[0] ? s_op_flat : "Free Roam goes on";   /* the Online prize line (online_progress.c) */
    rch_standings(t2, s_op_flat[0] ? 100 : 140, order);   /* the box holds RRA_TEXT bytes: room for the other lines */
    rch_fmt_time(t1, sizeof t1, mine->fin);
    big = now < s_rch.until - (RCH_RESULT_S - 2.0) ? RCH_BIG : "";   /* big the first 2 s */
    if (mine->fin == 0.0)
        snprintf(text, sizeof text, "%sDid not finish\n%s\n%s", big, t2, tail);
    else if (place == 1 && s_rch.nrc > 1 && s_rch.rc[order[1]].fin == mine->fin)
        snprintf(text, sizeof text, "%sDead heat! %s\n%s\n%s", big, t1, t2, tail);
    else if (place == 1)
        snprintf(text, sizeof text, "%sYou win! %s\n%s\n%s", big, t1, t2, tail);
    else
        snprintf(text, sizeof text, "%s%s of %d \xC2\xB7 %s\n%s\n%s", big, rch_ord(place), s_rch.nrc, t1, t2, tail);
    rch_prompt(text);
    return 1;
}

/* The course is on our maps (preview while invited, then the race). */
static int rch_shown(void)
{
    return rch_on() && !s_rch.ai && (s_rch.st == RCH_INVITING || s_rch.st == RCH_INVITED || s_rch.st == RCH_JOINED ||
                        s_rch.st == RCH_COUNT || (s_rch.st == RCH_RACE && s_rch.rc[s_rch.me].fin == 0.0));
}

/* After sub_000BF850 (map): the course's checkpoints in place of the race start icons; 1 drawn (the start
 * icons then stay off). As the game's ordered races (sub_000C2CE0): the next one and the one after, in the map
 * tune's colours ([0x507508] +0x350 next, +0x390 after, +0x370 finish). */
static int rch_map(uint32_t map, uint32_t top)
{
    static const float next[4] = { 1.0f, 0.85f, 0.1f, 1.0f }, later[4] = { 1.0f, 1.0f, 1.0f, 0.6f },
                       finish[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    uint32_t g = MEM32(RMAP_G_VA), buf, item, col, mat, tex, args[5], k;
    int mini, i;
    float s;
    const float *c;
    if (!rch_shown() || !rmap_on() || !online_ram(map))
        return 0;
    buf = (top - 0x200u) & ~0xFu;
    col = buf + 0x40u;
    item = buf + 0x80u;
    if (!(mat = rmap_car_mat(map, buf)))
        return 1;
    mini = MEM32(map) == RMAP_MINIMAP_VT;
    s = online_ram(g) ? MEMF(g + 0x130u) * RMAP_RACE_SCALE : RMAP_RACE_SCALE;
    for (k = 0; k < 0x50u; k += 4)
        MEM32(item + k) = 0;
    MEMF(item + 0x44u) = 1.0f;
    (void)mini;
    for (i = s_rch.next + 1 < s_rch.ncp ? s_rch.next + 1 : s_rch.next; i >= s_rch.next; i--) {  /* next on top */
        int last = i == s_rch.ncp - 1;
        uint32_t tune = MEM32(RCH_MAPTUNE_VA), tc = 0;
        if (!online_ram(tex = MEM32(map + (last ? 0x9Cu : 0xA8u))))
            continue;
        c = i == s_rch.next ? next : last ? finish : later;
        for (k = 0; k < 3; k++)
            MEMF(item + 0x34u + 4u * k) = s_rch.cp[i][k];
        if (online_ram(tune))
            tc = tune + (last ? 0x370u : i == s_rch.next ? 0x350u : 0x390u);
        for (k = 0; k < 4; k++)
            MEMF(col + 4u * k) = tc && isfinite(MEMF(tc + 4u * k)) ? MEMF(tc + 4u * k) : c[k];
        args[0] = item; args[1] = mat; args[2] = tex; args[3] = col;
        args[4] = rmap_f2u(i == s_rch.next ? s * 1.25f : s);
        roam_call(sub_000BDBE0, map, buf, 5, args);
    }
    return 1;
}

#ifdef __EMSCRIPTEN__
/* Read by the page (web/pre.js, "Challenge marker"): seq +1 per update, on, the marker's place on the picture
 * (u, v 0..1), edge (1: off the picture, an arrow at the edge turned by ang radians, 0 = right, clockwise), the
 * distance in m, the checkpoint (1-based) of n, finish (1: the last one). */
static struct {
    volatile int32_t seq;
    int32_t on;
    float u, v, ang;
    int32_t edge;
    float dist;
    int32_t idx, n, finish;
} s_rch_mark;
EMSCRIPTEN_KEEPALIVE uintptr_t roam_cp_box(void) { return (uintptr_t)&s_rch_mark; }

/* From roam_tags_view (the main camera): the next checkpoint's marker. */
static void rch_view(const float vw[16], const float pr[16], float k)
{
    float p[4], e[4], c[4], x, y, s, me[3];
    int i, j;
    if (!(rch_shown() && (s_rch.st == RCH_RACE || s_rch.st == RCH_COUNT) && s_rch.next < s_rch.ncp &&
          s_roam_nhist && !s_nat.on)) {         /* the game's flare and arrow show it */
        if (s_rch_mark.on) {
            s_rch_mark.on = 0;
            s_rch_mark.seq++;
        }
        return;
    }
    memcpy(me, s_roam_hist[(s_roam_nhist - 1) % ROAM_HIST].m + 9, sizeof me);
    p[0] = s_rch.cp[s_rch.next][0];
    p[1] = s_rch.cp[s_rch.next][1] + 4.0f;      /* a little above the road */
    p[2] = s_rch.cp[s_rch.next][2];
    p[3] = 1.0f;
    for (j = 0; j < 4; j++)
        for (e[j] = 0.0f, i = 0; i < 4; i++)
            e[j] += p[i] * vw[4 * i + j];
    for (j = 0; j < 4; j++)
        for (c[j] = 0.0f, i = 0; i < 4; i++)
            c[j] += e[i] * pr[4 * i + j];
    s_rch_mark.edge = 1;
    if (c[3] > 0.5f) {
        x = c[0] / c[3] / k;
        y = c[1] / c[3];
        if (fabsf(x) < 0.92f && fabsf(y) < 0.85f) {
            s_rch_mark.edge = 0;
            s_rch_mark.u = 0.5f + 0.5f * x;
            s_rch_mark.v = 0.5f - 0.5f * y;
        }
    }
    if (s_rch_mark.edge) {                      /* the way to turn: view x (right) and y (up) */
        x = e[0];
        y = e[1] * 0.3f;                        /* mostly sideways */
        if (fabsf(x) + fabsf(y) < 1e-3f)
            x = 1.0f;
        s = 1.0f / fmaxf(fabsf(x) / 0.44f, fabsf(y) / 0.40f);
        s_rch_mark.u = 0.5f + x * s;
        s_rch_mark.v = 0.5f - y * s;
        s_rch_mark.ang = atan2f(-y, x);
    }
    s_rch_mark.dist = rch_hd(s_rch.cp[s_rch.next], me);
    s_rch_mark.idx = s_rch.next + 1;
    s_rch_mark.n = s_rch.ncp;
    s_rch_mark.finish = s_rch.next + 1 == s_rch.ncp;
    s_rch_mark.on = 1;
    s_rch_mark.seq++;
}
#endif
