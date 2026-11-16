/* orbit_cam.c -- mouse orbit camera (browser only; 2026-11-16, untested in game). #included by drift.c, before
 * drift_cam, which calls orbit_pre / orbit_post around the race chase camera's update (vtable 0x00476BEC, slot 10
 * sub_002CF760). Needs mc3_live, MEM*, online_ram, drift_clamp.
 *
 * The page (web/pre.js, Orbit camera block) turns a left-button drag on the picture (or a finger that lands on the
 * picture itself, not a touch control) into running totals in mc3_live_box: MC3_LIVE_ORBIT_YAW / _PITCH (radians
 * x 1000), _ZOOM (wheel, distance factor x 1000), _HELD (1 while the button or finger is down). Only the change
 * between two reads counts, so the game owns the offsets: yaw (any angle), pitch (elevation), zoom 0.7..1.6. 1.5 s
 * after the last move with nothing held, yaw and pitch ease back to 0 (0.35 s), not while the car stands (< 1 m/s
 * horizontal). Zoom stays. More than 0.5 s without a chase-camera frame (menus, pause, garage) drops what the page
 * sent meanwhile and the offsets.
 *
 * How the camera is moved (read from the lifted sub_00355650, the pose step the update reaches via sub_00357960):
 * back direction d = -normalize(ics +0x44) on the ground above 10 m/s, else row +0x18 of the 3x4 matrix at
 * [cam+0x6C] (rows +0, +0xC, +0x18, position +0x24; the look-at target is that matrix x (cam+0x98, [car]+0x54,
 * cam+0xA0)); with U the world up 0x4B00E4, R = norm(U x d), C = U x R (= -d's horizontal part):
 * position = target + R [cam+0x150] - C [cam+0x158] + (0, [cam+0x154], 0), looking at the target (sub_00087E00).
 * So +0x158 is the distance behind along the ground, +0x154 the height. Yaw: the velocity's horizontal part and
 * the matrix's three rows are turned about world y by the yaw (whichever path the update takes, d turns with them;
 * the whole car turned, so the target offset turns too). Pitch and zoom: (+0x158, +0x154) set to the same
 * elevation + pitch (kept within -3 .. 69 deg) at the distance x zoom. Everything is put back after the update.
 * Unproven: which way a positive yaw turns on screen (y up assumed; flip the yaw's sign in web/pre.js if a drag
 * turns the wrong way), whether the update's later steps (sub_00356210, camera collision) pull a low camera back.
 * MC3_ORBIT_CAM=0 off (page 'Orbit camera'; live word MC3_LIVE_ORBIT 1 off / 2 on). '[ORBIT] camera on|off'. */
#ifdef __EMSCRIPTEN__
static struct {
    float yaw, pitch, zoom, idle;
    int32_t py, pp, pz;                         /* the page's totals last read */
    double seen;                                /* emscripten_get_now() of the last orbit frame */
    int rot, elev;                              /* what orbit_pre changed: put back by orbit_post */
    uint32_t cam, ics, mtx;
    float v[2], rows[6], d, h;
} s_orb = { .zoom = 1.0f };

