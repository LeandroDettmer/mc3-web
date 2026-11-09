/* Free roam: each remote player's marker and name over its car.
 *
 * Included by recomp_manual.c after roam_cars.c (it reads their slots and
 * standalone cars).
 *
 * The marker is the game's own. The race object ([[0x533E24]+4]) draws, from
 * its draw method (vtable +0x110, sub_001CF3B0, called by the player
 * manager's draw sub_002BE8E0 with the viewer's index), its icons (vtable
 * +0x114): System Link races put an opponent arrow over every other player
 * (sub_001CBA50, sub_001C4480) with the car's icon method sub_000FA070
 * (render object [veh+0xC], thiscall (colour, type, name, pulse, blink,
 * force), ret 0x18). Type 1 is fx/fx_opponent_icon ([0x533E84], loaded by
 * sub_001C8830), a small spinning arrow in mcIconGlobal's m_iconColor<n>
 * ([0x50A098]+0xAC + 16n, RGBA floats) at the car's top + m_iconHeight
 * (+0x274) + 0.125 m, sized with the distance (about constant on screen,
 * times +0x278), invisible within 25 m, full from 100 m, gone at 300 m
 * (alpha (d-25)/75 below 100, 1-(d-200)/100 above 200). It takes the
 * player's name (the session member's gamertag, +0xA8) but never draws
 * it: the Remix shows no names over cars. The cruise's race draws arrows
 * for its own racer list only (a session-less cruise skips the players'
 * loop), and standalone cars (roam_cars.c) are no players at all, so after
 * the race's draw every driven remote car the game did not mark this frame
 * (sub_000FA070 calls are watched) gets the same arrow, with the colour the
 * game gave a remote slot (else 0, a System Link race's), under the same
 * render state and the same gate (the viewer's HUD +0x58, flag +0x8C).
 *
 * The name (browser only): each frame the main camera's view and
 * projection (sub_002D4AE0's override) put the arrow's top on the picture
 * (0..1 across), published in roam_tags_box with the arrow's alpha and
 * colour, and the page draws the names there (web/pre.js, "Name tags"):
 * no background, the arrow's colour, fading as it does.
 *
 * The camera: a camera object applies itself to its viewport by setting
 * the camera matrix (sub_002D3DA0: the view matrix, its inverse, into
 * [0x559758], 4x4, row vectors as D3D) and then building the viewport's
 * projection (sub_002D4AE0, viewport +4, 4x4: w = view z; the D3D viewport
 * X, Y, W, H at +0x160, the fov in degrees at +0x178). Callers of the
 * builder from the camera classes (sub_00138510, sub_00138750) are taken;
 * of those, the biggest viewport (the mirror and the minimap are smaller).
 * Hor+ (WebGPU): the projection the title built is the 16:9 one and the
 * renderer squeezes x by 1/k (nv2a_wgpu_wide), so the picture's x is
 * x_ndc / k.
 *
 * Shown: slots and standalone cars that are driven (their peer here, not in
 * its garage, the car drawn); names none while not driving (paused, menus,
 * garage: remix_driving) or with MC3_ROAM_FAKE. Always published (a few
 * dozen multiplies): the page's MC3_NAMETAGS=0 only stops drawing the
 * names, so it can turn on and off live; the arrows stay, as the game's
 * own do. Names are not hidden behind buildings (no depth test).
 * MC3_ROAM_TRACE=1: once a second, where our own car projects (expect the
 * middle, lower half), and how many arrows the game drew itself. */

#define TAGS_MAX       32
#define TAGS_NAME      28
#define TAGS_VIEW_VA   0x00559758u      /* the view matrix, set from the camera (sub_002D3DA0) */
#define TAGS_CAM_VA    0x00559890u      /* the camera's place, as sub_000FA070 reads it */
#define TAGS_ICON_VA   0x0050A098u      /* mcIconGlobal: m_iconColor<n> at +0xAC */
#define TAGS_ARROW_H   1.0f             /* shortcut: the arrow model's height in its units, guessed; read its bounds if names sit off */

