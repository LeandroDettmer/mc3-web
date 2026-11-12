/* roam_challenge.c -- Free Roam feature 2: a 1v1 street challenge inside the
 * cruise (docs/roam-races.md, F2).  No load, no menu: the two racers stay in
 * the Free Roam cruise, everyone else just sees two cars driving.
 *
 * Included by recomp_manual.c after roam_tags.c (uses roam_call, roam_set_pose,
 * roam_races_*, rra_prompt, rra_coll, s_rra_race/s_rra_back, the map helpers
 * rmap_car_mat/rmap_f2u, tags_proj's camera, s_roam_seen).
 *
 * Flow (both players in the Free Roam cruise, driving):
 *  - The trigger is a headlight flash: White (Q; the touch pad's Flash), the
 *    game's own headlight toggle.  Nothing shows for merely being near someone.
 *    A flashes within RCH_NEAR m of B, both slower than RCH_SLOW: A builds a
 *    course and sends ASK (race id, course) to B every RCH_RESEND s.  Too fast
 *    or no course near: a short note, only then.  White near a player (held
 *    too) is the challenge's alone, never the AI race prompt's.
 *  - B sees "<A> flashed their lights at you!" with the course on its maps;
 *    flashing back (White) accepts (B slower than RCH_SLOW, A too) within
 *    RCH_ASK_S s.  The headlights are not shared (52-byte roam state): the
 *    ASK message is the signal.  Driving more than RCH_AWAY m away, a timeout
 *    or a busy B declines (NO).
 *  - A gets ACCEPT and sends GO (milliseconds left to the start) during the
 *    RCH_COUNT_S s countdown: both cars are put on the event's own start grid
 *    (the challenger on the .rac's PlayerPosDir, the challenged on its first
 *    Opponent's PosDir, each facing the .rac's direction) and held there; the
 *    game's own 3-2-1-GO (HUD message queue + countdown sound) where the HUD
 *    has it, else in the prompt box.
 *  - The race: the game's own checkpoints (below, "Native checkpoints"): the
 *    flare on the next one, the HUD's next-checkpoint arrow, the game's pass
 *    test and checkpoint sound; the course is on both maps (the race start
 *    icons are hidden meanwhile); the prompt box shows place, checkpoint,
 *    distance and time.  Each racer sends PROG (checkpoints, its finish time)
 *    to the other every RCH_RESEND s.  Without native checkpoints (another
 *    race object, MC3_ROAM_CHALLENGE=3) the old page marker and a RCH_CP_R m
 *    test stand in.
 *  - Result from the two finish times (each on its own clock from its GO; the
 *    GO messages carry the time left, so the clocks start within one trip).
 *    One finished and the other not within RCH_LATE s of that time: decided.
 *    Shown RCH_RESULT_S s, then Free Roam goes on.  Peer gone or not roaming
 *    for RCH_GONE_S s, our garage or a race load, or RCH_MAX_S s: cancelled.
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
 * a chain of the city's race start beacons ahead of the challenger (on roads
 * too), RCH_COURSE_M m or RCH_MSG_CP checkpoints, sent in the ASK.
 *
 * Each checkpoint keeps the .rac's four sizes (after x y z: +0x10C, +0x110
 * radius, +0x108 height, +0).  The start grid is the race's own: the player
 * slot at PlayerPosDir, the opponent slot at the first "Opponent { ... PosDir
 * x y z dx dy dz }" (3.5 m to the +(up x dir) side in 241 of 270 files; one
 * farther than RCH_OPP_M from the player slot, or none: RCH_OPP_W m to that
 * side).  dir is the direction of travel (the grid's back rows lie behind
 * it; dir points at checkpoint 0 in 739 of 787 files).  The loader
 * sub_001D4F20 stores the heading h = atan2(-dx, -dz), and the racer placement
 * (sub_001CBCA0 -> sub_002C8860, rotation sub_0008C2C0: local +z goes to
 * (sin h, 0, cos h)) turns the car so that its local +z is -dir: the car's
 * matrix row +0x28 points backwards (the engine's cars face -z), and
 * rch_grid_pose writes -dir there (until 2026-11-12 it wrote dir: both cars
 * stood turned around).  A slot whose dir points away from checkpoint 0
 * (dot < 0) is turned to face it.  Start beacons only: both slots RCH_GRID_W
 * m to each side of where the challenger stood, facing the first beacon.
 *
 * Native checkpoints (both racers, each in its own world): the Free Roam
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
 * HUD's arrow asks race vt+0xE8 for the target: the cruise's is a stub
 * (sub_00129FA0), so the cruise vtable's slot points at the ordered races'
 * sub_001DF290 meanwhile (false with no checkpoints).  Countdown: the HUD
 * message queue [[player+0x58]+0x50] sub_000B6330(id: 2 "3", 1 "2", 0 "1",
 * 3 "GO") and audio event 0xC, as the race flow's phase 3 (sub_001C20E0).
 * Afterwards race+0x24 = 0 and the record back.  The game's map draws race
 * checkpoints only for race types it knows (sub_000C2CE0: none for the
 * cruise), so ours draw them in its colours (map tune [0x507508] +0x350 next,
 * +0x360 the one after, +0x370 finish).
 *
 * Messages (roam_send_msg, peer to peer, ROAM_MSG_MAX bytes): 'C', version 4,
 * op, arg, race id u32, then by op: ASK (arg = n) start line (x, y, z) f32
 * (PlayerPosDir's position), city u8, event u16, then only for event 0xFFFF
 * (start beacons) n x (x, y, z) f32; with an event the peer reads the course,
 * sizes and grid slots from its own .rac and declines (NO busy) when that
 * gives another count or start line; ACCEPT; NO (arg = reason); GO u16 ms to the start; PROG
 * (arg = checkpoints passed) finished u8, 3 pad, finish ms u32.  Anything
 * else, or another race id, is dropped (an ASK while busy is answered NO
 * busy).
 *
 * MC3_ROAM_CHALLENGE=0 turns it off, =2 native without the HUD/audio calls,
 * =3 without native checkpoints.  Log lines '[CHALLENGE]'. */

