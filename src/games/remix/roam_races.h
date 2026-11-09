/* roam_races.h -- free roam: the cities' race start points (Remix).
 *
 * The game's own per-city event list, read from guest memory (roam_races.c,
 * included by recomp_manual.c after roam_game.c).  Loaded at boot
 * (sub_0001DC80 -> sub_0026F910 -> sub_0026F700 per city: tune/race/<city>.loc
 * + .locinf -> sub_0026F620 -> sub_0026F010), so it is there in Free Roam.
 * City table [0x557EE4]: 14 entries of 0x50 bytes (0 San Diego, 1 Atlanta,
 * 2 Detroit, 3 Tokyo; name at +0, .loc name at +8); per city +0x14 count
 * (int), +0x18 names (char *[]), +0x20 events (0x30 bytes each, same order).
 * Event (filled by sub_0026DA70 from the .locinf): +0x00 RaceType (int,
 * names at 0x4B37D0, parser sub_0026D430), +0x04 PedDensity, +0x08
 * AmbientDensity, +0x0C Weather, +0x0D Time, +0x0F OpponentCount, +0x10
 * NumLaps (default 1), +0x14 StartBeaconPosition (vec3; (50, 0, 0) when the
 * file has none), +0x20 DifficultyModifier, +0x27 CutSceneBitMask.  The
 * career's map draws its race icons at +0x14 (sub_000C02C0).
 * Arcade's setup screen picks a race by name: settings [0x53F2E8] +0x68,
 * looked up in the city's names by sub_0026B010 -> sub_0026DFB0 (index).
 * The list has no heading: the starting grid (RacerData PosDir) is in the
 * race's .rac, read only when the race loads (sub_001D4F20). */
#ifndef ROAM_RACES_H
#define ROAM_RACES_H

#include <stdint.h>

enum {                                  /* what the map shows; from RaceType + NumLaps */
    RR_SPRINT,                          /* cp_ordered*, one lap */
    RR_CIRCUIT,                         /* cp_ordered*, laps > 1 */
    RR_UNORDERED,                       /* cp_unordered* */
    RR_AUTOCROSS,                       /* autocross */
    RR_TRACK,                           /* ordered_track */
    RR_FRENZY,                          /* frenzy */
    RR_KINDS
};

typedef struct roam_race {
    char name[32];                      /* event name: what Arcade's setup puts at settings +0x68 */
    uint16_t index;                     /* its index in the city's list (what sub_0026B010 returns) */
    uint8_t city;                       /* index into the city table [0x557EE4] */
    uint8_t kind;                       /* RR_* */
    uint8_t type;                       /* the game's RaceType, 0..22 */
    uint8_t laps;                       /* NumLaps */
    uint8_t opponents;                  /* OpponentCount */
    float pos[3];                       /* StartBeaconPosition, world (y up) */
} roam_race;

int roam_races_count(void);             /* races with a start point, all cities; 0 before boot loaded them */
const roam_race *roam_races_get(int i); /* 0 <= i < count, else NULL */
/* The nearest race of the city within r metres (x/z plane) of (x, z); NULL
 * none.  *dist (may be NULL) gets its distance. */
const roam_race *roam_races_nearest(unsigned city, float x, float z, float r, float *dist);
const char *roam_races_kind_name(int kind);

#endif