static void orbit_pre(uint32_t cam)
{
    static int env = -1, said = -1;
    int32_t l = mc3_live(MC3_LIVE_ORBIT), y = mc3_live(MC3_LIVE_ORBIT_YAW), p = mc3_live(MC3_LIVE_ORBIT_PITCH),
            z = mc3_live(MC3_LIVE_ORBIT_ZOOM);
    uint32_t pm, pl, veh, car, ph, ics, sm, mtx;
    double now = emscripten_get_now();
    int on, k, moved;
    float dt, c, s, sp, r, a0, d, h;
    s_orb.rot = s_orb.elev = 0;
    if (env < 0) {
        const char *e = getenv("MC3_ORBIT_CAM");
        env = !(e && e[0] == '0');
    }
    on = l ? l == 2 : env;
    if (on != said) {
        fprintf(stderr, "[ORBIT] camera %s (MC3_ORBIT_CAM%s)\n", on ? "on" : "off", l ? ", in-game Settings" : "");
        said = on;
    }
    if (!on || !online_ram(cam) || MEM32(cam) != 0x00476BECu || !online_ram(pm = MEM32(0x00558814u)) ||
        !online_ram(pl = MEM32(pm + 8u)) || !online_ram(veh = MEM32(pl + 0x14u)) || MEM32(cam + 0x130u) != veh ||
        !online_ram(sm = MEM32(0x00557ED0u)) || MEM32(sm + 4u) == 3u || MEM8(sm + 9u) != 0 ||
        !online_ram(car = MEM32(veh + 8u)) || !online_ram(ph = MEM32(car + 0x68u)) ||
        !online_ram(ics = MEM32(ph + 0x10u)) || !online_ram(mtx = MEM32(cam + 0x6Cu)))
        return;
    if (now - s_orb.seen > 500.0) {             /* back from menus or pause: start behind the car */
        s_orb.py = y;
        s_orb.pp = p;
        s_orb.pz = z;
        s_orb.yaw = s_orb.pitch = s_orb.idle = 0;
    }
    s_orb.seen = now;
    dt = drift_clamp(MEMF(0x004B2D70u), 0.005f, 0.07f);
    moved = y != s_orb.py || p != s_orb.pp || z != s_orb.pz;
    s_orb.yaw += (float)(y - s_orb.py) * 0.001f;
    s_orb.pitch += (float)(p - s_orb.pp) * 0.001f;
    s_orb.zoom = drift_clamp(s_orb.zoom + (float)(z - s_orb.pz) * 0.001f, 0.7f, 1.6f);
    s_orb.py = y;
    s_orb.pp = p;
    s_orb.pz = z;
    s_orb.yaw = remainderf(s_orb.yaw, 6.2831853f);   /* (-pi, pi]: the way back is the short one */
    s_orb.idle = moved || mc3_live(MC3_LIVE_ORBIT_HELD) == 1 ? 0.0f : s_orb.idle + dt;
    sp = sqrtf(MEMF(ics + 0x44u) * MEMF(ics + 0x44u) + MEMF(ics + 0x4Cu) * MEMF(ics + 0x4Cu));
    if (s_orb.idle > 1.5f && sp >= 1.0f) {
        float a = 1.0f - expf(-dt / 0.35f);
        s_orb.yaw -= s_orb.yaw * a;
        s_orb.pitch -= s_orb.pitch * a;
        if (fabsf(s_orb.yaw) < 1e-3f && fabsf(s_orb.pitch) < 1e-3f)
            s_orb.yaw = s_orb.pitch = 0;
    }
    s_orb.cam = cam;
    if (s_orb.yaw != 0) {
        c = cosf(s_orb.yaw);
        s = sinf(s_orb.yaw);
        s_orb.rot = 1;
        s_orb.ics = ics;
        s_orb.mtx = mtx;
        s_orb.v[0] = MEMF(ics + 0x44u);         /* already turned by the drift camera, if on: drift_cam puts its own back */
        s_orb.v[1] = MEMF(ics + 0x4Cu);
        MEMF(ics + 0x44u) = c * s_orb.v[0] - s * s_orb.v[1];
        MEMF(ics + 0x4Cu) = s * s_orb.v[0] + c * s_orb.v[1];
        for (k = 0; k < 3; k++) {
            float x = MEMF(mtx + 0xCu * k), w = MEMF(mtx + 0xCu * k + 8u);
            s_orb.rows[2 * k] = x;
            s_orb.rows[2 * k + 1] = w;
            MEMF(mtx + 0xCu * k) = c * x - s * w;
            MEMF(mtx + 0xCu * k + 8u) = s * x + c * w;
        }
    }
    d = MEMF(cam + 0x158u);
    h = MEMF(cam + 0x154u);
    r = sqrtf(d * d + h * h);
    if ((s_orb.pitch != 0 || s_orb.zoom != 1.0f) && r > 0.1f && r < 1000.0f) {
        a0 = atan2f(h, fabsf(d));
        s_orb.pitch = drift_clamp(s_orb.pitch, fminf(0, -0.05f - a0), fmaxf(0, 1.2f - a0));
        s_orb.elev = 1;
        s_orb.d = d;
        s_orb.h = h;
        r *= s_orb.zoom;
        MEMF(cam + 0x158u) = copysignf(r * cosf(a0 + s_orb.pitch), d);
        MEMF(cam + 0x154u) = r * sinf(a0 + s_orb.pitch);
    }
}

static void orbit_post(void)
{
    int k;
    if (s_orb.rot) {
        MEMF(s_orb.ics + 0x44u) = s_orb.v[0];
        MEMF(s_orb.ics + 0x4Cu) = s_orb.v[1];
        for (k = 0; k < 3; k++) {
            MEMF(s_orb.mtx + 0xCu * k) = s_orb.rows[2 * k];
            MEMF(s_orb.mtx + 0xCu * k + 8u) = s_orb.rows[2 * k + 1];
        }
    }
    if (s_orb.elev) {
        MEMF(s_orb.cam + 0x158u) = s_orb.d;
        MEMF(s_orb.cam + 0x154u) = s_orb.h;
    }
    s_orb.rot = s_orb.elev = 0;
}
#else
static void orbit_pre(uint32_t cam) { (void)cam; }
static void orbit_post(void) {}
#endif
