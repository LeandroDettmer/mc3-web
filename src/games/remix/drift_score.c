/* drift_score.c -- Drift scoring with a money payout (NFSU2 / Forza / CarX
 * style). Included by recomp_manual.c after online_progress.c (uses s_online,
 * s_on_money, s_on_drift, online_money_va, online_garage_left, op_dollars,
 * ROAM_SETUP_VA); fed every frame by drift.c's drift_step (slip angle, speed,
 * wall hit), Drift mode on or off.
 *
 * A drift: our car over DS_BETA deg of slip above DS_SPEED, on the ground
 * (not DS_AIR s in free fall), in a cruise (race type [[0x53F2E0]+0x18] == 0:
 * never in a race). Points/s = DS_K x clamp(slip, 10, 60) x speed (m/s):
 * 35 deg at 80 km/h ~460/s, a good 5 s drift ~2,300. It ends after DS_GRACE s
 * under the threshold (or at once when the car is stopped) and is banked x the
 * multiplier into the total (under DS_MIN points it is dropped quietly). A
 * drift that starts within DS_CHAIN s of the last bank chains: the chain's
 * points raise the multiplier one step per DS_STEP, x1..x5. A wall or car hit
 * (drift.c: speed lost faster than 15 m/s^2) or a spin-out (side-on past 90 deg
 * by DS_SPIN, on the other side from where the drift began: sign-free) loses
 * the drift and the chain ('Drift lost').
 *
 * Money, browser only: in the Online session's cruise, each DS_PAY_PTS of the
 * lifetime total (s_on_drift, profile.bin version 4) pays $DS_PAY into
 * [[0x557F0C]+0xC]+0xACC and saves the profile at once; the remainder counts
 * on toward the next. Elsewhere (career, Arcade, native) the points show with
 * this run's total, no money.
 *
 * HUD: drift_hud_box (seq; text "line\nline": the live drift / banked / lost,
 * then the progress; big: the payout), refilled and its seq bumped every frame
 * the car moves (paused: stale, the page hides it); web/pre.js shows it
 * bottom-centre. '[DRIFT] score: ...' per bank, lost drift and payout. */

#define DS_BETA     10.0f           /* deg of slip: a drift */
#define DS_SPEED    (30.0f / 3.6f)  /* m/s */
#define DS_K        0.6f            /* points/s = DS_K x clamp(beta, 10, 60) x speed (m/s) */
#define DS_GRACE    1.0f            /* s under the threshold before the drift is banked */
#define DS_AIR      0.1f            /* s in free fall (vertical speed falling > 7 m/s^2): in the air */
#define DS_CHAIN    1.5f            /* s after a bank for the next drift to chain */
#define DS_STEP     2500.0f         /* chain points per multiplier step */
#define DS_MULT     5
#define DS_SPIN     10.0f           /* deg past side-on: a spin-out */
#define DS_MIN      100u            /* points: smaller drifts are dropped (ordinary cornering) */
#define DS_PAY_PTS  10000u
#define DS_PAY      10000u          /* $ */
#define DS_SHOW     2.0f            /* s an event (banked, lost, payout) shows */

enum { DS_NONE, DS_BANKED, DS_LOST };
static struct {
    int on;                         /* drifting (the grace time included) */
    float pend, chain, below, since, a0, air, vy;
    int what;                       /* the last event, shown for show s */
    float show, pay_show;
    uint32_t got, paid;
    int got_mult;
    uint32_t run;                   /* banked this run without money */
} s_ds = { .since = DS_CHAIN + 1.0f };

static struct { int32_t seq; char text[96]; char big[32]; } s_ds_box;
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EMSCRIPTEN_KEEPALIVE uintptr_t drift_hud_box(void) { return (uintptr_t)&s_ds_box; }
#endif

static int ds_mult(void)
{
    int m = 1 + (int)(s_ds.chain / DS_STEP);
    return m > DS_MULT ? DS_MULT : m;
}

static void ds_bank(int money)
{
    int mult = ds_mult();
    uint32_t got = (uint32_t)(s_ds.pend * (float)mult + 0.5f), m = money ? online_money_va() : 0, n;
    s_ds.on = 0;
    if (got < DS_MIN)
        return;
    s_ds.since = 0;
    s_ds.what = DS_BANKED;
    s_ds.show = DS_SHOW;
    s_ds.got = got;
    s_ds.got_mult = mult;
    if (!m) {
        s_ds.run += got;
        fprintf(stderr, "[DRIFT] score: banked %u (x%d), total %u, paid $0 (no money here)\n", (unsigned)got, mult,
                (unsigned)s_ds.run);
        return;
    }
    n = (s_on_drift + got) / DS_PAY_PTS - s_on_drift / DS_PAY_PTS;
    s_on_drift += got;
    if (n) {
        MEM32(m) += n * DS_PAY;
        s_on_money = MEM32(m);
        s_ds.paid = n * DS_PAY;
        s_ds.pay_show = DS_SHOW;
    }
    fprintf(stderr, "[DRIFT] score: banked %u (x%d), total %u, paid $%u%s\n", (unsigned)got, mult, (unsigned)s_on_drift,
            (unsigned)(n * DS_PAY), n ? "" : " (next at the next 10,000)");
    if (n)
        online_garage_left();                   /* the profile saved now (money, drift total) */
}

