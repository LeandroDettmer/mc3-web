/* drift.c -- Drift mode (MC3_DRIFT, live through mc3_live_box): an arcade drift
 * assist on our own car. #included by recomp_manual.c (needs mc3_live, MEM*,
 * online_ram); drift_step runs at the start of every frame from the
 * sub_0020B6F0 override, before the frame's physics.
 *
 * Ideas (no code) from nfsu2-web's drift mode (MIT, th0mk): NFSU2's drift
 * events run a drift assist every physics step that sets the rear wheels'
 * grip factor from the car's state, and a steering curve of their own. Here
 * the same shape on MC3's car sim, per frame, from the slip angle and speed:
 * the rear lets go on a handbrake tap or a lift-off with the wheel turned,
 * stays loose (looser with throttle) through the drift, grips back past the
 * angle limit; the front gets more lock and a little more bite to catch it;
 * the yaw is cut past the limit (no spin-outs); and part of the speed a slide
 * scrubs off is given back, never above the speed at the slide's start.
 *
 * Only player 0's vehicle ([[0x558814]+8]+0x14 = veh), so AI, traffic and
 * remote cars keep theirs; peers just see our pose. What it touches:
 *  - the car sim [veh+0x50] (AGE vehCarSim, vtable 0x44EA64; mcCarSim
 *    0x456550, ctor sub_00121550). Its tune (FileIO sub_000A3CA0: Mass +0x24,
 *    CenterOfMass +0x18, InertiaBox +0x34, ...) owns WheelFront [sim+0x54] and
 *    WheelBack [sim+0x58], wheel tunes (vtable 0x44D6C8, ctor sub_000A69C0,
 *    fields sub_000A6DA0: +0x24 SteeringLimit, +0x34 BrakeCoef, +0x38
 *    HandbrakeCoef, +0x3C SteeringOffset, +0x64 OptimumSlipPercent 0.14,
 *    +0x68 StaticFric 2.5, +0x6C SlidingFric 2.3). The friction curve
 *    sub_000A6220 (from the wheel force sub_000A8770, longitudinal and lateral
 *    slip each) rises to StaticFric at the optimum slip and falls to
 *    SlidingFric past it, times the ground's grip [wheel+0x118]. A wheel's
 *    steer angle is -SteeringLimit x the steer input (sub_000A6900 ->
 *    [wheel+0x140]). The upgrade blend sub_000A3A90 rewrites these tunes:
 *    a value that is not the one we wrote is the game's, the new original.
 *  - the body (phInertialCS) [[[veh+8]+0x68]+0x10]: +0x04 mass, +0x0C..0x14
 *    inertia (body axes), +0x18.. its inverse, +0x28 the angular speed cap,
 *    +0x2C momentum, +0x38 angular momentum, +0x44 velocity, +0x50 angular
 *    velocity (world), +0x5C 3x3 orientation, rows right, up, back (SetVel
 *    sub_003432B0 writes velocity + momentum = mass x velocity; SetAngVel
 *    sub_00343300 angular velocity + momentum = R^T I R w). Both pairs are
 *    written together here.
 *  - the inputs, read only: [[veh+4]+0x34] +0x6C steer, +0x70 throttle,
 *    +0x74 brake, +0x78 handbrake (the controller's +0x18/+0xC/+0x10/+0x14,
 *    copied by sub_002C9570), the frame before's.
 * Not found / not used: the speed-sensitive steering (vehInput tune
 * SSSValue +0x10 / SSSThreshold +0xC, FileIO sub_002C9840, vtable 0x47681C;
 * its owner not found): SteeringLimit scales the angle after it.
 *
 * Levels: MC3_DRIFT=1 normal, =2 strong, =3 controlled; the page's live word:
 * 1 off, 2 on (the environment's level, normal if that is off), 3 strong,
 * 4 controlled. Off: every value back (if still ours).
 *
 * Controlled (v4, 2026-11-15, untested in game): NFSU2's drift-event handling.
 * NFSU2 runs a drift routine every physics step of a drift event (PC
 * SPEED2.EXE 0x5AC260, "yaw assist + rear grip", nfsu2-web runtime/oracle.c;
 * th0mk's write-up, docs/port/drift-fidelity.md 2.2, is in a private repo).
 * The same routine survives in its successor's engine, NFS Most Wanted's
 * SuspensionRacer::DoDrifting (decompiled: github.com/dbalatoni13/nfsmw and
 * github.com/Brawltendo/Most-Wanted-Vehicles-Decomp, src/Speed/Indep/Src/
 * Physics/Behaviors/SuspensionRacer.cpp; its steering tables are marked as
 * HP2/UG1/UG2 leftovers). Read for the algorithm only, no code copied:
 *  1. a drift value D (0..1) and a state OUT/ENTER/IN/EXIT. D rises 8/s in
 *     ENTER and IN, falls 2/s in OUT and EXIT (in over 0.125 s, out over
 *     0.5 s); at 1 the state is IN, at 0 OUT. From OUT (or ENTER): above
 *     30 mph with the handbrake > 0.5 or the slip > 12 deg -> ENTER (ours,
 *     owner 2026-11-15: also |steer| > CTL_STEER or a lift-off kick, and while
 *     ENTER under 12 deg the rear target is at most CTL_THROW, so steering
 *     alone throws the car; the table adds |steer| x 0.5). From IN
 *     or EXIT, each step: IN if above 30 mph, slip 12..90 deg, the car
 *     rotating into the slide and the wheel not counter-steering; IN if
 *     throttle x counter-steer x slip > 12 deg above 30 mph; ENTER if
 *     throttle x slip > 12 deg; else EXIT.
 *  2. yaw stabilizer: while D > 0 and the car rotates into the slide (2+
 *     wheels down), a torque -D x S(slip) x 4 x yaw rate x inertia about up,
 *     S over the slip in rad: 0 0, 0.26 0.1, 0.52 0.45, 0.79 0.85, 1.05 0.95,
 *     1.55 1.15, 1.57 0. Rotating back toward straight is never damped: the
 *     slide straightens freely and never spins.
 *  3. rear grip: both rear tyres' maximum force x R(D x (|slip rad| x 0.5 +
 *     counter-steer x 4 + |yaw rate| x 0.5)), R over 0..1 in 10 steps: 1.1
 *     0.95 0.87 0.77 0.67 0.6 0.51 0.43 0.37 0.34 (counter-steer = |steer|
 *     when it points toward the slide). The rear lets go as the angle and
 *     the rotation grow and most when counter-steering: counter-steer holds
 *     the angle instead of snapping the car straight.
 *  4. counter-steer assist (its steering): while counter-steering the
 *     steering's speed-limited maximum is raised to at least the rear's slip
 *     angle. Also the stability boost of the rear grip is x (1 - D), and the
 *     drift steering remap/speed tables (not written here: input shaping).
 *  No speed keeping in that routine: the speed holds because the rear lets
 *  go instead of scrubbing.
 * Mapped on MC3 (our hooks, per frame of the real frame time 0x4B2D70):
 *  1. as is (30 mph = 13.4 m/s; slip and counter-steer from the body: the
 *     slip side is the velocity along the right row, the wheel's side is
 *     learned from the yaw it gives while gripping, CTL_SIDE: no sign guess).
 *  2. on w and L along up, as the yaw cut (dw = -D S 4 wy dt, at most half
 *     of wy a frame); "rotating into the slide" = (w x forward) . v < 0.
 *  3. the rear factor fr (Sliding x fr, Static x 0.6 + 0.4 fr) = 1 - (1 -
 *     R/1.1) x CTL_REAR_K: 1 out of a drift (MC3's own grip; NFSU2's 1.1 is
 *     its stability boost), ~0.5 at full R; smoothed over CTL_TAU.
 *  4. front SteeringLimit x max(1, slip rad / SteeringLimit) x D while
 *     counter-steering, at most x CTL_CS_MAX (the wheel angle is
 *     SteeringLimit x the input after the game's speed-sensitive steering).
 *  Kept from v3: smooth grip (now D's ramps + CTL_TAU), front SlidingFric
 *  +5%. Dropped: the held slide, the 50 deg/s slip-rate damp, the yaw cut and
 *  catch past beta_max, the drag (the stabilizer and D do those jobs). Speed:
 *  the give-back keeps 0.30 of what a slide scrubs, never above its start.
 *  The steer input is never written (peers get our inputs). '[DRIFT]
 *  controlled: steer side learned ...' once; the slide line adds the
 *  stabilizer's frames and the drift state at its end.
 * Owner on v1 (yaw cap + PD hold: no drift), v2 (normal row, no give-back,
 * a drag: throttle threw it, snapped straight mid-drift, handbrake threw it),
 * v3 (held slide, slip-rate limit; not judged). The drift camera (drift_cam, at the end) is its own switch, any
 * level. '[DRIFT]' lines: on/off, the car's layout once
 * per car, one line per drift of 0.5 s or more. Drift scoring (drift_score.c)
 * gets this frame's slip angle, speed and hit test with the mode on or off.
 *
 * A hit (wall, car, prop): the velocity's change in the car's ground plane (the plane the tyres push in, normal =
 * the body's up row; a landing pushes along it) of DH_D1 m/s within one frame or DH_D2 within two (absolute, not per
 * second: at 30 fps 3.9 m/s over two frames read as 56 m/s^2); DH_BRAKE more while braking or on the handbrake;
 * larger than the change along up (a tilted landing); and the speed must fall DH_DROP (a hit never leaves the car
 * faster; 2026-11-12 logs: 376 of 380 lost lines were slide dynamics, 4 real hits like 93 -> 76 km/h stay).
 * Tyres top out near StaticFric x g ~ 25 m/s^2. Until 2026-11-12 a speed drop over 15 m/s^2 counted: a slide
 * decelerates that hard by itself, and 'Drift lost' fired mid-drift without a hit. */