void sub_002D3F40(void);                        /* render state, cdecl (bool) */
void sub_002D3840(void);                        /* render state, cdecl (bool) */
extern void sub_000FA070_gen(void);             /* a car's icon, thiscall (colour, type, name, pulse, blink, force), ret 0x18 */

static uint32_t s_icon_seen[64];                /* render objects the game marked during this race draw */
static int s_icon_nseen, s_icon_in;
static int32_t s_icon_col;                      /* the colour the game gave a remote slot (0 until seen) */

/* The vehicle of slot k (0 none). */
static uint32_t tags_slot_veh(int k)
{
    uint32_t pm = MEM32(ROAM_PM_VA), p;
    if (!online_ram(pm) || !online_ram(p = MEM32(pm + 8u + 4u * (uint32_t)k)))
        return 0;
    return online_ram(MEM32(p + 0x14u)) ? MEM32(p + 0x14u) : 0;
}

/* Remote cars driven now: their vehicles and names, at most max. */
static int tags_cars(uint32_t *veh, const char **name, int max)
{
    int n = 0, j;
    for (j = 1; j <= s_roam_slots && n < max; j++)
        if (s_roam_peer[j].id && s_roam_peer[j].here && !s_roam_peer[j].away && s_roam_peer[j].live &&
            (veh[n] = tags_slot_veh(j)) != 0)
            name[n++] = s_roam_peer[j].name;
    for (j = 0; j < s_ncar && n < max; j++)
        if (s_car[j].veh && s_car[j].id && s_car[j].here && !s_car[j].away && s_car[j].live) {
            veh[n] = s_car[j].veh;
            name[n++] = s_car[j].name;
        }
    return n;
}

/* sub_000FA070 is called for render object r; sp: its esp (colour at +4, type at +8). */
static void roam_icon_seen(uint32_t r, uint32_t sp)
{
    uint32_t v;
    int k;
    if (!s_icon_in || MEM32(sp + 8u) != 1u)
        return;
    if (s_icon_nseen < (int)(sizeof s_icon_seen / sizeof s_icon_seen[0]))
        s_icon_seen[s_icon_nseen++] = r;
    for (k = 1; k <= s_roam_slots; k++)
        if (s_roam_peer[k].id && (v = tags_slot_veh(k)) != 0 && MEM32(v + 0xCu) == r)
            s_icon_col = (int32_t)MEM32(sp + 4u);
}

static void roam_icons_begin(void)
{
    s_icon_in = 1;
    s_icon_nseen = 0;
}