static void drift_score(int car, float beta, float b180, float s, float vy, float dt, int hit, int frozen)
{
    uint32_t setup = MEM32(ROAM_SETUP_VA), total;
    int money = 0, mult;
    char a[24], b[24];
#ifdef __EMSCRIPTEN__
    money = s_online;                           /* the native Online session stays as it was: no money */
#endif
    if (!car || (online_ram(setup) && MEM32(setup + 0x18u) != 0)) {   /* no car, or a race: nothing pending */
        s_ds.on = 0;
        s_ds.pend = s_ds.chain = s_ds.air = s_ds.show = s_ds.pay_show = 0;
        s_ds.since = DS_CHAIN + 1.0f;
        s_ds_box.text[0] = s_ds_box.big[0] = 0;
        s_ds_box.seq++;
        return;
    }
    if (frozen)                                 /* paused: the box goes stale */
        return;
    s_ds.air = vy - s_ds.vy < -7.0f * dt ? s_ds.air + dt : 0.0f;
    s_ds.vy = vy;
    s_ds.since += dt;
    s_ds.show -= dt;
    s_ds.pay_show -= dt;
    if (s_ds.on && (hit || (s_ds.a0 < 90.0f ? b180 > 90.0f + DS_SPIN : b180 < 90.0f - DS_SPIN))) {
        fprintf(stderr, "[DRIFT] score: drift lost (%s), %u pending x%d\n", hit ? "hit" : "spin-out",
                (unsigned)s_ds.pend, ds_mult());
        s_ds.on = 0;
        s_ds.pend = s_ds.chain = 0;
        s_ds.since = DS_CHAIN + 1.0f;
        s_ds.what = DS_LOST;
        s_ds.show = DS_SHOW;
    } else if (beta > DS_BETA && s > DS_SPEED && s_ds.air < DS_AIR) {
        float pts = DS_K * (beta > 60.0f ? 60.0f : beta) * s * dt;
        if (!s_ds.on) {
            s_ds.on = 1;
            s_ds.a0 = b180;
            s_ds.pend = 0;
            if (s_ds.since > DS_CHAIN)
                s_ds.chain = 0;
        }
        s_ds.below = 0;
        s_ds.pend += pts;
        s_ds.chain += pts;
    } else if (s_ds.on && ((s_ds.below += dt) >= DS_GRACE || s < 1.0f))
        ds_bank(money);

    /* the HUD: the live drift, else the last event; then the progress */
    mult = ds_mult();
    if (s_ds.on) {
        op_dollars(a, sizeof a, (uint32_t)s_ds.pend);
        snprintf(s_ds_box.text, sizeof s_ds_box.text, "DRIFT %s x%d", a, mult);
    } else if (s_ds.show > 0 && s_ds.what == DS_BANKED) {
        op_dollars(a, sizeof a, s_ds.got);
        if (s_ds.got_mult > 1)
            snprintf(s_ds_box.text, sizeof s_ds_box.text, "+%s (x%d)", a, s_ds.got_mult);
        else
            snprintf(s_ds_box.text, sizeof s_ds_box.text, "+%s", a);
    } else if (s_ds.show > 0 && s_ds.what == DS_LOST)
        snprintf(s_ds_box.text, sizeof s_ds_box.text, "Drift lost");
    else
        s_ds_box.text[0] = 0;
    if (s_ds_box.text[0]) {
        size_t l = strlen(s_ds_box.text);
        total = money && online_money_va() ? s_on_drift : s_ds.run;
        if (money && online_money_va()) {
            op_dollars(a, sizeof a, total % DS_PAY_PTS);
            op_dollars(b, sizeof b, DS_PAY_PTS);
            snprintf(s_ds_box.text + l, sizeof s_ds_box.text - l, "\n%s / %s \xE2\x86\x92 $%s", a, b, b);
        } else {
            op_dollars(a, sizeof a, total);
            snprintf(s_ds_box.text + l, sizeof s_ds_box.text - l, "\nTotal %s", a);
        }
    }
    if (s_ds.pay_show > 0) {
        op_dollars(a, sizeof a, s_ds.paid);
        snprintf(s_ds_box.big, sizeof s_ds_box.big, "+$%s", a);
    } else
        s_ds_box.big[0] = 0;
    s_ds_box.seq++;
}
