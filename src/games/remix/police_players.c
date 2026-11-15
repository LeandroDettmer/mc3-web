/* police_players.c -- a standard Online Free Roam feature (no mod pack), players in police cars: a player
 * driving a police car (CarType name with "_cop": vp_impalass_cop_s/a/d_96,
 * vp_d_tokyo_cop_01, the bike vp_kwz_cop_98) in the Online Free Roam cruise
 * arrests other players by ramming them: PPL_HITS hits on the same player,
 * each within PPL_WINDOW_S of the last, and that player is BUSTED: it pays
 * PPL_FINE (never more than it has) out of its Online money, and the police
 * player gets what was paid.  Both profiles are saved at once.
 *
 * Included by recomp_manual.c right after police.c (uses police_p0, police_car, police_dist, police_acc, op_dollars, online_money_va,
 * s_on_money, online_garage_left, roam_now, s_roam_seen, the pool's
 * roam_pool_of/roam_rc_veh, roam_peer_rec, roam_cfg_name, s_rch, s_rra_race,
 * roam_free_cruise).  Hooks: sub_002C1520 (player update) calls ppl_frame,
 * rch_recv (roam_challenge.c) hands 'P' messages to ppl_rx, op_note
 * (online_progress.c) shows ppl_note.
 *
 * Who counts: the police player only (remote cars are puppets: each side feels
 * its own collisions; the police player's car is the one bumped in its own
 * world).  A hit = our car's ground velocity changing faster than POL_HIT_ACC
 * (police.c's test, 25 m/s^2) with a player's car within PPL_HIT_M (the drawn
 * car's centre, else the peer's interpolated position); the nearest such
 * player, driving (status roam, no race bits), not in a police car itself.
 * One hit per PPL_HIT_GAP s (a crash spans frames).  No hit on that player for
 * PPL_WINDOW_S: its count starts again.  After a bust, PPL_COOL_S before the
 * same player counts again (both sides).
 *
 * Messages (roam_send_msg, peer to peer, the roam core's GAME record like
 * roam_challenge.c's 'C' ones, which drop ours and we theirs): 'P', version 1,
 * op, arg, id u32, amount u32 (little-endian, 12 bytes).
 *   HIT  (1) police -> player: arg = hits so far (1..PPL_HITS), id = 0, amount 0.
 *   BUST (2) police -> player: arg = PPL_HITS, id = the bust (random), amount =
 *        the fine asked; sent every PPL_RESEND s until PAID, PPL_ASK_S s at most.
 *   PAID (3) player -> police: arg = 0 paid, 1 no police mod, 2 busy (not
 *        driving in the Free Roam cruise, racing), 3 cool-down, 4 the sender's
 *        car is no police car; id = the bust; amount = what was paid.  Every
 *        BUST is answered, a repeated one with the same answer (paid once).
 * The police player is credited once per bust id.  Old builds drop 'P'.
 *
 * Log lines '[POLICE] player: ...'.  Browser only. */

#define PPL_HITS      9                 /* hits on the same player: busted (owner, 2026-11-15: "umas 9 vezes") */
#define PPL_HIT_M     6.0f              /* m between car centres for a hit */
#define PPL_HIT_GAP   1.0               /* s: one hit at most per player in this time */
#define PPL_WINDOW_S  60.0              /* s without a hit on that player: its count starts again */
#define PPL_FINE      50000u            /* the AI police's fine (POLICE_FINE) */
#define PPL_COOL_S    60.0              /* s after a bust: that player is not counted (police) / not fined (player) */
#define PPL_RESEND    0.3               /* s between BUSTs */
#define PPL_ASK_S     5.0               /* s of BUSTs without an answer: given up */
#define PPL_NOTE_S    6.0               /* s a note shows (big the first 2 for a bust) */
#define PPL_MAX       8                 /* players followed at once */
enum { PPL_OP_HIT = 1, PPL_OP_BUST = 2, PPL_OP_PAID = 3 };
enum { PPL_PAID = 0, PPL_NOMOD = 1, PPL_BUSY = 2, PPL_COOL = 3, PPL_NOTCOP = 4 };

