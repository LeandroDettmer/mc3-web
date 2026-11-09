/* roam_races.c -- free roam: the cities' race start points (API and layout:
 * roam_races.h).  Included by recomp_manual.c (MEM32/MEMF, online_ram).
 *
 * Read once from the game's event list (loaded at boot); read again if the
 * table moves.  Only the city races get a kind (roam_races.h); the free roam
 * entries (RaceType 0 'roam'), hookman chases, follows, lose-the-cops and the
 * battle modes are left out, as are events without a StartBeaconPosition.
 * MC3_ROAM_RACES=0: no race icons (roam_map.c); =2: every race is logged. */

#include <math.h>
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

static void rr_load(uint32_t tbl)
{
    int per[RR_KINDS] = { 0 }, nobeacon = 0, k;
    uint32_t c, i, j, city, n, names, ev, e, name;
    s_rr_n = 0;
    s_rr_tbl = tbl;
    for (c = 0; c < RR_CITIES; c++) {
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
            per[k]++;
            s_rr_n++;
            if (rr_mode() >= 2)
                fprintf(stderr, "[ROAM] race city %u #%u %-24s %-9s laps %u opp %u at (%.1f, %.1f, %.1f)\n",
                        c, i, r->name, roam_races_kind_name(k), r->laps, r->opponents,
                        r->pos[0], r->pos[1], r->pos[2]);
        }
    }
    fprintf(stderr, "[ROAM] races: %d start points (sprint %d, circuit %d, unordered %d, autocross %d, "
            "track %d, frenzy %d), %d without a start beacon\n", s_rr_n, per[RR_SPRINT], per[RR_CIRCUIT],
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
