/* roam_challenge.c -- Free Roam feature 2: a 1v1 street challenge inside the
 * cruise (docs/roam-races.md, F2).  No load, no menu: the two racers stay in
 * the Free Roam cruise, everyone else just sees two cars driving.
 *
 * Included by recomp_manual.c after roam_tags.c (uses roam_call, roam_set_pose,
 * roam_races_*, rra_prompt, rra_coll, s_rra_race/s_rra_back, the map helpers
 * rmap_car_mat/rmap_f2u, tags_proj's camera, s_roam_seen).
 *
 * Flow (both players in the Free Roam cruise, driving):
 *  - A is within RCH_NEAR m of B and slower than RCH_SLOW: the prompt box says
 *    "Challenge <B>"; White (Q) builds a course and sends ASK (race id, course)
 *    to B every RCH_RESEND s.
 *  - B sees "<A> challenges you" with the course on its maps; White accepts
 *    (B slower than RCH_SLOW, A too) within RCH_ASK_S s.  Driving more than
 *    RCH_AWAY m away, a timeout or a busy B declines (NO).
 *  - A gets ACCEPT and sends GO (milliseconds left to the start) during the
 *    RCH_COUNT_S s countdown: both cars are held where they stand, 3-2-1-GO in
 *    the prompt box.
 *  - The race: the next checkpoint gets a page marker (or an edge arrow when
 *    off the picture), the course is on both maps (the race start icons are
 *    hidden meanwhile), the prompt box shows place, checkpoint, distance and
 *    time.  A checkpoint counts within RCH_CP_R m.  Each racer sends PROG
 *    (checkpoints, its finish time) to the other every RCH_RESEND s.
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
 * sub_001CDC80).  The course starts at the event's checkpoint nearest to the
 * players when it is nearer than the first, and runs until RCH_COURSE_M m
 * (straight lines) or RCH_CP_MAX checkpoints.  Only if no file reads: a chain
 * of the city's race start beacons ahead of the challenger (on roads too).
 *
 * Messages (roam_send_msg, peer to peer, ROAM_MSG_MAX bytes): 'C', version 1,
 * op, arg, race id u32, then by op: ASK n x (x, y, z) f32 (arg = n); ACCEPT;
 * NO (arg = reason); GO u16 ms to the start; PROG (arg = checkpoints passed)
 * finished u8, 3 pad, finish ms u32.  Anything else, or another race id, is
 * dropped (an ASK while busy is answered NO busy).
 *
 * MC3_ROAM_CHALLENGE=0 turns it off.  Log lines '[CHALLENGE]'. */

#define RCH_NEAR       30.0f            /* m: a player this near can be challenged */
#define RCH_AWAY       60.0f            /* m: before the start, farther apart than this = declined */
#define RCH_SLOW       12.0f            /* m/s: both slower to challenge or accept */
#define RCH_ASK_S      10.0             /* s a challenge waits */
#define RCH_COUNT_S    3.0              /* s of countdown */
#define RCH_CP_R       25.0f            /* m: a checkpoint is reached (horizontal) */
#define RCH_CP_DY      15.0f            /* m: and this near in height */
#define RCH_GONE_S     5.0              /* s the opponent may be silent or away */
#define RCH_MAX_S      300.0            /* s: the race's time limit */
#define RCH_LATE       1.0              /* s past a finish without the other's: decided */
#define RCH_RESULT_S   8.0              /* s the result stays */
#define RCH_NOTE_S     3.5              /* s a notice stays */
#define RCH_RESEND     0.3              /* s between repeats */
#define RCH_CP_MAX     6                /* checkpoints a course has at most (ASK: 8 + 12 n bytes) */
#define RCH_COURSE_M   2000.0f          /* m of course (straight lines) */
#define RCH_EVENT_M    1500.0f          /* m: an event's start this near gives its course */
#define RCH_RAC_MAX    96               /* checkpoints read from a .rac at most */
#define RCH_TOKENS     8000             /* tokens skipped at most looking for numcheckpoints */

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

