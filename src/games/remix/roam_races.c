/* roam_races.c -- free roam: the cities' race start points (API and layout:
 * roam_races.h).  Included by recomp_manual.c (MEM32/MEMF, online_ram).
 *
 * Read once from the game's event list (loaded at boot); read again if the
 * table moves.  Only the city races get a kind (roam_races.h); the free roam
 * entries (RaceType 0 'roam'), hookman chases, follows, lose-the-cops and the
 * battle modes are left out, as are events without a StartBeaconPosition.
 * About 70 a city were too many for the map: each city's list is cut down
 * at load (rr_curate: variants merged, then a spread-out subset), and only
 * that subset is on the maps and gets the prompt.
 * MC3_ROAM_RACES=0: no race icons (roam_map.c); =2: every race is logged. */

#include <ctype.h>
#include <math.h>
#include <string.h>
#include "roam_races.h"

#define RR_CITY_VA      0x00557EE4u     /* city table pointer */
#define RR_CITIES       14u             /* 0x460 / 0x50 (sub_0026F910) */
#define RR_CITY_SIZE    0x50u
#define RR_EVENT_SIZE   0x30u
#define RR_MAX          512

static roam_race s_rr[RR_MAX];
static int s_rr_n;
static uint32_t s_rr_tbl;

static int rr_mode(void)                /* 0 off, 1 on, 2 on + log every race */
{
    static int m = -1;
    if (m < 0) {
        const char *v = getenv("MC3_ROAM_RACES");
        m = v && v[0] ? atoi(v) : 1;
    }
    return m;
}

static int rr_kind(uint32_t type, uint32_t laps)
{
    switch (type) {
    case 4: case 5: case 6: return RR_UNORDERED;           /* cp_unordered, _time_local, _time_global */
    case 7: case 8: case 9: return laps > 1 ? RR_CIRCUIT : RR_SPRINT;   /* cp_ordered* */
    case 18: return RR_FRENZY;
    case 21: return RR_AUTOCROSS;
    case 22: return RR_TRACK;                              /* ordered_track */
    default: return -1;
    }
}

const char *roam_races_kind_name(int kind)
{
    static const char *const k[RR_KINDS] = { "sprint", "circuit", "unordered", "autocross", "track", "frenzy" };
    return kind >= 0 && kind < RR_KINDS ? k[kind] : "?";
}

static float rr_d2(const roam_race *a, const roam_race *b)
{
    float dx = a->pos[0] - b->pos[0], dz = a->pos[2] - b->pos[2];
    return dx * dx + dz * dz;
}

/* The difficulty word in an event name ('autox\easy\sd_autox_easy_downtown'): 1 medium/normal, 0 other. */
static int rr_medium(const char *name)
{
    char low[sizeof s_rr[0].name];
    size_t i;
    for (i = 0; name[i] && i < sizeof low - 1; i++)
        low[i] = (char)tolower((unsigned char)name[i]);
    low[i] = 0;
    return strstr(low, "medium") || strstr(low, "normal");
}

/* One city's races in s_rr[from..s_rr_n): kept in place, the rest dropped. Variants of one course (same
 * kind, starts within RR_MERGE_M) become one race, the medium/normal one when there is one; then the
 * tracks and frenzies all stay, plus each other kind's first, and the rest fill up to RR_CITY_MAX by
 * farthest-point picks (the start farthest from every kept one first) while at least RR_SPACE_M away. */
#define RR_MERGE_M      50.0f
#define RR_SPACE_M      150.0f
#define RR_CITY_MAX     28
static int rr_curate(int from)
{
    static roam_race v[RR_MAX];
    static uint8_t keep[RR_MAX];
    static float near2[RR_MAX];
    int n = 0, nk = 0, i, j, best, seen[RR_KINDS] = { 0 };
    for (i = from; i < s_rr_n; i++) {           /* merge variants */
        for (j = 0; j < n && !(v[j].kind == s_rr[i].kind && rr_d2(&v[j], &s_rr[i]) <= RR_MERGE_M * RR_MERGE_M); j++)
            ;
        if (j == n)
            v[n++] = s_rr[i];
        else if (!rr_medium(v[j].name) && rr_medium(s_rr[i].name))
            v[j] = s_rr[i];
    }
    for (i = 0; i < n; i++) {
        keep[i] = v[i].kind == RR_TRACK || v[i].kind == RR_FRENZY || !seen[v[i].kind];
        seen[v[i].kind] = 1;
        nk += keep[i];
    }
    for (i = 0; i < n; i++) {
        near2[i] = 1e30f;
        for (j = 0; j < n; j++)
            if (keep[j] && rr_d2(&v[i], &v[j]) < near2[i])
                near2[i] = rr_d2(&v[i], &v[j]);
    }
    while (nk < RR_CITY_MAX) {
        for (best = -1, i = 0; i < n; i++)
            if (!keep[i] && (best < 0 || near2[i] > near2[best]))
                best = i;
        if (best < 0 || near2[best] < RR_SPACE_M * RR_SPACE_M)
            break;
        keep[best] = 1;
        nk++;
        for (i = 0; i < n; i++)
            if (rr_d2(&v[i], &v[best]) < near2[i])
                near2[i] = rr_d2(&v[i], &v[best]);
    }
    for (s_rr_n = from, i = 0; i < n; i++)
        if (keep[i])
            s_rr[s_rr_n++] = v[i];
    return n;
}