/* After the race's draw (sub_001CF3B0) for viewer vp; top: the caller's esp at the call (scratch below). */
static void roam_icons_end(uint32_t vp, uint32_t top)
{
    uint32_t veh[TAGS_MAX], pm = MEM32(ROAM_PM_VA), pl, hud, r, a[6] = { 0, 1, 0, 0, 0, 0 }, zero = 0;
    const char *name[TAGS_MAX];
    int n, i, k, set = 0;
    s_icon_in = 0;
    if (!s_online || !roam_running() || !online_ram(pm) || vp > 7u || !online_ram(pl = MEM32(pm + 8u + 4u * vp)) ||
        !online_ram(hud = MEM32(pl + 0x58u)) || !MEM8(hud + 0x8Cu))
        return;                                 /* the game's gate: no icons for this viewer now */
    n = tags_cars(veh, name, TAGS_MAX);
    a[0] = (uint32_t)(s_icon_col >= 0 && s_icon_col < 8 ? s_icon_col : 0);
    for (i = 0; i < n; i++) {
        if (!online_ram(r = MEM32(veh[i] + 0xCu)) || !MEM8(r + 0x12Du))
            continue;                           /* not drawn: the game skips it too */
        for (k = 0; k < s_icon_nseen && s_icon_seen[k] != r; k++)
            ;
        if (k < s_icon_nseen)
            continue;                           /* the game marked it */
        if (!set) {
            set = 1;                            /* the icon methods' render state */
            roam_call(sub_002D3F40, 0, top, 1, &zero);
            roam_call(sub_002D3840, 0, top, 1, &zero);
        }
        roam_call(sub_000FA070_gen, r, top, 6, a);
    }
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
extern float nv2a_wgpu_wide(void);              /* xboxrecomp nv2a_wgpu: Hor+ widening, 1 when off */

typedef struct { float u, v, alpha; char name[TAGS_NAME]; } roam_tag;
_Static_assert(ROAM_NAME_MAX < TAGS_NAME, "a name and its NUL fit");
_Static_assert(sizeof(roam_tag) == 40, "web/pre.js reads 40-byte tags");

/* Read by the page (web/pre.js): seq +1 per update, n tags, the picture's
 * width / height, the arrow's colour (0xRRGGBB), then the tags. A read
 * during a write may mix two frames. */
static struct {
    volatile int32_t seq;
    int32_t n;
    float aspect;
    int32_t rgb;
    roam_tag t[TAGS_MAX];
} s_tags;
EMSCRIPTEN_KEEPALIVE uintptr_t roam_tags_box(void) { return (uintptr_t)&s_tags; }

static int tags_env(const char *name, int def)
{
    const char *e = getenv(name);
    return e && *e ? e[0] != '0' : def;
}

/* p through view then projection; 1 when in front and on the picture. */
static int tags_proj(const float vw[16], const float pr[16], const float p[3], float k, float *u, float *v,
                     float *w)
{
    float x[4] = { p[0], p[1], p[2], 1.0f }, e[4], c[4];
    int i, j;
    for (j = 0; j < 4; j++)
        for (e[j] = 0.0f, i = 0; i < 4; i++)
            e[j] += x[i] * vw[4 * i + j];
    for (j = 0; j < 4; j++)
        for (c[j] = 0.0f, i = 0; i < 4; i++)
            c[j] += e[i] * pr[4 * i + j];
    if (c[3] < 0.5f)
        return 0;                               /* behind the camera, or at it */
    *u = 0.5f + 0.5f * c[0] / c[3] / k;
    *v = 0.5f - 0.5f * c[1] / c[3];
    *w = c[3];
    return *u > -0.05f && *u < 1.05f && *v > -0.05f && *v < 1.05f;
}

static float tags_clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

/* The arrow over vehicle veh as sub_000FA070 places it (type 1): its top through the camera, its alpha. */
static void tags_add(int *n, const float vw[16], const float pr[16], float k, float th, uint32_t veh,
                     const char *name)
{
    uint32_t r = MEM32(veh + 0xCu), b;
    float p[3], d = 0.0f, s, fade, top = 0.0f, u, v, w;
    roam_tag *t;
    int i;
    if (*n >= TAGS_MAX || !online_ram(r) || !MEM8(r + 0x12Du))
        return;
    for (i = 0; i < 3; i++) {
        p[i] = MEMF(r + 0xC4u + 4u * i);
        d += (MEMF(TAGS_CAM_VA + 4u * i) - p[i]) * (MEMF(TAGS_CAM_VA + 4u * i) - p[i]);
    }
    d = sqrtf(d);
    fade = d < 100.0f ? tags_clamp01((d - 25.0f) / 75.0f) : 1.0f - tags_clamp01((d - 200.0f) / 100.0f);
    if (fade <= 0.01f)
        return;
    /* the car model's bounds (sub_000A2450 on [[r+4]+8]): +0x18 its top */
    if (online_ram(b = MEM32(r + 4u)) && online_ram(b = MEM32(b + 8u)) && online_ram(b = MEM32(b + 4u)) &&
        online_ram(b = MEM32(b + 0x1Cu)) && online_ram(b = MEM32(b + 4u)) && online_ram(b = MEM32(b + 0xCu)))
        top = MEMF(b + 0x18u);
    s = ((0.03f - 0.01f * tags_clamp01(d / 600.0f)) * d * th + 0.25f) * MEMF(r + 0x278u);
    p[1] += MEMF(r + 0x274u) + top + 0.125f + s * TAGS_ARROW_H;
    if (!tags_proj(vw, pr, p, k, &u, &v, &w))
        return;
    t = &s_tags.t[(*n)++];
    t->u = u;
    t->v = v;
    t->alpha = fade;
    snprintf(t->name, sizeof t->name, "%s", name[0] ? name : "Player");
}

/* After the projection builder ran for viewport vp, called from ret. */
static void roam_tags_view(uint32_t vp, uint32_t ret)
{
    static int trace = -1, found;
    static uint32_t best;
    static double best_t, trace_t;
    uint32_t veh[TAGS_MAX], area, i, g;
    const char *name[TAGS_MAX];
    float vw[16], pr[16], k, fov, th;
    double now;
    int n = 0, nc, j;
    if (ret != 0x0013853Au && ret != 0x0013879Fu && ret != 0x001387BEu)
        return;                                 /* not a camera applying itself */
    if (trace < 0)
        trace = tags_env("MC3_ROAM_TRACE", 0);
    if (!s_online || !roam_running() || roam_fake() || !remix_driving()) {
        if (s_tags.n) {
            s_tags.n = 0;
            s_tags.seq++;
        }
        return;
    }
    now = roam_now();
    area = MEM32(vp + 0x168u) * MEM32(vp + 0x16Cu);
    if (area < best && now - best_t < 0.25)
        return;                                 /* a smaller view this frame: the mirror */
    best = area;
    best_t = now;
    for (i = 0; i < 16; i++) {
        vw[i] = MEMF(TAGS_VIEW_VA + 4u * i);
        pr[i] = MEMF(vp + 4u + 4u * i);
    }
    k = nv2a_wgpu_wide();
    if (!(k >= 1.0f))
        k = 1.0f;
    fov = MEMF(vp + 0x178u);
    if (!(fov > 1.0f && fov < 170.0f))
        fov = 60.0f;
    th = tanf(fov * 0.01745f * 0.5f);
    nc = tags_cars(veh, name, TAGS_MAX);
    for (j = 0; j < nc; j++)
        tags_add(&n, vw, pr, k, th, veh[j], name[j]);
    s_tags.aspect = (tags_env("RECOMP_WIDESCREEN", 1) ? 16.0f / 9.0f : 4.0f / 3.0f) * k;
    s_tags.rgb = 0xFFFFFF;
    if (online_ram(g = MEM32(TAGS_ICON_VA))) {
        uint32_t c = g + 0xACu + 16u * (uint32_t)(s_icon_col >= 0 && s_icon_col < 8 ? s_icon_col : 0);
        s_tags.rgb = (int32_t)((uint32_t)(tags_clamp01(MEMF(c)) * 255.0f) << 16 |
                               (uint32_t)(tags_clamp01(MEMF(c + 4u)) * 255.0f) << 8 |
                               (uint32_t)(tags_clamp01(MEMF(c + 8u)) * 255.0f));
    }
    s_tags.n = n;
    s_tags.seq++;
    if (!found) {
        found = 1;
        fprintf(stderr, "[ROAM] name tags: main view from %08X, viewport %ux%u, arrow colour %d #%06X\n",
                (unsigned)ret, (unsigned)MEM32(vp + 0x168u), (unsigned)MEM32(vp + 0x16Cu), (int)s_icon_col,
                (unsigned)s_tags.rgb);
    }
    if (trace && s_roam_nhist && now >= trace_t) {
        float u = -1, v = -1, w = 0;
        const float *me = s_roam_hist[(s_roam_nhist - 1) % ROAM_HIST].m;
        trace_t = now + 1.0;
        tags_proj(vw, pr, me + 9, k, &u, &v, &w);
        fprintf(stderr, "[ROAM] tags: view from %08X %ux%u k %.2f, our car at %.2f,%.2f depth %.1f m, %d tags, "
                "arrows the game drew %d, colour %d\n", (unsigned)ret, (unsigned)MEM32(vp + 0x168u),
                (unsigned)MEM32(vp + 0x16Cu), k, u, v, w, n, s_icon_nseen, (int)s_icon_col);
    }
}
#else
static void roam_tags_view(uint32_t vp, uint32_t ret) { (void)vp; (void)ret; }
#endif