#define RCH_NEAR       30.0f            /* m: a player this near can be challenged */
#define RCH_AWAY       60.0f            /* m: before the start, farther apart than this = declined */
#define RCH_SLOW       12.0f            /* m/s: both slower to challenge or accept */
#define RCH_ASK_S      10.0             /* s a challenge waits */
#define RCH_COUNT_S    3.0              /* s of countdown */
#define RCH_CP_R       25.0f            /* m: a checkpoint is reached (horizontal) */
#define RCH_CP_DY      15.0f            /* m: and this near in height */
#define RCH_GONE_S     5.0              /* s the opponent may be silent or away */
#define RCH_MAX_S      600.0            /* s: the race's time limit (a whole course: 14-29 checkpoints seen) */
#define RCH_LATE       1.0              /* s past a finish without the other's: decided */
#define RCH_RESULT_S   8.0              /* s the result stays */
#define RCH_NOTE_S     3.5              /* s a notice stays */
#define RCH_RESEND     0.3              /* s between repeats */
#define RCH_MSG_CP     6                /* start beacons only: checkpoints an ASK carries (23 + 12 n <= ROAM_MSG_MAX) */
#define RCH_COURSE_M   2000.0f          /* start beacons only: m of course (straight lines) */
#define RCH_EVENT_M    1500.0f          /* m: an event's start this near gives its course */
#define RCH_RAC_MAX    96               /* checkpoints read from a .rac at most */
#define RCH_TOKENS     8000             /* tokens skipped at most looking for numcheckpoints */
#define RCH_GRID_W     2.5f             /* m: start beacons only: each grid slot off the start line's middle */
#define RCH_OPP_W      3.5f             /* m: the opponent slot beside PlayerPosDir when the .rac has none */
#define RCH_OPP_M      8.0f             /* m: a first Opponent farther from PlayerPosDir is not beside it */
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

static const float rch_dims_def[RCH_DIMS] = { 12.0f, 12.0f, 8.0f, 0.0f };

enum { RCH_OP_ASK = 1, RCH_OP_ACCEPT = 2, RCH_OP_NO = 3, RCH_OP_GO = 4, RCH_OP_PROG = 5 };
enum { RCH_NO_DECLINE = 1, RCH_NO_BUSY = 2, RCH_NO_CANCEL = 3 };
enum { RCH_IDLE, RCH_ASKING, RCH_ASKED, RCH_ACCEPTED, RCH_COUNT, RCH_RACE, RCH_RESULT };

static struct {
    int st;
    uint32_t race, peer;                        /* race id, the opponent's roam id */
    int host;                                   /* 1: we challenged (we send GO) */
    char name[ROAM_NAME_MAX + 1];
    float cp[RCH_RAC_MAX][3];
    float dims[RCH_RAC_MAX][RCH_DIMS];
    float grid[3];                              /* the start line (PlayerPosDir, or where the challenger stood) */
    float slot[2][3], sdir[2][2];               /* grid slots (0 challenger, 1 challenged): position, forward x/z */
    unsigned ev_city, ev_index;                 /* the event the course is from (ev_index 0xFFFF none) */
    int ncp;
    float km;
    int said;                                   /* countdown: the last number shown (4 none) */
    int native;                                 /* the course is the game's checkpoints now */
    double t0;                                  /* ASK: when sent/heard; COUNT/RACE: the start (GO) */
    double tx, heard, away;                     /* last send, last message from the peer, since when it is away (0 not) */
    float hold[12];                             /* our car's matrix during the countdown */
    int held;
    int next;                                   /* our next checkpoint */
    double fin;                                 /* our finish time, s from GO (0 not yet) */
    int pcps;                                   /* the opponent's checkpoints passed */
    double pfin;                                /* its finish time (0 not yet) */
    int win;                                    /* RESULT: 1 won, 0 lost, 2 dead heat, -1 cancelled */
    double until;                               /* RESULT ends */
    int white_was;
    char note[RRA_TEXT];
    double note_until;
    struct { uint32_t to, race; int left; uint8_t why; } no;    /* a NO still to repeat */
} s_rch;

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

