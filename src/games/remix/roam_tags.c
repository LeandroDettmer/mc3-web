/* Free roam: each remote player's name over its car (browser only).
 *
 * Included by recomp_manual.c after roam_cars.c (it reads their slots and
 * standalone cars). Nothing is drawn by the game: each frame the cars'
 * places go through the main camera's view and projection into screen
 * positions (0..1 across the picture), published in roam_tags_box, and the
 * page draws the names over the canvas (web/pre.js, "Name tags").
 *
 * The camera: a camera object applies itself to its viewport by setting
 * the camera matrix (sub_002D3DA0: the view matrix, its inverse, into
 * [0x559758], 4x4, row vectors as D3D) and then building the viewport's
 * projection (sub_002D4AE0, viewport +4, 4x4: w = view z; the D3D viewport
 * X, Y, W, H at +0x160). Callers of the builder from the camera classes
 * (sub_00138510, sub_00138750) are taken; of those, the biggest viewport
 * (the mirror and the minimap are smaller). Hor+ (WebGPU): the projection
 * the title built is the 16:9 one and the renderer squeezes x by 1/k
 * (nv2a_wgpu_wide), so the picture's x is x_ndc / k.
 *
 * Shown: slots and standalone cars that are driven (their peer here, not in
 * its garage, the car shown), within TAGS_FAR m, in front of the camera;
 * none while not driving (paused, menus, garage: remix_driving) or with
 * MC3_ROAM_FAKE. Always published (a few dozen multiplies): the page's
 * MC3_NAMETAGS=0 only stops drawing, so it can turn on and off live. Tags are not hidden behind buildings
 * (no depth test). MC3_ROAM_TRACE=1: once a second, where our own car
 * projects (expect the middle, lower half). */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
extern float nv2a_wgpu_wide(void);              /* xboxrecomp nv2a_wgpu: Hor+ widening, 1 when off */

#define TAGS_MAX       32
#define TAGS_FAR       200.0f           /* m from the camera */
#define TAGS_UP        1.6f             /* m above the car's origin (y is up) */
#define TAGS_VIEW_VA   0x00559758u      /* the view matrix, set from the camera (sub_002D3DA0) */
#define TAGS_NAME      28

typedef struct { float u, v, dist; char name[TAGS_NAME]; } roam_tag;
_Static_assert(ROAM_NAME_MAX < TAGS_NAME, "a name and its NUL fit");
_Static_assert(sizeof(roam_tag) == 40, "web/pre.js reads 40-byte tags");

/* Read by the page (web/pre.js): seq +1 per update, n tags, the picture's
 * width / height, then the tags. A read during a write may mix two frames. */
static struct {
    volatile int32_t seq;
    int32_t n;
    float aspect;
    int32_t unused;
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

static void tags_add(int *n, const float vw[16], const float pr[16], float k, const float m[12], const char *name)
{
    float p[3] = { m[9], m[10] + TAGS_UP, m[11] }, u, v, w;
    roam_tag *t;
    if (*n >= TAGS_MAX || !tags_proj(vw, pr, p, k, &u, &v, &w) || w > TAGS_FAR)
        return;
    t = &s_tags.t[(*n)++];
    t->u = u;
    t->v = v;
    t->dist = w;
    snprintf(t->name, sizeof t->name, "%s", name[0] ? name : "Player");
}

/* After the projection builder ran for viewport vp, called from ret. */
static void roam_tags_view(uint32_t vp, uint32_t ret)
{
    static int trace = -1, found;
    static uint32_t best;
    static double best_t, trace_t;
    float vw[16], pr[16], k;
    uint32_t area, i;
    double now;
    int n = 0, j;
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
    for (j = 1; j <= s_roam_slots; j++)
        if (s_roam_peer[j].id && s_roam_peer[j].here && !s_roam_peer[j].away && s_roam_peer[j].live)
            tags_add(&n, vw, pr, k, s_roam_peer[j].m, s_roam_peer[j].name);
    for (j = 0; j < s_ncar; j++)
        if (s_car[j].veh && s_car[j].id && s_car[j].here && !s_car[j].away && s_car[j].live)
            tags_add(&n, vw, pr, k, s_car[j].m, s_car[j].name);
    s_tags.aspect = (tags_env("RECOMP_WIDESCREEN", 1) ? 16.0f / 9.0f : 4.0f / 3.0f) * k;
    s_tags.n = n;
    s_tags.seq++;
    if (!found) {
        found = 1;
        fprintf(stderr, "[ROAM] name tags: main view from %08X, viewport %ux%u\n", (unsigned)ret,
                (unsigned)MEM32(vp + 0x168u), (unsigned)MEM32(vp + 0x16Cu));
    }
    if (trace && s_roam_nhist && now >= trace_t) {
        float u = -1, v = -1, w = 0;
        const float *me = s_roam_hist[(s_roam_nhist - 1) % ROAM_HIST].m;
        trace_t = now + 1.0;
        tags_proj(vw, pr, me + 9, k, &u, &v, &w);
        fprintf(stderr, "[ROAM] tags: view from %08X %ux%u k %.2f, our car at %.2f,%.2f depth %.1f m, %d tags\n",
                (unsigned)ret, (unsigned)MEM32(vp + 0x168u), (unsigned)MEM32(vp + 0x16Cu), k, u, v, w, n);
    }
}
#else
static void roam_tags_view(uint32_t vp, uint32_t ret) { (void)vp; (void)ret; }
#endif