enum { RCH_OP_ASK = 1, RCH_OP_ACCEPT = 2, RCH_OP_NO = 3, RCH_OP_GO = 4, RCH_OP_PROG = 5 };
enum { RCH_NO_DECLINE = 1, RCH_NO_BUSY = 2, RCH_NO_CANCEL = 3 };
enum { RCH_IDLE, RCH_ASKING, RCH_ASKED, RCH_ACCEPTED, RCH_COUNT, RCH_RACE, RCH_RESULT };

static struct {
    int st;
    uint32_t race, peer;                        /* race id, the opponent's roam id */
    int host;                                   /* 1: we challenged (we send GO) */
    char name[ROAM_NAME_MAX + 1];
    float cp[RCH_CP_MAX][3];
    int ncp;
    float km;
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

static int rch_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("MC3_ROAM_CHALLENGE");
        on = !(v && v[0] == '0');
    }
    return on;
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

/* The checkpoints of race r from its .rac (the race loader's way); their count. */
static int rch_load_rac(const roam_race *r, float (*cp)[3], int max, uint32_t esp)
{
    uint32_t base = (esp - 0x1000u) & ~0xFu, dir = base, tok = base + 0x40u, buf = base + 0x100u, vec = base + 0x180u,
             ename = base + 0x200u;
    uint32_t fs = MEM32(RCH_FS_VA), tbl = MEM32(RCH_CITY_VA), cname, stream, vt, a[5], i;
    int n = 0, want, k, t;
    char dname[64];
    const char *pre = "tune/race/";
    if (!online_ram(fs) || !online_ram(MEM32(fs)) || !online_ram(tbl) || !online_ram(cname = MEM32(tbl + r->city * 0x50u)))
        return 0;
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
        return 0;
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
            if (n < max && isfinite(MEMF(vec)) && isfinite(MEMF(vec + 8u)) && fabsf(MEMF(vec)) < 20000.0f &&
                fabsf(MEMF(vec + 4u)) < 20000.0f && fabsf(MEMF(vec + 8u)) < 20000.0f) {
                cp[n][0] = MEMF(vec);
                cp[n][1] = MEMF(vec + 4u);
                cp[n][2] = MEMF(vec + 8u);
                n++;
            }
            for (i = 0; i < 4; i++) {           /* four floats we do not need */
                a[0] = buf; a[1] = 0x80u;
                roam_call(recomp_lookup(MEM32(vt + 4u)), tok, base, 2, a);
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
        fprintf(stderr, "[CHALLENGE] course: %s/%s.rac: %d of %d checkpoints read\n", dname, r->name, n, want);
    } else
        fprintf(stderr, "[CHALLENGE] course: %s/%s.rac has no numcheckpoints\n", dname, r->name);
    roam_call(sub_001A2630, stream, base, 0, NULL);
    return n;
}

/* s_rch.cp from the checkpoints all[0..n) (a loop when circuit) seen from me, from `start`; its count. */
static int rch_take(const float (*all)[3], int n, int circuit, const float me[3], int start)
{
    int i, k;
    float sum = 0.0f;
    const float *prev = me;
    s_rch.ncp = 0;
    for (k = 0; s_rch.ncp < RCH_CP_MAX && sum < RCH_COURSE_M; k++) {
        i = start + k;
        if (i >= n) {
            if (!circuit || k >= n)
                break;
            i -= n;
        }
        sum += rch_hd(all[i], prev);
        prev = all[i];
        memcpy(s_rch.cp[s_rch.ncp++], all[i], sizeof all[i]);
    }
    s_rch.km = sum / 1000.0f;
    return s_rch.ncp;
}

/* The course joins the event where it passes nearest to us (the next checkpoint when we stand at it); a sprint
 * with fewer than 2 checkpoints left from there runs from its first. */