#include <math.h>

static int online_ram(uint32_t va);
static void drift_score(int car, float beta, float s, float vy, float dt, const char *hit, int frozen);

#define DH_D1    6.0f       /* m/s of velocity change in the ground plane in one frame: a hit (absolute, not per second) */
#define DH_D2    8.0f       /* ... over two frames */
#define DH_DROP  1.111f     /* m/s (4 km/h): the speed must fall by this much; a hit never leaves the car faster */
#define DH_BRAKE 3.0f       /* m/s more while braking or on the handbrake */

typedef struct {
    float base;         /* rear grip factor when gripping (a little loose: a lift-off turns it) */
    float kick;         /* ... after a handbrake tap or a lift-off with the wheel turned (0.35 s) */
    float slide;        /* ... in the drift */
    float thr;          /* ... minus this x throttle: the throttle holds the angle */
    float catch_hi;     /* ... at beta_max + 25 deg: the rear grips back */
    float beta_max;     /* deg: past it the rear grips back and the yaw is cut */
    float steer;        /* front SteeringLimit x (1 + steer) at 30 deg of slip and more */
    float front;        /* front SlidingFric x (1 + front) at 20 deg of slip and more */
    float yaw_cut;      /* share of the yaw rate cut per 1/30 s at 20 deg past beta_max */
    float keep;         /* share of the speed a slide scrubs off given back */
} drift_tune;
static const drift_tune k_drift[3] = {
    /* base   kick   slide  thr    catch  bmax   steer  front  yaw    keep */
    { 0.92f, 0.50f, 0.62f, 0.10f, 1.05f, 40.0f, 0.60f, 0.15f, 0.25f, 0.50f },   /* 1 normal */
    { 0.85f, 0.42f, 0.52f, 0.12f, 1.00f, 50.0f, 1.00f, 0.20f, 0.35f, 0.70f },   /* 2 strong */
    { 1.00f, 0.00f, 0.00f, 0.00f, 0.00f, 90.0f, 0.00f, 0.05f, 0.00f, 0.30f },   /* 3 controlled: front, keep only */
};
/* Controlled only: NFSU2's drift routine (see the header) */
#define CTL_SPEED    8.33f  /* m/s (30 km/h; 30 mph until 2026-11-16, owner: lower): the drift's entry and hold speed */
#define CTL_SLIP     12.0f  /* deg */
#define CTL_STEER    0.70f  /* |steer| above which steering alone starts a drift (above CTL_SPEED) */
#define CTL_THROW    0.55f  /* rear factor while a drift starts with the car still straight (On's kick: 0.50) */
#define CTL_REAR_K   0.85f  /* share of NFSU2's rear grip range (R/1.1: 1 .. 0.31) used on MC3's tyres (0.70 until 2026-11-16, owner: slide more easily) */
#define CTL_TAU      0.10f  /* s: the rear factor follows its target (a keyboard's steer is all or nothing) */
#define CTL_CS_MAX   1.60f  /* the counter-steer assist's front SteeringLimit factor at most */
#define CTL_SIDE     1.0f   /* steer x yaw toward the right row (s) gripping before the steer side counts as learned */
static const float k_ctl_stab[7][2] = {   /* NFSU2's yaw stabilizer S over the slip (rad) */
    { 0.0f, 0.0f }, { 0.2618f, 0.10f }, { 0.5236f, 0.45f }, { 0.7854f, 0.85f }, { 1.0472f, 0.95f },
    { 1.5533f, 1.15f }, { 1.5708f, 0.0f } };