static void rr_load(uint32_t tbl)
{
    int per[RR_KINDS] = { 0 }, nobeacon = 0, k, from, all = 0, merged;
    uint32_t c, i, j, city, n, names, ev, e, name;
    s_rr_n = 0;
    s_rr_tbl = tbl;
    for (c = 0; c < RR_CITIES; c++) {
        from = s_rr_n;
        city = tbl + c * RR_CITY_SIZE;
        n = MEM32(city + 0x14u);
        names = MEM32(city + 0x18u);
        ev = MEM32(city + 0x20u);
        if (!n || n > 1024u || !online_ram(names) || !online_ram(ev))
            continue;
        for (i = 0; i < n && s_rr_n < RR_MAX; i++) {
            roam_race *r = &s_rr[s_rr_n];
            e = ev + i * RR_EVENT_SIZE;
            k = rr_kind(MEM32(e), MEM8(e + 0x10u));
            if (k < 0)
                continue;
            r->pos[0] = MEMF(e + 0x14u);
            r->pos[1] = MEMF(e + 0x18u);
            r->pos[2] = MEMF(e + 0x1Cu);
            if ((r->pos[0] == 50.0f && r->pos[1] == 0.0f && r->pos[2] == 0.0f) || !isfinite(r->pos[0])
                || !isfinite(r->pos[2])) {
                nobeacon++;                             /* sub_0026DA70's default: no StartBeaconPosition */
                continue;
            }
            name = MEM32(names + 4u * i);
            for (j = 0; online_ram(name) && j < sizeof r->name - 1 && MEM8(name + j); j++)
                r->name[j] = (char)MEM8(name + j);
            r->name[j] = 0;
            r->index = (uint16_t)i;
            r->city = (uint8_t)c;
            r->kind = (uint8_t)k;
            r->type = (uint8_t)MEM32(e);
            r->laps = MEM8(e + 0x10u);
            r->opponents = MEM8(e + 0x0Fu);
            s_rr_n++;
            if (rr_mode() >= 2)
                fprintf(stderr, "[ROAM] race city %u #%u %-24s %-9s laps %u opp %u at (%.1f, %.1f, %.1f)\n",
                        c, i, r->name, roam_races_kind_name(k), r->laps, r->opponents,
                        r->pos[0], r->pos[1], r->pos[2]);
        }
        if (s_rr_n == from)
            continue;
        all += s_rr_n - from;
        k = s_rr_n - from;
        merged = rr_curate(from);
        fprintf(stderr, "[ROAM] races: city %u: %d start points, %d after merging variants, %d on the map\n",
                c, k, merged, s_rr_n - from);
    }
    for (k = 0; k < s_rr_n; k++)
        per[s_rr[k].kind]++;
    fprintf(stderr, "[ROAM] races: %d of %d start points shown (sprint %d, circuit %d, unordered %d, autocross %d, "
            "track %d, frenzy %d), %d without a start beacon\n", s_rr_n, all, per[RR_SPRINT], per[RR_CIRCUIT],
            per[RR_UNORDERED], per[RR_AUTOCROSS], per[RR_TRACK], per[RR_FRENZY], nobeacon);
}

static void rr_refresh(void)
{
    static uint32_t events;                     /* the cities' counts summed at the last load */
    uint32_t tbl = MEM32(RR_CITY_VA), sum = 0, c;
    if (!online_ram(tbl))
        return;
    for (c = 0; c < RR_CITIES; c++)
        sum += MEM32(tbl + c * RR_CITY_SIZE + 0x14u);
    if (tbl != s_rr_tbl || sum != events) {     /* moved, or the boot load had not run yet */
        events = sum;
        rr_load(tbl);
    }
}

int roam_races_count(void)
{
    rr_refresh();
    return s_rr_n;
}

const roam_race *roam_races_get(int i)
{
    rr_refresh();
    return i >= 0 && i < s_rr_n ? &s_rr[i] : NULL;
}

const roam_race *roam_races_nearest(unsigned city, float x, float z, float r, float *dist)
{
    const roam_race *best = NULL;
    float bd = r * r, dx, dz, d;
    int i;
    rr_refresh();
    for (i = 0; i < s_rr_n; i++) {
        if (s_rr[i].city != city)
            continue;
        dx = s_rr[i].pos[0] - x;
        dz = s_rr[i].pos[2] - z;
        d = dx * dx + dz * dz;
        if (d <= bd) {
            bd = d;
            best = &s_rr[i];
        }
    }
    if (dist)
        *dist = best ? sqrtf(bd) : -1.0f;
    return best;
}