static int rch_trim(const float (*all)[3], int n, int circuit, const float me[3])
{
    int start = 0, i;
    float d, best = 1e30f;
    for (i = 0; i < n; i++)
        if ((d = rch_hd(all[i], me)) < best) {
            best = d;
            start = i;
        }
    if (best < 80.0f)
        start++;
    if (rch_take(all, n, circuit, me, circuit ? start % n : start) >= 2)
        return s_rch.ncp;
    return rch_take(all, n, circuit, me, 0);
}

/* Only if no .rac reads: the city's race start beacons chained ahead (fwd) of us, 250-700 m a leg. */
static int rch_beacons(unsigned city, const float me[3], const float fwd[3])
{
    const float *prev = me;
    float dir[2] = { fwd[0], fwd[2] }, l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]), sum = 0.0f;
    int used[RCH_CP_MAX], i, k, best;
    if (l < 1e-3f)
        dir[0] = 1.0f, dir[1] = 0.0f, l = 1.0f;
    dir[0] /= l;
    dir[1] /= l;
    s_rch.ncp = 0;
    while (s_rch.ncp < RCH_CP_MAX && sum < RCH_COURSE_M) {
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
        dir[0] = (s_rch.cp[s_rch.ncp][0] - prev[0]) / rch_hd(s_rch.cp[s_rch.ncp], prev);
        dir[1] = (s_rch.cp[s_rch.ncp][2] - prev[2]) / rch_hd(s_rch.cp[s_rch.ncp], prev);
        prev = s_rch.cp[s_rch.ncp++];
    }
    s_rch.km = sum / 1000.0f;
    return s_rch.ncp;
}