static const float k_ctl_rear[10] = { 1.1f, 0.95f, 0.87f, 0.77f, 0.67f, 0.6f, 0.51f, 0.43f, 0.37f, 0.34f };
enum { CTL_OUT, CTL_ENTER, CTL_IN, CTL_EXIT };

/* one tune value we scale: the game's (orig) and ours (set) */
typedef struct { uint32_t va; float orig, set; } drift_field;
enum { DF_RS, DF_RK, DF_FK, DF_FSTEER, DF_N };   /* rear Static/Sliding, front Sliding, front SteeringLimit */
static drift_field s_df[DF_N];

/* the game's value: another car's tune, or one the game wrote (upgrade blend), is the new original */
static float df_orig(const drift_field *f, uint32_t va)
{
    float cur = MEMF(va);
    return f->va == va && cur == f->set ? f->orig : cur;
}

static void df_set(drift_field *f, uint32_t va, float k)
{
    f->orig = df_orig(f, va);
    f->va = va;
    MEMF(va) = f->set = f->orig * k;
}

static void df_back(drift_field *f)
{
    if (f->va && MEMF(f->va) == f->set)
        MEMF(f->va) = f->orig;
    f->va = 0;
}

static float drift_clamp(float x, float lo, float hi) { return x != x ? lo : x < lo ? lo : x > hi ? hi : x; }

