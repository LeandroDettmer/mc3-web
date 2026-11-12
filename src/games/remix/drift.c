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
 * Levels: MC3_DRIFT=1 normal, =2 strong; the page's live word: 1 off, 2 on
 * (the environment's level, normal if that is off), 3 strong. Off: every
 * value back (if still ours). '[DRIFT]' lines: on/off, the car's layout once
 * per car, one line per drift of 0.5 s or more. Drift scoring (drift_score.c)
 * gets this frame's slip angle, speed and wall-hit test with the mode on or off. */
#include <math.h>

static int online_ram(uint32_t va);
static void drift_score(int car, float beta, float b180, float s, float vy, float dt, int hit, int frozen);

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
static const drift_tune k_drift[2] = {
    /* base   kick   slide  thr    catch  bmax   steer  front  yaw    keep */
    { 0.92f, 0.50f, 0.62f, 0.10f, 1.05f, 40.0f, 0.60f, 0.15f, 0.25f, 0.50f },   /* 1 normal */
    { 0.85f, 0.42f, 0.52f, 0.12f, 1.00f, 50.0f, 1.00f, 0.20f, 0.35f, 0.70f },   /* 2 strong */
};

/* one tune value we scale: the game's (orig) and ours (set) */
typedef struct { uint32_t va; float orig, set; } drift_field;
enum { DF_RS, DF_RK, DF_FK, DF_FSTEER, DF_N };   /* rear Static/Sliding, front Sliding, front SteeringLimit */
static drift_field s_df[DF_N];

static void df_set(drift_field *f, uint32_t va, float k)
{
    float cur = MEMF(va);
    if (f->va != va || cur != f->set)   /* another car's tune, or the game wrote it (upgrade blend): its value */
        f->orig = cur;
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
    static float v_last[3];
    static float fr = 1.0f, kick_t, lift_t, thr_prev, beta_prev, speed_prev;
    static struct { float t, max, v0, g, kept, frmin; int cuts; } dr;   /* one slide (beta > 6 deg, > 8 m/s) */
    const drift_tune *t;
    int32_t l = mc3_live(MC3_LIVE_DRIFT);
    uint32_t pm = MEM32(0x00558814u), p, veh, sim, tf = 0, tb = 0, car, ph, ics = 0, c, in = 0;
    float v[3], w[3], rt[3], up[3], bk[3], s = 0, beta = 0, b180 = 0, wy = 0, thr = 0, brk = 0, hb = 0, st = 0, dt, tgt,
          amt;
    int hit = 0, frozen = 0;
    int lvl, k;
    if (env < 0) {
        const char *e = getenv("MC3_DRIFT");
        env = e && (e[0] == '1' || e[0] == '2') ? e[0] - '0' : 0;
    }
    lvl = l ? (l == 1 ? 0 : l == 3 ? 2 : env ? env : 1) : env;
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
            fprintf(stderr, "[DRIFT] drift mode %s (MC3_DRIFT%s)\n", lvl == 2 ? "strong" : lvl ? "on" : "off",
                    l ? ", in-game Settings" : "");
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
        dr.t = 0;
        if (!tb) {
            drift_score(0, 0, 0, 0, 0, 0, 0, 0);
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
        float vl = 0, vf = 0;
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
        /* sign-free: forward or back makes no difference to the angle (reversing straight is 0) */
        beta = s > 6.0f ? atan2f(fabsf(vl), fabsf(vf)) * 57.29578f : 0.0f;
        b180 = s > 6.0f ? atan2f(fabsf(vl), -vf) * 57.29578f : 0.0f;   /* 0..180: past 90 the car goes tail first */
        if (beta != beta || s != s || b180 != b180)
            beta = b180 = s = 0;
        frozen = ics == ics_last && v[0] == v_last[0] && v[1] == v_last[1] && v[2] == v_last[2];
        /* a wall or car: speed lost faster than 15 m/s^2 (25 while braking) */
        hit = ics == ics_last && s - speed_prev < (brk > 0.3f ? -25.0f : -15.0f) * dt;
        if (ics != seen) {
            seen = ics;
            fprintf(stderr, "[DRIFT] car: mass %.0f, inertia up %.0f, yaw cap %.1f rad/s; front SteeringLimit %.3f, "
                            "rear StaticFric %.2f SlidingFric %.2f HandbrakeCoef %.2f; inputs %08X (thr %.2f brk %.2f "
                            "hb %.2f steer %+.2f); %s\n", (double)MEMF(ics + 4u), (double)MEMF(ics + 0x10u),
                    (double)MEMF(ics + 0x28u), (double)MEMF(tf + 0x24u), (double)MEMF(tb + 0x68u),
                    (double)MEMF(tb + 0x6Cu), (double)MEMF(tb + 0x38u), (unsigned)in, (double)thr, (double)brk,
                    (double)hb, (double)st, lvl == 2 ? "strong" : "normal");
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
    {
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
    df_set(&s_df[DF_FSTEER], tf + 0x24u, 1.0f + t->steer * amt * drift_clamp(beta / 30.0f, 0, 1));

    if (ics) {
        /* no spin-outs: past beta_max and still growing, cut the yaw rate (angular velocity and momentum together) */
        if (beta > t->beta_max && beta > beta_prev) {
            float cut = drift_clamp(t->yaw_cut * drift_clamp((beta - t->beta_max) / 20.0f, 0, 1) * dt * 30.0f, 0, 0.6f);
            float dw = -cut * wy, iy = MEMF(ics + 0x10u);
            for (k = 0; k < 3; k++) {
                MEMF(ics + 0x50u + 4u * k) = w[k] + dw * up[k];
                MEMF(ics + 0x38u + 4u * k) += iy * dw * up[k];
            }
            dr.cuts++;
        }
        /* speed: give back part of what the slide scrubbed off since its start. Measured on the slide's net change
         * (g, the game's own frame deltas summed), not per frame: giving back each frame's drop and keeping each
         * rise was a ratchet that turned the physics' frame-to-frame speed jitter into a steady gain. Braking,
         * handbrake and wall hits (> 15 m/s^2) stay out of g; never above the speed at the slide's start. */
        if (beta > 6.0f && s > 8.0f && dr.t > 0) {
            float d = s - speed_prev, ns;
            if (brk < 0.3f && hb < 0.5f && d > -15.0f * dt)
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
            dr.frmin = fr;
        }
        dr.t += dt;
        dr.max = beta > dr.max ? beta : dr.max;
        dr.frmin = fr < dr.frmin ? fr : dr.frmin;
    } else if (dr.t > 0) {
        if (dr.t >= 0.5f)
            fprintf(stderr, "[DRIFT] slide: start %.0f km/h, end %.0f km/h, give-back %.1f km/h; %.1f s, max %.0f deg, "
                            "rear grip min x%.2f, yaw cut %d frames, yaw rate %+.2f rad/s at the end\n",
                    (double)(dr.v0 * 3.6f), (double)(speed_prev * 3.6f), (double)(dr.kept * 3.6f), (double)dr.t,
                    (double)dr.max, (double)dr.frmin, dr.cuts, (double)wy);
        dr.t = 0;
    }
score:
    drift_score(ics != 0, beta, b180, s, ics ? v[1] : 0.0f, dt, hit, frozen);
    if (ics) {
        ics_last = ics;
        for (k = 0; k < 3; k++)
            v_last[k] = MEMF(ics + 0x44u + 4u * k);
    }
    beta_prev = beta;
    speed_prev = s;
}
