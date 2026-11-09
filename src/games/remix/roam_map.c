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
 * Linked players.  The game's loop draws only players whose car it counts
 * drawn; standalone cars (roam_cars.c) are no players at all, and with the
 * hub's K (32) every player of a small room is linked, so its FAR list is
 * empty: no icon showed.  Every peer of roam_net's list is drawn the same
 * way, full colour and size (a slot the game draws too gets ours on top).
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

/* Free Roam has no map that holds them.  The garage map (vtable 0x4509D0,
 * "hudMapGlobalCareer") lives only in the career's HUD page (sub_000B0F60, the
 * HUD at +0x20), which the HUD factory sub_000B7220 builds only in game mode 2
 * ([0x53F2D4]); the Arcade cruise behind Free Roam gets the plain minimap
 * (0x450850) and full map (0x450900), so sub_000BF060 never ran there.  We
 * build a hidden one of our own after the HUD's init (sub_000B6A90), as the
 * page does (sub_000B1810: operator new sub_001B3B90 (0x46C), ctor
 * sub_000BEDD0, init vtable +0xA4 = sub_000BF1D0 (player, HUD, 9, owner 0),
 * then vtable +0x38 (0)), and run sub_000BF060 on it right after every clear
 * of the HUD's map server (sub_000D0140 on [HUD+0x14]: the HUD's update
 * sub_000B6C20 and sub_000B6FA0), so the garages and movers sit in layer 1
 * whenever the minimap or the full map draws it.  Deleted with the HUD
 * (sub_000B7720). */
#define RMAP_POI_SIZE    0x46Cu
#define RMAP_POI_SLOT    9u                     /* the career page's widget id */

void sub_001B3B90(void);                        /* operator new, cdecl (size) */
void sub_000BEDD0(void);                        /* garage map ctor, thiscall () */
void sub_000BF1D0(void);                        /* garage map init, thiscall (player, HUD, id, owner), ret 0x10 */
void sub_000BF060(void);                        /* garage map: push the points of interest (override below) */

static uint32_t s_rmap_hud, s_rmap_poi;

/* After the HUD's init (sub_000B6A90, this = HUD, arg = player index). */
static void rmap_hud_init(uint32_t hud, uint32_t player, uint32_t top)
{
    uint32_t a[4], p;
    s_rmap_hud = s_rmap_poi = 0;
    if (!rmap_on() || !online_ram(hud) || MEM32(hud + 0x20u) || !online_ram(MEM32(hud + 0x14u)))
        return;                                 /* off, or a career page that has its own */
    a[0] = RMAP_POI_SIZE;
    p = roam_call(sub_001B3B90, 0, top, 1, a);
    if (!online_ram(p))
        return;
    p = roam_call(sub_000BEDD0, p, top, 0, NULL);
    if (!online_ram(p))
        return;
    a[0] = player; a[1] = hud; a[2] = RMAP_POI_SLOT; a[3] = 0;
    roam_call(sub_000BF1D0, p, top, 4, a);
    a[0] = 0;
    roam_call(recomp_lookup(MEM32(MEM32(p) + 0x38u)), p, top, 1, a);
    s_rmap_hud = hud;
    s_rmap_poi = p;
    fprintf(stderr, "[ONLINE] map: garage map 0x%08X for HUD 0x%08X (server 0x%08X, city %u)\n",
            p, hud, MEM32(hud + 0x14u), MEM32(p + 0x90u));
}

/* After sub_000D0140 cleared a map server's layers. */
static void rmap_server_cleared(uint32_t server, uint32_t top)
{
    if (s_rmap_poi && rmap_on() && server == MEM32(s_rmap_hud + 0x14u))
        roam_call(sub_000BF060, s_rmap_poi, top, 0, NULL);
}

/* Before the HUD's destructor (sub_000B7720, this = HUD). */
static void rmap_hud_gone(uint32_t hud, uint32_t top)
{
    uint32_t p = s_rmap_poi;
    if (!p || hud != s_rmap_hud)
        return;
    s_rmap_hud = s_rmap_poi = 0;
    roam_vdelete(p, top);
}