static void drift_step(void)
{
    static int env = -1, said = -1;
    static uint32_t seen;                       /* the body whose layout line went out */
    static uint32_t ics_last;                   /* the body, and its velocity as the frame left it (paused: unchanged) */
    static float v_last[3], dv_prev[3], dn_prev, dt_prev, wy_prev;
    static char why[200];
    static float fr = 1.0f, kick_t, lift_t, thr_prev, beta_prev, speed_prev, speed_prev2;
    static float dval, side_acc;                /* controlled: NFSU2's drift value D, the learned steer side */
    static int dstate, side_said;
    static struct { float t, max, v0, g, kept, frmin; int cuts, damps; } dr;   /* one slide (beta > 6 deg, > 8 m/s) */
    static const char *const k_name[4] = { "off", "on", "strong", "controlled" };
    static const char *const k_state[4] = { "out", "enter", "in", "exit" };
    const drift_tune *t;
    int32_t l = mc3_live(MC3_LIVE_DRIFT);
    uint32_t pm = MEM32(0x00558814u), p, veh, sim, tf = 0, tb = 0, car, ph, ics = 0, c, in = 0;
    float v[3], w[3], rt[3], up[3], bk[3], s = 0, beta = 0, wy = 0, thr = 0, brk = 0, hb = 0, st = 0, dt, tgt, amt;
    float vl = 0, vf = 0, grow = 0, cs = 0, ksteer;   /* grow < 0: rotating into the slide; cs: counter-steer 0..1 */
    int hit = 0, frozen = 0;   /* hit: why[] says what was measured */
    int lvl, k;
    if (env < 0) {
        const char *e = getenv("MC3_DRIFT");
        env = e && e[0] >= '1' && e[0] <= '3' ? e[0] - '0' : 0;
    }
    lvl = l ? (l == 1 ? 0 : l == 3 ? 2 : l == 4 ? 3 : env ? env : 1) : env;
    if (online_ram(pm) && online_ram(p = MEM32(pm + 8u)) && online_ram(veh = MEM32(p + 0x14u)) &&
        online_ram(sim = MEM32(veh + 0x50u)) && MEM32(sim) == 0x00456550u &&
        online_ram(tf = MEM32(sim + 0x54u)) && MEM32(tf) == 0x0044D6C8u &&
        online_ram(tb = MEM32(sim + 0x58u)) && MEM32(tb) == 0x0044D6C8u) {
        if (!(online_ram(car = MEM32(veh + 8u)) && online_ram(ph = MEM32(car + 0x68u)) &&
              online_ram(ics = MEM32(ph + 0x10u)) && MEMF(ics + 4u) > 50.0f && MEMF(ics + 4u) < 50000.0f &&
              MEMF(ics + 0x10u) > 0.0f))
            ics = 0;
        if (!(online_ram(c = MEM32(veh + 4u)) && online_ram(in = MEM32(c + 0x34u))))
            in = 0;
    } else
        tf = tb = 0;
    if (lvl != said) {
        if (said >= 0 || lvl)
            fprintf(stderr, "[DRIFT] drift mode %s (MC3_DRIFT%s)\n", k_name[lvl], l ? ", in-game Settings" : "");
        said = lvl;
    }
    if (!lvl || !tb) {
        /* the values go back only to the car they came from (another car's, or a freed one's, are not ours) */
        if (tb && s_df[DF_RK].va == tb + 0x6Cu)
            fprintf(stderr, "[DRIFT] tyres and steering back (WheelBack %08X)\n", (unsigned)tb);
        for (k = 0; k < DF_N; k++) {
            if (!tb || s_df[k].va - (k < DF_FK ? tb : tf) >= 0x100u)
                s_df[k].va = 0;
            df_back(&s_df[k]);
        }
        fr = 1.0f;
        kick_t = lift_t = 0;
        dval = 0;
        dstate = CTL_OUT;
        dr.t = 0;
        if (!tb) {
            drift_score(0, 0, 0, 0, 0, NULL, 0);
            return;
        }
    }
    t = &k_drift[lvl ? lvl - 1 : 0];
    dt = drift_clamp(MEMF(0x004B2D70u), 0.005f, 0.07f);
    if (in) {
        thr = drift_clamp(MEMF(in + 0x70u), 0, 1);
        brk = drift_clamp(MEMF(in + 0x74u), 0, 1);
        hb = drift_clamp(MEMF(in + 0x78u), 0, 1);
        st = drift_clamp(MEMF(in + 0x6Cu), -1, 1);
    }
    if (ics) {
        for (k = 0; k < 3; k++) {
            v[k] = MEMF(ics + 0x44u + 4u * k);
            w[k] = MEMF(ics + 0x50u + 4u * k);
            rt[k] = MEMF(ics + 0x5Cu + 4u * k);
            up[k] = MEMF(ics + 0x68u + 4u * k);
            bk[k] = MEMF(ics + 0x74u + 4u * k);
            s += v[k] * v[k];
            vl += v[k] * rt[k];
            vf += v[k] * bk[k];
            wy += w[k] * up[k];
        }
        s = sqrtf(s);
        {   /* forward = -back row; w x forward is how the heading turns: against v = rotating into the slide */
            float cw[3] = { w[1] * bk[2] - w[2] * bk[1], w[2] * bk[0] - w[0] * bk[2], w[0] * bk[1] - w[1] * bk[0] };
            float turn = -(cw[0] * rt[0] + cw[1] * rt[1] + cw[2] * rt[2]);   /* the heading turning toward the right row */
            grow = -(cw[0] * v[0] + cw[1] * v[1] + cw[2] * v[2]);
            /* which way the wheel turns the car, learned while gripping (no sign guessed for the steer input) */
            if (lvl == 3 && vf < -10.0f && fabsf(vl) < 0.07f * -vf && fabsf(st) > 0.3f && hb < 0.5f && turn == turn) {
                side_acc = drift_clamp(side_acc + st * turn * dt, -5.0f, 5.0f);
                if (!side_said && fabsf(side_acc) >= CTL_SIDE) {
                    side_said = 1;
                    fprintf(stderr, "[DRIFT] controlled: steer side learned: steer %s turns the car toward the body's "
                                    "right row\n", side_acc > 0 ? "+" : "-");
                }
            }
            if (fabsf(side_acc) >= CTL_SIDE && vf < -1.0f) {
                float c = (side_acc > 0 ? st : -st) * (vl > 0 ? 1.0f : -1.0f);   /* > 0: toward the slide */
                cs = c > 0 ? c : 0.0f;
            }
        }
        /* sign-free: forward or back makes no difference to the angle (reversing straight is 0) */
        beta = s > 6.0f ? atan2f(fabsf(vl), fabsf(vf)) * 57.29578f : 0.0f;
        if (beta != beta || s != s)
            beta = s = 0;
        frozen = ics == ics_last && v[0] == v_last[0] && v[1] == v_last[1] && v[2] == v_last[2];
        /* a hit (see the header): v_last is the velocity as the last frame left it, our own writes included */
        if (ics == ics_last && !frozen) {
            float dv[3], dn = 0, d1 = 0, d2 = 0, a1, a2, lim = brk > 0.3f || hb > 0.5f ? DH_BRAKE : 0.0f;
            for (k = 0; k < 3; k++) {
                dv[k] = v[k] - v_last[k];
                dn += dv[k] * up[k];
            }
            for (k = 0; k < 3; k++) {
                float p = dv[k] - dn * up[k], q = p + dv_prev[k];
                d1 += p * p;
                d2 += q * q;
                dv_prev[k] = p;
            }
            d1 = sqrtf(d1);
            d2 = sqrtf(d2);
            a1 = d1 / dt;
            a2 = d2 / (dt + dt_prev);
            if (((d1 >= DH_D1 + lim && d1 > fabsf(dn) && s < speed_prev - DH_DROP) ||
                 (d2 >= DH_D2 + lim && d2 > fabsf(dn + dn_prev) && s < speed_prev2 - DH_DROP))) {
                hit = 1;
                snprintf(why, sizeof why, "hit, %.1f m/s in the ground plane in 1 frame (%.0f m/s^2), %.1f in 2 "
                         "(%.0f m/s^2), %.1f m/s along up; %.0f -> %.0f km/h, yaw rate %+.2f -> %+.2f rad/s, slip "
                         "%.0f deg, brake %.1f handbrake %.1f", (double)d1, (double)a1, (double)d2, (double)a2,
                         (double)dn, (double)(speed_prev * 3.6f), (double)(s * 3.6f), (double)wy_prev, (double)wy,
                         (double)beta, (double)brk, (double)hb);
            }
            dn_prev = dn;
            dt_prev = dt;
        } else if (!frozen) {
            dv_prev[0] = dv_prev[1] = dv_prev[2] = dn_prev = 0;
            dt_prev = dt;
        }
        if (ics != seen) {
            seen = ics;
            fprintf(stderr, "[DRIFT] car: mass %.0f, inertia up %.0f, yaw cap %.1f rad/s; front SteeringLimit %.3f, "
                            "rear StaticFric %.2f SlidingFric %.2f HandbrakeCoef %.2f; inputs %08X (thr %.2f brk %.2f "
                            "hb %.2f steer %+.2f); %s\n", (double)MEMF(ics + 4u), (double)MEMF(ics + 0x10u),
                    (double)MEMF(ics + 0x28u), (double)MEMF(tf + 0x24u), (double)MEMF(tb + 0x68u),
                    (double)MEMF(tb + 0x6Cu), (double)MEMF(tb + 0x38u), (unsigned)in, (double)thr, (double)brk,
                    (double)hb, (double)st, lvl == 3 ? "controlled" : lvl == 2 ? "strong" : "normal");
        }
    }

    if (!lvl)
        goto score;                             /* Drift mode off: the car as the game made it, scoring only */
    /* initiation: a handbrake tap, or the throttle let go (from > 0.6 to < 0.2 within 0.3 s) with the wheel turned */
    lift_t = thr_prev > 0.6f && thr < 0.2f ? 0.3f : lift_t > dt ? lift_t - dt : 0.0f;
    thr_prev = thr;
    if (s > 8.0f && (hb > 0.5f || (lift_t > 0 && fabsf(st) > 0.6f && s > 12.0f)))
        kick_t = 0.35f;
    else
        kick_t = kick_t > dt ? kick_t - dt : 0.0f;

    /* rear grip factor: gripping -> drift over 0..6 deg of slip, the throttle loosens it, past beta_max it grips back */
    if (lvl == 3) {
        /* controlled: NFSU2's drift value and rear grip (see the header, 1 and 3) */
        int fwd = ics && vf < -1.0f, into = fwd && grow < 0;
        float x;
        dval += (dstate == CTL_ENTER || dstate == CTL_IN ? 8.0f : -1.0f) * dt;   /* out -1/s (NFSU2 -2; owner: it straightened too soon) */
        if (dval <= 0) {
            dval = 0;
            dstate = CTL_OUT;
        } else if (dval >= 1) {
            dval = 1;
            dstate = CTL_IN;
        }
        if (dstate == CTL_IN || dstate == CTL_EXIT)
            dstate = (into && s > CTL_SPEED && beta > CTL_SLIP && cs <= 0) ||
                     (fwd && s > CTL_SPEED && thr * cs * beta > CTL_SLIP) ? CTL_IN :
                     fwd && thr * beta > CTL_SLIP ? CTL_ENTER : CTL_EXIT;
        else if (fwd && s > CTL_SPEED && (hb > 0.5f || beta > CTL_SLIP || fabsf(st) > CTL_STEER || kick_t > 0))
            dstate = CTL_ENTER;   /* owner 2026-11-15: steering alone must throw the car (NFSU2's drift events do) */
        x = drift_clamp(dval * (beta / 57.29578f * 0.5f + cs * 4.0f + fabsf(wy) * 0.5f + fabsf(st) * 0.5f), 0, 1) * 9.0f;
        k = x < 8.0f ? (int)x : 8;
        tgt = k_ctl_rear[k] + (k_ctl_rear[k + 1] - k_ctl_rear[k]) * (x - (float)k);
        tgt = 1.0f - (1.0f - tgt / 1.1f) * CTL_REAR_K;
        /* the throw: entering with the car still straight (a steer, a lift-off or a handbrake), the rear lets go
         * at once to CTL_THROW, as On's kick; the table takes over as the slip builds */
        if (dstate == CTL_ENTER && beta < CTL_SLIP && tgt > CTL_THROW)
            tgt = CTL_THROW;
        fr += (tgt - fr) * (1.0f - expf(-dt / CTL_TAU));
    } else {
        float slide = t->slide - t->thr * thr;
        tgt = t->base + (slide - t->base) * drift_clamp(beta / 6.0f, 0, 1);
        if (kick_t > 0 && tgt > t->kick)
            tgt = t->kick;
        if (beta > t->beta_max)
            tgt = slide + (t->catch_hi - slide) * drift_clamp((beta - t->beta_max) / 25.0f, 0, 1);
        fr += (tgt - fr) * drift_clamp(dt / 0.08f, 0, 1);
    }
    df_set(&s_df[DF_RS], tb + 0x68u, 0.6f + 0.4f * fr);
    df_set(&s_df[DF_RK], tb + 0x6Cu, fr);
    amt = s > 8.0f ? 1.0f : 0.0f;
    df_set(&s_df[DF_FK], tf + 0x6Cu, 1.0f + t->front * amt * drift_clamp(beta / 20.0f, 0, 1));
    if (lvl == 3) {
        /* controlled: counter-steer assist (header, 4): full lock reaches at least the slip angle */
        float lim0 = df_orig(&s_df[DF_FSTEER], tf + 0x24u);
        ksteer = cs > 0 && lim0 > 0.05f ?
                 1.0f + (drift_clamp(beta / 57.29578f / lim0, 1.0f, CTL_CS_MAX) - 1.0f) * dval : 1.0f;
    } else
        ksteer = 1.0f + t->steer * amt * drift_clamp(beta / 30.0f, 0, 1);
    df_set(&s_df[DF_FSTEER], tf + 0x24u, ksteer);

    if (ics) {
        float dw = 0, iy = MEMF(ics + 0x10u);
        /* no spin-outs: past beta_max and still growing, cut the yaw rate (angular velocity and momentum together) */
        if (lvl != 3 && beta > t->beta_max && beta > beta_prev) {
            float cut = drift_clamp(t->yaw_cut * drift_clamp((beta - t->beta_max) / 20.0f, 0, 1) * dt * 30.0f, 0, 0.6f);
            dw = -cut * wy;
            dr.cuts++;
        }
        /* controlled: NFSU2's yaw stabilizer (header, 2): damps only the rotation into the slide, harder the wider */
        if (lvl == 3 && dval > 0 && vf < -1.0f && grow < 0 && s > 8.0f && !hit) {
            float br = beta / 57.29578f, sv = 0;
            for (k = 1; k < 7; k++)
                if (br <= k_ctl_stab[k][0]) {
                    sv = k_ctl_stab[k - 1][1] + (k_ctl_stab[k][1] - k_ctl_stab[k - 1][1]) *
                         (br - k_ctl_stab[k - 1][0]) / (k_ctl_stab[k][0] - k_ctl_stab[k - 1][0]);
                    break;
                }
            dw = -wy * drift_clamp(dval * sv * 2.5f * dt, 0, 0.5f);   /* 2.5 (NFSU2 4): less self-straightening */
            if (dw != 0)
                dr.damps++;
        }
        if (dw != 0) {
            for (k = 0; k < 3; k++) {
                MEMF(ics + 0x50u + 4u * k) = w[k] + dw * up[k];
                MEMF(ics + 0x38u + 4u * k) += iy * dw * up[k];
            }
        }
        /* speed: give back part of what the slide scrubbed off since its start. Measured on the slide's net change
         * (g, the game's own frame deltas summed), not per frame: giving back each frame's drop and keeping each
         * rise was a ratchet that turned the physics' frame-to-frame speed jitter into a steady gain. Braking,
         * handbrake and hits stay out of g; never above the speed at the slide's start. */
        if (beta > 6.0f && s > 8.0f && dr.t > 0) {
            float d = s - speed_prev, ns;
            if (brk < 0.3f && hb < 0.5f && !hit)
                dr.g += d;
            ns = s + (dr.g < 0 ? -dr.g * t->keep : 0.0f) - dr.kept;
            if (ns > s && ns > dr.v0)
                ns = s > dr.v0 ? s : dr.v0;
            if (ns != s && ns > 0.0f) {
                float f = ns / s, m = MEMF(ics + 4u);
                for (k = 0; k < 3; k++) {
                    MEMF(ics + 0x44u + 4u * k) = v[k] * f;
                    MEMF(ics + 0x2Cu + 4u * k) = v[k] * f * m;   /* SetVel sub_003432B0: P = v x mass */
                }
                dr.kept += ns - s;
                s = ns;
            }
        }
    }
    /* one line per slide (6 deg and more, above 8 m/s, 0.5 s or longer) */
    if (beta > 6.0f && s > 8.0f) {
        if (dr.t == 0) {
            dr.v0 = s;
            dr.g = 0;
            dr.max = 0;
            dr.kept = 0;
            dr.cuts = 0;
            dr.damps = 0;
            dr.frmin = fr;
        }
        dr.t += dt;
        dr.max = beta > dr.max ? beta : dr.max;
        dr.frmin = fr < dr.frmin ? fr : dr.frmin;
    } else if (dr.t > 0) {
        char ext[96] = "";
        if (lvl == 3)
            snprintf(ext, sizeof ext, "; controlled: stabilizer %d frames, steer side %s, drift value %.2f (%s)",
                     dr.damps, side_said ? "learned" : "not learned yet", (double)dval, k_state[dstate]);
        if (dr.t >= 0.5f)
            fprintf(stderr, "[DRIFT] slide: start %.0f km/h, end %.0f km/h, give-back %.1f km/h; %.1f s, max %.0f deg, "
                            "rear grip min x%.2f, yaw cut %d frames, yaw rate %+.2f rad/s at the end%s\n",
                    (double)(dr.v0 * 3.6f), (double)(speed_prev * 3.6f), (double)(dr.kept * 3.6f), (double)dr.t,
                    (double)dr.max, (double)dr.frmin, dr.cuts, (double)wy, ext);
        dr.t = 0;
    }
score:
    drift_score(ics != 0, beta, s, ics ? v[1] : 0.0f, dt, hit ? why : NULL, frozen);
    if (ics) {
        ics_last = ics;
        for (k = 0, wy_prev = 0; k < 3; k++) {
            v_last[k] = MEMF(ics + 0x44u + 4u * k);
            wy_prev += MEMF(ics + 0x50u + 4u * k) * up[k];   /* after the yaw cut */
        }
    }
    beta_prev = beta;
    speed_prev2 = speed_prev;
    speed_prev = s;
}