/* The course for a race from where we stand; its checkpoint count (2 at least), 0 none. */
static int rch_course(unsigned city, const float me[3], const float fwd[3], uint32_t esp)
{
    static struct { uint16_t index; uint8_t city; int n; float cp[RCH_RAC_MAX][3]; } cache[4];
    static int ncache;
    const roam_race *cand[3] = { 0 }, *r;
    float cd[3] = { 0 }, d;
    int nc = 0, i, j, n, c;
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
        for (c = 0; c < ncache && !(cache[c].index == r->index && cache[c].city == r->city); c++)
            ;
        if (c == ncache) {
            c = ncache < 4 ? ncache++ : (int)(roam_now() * 7.0) % 4;
            cache[c].index = r->index;
            cache[c].city = r->city;
            cache[c].n = rch_load_rac(r, cache[c].cp, RCH_RAC_MAX, esp);
        }
        if (cache[c].n >= 2 && (n = rch_trim((const float (*)[3])cache[c].cp, cache[c].n, r->kind == RR_CIRCUIT, me)) >= 2) {
            fprintf(stderr, "[CHALLENGE] course: %s (%s, start %.0f m away): %d checkpoints, %.1f km\n", r->name,
                    roam_races_kind_name(r->kind), cd[i], n, s_rch.km);
            return n;
        }
    }
    if ((n = rch_beacons(city, me, fwd)) >= 2) {
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
    b[0] = 'C'; b[1] = 1; b[2] = (uint8_t)op; b[3] = (uint8_t)arg;
    rch_put32(b + 4, race);
    if (op == RCH_OP_ASK) {
        b[3] = (uint8_t)s_rch.ncp;
        for (k = 0; k < s_rch.ncp; k++)
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

static void rch_recv(const float me[3], double now)
{
    uint8_t b[ROAM_MSG_MAX];
    uint32_t from, race;
    const roam_peer *pp;
    int n, op, k, i, ok;
    while ((n = roam_recv_msg(&from, b, sizeof b)) > 0) {
        if (n < 8 || b[0] != 'C' || b[1] != 1)
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
            if (b[3] < 2 || b[3] > RCH_CP_MAX || n < 8 + 12 * b[3] || !(pp = rch_peer(from)) ||
                rch_hd(pp->p, me) > RCH_AWAY) {
                rch_send(from, race, RCH_OP_NO, RCH_NO_BUSY, now);
                continue;
            }
            ok = 1;
            for (k = 0; k < b[3]; k++)
                for (i = 0; i < 3; i++) {
                    float f;
                    memcpy(&f, b + 8 + 12 * k + 4 * i, 4);
                    ok &= isfinite(f) && fabsf(f) < 20000.0f;
                    s_rch.cp[k][i] = f;
                }
            if (!ok)
                continue;
            s_rch.ncp = b[3];
            s_rch.km = rch_hd(s_rch.cp[0], me);
            for (k = 1; k < s_rch.ncp; k++)
                s_rch.km += rch_hd(s_rch.cp[k], s_rch.cp[k - 1]);
            s_rch.km /= 1000.0f;
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

/* ---- the frame ---- */

/* Each player's update (sub_002C1520) for slot 0, before rra_player; 1: the challenge owns the prompt and White
 * (rra_player then stays out). */
static int rch_player(uint32_t p)
{
    uint32_t setup = MEM32(ROAM_SETUP_VA), veh, mtx, coll;
    XBOX_INPUT_STATE st;
    const roam_peer *pp = NULL, *near = NULL;
    float me[3], fwd[3], v[3] = { 0 }, speed, d, nd = RCH_NEAR;
    double now = roam_now(), el;
    int white, press, i, k, go, place;
    char text[RRA_TEXT] = "", t1[16], t2[16];
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
        fwd[i] = MEMF(mtx + 0x28u + 4u * i);
    }
    if ((coll = rra_coll(veh)) != 0)
        for (i = 0; i < 3; i++)
            v[i] = MEMF(coll + 0x44u + 4u * i);
    speed = rch_speed(v);
    white = xbox_InputGetState(0, &st) == 0 && st.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] > XBOX_ANALOG_BUTTON_THRESHOLD;
    press = white && !s_rch.white_was;
    s_rch.white_was = white;
    rch_recv(me, now);
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
        if (!near || speed > RCH_SLOW)
            return 0;                           /* nothing for us: the AI race prompt may show */
        if (rch_speed(near->v) > RCH_SLOW) {
            snprintf(text, sizeof text, "Street race\nChallenge %s\nBoth slow down to challenge", near->name);
            rra_prompt(text);
            return 1;
        }
        if (press) {
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
            fprintf(stderr, "[CHALLENGE] we challenge %s (race %08X, %d checkpoints, %.1f km)\n", s_rch.name,
                    (unsigned)s_rch.race, s_rch.ncp, s_rch.km);
            return 1;
        }
        snprintf(text, sizeof text, "Street race\nChallenge %s\nPress White [Q] to challenge", near->name);
        rra_prompt(text);
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
            snprintf(text, sizeof text, "Challenge sent to %s\n%d checkpoints \xC2\xB7 %.1f km\nWaiting for an answer \xC2\xB7 %d s",
                     s_rch.name, s_rch.ncp, s_rch.km, k > 0 ? k : 0);
        else if (s_rch.st == RCH_ACCEPTED)
            snprintf(text, sizeof text, "Race vs %s\n%d checkpoints \xC2\xB7 %.1f km\nGet ready", s_rch.name, s_rch.ncp,
                     s_rch.km);
        else if (speed > RCH_SLOW || (pp && rch_speed(pp->v) > RCH_SLOW))
            snprintf(text, sizeof text, "%s challenges you!\n%d checkpoints \xC2\xB7 %.1f km\nBoth slow down to accept \xC2\xB7 %d s",
                     s_rch.name, s_rch.ncp, s_rch.km, k > 0 ? k : 0);
        else {
            snprintf(text, sizeof text, "%s challenges you!\n%d checkpoints \xC2\xB7 %.1f km\nPress White [Q] to accept \xC2\xB7 %d s",
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
            for (k = 0; k < 12; k++)
                s_rch.hold[k] = MEMF(mtx + 0x10u + 4u * k);
            s_rch.hold[10] += 0.1f;
            s_rch.held = 1;
            s_rch.next = 0;
            s_rch.fin = s_rch.pfin = 0;
            s_rch.pcps = 0;
        }
        if (s_rch.host && now - s_rch.tx >= RCH_RESEND) {
            rch_send(s_rch.peer, s_rch.race, RCH_OP_GO, 0, now);
            s_rch.tx = now;
        }
        if (now < s_rch.t0) {
            float zero[3] = { 0 };
            roam_set_pose(veh, s_rch.hold, zero);   /* both wait on their spot */
            snprintf(text, sizeof text, "%d\nRace vs %s \xC2\xB7 %d checkpoints\nGet ready", (int)(s_rch.t0 - now) + 1,
                     s_rch.name, s_rch.ncp);
            rra_prompt(text);
            return 1;
        }
        s_rch.st = RCH_RACE;
        s_rch.tx = 0;
        fprintf(stderr, "[CHALLENGE] GO: race vs %s\n", s_rch.name);
        /* fall through */
    case RCH_RACE:
        el = now - s_rch.t0;
        if (s_rch.fin == 0.0 && s_rch.next < s_rch.ncp && rch_hd(s_rch.cp[s_rch.next], me) < RCH_CP_R &&
            fabsf(s_rch.cp[s_rch.next][1] - me[1]) < RCH_CP_DY) {
            s_rch.next++;
            s_rch.tx = 0;                       /* tell now */
            fprintf(stderr, "[CHALLENGE] checkpoint %d of %d at %.2f s\n", s_rch.next, s_rch.ncp, el);
            if (s_rch.next == s_rch.ncp) {
                s_rch.fin = el > 0.001 ? el : 0.001;
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
        go = el < 1.0;
        if (s_rch.fin > 0.0) {
            rch_fmt_time(t2, sizeof t2, s_rch.fin);
            snprintf(text, sizeof text, "Finished!\n%s\nWaiting for %s", t2, s_rch.name);
        } else if (s_rch.next < s_rch.ncp)
            snprintf(text, sizeof text, "%s\n%s %d/%d \xC2\xB7 %.0f m\n%s", go ? "GO!" : place == 1 ? "1st" : "2nd",
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
    if (s_rch.win == 2)
        snprintf(text, sizeof text, "Dead heat!\nYou and %s: %s\nFree Roam goes on", s_rch.name, t1);
    else if (s_rch.win == 1)
        snprintf(text, sizeof text, "You win!\nYou %s \xC2\xB7 %s %s\nFree Roam goes on", t1, s_rch.name,
                 s_rch.pfin > 0.0 ? t2 : "did not finish");
    else if (s_rch.fin > 0.0)
        snprintf(text, sizeof text, "%s wins\n%s %s \xC2\xB7 you %s\nFree Roam goes on", s_rch.name, s_rch.name, t2, t1);
    else
        snprintf(text, sizeof text, "%s wins\n%s %s \xC2\xB7 you %d/%d checkpoints\nFree Roam goes on", s_rch.name,
                 s_rch.name, t2, s_rch.next, s_rch.ncp);
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
 * icons then stay off). Full map: every one left; minimap: the next and the finish. */
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
    for (i = s_rch.ncp - 1; i >= s_rch.next; i--) {   /* the next one last: on top */
        int last = i == s_rch.ncp - 1;
        if (mini && i != s_rch.next && !last)
            continue;
        if (!online_ram(tex = MEM32(map + (last ? 0x9Cu : 0xA8u))))
            continue;
        c = i == s_rch.next ? next : last ? finish : later;
        for (k = 0; k < 3; k++)
            MEMF(item + 0x34u + 4u * k) = s_rch.cp[i][k];
        for (k = 0; k < 4; k++)
            MEMF(col + 4u * k) = c[k];
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
          s_roam_nhist)) {
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
