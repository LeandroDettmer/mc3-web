/* roam_map.c -- free roam (Online cruise): map icons.  Included by
 * recomp_manual.c after roam_game.c (uses roam_call, online_ram, s_online).
 *
 * Garages and movers.  The HUD map (vtable 0x4509D0; init sub_000BF1D0)
 * holds eight built-in points of interest, 0x50 bytes each: the four
 * cities' garages at +0x1CC/+0x21C/+0x26C/+0x2BC (sd/atl/det/tok_garage,
 * type 6 = icon_garage, texture [map+0x98], GarageColor/GarageIconScale) and
 * four movers at +0x30C.. (type 5 = icon_warehouse, [map+0xA0]; the ones of
 * locked cities left out), each with its city at +0xE and its place from the
 * city's "<city>_garagePos" tune entry.  sub_000BF060 (vtable 0x450A00)
 * pushes them into the map server's icon list ([map+0x44C], layer 1, rebuilt
 * every frame: the events push theirs every frame too, sub_00266660) every
 * frame -- but only once, through the "dirty" byte [map+0x468], when
 * [map+0x28] is set, when the race type ([0x53F2E0]+0x18) is not 0..2
 * (sub_000B6A70), or when sub_0001D9B0 says so (network session state): the
 * icons then show a single frame.  The minimap and the full map draw the list
 * (sub_000C2620 per layer, only the current city's: item +0xE == [map+0x90]).
 * In the Online session we set the dirty byte before each call, so the
 * garages and movers are pushed every frame whatever those gates say (where
 * the gates already pass, nothing changes: that path adds them anyway).
 *
 * Far players.  The hub's FAR list (roam_far: the players beyond the K
 * nearest, ~every 2 s) is drawn as blips only, no car, after the game's own
 * player blips (sub_000BF850, minimap and full map), with the same primitive
 * as a car's blip (sub_000BE140's path): the map's world -> map transform
 * (vtable +0x94, pins far points to the minimap's rim, sub_000B95B0) about
 * our car, then sub_000BB310 (this, colour float4 *, Matrix34 *, scale, 0)
 * twice -- the outline colour ([0x507508]+0x90) at [0x507508]+0x1C times the
 * scale, then the fill.  The scale is the map's vtable +0x80 (minimap
 * sub_000BEB70: [[0x507504]+0x28]; full map sub_000BEDA0: [[0x507518]+0x18]),
 * read from its two instructions.  Fill: the near players' colour
 * ([0x507508]+0x40) at 55 %, alpha too, and 80 % of the size.
 *
 * MC3_ROAM_MAP=0 turns both off. */

#define RMAP_G_VA        0x00507508u     /* the HUD's tuning: colours (float4) and scales */
#define RMAP_RSTATE_VA   0x00507A94u     /* render state sub_0003A6A0 sets (sub_000BE140 saves it) */
#define RMAP_FAR_DIM     0.55f
#define RMAP_FAR_SIZE    0.8f

void sub_0003A6A0(void);                        /* HUD render state, cdecl (int) */
void sub_00030780(void);                        /* HUD render state flush */
void sub_000BB310(void);                        /* map blip, thiscall (float4 *colour, Matrix34 *, float scale, int), ret 0x10 */
void sub_002BF570(void);                        /* GetPlayer, cdecl (index) */
void sub_002C84D0(void);                        /* a player's car matrix, thiscall */

static int rmap_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("MC3_ROAM_MAP");
        on = !(v && v[0] == '0');
    }
    return on && s_online;
}