/* Drift camera (MC3_DRIFT_CAM=1; live word MC3_LIVE_DRIFT_CAM 1 off / 2 on; any Drift mode level, off too;
 * 2026-11-15, untested in game). Around the race chase camera's update (vtable 0x00476BEC, slot 10 sub_002CF760)
 * for player 0's car. That update's pose (sub_00355650, 0x003557F3) puts the camera behind the body's velocity
 * (ics +0x44, -normalize) when the car is on the ground above 10 m/s, else behind its heading: in a slide it looks
 * along the slide, the car shown side on. Here, while the update runs, the velocity reads turned from the heading
 * toward the real one by m x the slip (m 1 gripping -> 0.65 at 35 deg of slip and more; same speed and vertical
 * part, so the update's speed tests do not change), FOV offset +0x144 + 5 deg x k and distance scale +0x148 x
 * (1 + 0.15 k), k 0..1 from 10 to 35 deg of slip; m and k follow their targets over 0.25 s of the frame time
 * (0x4B2D70: the step, 1/30, 1/60, 1/N s), so every frame rate moves alike. Every value is put back after the
 * update (the camera runs after the frame's physics: the car never sees them). Not while paused or in state 3.
 * Unproven: that the chase camera's FOV (+0x84) reaches the projection (MC3_CAM_TRACE=1 '[CAM] projection' lines:
 * if fov does not move with k, the delta goes on sub_002D4AE0's fov argument instead). '[DRIFT] camera on|off'. */