static struct {                         /* police side: one per player hit */
    uint32_t id;                        /* the peer, 0 free */
    char name[ROAM_NAME_MAX + 1];
    int hits;
    double hit_t;                       /* last hit */
    uint32_t bust;                      /* bust asked (0 none) */
    double bust_t, bust_tx;             /* when it was first / last sent */
    double cool;                        /* not counted before */
} s_ppl[PPL_MAX];
static struct { uint32_t from, bust, amount; uint8_t why; } s_ppl_ans[4];   /* player side: answers given */
static unsigned s_ppl_nans;
static double s_ppl_paid_t;             /* player side: our last bust (cool-down) */
static char s_ppl_note[112];
static double s_ppl_note_t;
static int s_ppl_note_big;

/* A note in the prompt box (big: centred the first 2 s). */
#define ppl_say(big, ...) \
    (snprintf(s_ppl_note, sizeof s_ppl_note, __VA_ARGS__), s_ppl_note_t = roam_now(), s_ppl_note_big = (big))

/* The prompt box's line for an empty prompt (op_note): "" none. */
static const char *ppl_note(void)
{
    static char out[120];
    double now = roam_now();
    if (!s_ppl_note_t)
        return "";
    if (now - s_ppl_note_t > PPL_NOTE_S) {
        s_ppl_note_t = 0;
        return "";
    }
    snprintf(out, sizeof out, "%s%s", s_ppl_note_big && now - s_ppl_note_t < 2.0 ? "\x01" : "", s_ppl_note);
    return out;
}

static int ppl_cop_name(const char *s) { return s && strstr(s, "_cop") != NULL; }

/* Our car is a police car: the race setup's entry 0 CarCfg (the car the cruise built for us). */
static int ppl_we_cop(void)
{
    uint32_t setup = MEM32(ROAM_SETUP_VA), cfg;
    char name[33];
    if (!online_ram(setup) || !online_ram(cfg = MEM32(setup + ROAM_ENTRY + 0x0Cu)) || MEM32(cfg) != ROAM_CFG_VT)
        return 0;
    memcpy(name, (const char *)XBOX_PTR(cfg + ROAM_CFG_NAME), 32);
    name[32] = 0;
    return ppl_cop_name(name);
}

static int ppl_peer_cop(uint32_t id)    /* the peer's car record says a police car */
{
    uint8_t blob[ROAM_CFG_SIZE + 4];
    return roam_peer_rec(id, blob) && ppl_cop_name(roam_cfg_name(blob));
}

/* Driving in the Online Free Roam cruise, not racing. */
static int ppl_live(void)
{
    uint32_t setup = MEM32(ROAM_SETUP_VA);
#ifndef __EMSCRIPTEN__
    return 0;                                   /* browser only: the native Online session stays as it was */
#endif
    return roam_free_cruise() && online_ram(setup) && MEM32(setup + 0x18u) == 0 &&
           s_rch.st == RCH_IDLE && !s_rra_race;
}

static void ppl_send(uint32_t to, int op, int arg, uint32_t id, uint32_t amount)
{
    uint8_t b[12] = { 'P', 1, (uint8_t)op, (uint8_t)arg };
    memcpy(b + 4, &id, 4);
    memcpy(b + 8, &amount, 4);
    roam_send_msg(to, b, sizeof b);
}

static const char *ppl_peer_name(uint32_t id)
{
    int i;
    for (i = 0; s_roam_seen && i < s_roam_nseen; i++)
        if (s_roam_seen[i].id == id)
            return s_roam_seen[i].name[0] ? s_roam_seen[i].name : "Player";
    return "Player";
}

static int ppl_of(uint32_t id, double now)      /* the police side's entry for peer id, made if missing */
{
    int i, old = 0;
    for (i = 0; i < PPL_MAX; i++)
        if (s_ppl[i].id == id)
            return i;
    for (i = 0; i < PPL_MAX; i++) {
        if (!s_ppl[i].id || (!s_ppl[i].bust && now - s_ppl[i].hit_t > PPL_WINDOW_S && now >= s_ppl[i].cool))
            break;
        if (s_ppl[i].hit_t < s_ppl[old].hit_t)
            old = i;
    }
    if (i == PPL_MAX)
        i = old;
    memset(&s_ppl[i], 0, sizeof s_ppl[i]);
    s_ppl[i].id = id;
    return i;
}