/* A race's .rac as read: checkpoints (x y z + the RCH_DIMS sizes), PlayerPosDir and the first Opponent's PosDir
 * (x y z dx dy dz; has_* 0 none). */
typedef struct {
    uint16_t index; uint8_t city; int n;
    float cp[RCH_RAC_MAX][3 + RCH_DIMS];
    int has_ppd, has_opd;
    float ppd[6], opd[6];
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

/* Race r's .rac (the race loader's way) into out: checkpoints, PlayerPosDir, the first Opponent's PosDir. */
static void rch_load_rac(const roam_race *r, rch_rac *out, uint32_t esp)
{
    uint32_t base = (esp - 0x1000u) & ~0xFu, dir = base, tok = base + 0x40u, buf = base + 0x100u, vec = base + 0x180u,
             ename = base + 0x200u, s_ppd = base + 0x300u, s_pd = base + 0x320u;
    uint32_t fs = MEM32(RCH_FS_VA), tbl = MEM32(RCH_CITY_VA), cname, stream, vt, a[5], i;
    int n = 0, want, k, t, max = RCH_RAC_MAX;
    float (*cp)[3 + RCH_DIMS] = out->cp;
    char dname[64];
    const char *pre = "tune/race/", *kw_ppd = "PlayerPosDir", *kw_pd = "PosDir";
    out->n = out->has_ppd = out->has_opd = 0;
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
        for (t = 0; t < RCH_TOKENS; t++) {      /* the first PosDir after them: Opponent 0's grid slot */
            a[0] = s_pd; a[1] = 1;
            if (roam_call(sub_0020F3C0, tok, base, 2, a) & 0xFFu) {
                out->has_opd = rch_posdir(tok, vt, base, vec, out->opd);
                break;
            }
            a[0] = buf; a[1] = 0x80u;
            if (!roam_call(recomp_lookup(MEM32(vt + 4u)), tok, base, 2, a))
                break;
        }
        fprintf(stderr, "[CHALLENGE] course: %s/%s.rac: %d of %d checkpoints read; PlayerPosDir %s, opponent PosDir %s\n",
                dname, r->name, n, want, out->has_ppd ? "yes" : "no", out->has_opd ? "yes" : "no");
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

/* Both grid slots: the race's own (rac's PlayerPosDir at s_rch.grid: the player slot there, the opponent's at its
 * first Opponent PosDir, else RCH_OPP_W m to the +(up x dir) side, as the game's opponent 0 stands), else the
 * start beacons' (RCH_GRID_W m to each side of s_rch.grid facing the first checkpoint, the challenger at -). */
static void rch_slots(const rch_rac *rac)
{
    float f[2], l;
    int k;
    if (rac && rac->has_ppd && rch_hd(rac->ppd, s_rch.grid) < 1.0f && fabsf(rac->ppd[1] - s_rch.grid[1]) < 1.0f) {
        const float *o = rac->has_opd && rch_hd(rac->opd, rac->ppd) < RCH_OPP_M ? rac->opd : NULL;
        for (k = 0; k < 2; k++) {
            const float *pd = k && o ? o : rac->ppd;
            l = sqrtf(pd[3] * pd[3] + pd[5] * pd[5]);
            s_rch.sdir[k][0] = pd[3] / l;
            s_rch.sdir[k][1] = pd[5] / l;
            memcpy(s_rch.slot[k], pd, sizeof s_rch.slot[0]);
        }
        if (!o) {
            s_rch.slot[1][0] += RCH_OPP_W * s_rch.sdir[0][1];    /* up x dir = (dz, 0, -dx) */
            s_rch.slot[1][2] -= RCH_OPP_W * s_rch.sdir[0][0];
        }
        for (k = 0; k < 2; k++)                 /* guard: a slot facing away from checkpoint 0 is turned around */
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
    for (k = 0; k < 2; k++) {
        s_rch.sdir[k][0] = f[0] / l;
        s_rch.sdir[k][1] = f[1] / l;
        s_rch.slot[k][0] = s_rch.grid[0] + (k ? RCH_GRID_W : -RCH_GRID_W) * f[1] / l;
        s_rch.slot[k][1] = s_rch.grid[1];
        s_rch.slot[k][2] = s_rch.grid[2] - (k ? RCH_GRID_W : -RCH_GRID_W) * f[0] / l;
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

static void rch_send(uint32_t to, uint32_t race, int op, int arg, double now)
{
    uint8_t b[ROAM_MSG_MAX];
    int n = 8, k, i;
    float f;
    double left;
    memset(b, 0, sizeof b);
    b[0] = 'C'; b[1] = 4; b[2] = (uint8_t)op; b[3] = (uint8_t)arg;
    rch_put32(b + 4, race);
    if (op == RCH_OP_ASK) {
        b[3] = (uint8_t)s_rch.ncp;
        for (i = 0; i < 3; i++) {               /* the start line */
            memcpy(b + n, &s_rch.grid[i], 4);
            n += 4;
        }
        b[n++] = (uint8_t)s_rch.ev_city;
        b[n++] = (uint8_t)s_rch.ev_index;
        b[n++] = (uint8_t)(s_rch.ev_index >> 8);
        for (k = 0; s_rch.ev_index == 0xFFFFu && k < s_rch.ncp && k < RCH_MSG_CP; k++)  /* beacons: the checkpoints */
            for (i = 0; i < 3; i++) {
                f = s_rch.cp[k][i];
                memcpy(b + n, &f, 4);
                n += 4;
            }
    } else if (op == RCH_OP_GO) {
        left = (s_rch.t0 - now) * 1000.0;
        k = left < 0.0 ? 0 : left > 60000.0 ? 60000 : (int)left;
        b[8] = (uint8_t)k; b[9] = (uint8_t)(k >> 8);
        n = 10;
    } else if (op == RCH_OP_PROG) {
        b[3] = (uint8_t)s_rch.next;
        b[8] = s_rch.fin > 0.0;
        rch_put32(b + 12, (uint32_t)(s_rch.fin * 1000.0 + 0.5));
        n = 16;
    }
    roam_send_msg(to, b, n);
}

static void rch_note(const char *text, double now)
{
    snprintf(s_rch.note, sizeof s_rch.note, "%s", text);
    s_rch.note_until = now + RCH_NOTE_S;
}

/* Out of the race (state back to idle), the peer told `why` (0: nothing to tell). */
static void rch_end(int why, const char *text, double now)
{
    if (why && s_rch.peer) {
        s_rch.no.to = s_rch.peer;
        s_rch.no.race = s_rch.race;
        s_rch.no.why = (uint8_t)why;
        s_rch.no.left = 3;
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

static void rch_result(double now)
{
    char a[16], b[16], t[RRA_TEXT];
    rch_nat_off();
    s_rch.st = RCH_RESULT;
    s_rch.until = now + RCH_RESULT_S;
    if (s_rch.fin > 0.0 && s_rch.pfin > 0.0)
        s_rch.win = s_rch.fin < s_rch.pfin ? 1 : s_rch.fin > s_rch.pfin ? 0 : 2;
    else
        s_rch.win = s_rch.fin > 0.0;
    rch_fmt_time(a, sizeof a, s_rch.fin);
    rch_fmt_time(b, sizeof b, s_rch.pfin);
    snprintf(t, sizeof t, "result vs %s: %s (we %s, they %s, their checkpoints %d of %d)", s_rch.name,
             s_rch.win == 1 ? "won" : s_rch.win == 2 ? "dead heat" : "lost", s_rch.fin > 0.0 ? a : "-",
             s_rch.pfin > 0.0 ? b : "-", s_rch.pcps, s_rch.ncp);
    fprintf(stderr, "[CHALLENGE] %s\n", t);
}

static void rch_recv(const float me[3], double now, uint32_t esp)
{
    uint8_t b[ROAM_MSG_MAX];
    uint32_t from, race;
    const roam_peer *pp;
    int n, op, k, i, ok;
    while ((n = roam_recv_msg(&from, b, sizeof b)) > 0) {
        if (n < 8 || b[0] != 'C' || b[1] != 4)  /* v4: the whole course from the event's .rac; v3 builds are dropped */
            continue;
        op = b[2];
        race = rch_get32(b + 4);
        if (op == RCH_OP_ASK) {
            if (s_rch.st != RCH_IDLE && s_rch.peer == from && s_rch.race == race) {
                s_rch.heard = now;
                continue;
            }
            /* both challenged each other: the lower race id goes on */
            if (s_rch.st == RCH_ASKING && s_rch.peer == from && race < s_rch.race)
                s_rch.st = RCH_IDLE;
            if (s_rch.st != RCH_IDLE) {
                if (!(s_rch.st == RCH_ASKING && s_rch.peer == from))
                    rch_send(from, race, RCH_OP_NO, RCH_NO_BUSY, now);
                continue;
            }
            if (b[3] < 2 || n < 23 || !(pp = rch_peer(from)) || rch_hd(pp->p, me) > RCH_AWAY) {
                rch_send(from, race, RCH_OP_NO, RCH_NO_BUSY, now);
                continue;
            }
            /* start line (8), city (20), event (21); start beacons only (event 0xFFFF): the checkpoints (23) */
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
                /* our own read of the event's .rac, as the challenger took it (both sides read the same disc) */
                rac = rch_rac_of(s_rch.ev_city, s_rch.ev_index, esp);
                ok = rac && rac->has_ppd && rch_take(rac) == b[3] && rch_hd(g, s_rch.grid) < 1.0f &&
                     fabsf(g[1] - s_rch.grid[1]) < 1.0f;
                if (!ok) {
                    fprintf(stderr, "[CHALLENGE] ASK from %08X: event %u/%u: our .rac gives another course (%d of %d "
                            "checkpoints), declined\n", (unsigned)from, s_rch.ev_city, s_rch.ev_index,
                            rac ? rac->n : -1, b[3]);
                    rch_send(from, race, RCH_OP_NO, RCH_NO_BUSY, now);
                    continue;
                }
                rch_slots(rac);
            } else if (ok) {
                if (b[3] > RCH_MSG_CP || n < 23 + 12 * b[3]) {
                    rch_send(from, race, RCH_OP_NO, RCH_NO_BUSY, now);
                    continue;
                }
                for (k = 0; k < b[3]; k++) {
                    for (i = 0; i < 3; i++) {
                        memcpy(&s_rch.cp[k][i], b + 23 + 12 * k + 4 * i, 4);
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
            s_rch.st = RCH_ASKED;
            s_rch.race = race;
            s_rch.peer = from;
            s_rch.host = 0;
            snprintf(s_rch.name, sizeof s_rch.name, "%s", pp->name);
            s_rch.t0 = s_rch.heard = now;
            s_rch.away = 0;
            fprintf(stderr, "[CHALLENGE] %s challenges us (race %08X, %d checkpoints, %.1f km)\n", s_rch.name,
                    (unsigned)race, s_rch.ncp, s_rch.km);
            continue;
        }
        if (s_rch.st == RCH_IDLE || from != s_rch.peer || race != s_rch.race)
            continue;
        s_rch.heard = now;
        switch (op) {
        case RCH_OP_ACCEPT:
            if (s_rch.st == RCH_ASKING) {
                s_rch.st = RCH_COUNT;
                s_rch.t0 = now + RCH_COUNT_S;
                s_rch.held = 0;
                s_rch.tx = 0;
                fprintf(stderr, "[CHALLENGE] %s accepted: countdown\n", s_rch.name);
            }
            break;
        case RCH_OP_GO:
            if (s_rch.st == RCH_ACCEPTED && n >= 10) {
                s_rch.st = RCH_COUNT;
                s_rch.t0 = now + (b[8] | b[9] << 8) / 1000.0;
                s_rch.held = 0;
                fprintf(stderr, "[CHALLENGE] GO from %s: start in %d ms\n", s_rch.name, b[8] | b[9] << 8);
            }
            break;
        case RCH_OP_NO: {
            char t[RRA_TEXT];
            snprintf(t, sizeof t, "%s %s\n\nFree Roam goes on", s_rch.name,
                     b[3] == RCH_NO_BUSY ? "can't race now" : b[3] == RCH_NO_DECLINE ? "declined" : "left the race");
            rch_end(0, t, now);
            break; }
        case RCH_OP_PROG:
            if (n >= 16 && (s_rch.st == RCH_COUNT || s_rch.st == RCH_RACE || s_rch.st == RCH_RESULT)) {
                s_rch.pcps = b[3] > s_rch.ncp ? s_rch.ncp : b[3];
                if (b[8] && !s_rch.pfin) {
                    s_rch.pfin = rch_get32(b + 12) / 1000.0;
                    if (s_rch.pfin <= 0.0)
                        s_rch.pfin = 0.001;
                    fprintf(stderr, "[CHALLENGE] %s finished in %.2f s\n", s_rch.name, s_rch.pfin);
                    if (s_rch.st == RCH_RESULT)
                        rch_result(now);        /* both times known now: decided again */
                }
            }
            break;
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

/* Our grid slot (rch_slots: challenger slot 0, challenged slot 1) facing its dir: the car's local +z (row +0x28)
 * is its back, so that row is -dir, as the game's racer placement turns it (sub_0008C2C0 with h = atan2(-dx, -dz));
 * the car's own matrix turned there (its handedness kept). */
static void rch_grid_pose(uint32_t mtx, float m[12])
{
    const float *f = s_rch.sdir[s_rch.host ? 0 : 1], *at = s_rch.slot[s_rch.host ? 0 : 1];
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

/* ---- the frame ---- */

/* Each player's update (sub_002C1520) for slot 0, before rra_player; 1: the challenge owns the prompt and White
 * (rra_player then stays out). */
static int rch_player(uint32_t p)
{
    uint32_t setup = MEM32(ROAM_SETUP_VA), veh, mtx, coll, top = (g_esp - 0x400u) & ~0xFu;
    XBOX_INPUT_STATE st;
    const roam_peer *pp = NULL, *near = NULL;
    float me[3], fwd[3], v[3] = { 0 }, speed, d, nd = RCH_NEAR;
    double now = roam_now(), el;
    int white, press, i, k, go, place;
    char text[RRA_TEXT] = "", t1[16], t2[16];
    const char *big;
    if (!rch_on() || !online_ram(p) || MEM32(p + 0x5Cu) != 0)
        return 0;
    if (!s_online || !roam_running() || !online_ram(setup) || MEM32(setup + 0x18u) != 0 || s_rra_race || s_rra_back ||
        MEM8(setup + 0x108u) || MEM32(0x0050F14Cu) != 0 || !online_ram(veh = MEM32(p + 0x14u)) ||
        !online_ram(mtx = MEM32(veh + 0x1Cu))) {
        if (s_rch.st != RCH_IDLE)               /* a race load, the garage, the menus: we are out */
            rch_end(RCH_NO_CANCEL, "cancelled: we left the cruise", now);
        return 0;
    }
    for (i = 0; i < 3; i++) {
        me[i] = MEMF(mtx + 0x34u + 4u * i);
        fwd[i] = -MEMF(mtx + 0x28u + 4u * i);  /* row +0x28 is the car's back (rch_grid_pose) */
    }
    if ((coll = rra_coll(veh)) != 0)
        for (i = 0; i < 3; i++)
            v[i] = MEMF(coll + 0x44u + 4u * i);
    speed = rch_speed(v);
    white = xbox_InputGetState(0, &st) == 0 && st.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] > XBOX_ANALOG_BUTTON_THRESHOLD;
    press = white && !s_rch.white_was;
    s_rch.white_was = white;
    rch_recv(me, now, g_esp);
    if (s_rch.no.left > 0 && now - s_rch.tx >= RCH_RESEND / 3) {    /* a NO three times */
        rch_send(s_rch.no.to, s_rch.no.race, RCH_OP_NO, s_rch.no.why, now);
        s_rch.no.left--;
        s_rch.tx = now;
    }
    if (s_rch.st != RCH_IDLE && s_rch.st != RCH_RESULT) {
        pp = rch_peer(s_rch.peer);
        if (pp && pp->status == RC_ST_ROAM)
            s_rch.away = 0;
        else if (!s_rch.away)
            s_rch.away = now;
        if ((s_rch.away && now - s_rch.away > RCH_GONE_S) ||
            (s_rch.st >= RCH_COUNT && now - s_rch.heard > RCH_GONE_S)) {
            if (s_rch.st == RCH_RACE && s_rch.fin > 0.0)
                rch_result(now);                /* we finished; they never did */
            else {
                snprintf(text, sizeof text, "%s left the race\n\nFree Roam goes on", s_rch.name);
                rch_end(RCH_NO_CANCEL, text, now);
            }
        }
    }
    switch (s_rch.st) {
    case RCH_IDLE:
        if (now < s_rch.note_until) {
            rra_prompt(s_rch.note);
            return 1;
        }
        if (!remix_driving() || !s_roam_seen || now - s_roam_seen_t > 0.5)
            return 0;
        for (i = 0; i < s_roam_nseen; i++)
            if (s_roam_seen[i].status == RC_ST_ROAM && (d = rch_hd(s_roam_seen[i].p, me)) < nd &&
                fabsf(s_roam_seen[i].p[1] - me[1]) < 10.0f) {
                nd = d;
                near = &s_roam_seen[i];
            }
        if (!near || !white)
            return 0;                           /* no flash near anyone: the AI race prompt may show */
        if (!press) {                           /* White still held after a flash near someone: not the AI race's */
            rra_prompt("");
            return 1;
        }
        if (speed > RCH_SLOW || rch_speed(near->v) > RCH_SLOW) {
            snprintf(text, sizeof text, "Street race\nToo fast to challenge %s\nBoth slow down, then flash again",
                     near->name);
            rch_note(text, now);
            rra_prompt(s_rch.note);
            return 1;
        }
        if (!rch_course(MEM32(setup), me, fwd, g_esp)) {
            rch_note("Street race\nNo race course near here\nTry closer to a race start", now);
            rra_prompt(s_rch.note);
            return 1;
        }
        s_rch.st = RCH_ASKING;
        s_rch.peer = near->id;
        s_rch.host = 1;
        s_rch.race = (uint32_t)rand() ^ (uint32_t)(now * 1e6) ^ near->id;
        if (!s_rch.race)
            s_rch.race = 1;
        snprintf(s_rch.name, sizeof s_rch.name, "%s", near->name);
        s_rch.t0 = s_rch.heard = now;
        s_rch.tx = 0;
        s_rch.away = 0;
        fprintf(stderr, "[CHALLENGE] we flash %s: challenge (race %08X, %d checkpoints, %.1f km)\n", s_rch.name,
                (unsigned)s_rch.race, s_rch.ncp, s_rch.km);
        return 1;
    case RCH_ASKING:
    case RCH_ASKED:
    case RCH_ACCEPTED:
        if (now - s_rch.t0 > RCH_ASK_S + (s_rch.st == RCH_ACCEPTED ? 3.0 : 0.0)) {
            snprintf(text, sizeof text, s_rch.st == RCH_ASKING ? "No answer from %s\n\nFree Roam goes on"
                                                              : "Challenge from %s expired\n\nFree Roam goes on",
                     s_rch.name);
            rch_end(RCH_NO_DECLINE, text, now);
            return 1;
        }
        if (pp && rch_hd(pp->p, me) > RCH_AWAY) {
            snprintf(text, sizeof text, "Challenge %s\nToo far apart\nFree Roam goes on",
                     s_rch.st == RCH_ASKING ? "cancelled" : "declined");
            rch_end(RCH_NO_DECLINE, text, now);
            return 1;
        }
        if (s_rch.st == RCH_ASKING && now - s_rch.tx >= RCH_RESEND) {
            rch_send(s_rch.peer, s_rch.race, RCH_OP_ASK, 0, now);
            s_rch.tx = now;
        }
        if (s_rch.st == RCH_ACCEPTED && now - s_rch.tx >= RCH_RESEND) {
            rch_send(s_rch.peer, s_rch.race, RCH_OP_ACCEPT, 0, now);
            s_rch.tx = now;
        }
        k = (int)(RCH_ASK_S - (now - s_rch.t0) + 0.99);
        if (s_rch.st == RCH_ASKING)
            snprintf(text, sizeof text, "You flashed %s\n%d checkpoints \xC2\xB7 %.1f km\nWaiting for a flash back \xC2\xB7 %d s",
                     s_rch.name, s_rch.ncp, s_rch.km, k > 0 ? k : 0);
        else if (s_rch.st == RCH_ACCEPTED)
            snprintf(text, sizeof text, "Race vs %s\n%d checkpoints \xC2\xB7 %.1f km\nGet ready", s_rch.name, s_rch.ncp,
                     s_rch.km);
        else if (speed > RCH_SLOW || (pp && rch_speed(pp->v) > RCH_SLOW))
            snprintf(text, sizeof text, "%s flashed their lights at you!\n%d checkpoints \xC2\xB7 %.1f km\nBoth slow down, then flash back \xC2\xB7 %d s",
                     s_rch.name, s_rch.ncp, s_rch.km, k > 0 ? k : 0);
        else {
            snprintf(text, sizeof text, "%s flashed their lights at you!\n%d checkpoints \xC2\xB7 %.1f km\nFlash back (White [Q]) to accept \xC2\xB7 %d s",
                     s_rch.name, s_rch.ncp, s_rch.km, k > 0 ? k : 0);
            if (press) {
                s_rch.st = RCH_ACCEPTED;
                s_rch.tx = 0;
                fprintf(stderr, "[CHALLENGE] we accept %s's challenge\n", s_rch.name);
            }
        }
        rra_prompt(text);
        return 1;
    case RCH_COUNT:
        if (!s_rch.held) {
            rch_grid_pose(mtx, s_rch.hold);     /* our slot on the start line */
            s_rch.held = 1;
            s_rch.next = 0;
            s_rch.fin = s_rch.pfin = 0;
            s_rch.pcps = 0;
            s_rch.said = 4;
            s_rch.native = rch_nat_on(top);
            rch_audio(0xC, top);                /* the countdown sound */
            fprintf(stderr, "[CHALLENGE] grid: %s: we are the %s; challenger (%.1f, %.1f, %.1f) heading %.0f deg, "
                    "challenged (%.1f, %.1f, %.1f) heading %.0f deg (the game's atan2(-dx, -dz)); %.0f m from where "
                    "we stood; %s checkpoints\n", rch_ev_name(), s_rch.host ? "challenger" : "challenged",
                    s_rch.slot[0][0], s_rch.slot[0][1], s_rch.slot[0][2],
                    atan2f(-s_rch.sdir[0][0], -s_rch.sdir[0][1]) * 57.29578f, s_rch.slot[1][0], s_rch.slot[1][1],
                    s_rch.slot[1][2], atan2f(-s_rch.sdir[1][0], -s_rch.sdir[1][1]) * 57.29578f,
                    rch_hd(&s_rch.hold[9], me), s_rch.native ? "the game's" : "page");
        }
        if (s_rch.host && now - s_rch.tx >= RCH_RESEND) {
            rch_send(s_rch.peer, s_rch.race, RCH_OP_GO, 0, now);
            s_rch.tx = now;
        }
        if (now < s_rch.t0) {
            float zero[3] = { 0 };
            int left = (int)(s_rch.t0 - now) + 1, hud = 0;
            roam_set_pose(veh, s_rch.hold, zero);   /* both wait on the grid */
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
                snprintf(text, sizeof text, "Race vs %s\n%d checkpoints \xC2\xB7 %.1f km\nGet ready", s_rch.name,
                         s_rch.ncp, s_rch.km);
            else
                snprintf(text, sizeof text, RCH_BIG "%d\nRace vs %s \xC2\xB7 %d checkpoints\nGet ready", left, s_rch.name,
                         s_rch.ncp);
            rra_prompt(text);
            return 1;
        }
        s_rch.st = RCH_RACE;
        s_rch.tx = 0;
        rch_hud(p, 3, top);                     /* GO */
        fprintf(stderr, "[CHALLENGE] GO: race vs %s (HUD countdown %s)\n", s_rch.name,
                s_rch.native & 2 ? "the game's" : "not there: prompt");
        /* fall through */
    case RCH_RACE:
        el = now - s_rch.t0;
        if (s_rch.fin == 0.0 && s_rch.next < s_rch.ncp &&
            (s_nat.on ? rch_nat_passed(mtx, top)
                      : rch_hd(s_rch.cp[s_rch.next], me) < RCH_CP_R && fabsf(s_rch.cp[s_rch.next][1] - me[1]) < RCH_CP_DY)) {
            rch_nat_pass(s_rch.next, el, top);
            s_rch.next++;
            s_rch.tx = 0;                       /* tell now */
            fprintf(stderr, "[CHALLENGE] checkpoint %d of %d at %.2f s\n", s_rch.next, s_rch.ncp, el);
            if (s_rch.next == s_rch.ncp) {
                s_rch.fin = el > 0.001 ? el : 0.001;
                rch_nat_off();                  /* the finish: flares and arrow gone */
                fprintf(stderr, "[CHALLENGE] we finished in %.2f s\n", s_rch.fin);
            }
        }
        if (now - s_rch.tx >= RCH_RESEND) {
            rch_send(s_rch.peer, s_rch.race, RCH_OP_PROG, 0, now);
            s_rch.tx = now;
        }
        if ((s_rch.fin > 0.0 && (s_rch.pfin > 0.0 || el > s_rch.fin + RCH_LATE)) ||
            (s_rch.pfin > 0.0 && s_rch.fin == 0.0 && el > s_rch.pfin)) {
            rch_result(now);
            break;
        }
        if (el > RCH_MAX_S) {
            rch_end(RCH_NO_CANCEL, "Race over: time limit\n\nFree Roam goes on", now);
            return 1;
        }
        /* place: checkpoints, then distance to the next */
        place = 1;
        if (s_rch.fin == 0.0 && pp && s_rch.pcps >= s_rch.next) {
            if (s_rch.pcps > s_rch.next || s_rch.pfin > 0.0)
                place = 2;
            else if (s_rch.next < s_rch.ncp && rch_hd(s_rch.cp[s_rch.next], pp->p) < rch_hd(s_rch.cp[s_rch.next], me))
                place = 2;
        }
        rch_fmt_time(t1, sizeof t1, el);
        go = el < 1.0 && !(s_rch.native & 2);   /* the HUD said GO itself */
        if (s_rch.fin > 0.0) {
            rch_fmt_time(t2, sizeof t2, s_rch.fin);
            snprintf(text, sizeof text, "Finished!\n%s\nWaiting for %s", t2, s_rch.name);
        } else if (s_rch.next < s_rch.ncp)
            snprintf(text, sizeof text, "%s%s\n%s %d/%d \xC2\xB7 %.0f m\n%s", go ? RCH_BIG : "",
                     go ? "GO!" : place == 1 ? "1st" : "2nd",
                     s_rch.next + 1 == s_rch.ncp ? "Finish" : "Checkpoint", s_rch.next + 1, s_rch.ncp,
                     rch_hd(s_rch.cp[s_rch.next], me), t1);
        rra_prompt(text);
        return 1;
    case RCH_RESULT:
        break;
    }
    /* RESULT: keep telling our finish a while, then Free Roam */
    if (now - s_rch.tx >= RCH_RESEND) {
        rch_send(s_rch.peer, s_rch.race, RCH_OP_PROG, 0, now);
        s_rch.tx = now;
    }
    if (now > s_rch.until) {
        s_rch.st = RCH_IDLE;
        s_rch.peer = 0;
        return 0;
    }
    rch_fmt_time(t1, sizeof t1, s_rch.fin);
    rch_fmt_time(t2, sizeof t2, s_rch.pfin);
    big = now < s_rch.until - (RCH_RESULT_S - 2.0) ? RCH_BIG : "";   /* big the first 2 s */
    if (s_rch.win == 2)
        snprintf(text, sizeof text, "%sDead heat!\nYou and %s: %s\nFree Roam goes on", big, s_rch.name, t1);
    else if (s_rch.win == 1)
        snprintf(text, sizeof text, "%sYou win!\nYou %s \xC2\xB7 %s %s\nFree Roam goes on", big, t1, s_rch.name,
                 s_rch.pfin > 0.0 ? t2 : "did not finish");
    else if (s_rch.fin > 0.0)
        snprintf(text, sizeof text, "%s%s wins\n%s %s \xC2\xB7 you %s\nFree Roam goes on", big, s_rch.name, s_rch.name,
                 t2, t1);
    else
        snprintf(text, sizeof text, "%s%s wins\n%s %s \xC2\xB7 you %d/%d checkpoints\nFree Roam goes on", big,
                 s_rch.name, s_rch.name, t2, s_rch.next, s_rch.ncp);
    rra_prompt(text);
    return 1;
}

/* The course is on our maps (preview while asked, then the race). */
static int rch_shown(void)
{
    return rch_on() && (s_rch.st == RCH_ASKING || s_rch.st == RCH_ASKED || s_rch.st == RCH_ACCEPTED ||
                        s_rch.st == RCH_COUNT || (s_rch.st == RCH_RACE && s_rch.fin == 0.0));
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