static void drift_cam(void)
{
    static int env = -1, said = -1;
    static float m = 1.0f, kf;                  /* the smoothed share of the slip the camera turns, the slip factor */
    uint32_t cam = g_ecx, pm, sm, p, veh, car, ph, ics = 0;
    int32_t l = mc3_live(MC3_LIVE_DRIFT_CAM);
    int on, k, swap = 0, fk = 0;
    float v[3], bk[3], fx, fz, n, sh, beta, mt = 1.0f, kt = 0.0f, a, dt, fov = 0, dsc = 0;
    if (env < 0) {
        const char *e = getenv("MC3_DRIFT_CAM");
        env = e && e[0] == '1';
    }
    on = l ? l == 2 : env;
    if (on != said) {
        if (said >= 0 || on)
            fprintf(stderr, "[DRIFT] camera %s (MC3_DRIFT_CAM%s)\n", on ? "on" : "off", l ? ", in-game Settings" : "");
        said = on;
        m = 1.0f;
        kf = 0.0f;
    }
    if (on && online_ram(cam) && MEM32(cam) == 0x00476BECu && online_ram(pm = MEM32(0x00558814u)) &&
        online_ram(p = MEM32(pm + 8u)) && online_ram(veh = MEM32(p + 0x14u)) && MEM32(cam + 0x130u) == veh &&
        online_ram(sm = MEM32(0x00557ED0u)) && MEM32(sm + 4u) != 3u && MEM8(sm + 9u) == 0 &&
        online_ram(car = MEM32(veh + 8u)) && online_ram(ph = MEM32(car + 0x68u)) && online_ram(ics = MEM32(ph + 0x10u))) {
        dt = drift_clamp(MEMF(0x004B2D70u), 0.005f, 0.07f);
        for (k = 0; k < 3; k++) {
            v[k] = MEMF(ics + 0x44u + 4u * k);
            bk[k] = MEMF(ics + 0x74u + 4u * k);
        }
        fx = -bk[0];
        fz = -bk[2];
        n = sqrtf(fx * fx + fz * fz);
        sh = sqrtf(v[0] * v[0] + v[2] * v[2]);
        if (n > 0.1f && sh >= 10.5f && sh == sh) {
            fx /= n;
            fz /= n;
            /* slip measured toward the side (-fz, fx) and the direction turned toward that same side: m = 1 gives
             * the velocity back exactly, whatever the rows' handedness */
            beta = atan2f(v[2] * fx - v[0] * fz, v[0] * fx + v[2] * fz);
            if (fabsf(beta) <= 100.0f / 57.29578f) {   /* past it: reversing or spun, the game's own */
                float bd = fabsf(beta) * 57.29578f;
                swap = 1;
                mt = 1.0f - 0.35f * drift_clamp((bd - 5.0f) / 30.0f, 0, 1);
                kt = drift_clamp((bd - 10.0f) / 25.0f, 0, 1);
            }
        }
        a = 1.0f - expf(-dt / 0.25f);
        m += (mt - m) * a;
        kf += (kt - kf) * a;
        if (swap && fabsf(1.0f - m) > 1e-4f) {
            float r = m * beta, c = cosf(r), s = sinf(r);
            MEMF(ics + 0x44u) = (c * fx - s * fz) * sh;
            MEMF(ics + 0x4Cu) = (c * fz + s * fx) * sh;
        } else
            swap = 0;
        if (kf > 1e-3f) {
            fk = 1;
            fov = MEMF(cam + 0x144u);
            dsc = MEMF(cam + 0x148u);
            MEMF(cam + 0x144u) = fov + 5.0f * kf;
            MEMF(cam + 0x148u) = dsc * (1.0f + 0.15f * kf);
        }
    }
    cam_call(13, 0x002CF760u, sub_002CF760_gen);
    if (swap) {
        MEMF(ics + 0x44u) = v[0];
        MEMF(ics + 0x4Cu) = v[2];
    }
    if (fk) {
        MEMF(cam + 0x144u) = fov;
        MEMF(cam + 0x148u) = dsc;
    }
}