/* A 'P' message (rch_recv hands them over; n >= 8). */
static void ppl_rx(uint32_t from, const uint8_t *b, int n)
{
    uint32_t id = 0, amount = 0, m, paid = 0;
    double now = roam_now();
    char d[24];
    unsigned i;
    int k, why;
    if (n < 12 || b[1] != 1)
        return;
    memcpy(&id, b + 4, 4);
    memcpy(&amount, b + 8, 4);
    if (b[2] == PPL_OP_HIT) {
        if (ppl_live() && b[3] >= 1 && b[3] < PPL_HITS)
            ppl_say(0, "Police %s\nHit %d/%d: stop or be busted", ppl_peer_name(from), b[3], PPL_HITS);
        return;
    }
    if (b[2] == PPL_OP_BUST) {
        for (i = 0; i < s_ppl_nans && i < 4u; i++)
            if (s_ppl_ans[i].from == from && s_ppl_ans[i].bust == id) {      /* asked again: the same answer */
                ppl_send(from, PPL_OP_PAID, s_ppl_ans[i].why, id, s_ppl_ans[i].amount);
                return;
            }
        why = !ppl_live() ? PPL_BUSY :
              s_ppl_paid_t && now - s_ppl_paid_t < PPL_COOL_S ? PPL_COOL : !ppl_peer_cop(from) ? PPL_NOTCOP : PPL_PAID;
        if (why == PPL_PAID) {
            if (amount > PPL_FINE)
                amount = PPL_FINE;
            if ((m = online_money_va()) != 0) {
                paid = MEM32(m) < amount ? MEM32(m) : amount;
                MEM32(m) -= paid;
                s_on_money = MEM32(m);
            }
            s_ppl_paid_t = now;
            op_dollars(d, sizeof d, paid);
            ppl_say(1, "BUSTED by %s\nFine: -$%s", ppl_peer_name(from), d);
            fprintf(stderr, "[POLICE] player: busted by %s (%08X, bust %08X): paid $%u of $%u, money %u\n",
                    ppl_peer_name(from), (unsigned)from, (unsigned)id, (unsigned)paid, (unsigned)amount,
                    m ? (unsigned)MEM32(m) : 0u);
            online_garage_left();                   /* the profile saved now */
        } else
            fprintf(stderr, "[POLICE] player: bust from %08X refused (%s)\n", (unsigned)from,
                    why == PPL_NOMOD ? "no police mod" : why == PPL_BUSY ? "busy" : why == PPL_COOL ? "cool-down"
                                                                                  : "its car is no police car");
        i = s_ppl_nans++ % 4u;
        s_ppl_ans[i].from = from;
        s_ppl_ans[i].bust = id;
        s_ppl_ans[i].amount = paid;
        s_ppl_ans[i].why = (uint8_t)why;
        ppl_send(from, PPL_OP_PAID, why, id, paid);
        return;
    }
    if (b[2] != PPL_OP_PAID)
        return;
    for (k = 0; k < PPL_MAX && !(s_ppl[k].id == from && s_ppl[k].bust && s_ppl[k].bust == id); k++)
        ;
    if (k == PPL_MAX)                               /* not ours, or credited already */
        return;
    s_ppl[k].bust = 0;
    s_ppl[k].hits = 0;
    s_ppl[k].cool = now + PPL_COOL_S;
    if (b[3] != PPL_PAID) {
        ppl_say(0, "%s not busted\n%s", s_ppl[k].name, b[3] == PPL_NOMOD ? "No Police mod on" : b[3] == PPL_COOL ?
                "Busted a moment ago" : b[3] == PPL_NOTCOP ? "Not a police car there" : "Busy");
        fprintf(stderr, "[POLICE] player: %s (%08X) not busted: reason %u\n", s_ppl[k].name, (unsigned)from, b[3]);
        return;
    }
    if (amount > PPL_FINE)
        amount = PPL_FINE;
    if ((m = online_money_va()) != 0) {
        MEM32(m) += amount;
        s_on_money = MEM32(m);
    }
    op_dollars(d, sizeof d, amount);
    ppl_say(1, "BUSTED %s\nReward: +$%s", s_ppl[k].name, d);
    fprintf(stderr, "[POLICE] player: busted %s (%08X, bust %08X): +$%u, money %u\n", s_ppl[k].name, (unsigned)from,
            (unsigned)id, (unsigned)amount, m ? (unsigned)MEM32(m) : 0u);
    online_garage_left();                           /* the profile saved now */
}