/* A remote player's status that gets a blip. */
static int rmap_shown(uint8_t st)
{
    return st == RC_ST_ROAM || st == RC_ST_EVENT || st == RC_ST_SHOP || st == RC_ST_PAUSED;
}

/* After sub_000BF850 (map, the game's player blips); top: the caller's esp at the call (scratch goes below it).
 * Every linked peer (roam_net's list: slots, standalone cars, peers still without a car) gets the near
 * players' blip, every FAR one the dimmed one. The game's own loop skips most of them (standalone cars are
 * not players; a slot's car only when the game counts it drawn); a slot it does draw gets ours on the same
 * spot, same colour. */
static void rmap_far(uint32_t map, uint32_t top)
{
    static int said;
    static struct { float p[3], q[4]; int far; } dot[ROAM_K_MAX + ROAM_FAR_MAX];
    roam_far_peer far[ROAM_FAR_MAX];
    uint32_t g = MEM32(RMAP_G_VA), vt, xf, buf, col, args[5], pl, mat, saved, idx;
    float rot[9], s, si;
    int n = 0, nf, nnear = 0, i, k;
    if (!rmap_on() || !roam_running() || !online_ram(map) || !online_ram(g) || !online_ram(vt = MEM32(map)))
        return;
    if (s_roam_seen && roam_now() - s_roam_seen_t < 0.5)
        for (i = 0; i < s_roam_nseen; i++)
            if (rmap_shown(s_roam_seen[i].status)) {
                memcpy(dot[n].p, s_roam_seen[i].p, sizeof dot[n].p);
                memcpy(dot[n].q, s_roam_seen[i].q, sizeof dot[n].q);
                dot[n++].far = 0;
            }
    nnear = n;
    nf = roam_far(far, ROAM_FAR_MAX);
    for (i = 0; i < nf; i++)
        if (rmap_shown(far[i].status)) {
            memcpy(dot[n].p, far[i].p, sizeof dot[n].p);
            memcpy(dot[n].q, far[i].q, sizeof dot[n].q);
            dot[n++].far = 1;
        }
    if (!n)
        return;
    xf = MEM32(vt + 0x94u);
    buf = (top - 0x200u) & ~0xFu;               /* Matrix34 at buf, far fill colour at buf + 0x40 */
    col = buf + 0x40u;
    idx = MEM32(map + 0xCu);
    args[0] = idx == 0xFFFFFFFFu ? 0u : idx;
    pl = roam_call(sub_002BF570, 0, buf, 1, args);
    if (!online_ram(pl) || !online_ram(mat = roam_call(sub_002C84D0, pl, buf, 0, NULL)))
        return;
    s = rmap_scale(map);
    for (k = 0; k < 4; k++)
        MEMF(col + 4u * k) = MEMF(g + 0x40u + 4u * k) * RMAP_FAR_DIM;
    saved = MEM32(RMAP_RSTATE_VA);
    args[0] = 0;
    roam_call(sub_0003A6A0, 0, buf, 1, args);
    for (i = 0; i < n; i++) {
        si = dot[i].far ? s * RMAP_FAR_SIZE : s;
        roam_quat_mat(dot[i].q, rot);
        for (k = 0; k < 9; k++)
            MEMF(buf + 4u * k) = rot[k];
        for (k = 0; k < 3; k++)
            MEMF(buf + 0x24u + 4u * k) = dot[i].p[k];
        args[0] = buf + 0x24u; args[1] = mat + 0x24u; args[2] = buf + 0x24u;   /* in, about, out */
        roam_call(recomp_lookup(xf), map, buf, 3, args);
        args[0] = g + 0x90u; args[1] = buf; args[2] = rmap_f2u(MEMF(g + 0x1Cu) * si); args[3] = 0;
        roam_call(sub_000BB310, map, buf, 4, args);
        args[0] = dot[i].far ? col : g + 0x40u; args[2] = rmap_f2u(si);
        roam_call(sub_000BB310, map, buf, 4, args);
    }
    args[0] = saved;
    roam_call(sub_0003A6A0, 0, buf, 1, args);
    roam_call(sub_00030780, 0, buf, 0, NULL);
    if (!said) {
        said = 1;
        fprintf(stderr, "[roam] map: %d linked + %d far player(s) drawn as blips\n", nnear, n - nnear);
    }
}