static uint32_t rmap_f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* The map's blip scale: its vtable +0x80 is `mov eax, [abs]; fld dword [eax + d8]; ret`. */
static float rmap_scale(uint32_t map)
{
    uint32_t fn = MEM32(MEM32(map) + 0x80u), p;
    if (fn < 0x10000u || fn >= 0x00440000u)
        return 1.0f;                            /* not in the game's code (.text ends below 0x440000) */
    if (MEM8(fn) != 0xA1u || MEM8(fn + 5u) != 0xD9u || MEM8(fn + 6u) != 0x40u)
        return 1.0f;
    p = MEM32(MEM32(fn + 1u));
    return online_ram(p) ? MEMF(p + (uint32_t)(int32_t)(int8_t)MEM8(fn + 7u)) : 1.0f;
}

/* sub_000BF060 runs with g_ecx = the HUD map. */
static void rmap_pois(void)
{
    static int said;
    if (!rmap_on() || !online_ram(g_ecx) || !online_ram(MEM32(g_ecx + 0x44Cu)))
        return;
    MEM8(g_ecx + 0x468u) = 1;
    if (!said) {
        said = 1;
        fprintf(stderr, "[ONLINE] map: garage and mover icons every frame\n");
    }
}

/* After sub_000BF850 (map, the game's player blips); top: the caller's esp at the call (scratch goes below it). */
static void rmap_far(uint32_t map, uint32_t top)
{
    static int said;
    roam_far_peer far[ROAM_FAR_MAX];
    uint32_t g = MEM32(RMAP_G_VA), vt, xf, buf, col, args[5], pl, mat, saved, idx;
    float rot[9], s;
    int n, i, k, drawn = 0;
    if (!rmap_on() || !roam_running() || !online_ram(map) || !online_ram(g) || !online_ram(vt = MEM32(map)))
        return;
    if ((n = roam_far(far, ROAM_FAR_MAX)) <= 0)
        return;
    xf = MEM32(vt + 0x94u);
    buf = (top - 0x200u) & ~0xFu;               /* Matrix34 at buf, fill colour at buf + 0x40 */
    col = buf + 0x40u;
    idx = MEM32(map + 0xCu);
    args[0] = idx == 0xFFFFFFFFu ? 0u : idx;
    pl = roam_call(sub_002BF570, 0, buf, 1, args);
    if (!online_ram(pl) || !online_ram(mat = roam_call(sub_002C84D0, pl, buf, 0, NULL)))
        return;
    s = rmap_scale(map) * RMAP_FAR_SIZE;
    for (k = 0; k < 4; k++)
        MEMF(col + 4u * k) = MEMF(g + 0x40u + 4u * k) * RMAP_FAR_DIM;
    saved = MEM32(RMAP_RSTATE_VA);
    args[0] = 0;
    roam_call(sub_0003A6A0, 0, buf, 1, args);
    for (i = 0; i < n; i++) {
        if (far[i].status != RC_ST_ROAM && far[i].status != RC_ST_EVENT && far[i].status != RC_ST_SHOP &&
            far[i].status != RC_ST_PAUSED)
            continue;
        roam_quat_mat(far[i].q, rot);
        for (k = 0; k < 9; k++)
            MEMF(buf + 4u * k) = rot[k];
        for (k = 0; k < 3; k++)
            MEMF(buf + 0x24u + 4u * k) = far[i].p[k];
        args[0] = buf + 0x24u; args[1] = mat + 0x24u; args[2] = buf + 0x24u;   /* in, about, out */
        roam_call(recomp_lookup(xf), map, buf, 3, args);
        args[0] = g + 0x90u; args[1] = buf; args[2] = rmap_f2u(MEMF(g + 0x1Cu) * s); args[3] = 0;
        roam_call(sub_000BB310, map, buf, 4, args);
        args[0] = col; args[2] = rmap_f2u(s);
        roam_call(sub_000BB310, map, buf, 4, args);
        drawn++;
    }
    args[0] = saved;
    roam_call(sub_0003A6A0, 0, buf, 1, args);
    roam_call(sub_00030780, 0, buf, 0, NULL);
    if (drawn && !said) {
        said = 1;
        fprintf(stderr, "[roam] map: %d far player(s) drawn as blips\n", drawn);
    }
}