/* Each player update (sub_002C1520), player 0's only: the police side's hit count and busts. */
static void ppl_frame(uint32_t p)
{
    static float uv0[3];
    static double last_t;
    static int said_cop = -1;
    float up[3], uv[3], cp[3], cv[3], d, bd = PPL_HIT_M, acc;
    double now, dt;
    int i, k, best = -1, cop;
    uint32_t veh;
    if (!p || p != police_p0())
        return;
    if (!ppl_live()) {
        last_t = 0;
        return;
    }
    cop = ppl_we_cop();
    if (cop != said_cop) {
        said_cop = cop;
        fprintf(stderr, "[POLICE] player: our car is %sa police car%s\n", cop ? "" : "not ",
                cop ? ": ramming a player 9 times busts it" : "");
    }
    now = roam_now();
    for (k = 0; k < PPL_MAX; k++) {                 /* busts waiting for an answer */
        if (!s_ppl[k].id || !s_ppl[k].bust)
            continue;
        if (now - s_ppl[k].bust_t > PPL_ASK_S) {
            ppl_say(0, "%s not busted\nNo answer (older game or gone)", s_ppl[k].name);
            fprintf(stderr, "[POLICE] player: bust of %s (%08X) got no answer in %.0f s\n", s_ppl[k].name,
                    (unsigned)s_ppl[k].id, PPL_ASK_S);
            s_ppl[k].bust = 0;
            s_ppl[k].hits = 0;
        } else if (now - s_ppl[k].bust_tx >= PPL_RESEND) {
            ppl_send(s_ppl[k].id, PPL_OP_BUST, PPL_HITS, s_ppl[k].bust, PPL_FINE);
            s_ppl[k].bust_tx = now;
        }
    }
    dt = last_t ? now - last_t : 0;
    last_t = now;
    if (!cop || !police_car(MEM32(p + 0x14u), up, uv)) {
        last_t = 0;
        return;
    }
    acc = dt > 0.002 && dt < 0.2 ? police_acc(uv, uv0, dt) : 0.0f;    /* not after a pause or a load */
    memcpy(uv0, uv, sizeof uv);
    if (acc <= POL_HIT_ACC || !s_roam_seen || now - s_roam_seen_t > 0.5)
        return;
    for (i = 0; i < s_roam_nseen; i++) {            /* the nearest driving player within PPL_HIT_M */
        const roam_peer *pp = &s_roam_seen[i];
        if (pp->status != RC_ST_ROAM || (pp->bits & (ROAM_BIT_RACE | ROAM_BIT_STREET)))
            continue;
        k = roam_pool_of(pp->id);
        if (!(k && (veh = roam_rc_veh(k)) && police_car(veh, cp, cv)))
            memcpy(cp, pp->p, sizeof cp);
        if ((d = police_dist(cp, up)) < bd) {
            bd = d;
            best = i;
        }
    }
    if (best < 0 || ppl_peer_cop(s_roam_seen[best].id))
        return;
    k = ppl_of(s_roam_seen[best].id, now);
    snprintf(s_ppl[k].name, sizeof s_ppl[k].name, "%s", s_roam_seen[best].name[0] ? s_roam_seen[best].name : "Player");
    if (s_ppl[k].bust || now < s_ppl[k].cool || now - s_ppl[k].hit_t < PPL_HIT_GAP)
        return;
    if (now - s_ppl[k].hit_t > PPL_WINDOW_S)
        s_ppl[k].hits = 0;
    s_ppl[k].hit_t = now;
    s_ppl[k].hits++;
    fprintf(stderr, "[POLICE] player: hit %d/%d on %s (%08X): %.1f m apart, our speed changed %.0f m/s^2\n",
            s_ppl[k].hits, PPL_HITS, s_ppl[k].name, (unsigned)s_ppl[k].id, (double)bd, (double)acc);
    if (s_ppl[k].hits < PPL_HITS) {
        ppl_send(s_ppl[k].id, PPL_OP_HIT, s_ppl[k].hits, 0, 0);
        ppl_say(0, "Arrest %s\nHit %d/%d", s_ppl[k].name, s_ppl[k].hits, PPL_HITS);
        return;
    }
    s_ppl[k].bust = ((uint32_t)rand() ^ (uint32_t)(now * 1e6) ^ s_ppl[k].id) | 1u;
    s_ppl[k].bust_t = s_ppl[k].bust_tx = now;
    ppl_send(s_ppl[k].id, PPL_OP_BUST, PPL_HITS, s_ppl[k].bust, PPL_FINE);
    ppl_say(0, "Arrest %s\nHit %d/%d: busting...", s_ppl[k].name, PPL_HITS, PPL_HITS);
}
