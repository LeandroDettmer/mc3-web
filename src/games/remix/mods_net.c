/* mods_net.c -- mod cars online: a player driving a car from mods/ must never
 * crash or freeze one who lacks it. Included by recomp_manual.c after
 * roam_game.c (it uses roam_call, roam_car_index, roam_cfg_name). Remix only.
 *
 * Where a car's identity crosses the network:
 *  - Free roam (roam core CFG record, peer to peer): our CarCfg, 0x108 B,
 *    vtable zeroed (roam_send_cfg); the car is its name at +0xE3 (the builder,
 *    the model read and sub_0026A6A0 go by it). Every receiver reads it through
 *    roam_peer_rec (load slots, the pool's look/rebuild, race ghosts), then
 *    copies it into a setup entry with operator= sub_00272010 and builds it.
 *  - Street / group challenge (roam GAME records): no car identity.
 *  - System Link (Online Races): CAR_CONFIG msg 0x44, a whole CarCfg; see
 *    mn_sl_send / mn_sl_recv at the end.
 *  - Race ghosts and the career's online cruise: the free-roam record above.
 *
 * Receiver guard (mods_net_peer_rec, from roam_peer_rec): a name not in our
 * car list (sub_0026A6A0 -1) becomes a stock donor, the showroom's CarCfg of
 * that car (0x541700 + i*0x108, stock parts: a mod car's parts would index
 * the donor's tables out of range): the donor the sender named, else
 * vp_350z_04, else our own car. '[MOD] peer X drives <car>, not installed
 * here: shown as <donor>'.
 *
 * Sender guard (mods_net_send_cfg, from roam_send_cfg): our car is a mod car
 * when a car list in mods/ defines it (content.lst, the downloadable-content
 * list; or vehicle.lst past the archive copy's Vehicle blocks) or its index is
 * >= the stock count (that count, or the list's size when content.lst is
 * first read; none without mods/: every car sent as it is). Then peers with a roam core >= 19 (RC_CORE_CFG2: they
 * have the receiver guard) get our CarCfg + a trailer 'MODD' + the donor's
 * name (24 B), and older cores (no guard; a 0x108-byte record only) get the
 * donor's stock CarCfg (roam_set_cfg2).
 *
 * Donor per mod car: an optional line 'Donor <vp_ name>' inside the mod's
 * Vehicle block in mods/content.lst (or mods/tune/vehicle/vehicle.lst). It is read and cut out here
 * before the game parses the file (whether the game's reader skips unknown
 * keys was not checked); none: vp_350z_04. */

#define MN_DEFAULT_DONOR "vp_350z_04"
#define MN_TAG           "MODD"
#define MN_NAME          24u                     /* a donor's name in the trailer, NUL padded */
#define MN_REC           (ROAM_CFG_SIZE + 4u + MN_NAME)
#define MN_SHOWROOM      0x00541700u             /* showroom CarCfg i at + i*0x108 (127) */

static int s_mn_stock;                           /* stock cars; 0: unknown */
static struct { char name[32], donor[32]; } s_mn_mod[MODS_CARS];   /* mod cars (and Donor lines) */
static int s_mn_nmod;

/* A line's first token after its key ("Name   vp_x" -> "vp_x"), into out. */
static void mn_token(const uint8_t *a, const uint8_t *b, char *out, size_t size)
{
    size_t i = 0;
    while (a < b && !isspace(*a))
        a++;
    while (a < b && isspace(*a))
        a++;
    while (a < b && !isspace(*a) && i + 1 < size)
        out[i++] = (char)*a++;
    out[i] = 0;
}

static int mn_key(const uint8_t *a, const uint8_t *b, const char *key)
{
    size_t n = strlen(key);
    return (size_t)(b - a) > n && !memcmp(a, key, n) && isspace(a[n]);
}

/* Vehicle blocks at the top level of a vehicle.lst. */
static int mn_count_vehicles(const uint8_t *d, uint32_t n)
{
    uint32_t r = 0;
    int depth = 0, cars = 0;
    while (r < n) {
        uint32_t e = r, a, b;
        while (e < n && d[e] != '\n')
            e++;
        for (a = r; a < e && isspace(d[a]); a++) {}
        for (b = e; b > a && isspace(d[b - 1]); b--) {}
        cars += depth == 0 && b - a == 7 && !memcmp(d + a, "Vehicle", 7);
        for (uint32_t i = a; i < b; i++)
            depth += d[i] == '{' ? 1 : d[i] == '}' ? -1 : 0;
        r = e + 1;
    }
    return cars;
}

/* The archive's own vehicle.lst (Open/Size/Read/Close of the archive device,
 * name = the guest string the game opened): its Vehicle blocks, 0 on failure.
 * Runs inside the device's Open: the frames go below the caller's stack. */
static int mn_archive_cars(uint32_t name)
{
    uint32_t buf = (g_esp - 0x1100u) & ~0xFu, top = buf - 0x40u, a[3], h, size, got = 0, k;
    uint8_t *d;
    int cars;
    a[0] = name; a[1] = 1;
    h = roam_call(sub_002B3B80, 0, top, 2, a);
    if (h >= 16u)
        return 0;
    size = roam_call(sub_002B2C00, 0, top, 1, &h);
    d = size && size < (16u << 20) ? malloc(size) : NULL;
    while (d && got < size) {
        a[0] = h; a[1] = buf; a[2] = size - got < 0x1000u ? size - got : 0x1000u;
        k = roam_call(sub_002B3C40, 0, top, 3, a);
        if (!k || k > a[2])
            break;
        memcpy(d + got, (uint8_t *)XBOX_PTR(buf), k);
        got += k;
    }
    roam_call(sub_002B2BD0, 0, top, 1, &h);
    cars = d && got == size ? mn_count_vehicles(d, size) : 0;
    free(d);
    return cars;
}

/* A car list from mods/ as it is served (mods_dev_open, inside the device's
 * Open): tune/vehicle/vehicle.lst (content 0: the stock count from the
 * archive's copy, its blocks past it are mod cars) or content.lst (content 1:
 * downloadable content, every block a mod car; the stock count is the car
 * list's size when it is first opened, after vehicle.lst). Each mod car and
 * each block with a 'Donor' line is kept (that line cut out). Returns the
 * new length. */
static uint32_t mods_net_vehicles(uint8_t *d, uint32_t n, int content)
{
    uint32_t r = 0, w = 0;
    int depth = 0, veh = 0, cars = 0, mods = 0;
    char name[32] = "", donor[32] = "";

    if (!content)
        s_mn_stock = mn_archive_cars(MEM32(g_esp + 4));
    else if (!s_mn_stock)
        s_mn_stock = MEM16(0x00557EB4u);
    while (r < n) {
        uint32_t e = r, a, b;
        int cut = 0;
        while (e < n && d[e] != '\n')
            e++;
        if (e < n)
            e++;
        for (a = r; a < e && isspace(d[a]); a++) {}
        for (b = e; b > a && isspace(d[b - 1]); b--) {}
        if (depth == 0 && b - a == 7 && !memcmp(d + a, "Vehicle", 7)) {
            veh = 1;
            cars++;
            name[0] = donor[0] = 0;
        } else if (veh && depth == 1 && mn_key(d + a, d + b, "Name")) {
            mn_token(d + a, d + b, name, sizeof name);
        } else if (veh && depth == 1 && mn_key(d + a, d + b, "Donor")) {
            mn_token(d + a, d + b, donor, sizeof donor);
            cut = 1;
        }
        for (uint32_t i = a; i < b; i++)
            depth += d[i] == '{' ? 1 : d[i] == '}' ? -1 : 0;
        if (veh && depth == 0 && memchr(d + a, '}', b - a)) {
            int mod = content || (s_mn_stock > 0 && cars > s_mn_stock), i;
            for (i = 0; i < s_mn_nmod && strcmp(s_mn_mod[i].name, name); i++) {}
            if (name[0] && (mod || donor[0]) && i < MODS_CARS) {
                snprintf(s_mn_mod[i].name, sizeof s_mn_mod[i].name, "%s", name);
                snprintf(s_mn_mod[i].donor, sizeof s_mn_mod[i].donor, "%s", donor[0] ? donor : MN_DEFAULT_DONOR);
                s_mn_nmod += i == s_mn_nmod;
                mods++;
            }
            veh = 0;
        }
        if (!cut) {
            memmove(d + w, d + r, e - r);
            w += e - r;
        }
        r = e;
    }
    fprintf(stderr, "[MOD] %s: %d cars, %d mod cars (donor: their 'Donor' line, else %s); stock cars %d%s\n",
            content ? "content.lst" : "vehicle.lst", cars, mods, MN_DEFAULT_DONOR, s_mn_stock,
            s_mn_stock ? "" : " (unknown: only the cars named in mods/ count as mod cars)");
    return w;
}

/* A mod car: one a car list in mods/ defines, or past the stock cars. */
static int mods_net_is_mod(const char *name)
{
    int32_t car;
    for (int i = 0; name && i < s_mn_nmod; i++)
        if (!strcmp(s_mn_mod[i].name, name))
            return 1;
    car = name ? roam_car_index(name) : -1;
    return s_mn_stock > 0 && car >= s_mn_stock;
}

static const char *mn_donor_of(const char *name)
{
    for (int i = 0; name && i < s_mn_nmod; i++)
        if (!strcmp(s_mn_mod[i].name, name))
            return s_mn_mod[i].donor;
    return MN_DEFAULT_DONOR;
}

/* A stock CarCfg into out (vtable word 0): the showroom's of want, else of
 * vp_350z_04, else our own car's (setup entry 0). The car's name; NULL none. */
static uint32_t mn_showroom(const char *want)    /* the showroom's stock CarCfg (VA) of want, else vp_350z_04; 0 none */
{
    const char *names[2] = { want, MN_DEFAULT_DONOR };
    uint32_t n = MEM16(0x00557EB4u), cfg;
    if (n > MODS_CARS)
        n = MODS_CARS;
    if (s_mn_stock > 0 && (uint32_t)s_mn_stock < n)
        n = (uint32_t)s_mn_stock;
    for (int k = 0; k < 2; k++) {
        int32_t i = names[k] ? roam_car_index(names[k]) : -1;
        if (i < 0 || (uint32_t)i >= n)
            continue;
        cfg = MN_SHOWROOM + (uint32_t)i * ROAM_CFG_SIZE;
        if (MEM32(cfg) == ROAM_CFG_VT && !strncmp((const char *)XBOX_PTR(cfg + ROAM_CFG_NAME), names[k], 32))
            return cfg;
    }
    return 0;
}

static const char *mn_stock_cfg(const char *want, uint8_t out[ROAM_CFG_SIZE])
{
    uint32_t setup = MEM32(ROAM_SETUP_VA), cfg = mn_showroom(want);
    if (cfg) {
        memcpy(out, (uint8_t *)XBOX_PTR(cfg), ROAM_CFG_SIZE);
        memset(out, 0, 4);
        return roam_cfg_name(out);
    }
    cfg = online_ram(setup) ? MEM32(setup + ROAM_ENTRY + 0x0Cu) : 0;
    if (!online_ram(cfg) || MEM32(cfg) != ROAM_CFG_VT)
        return NULL;
    memcpy(out, (uint8_t *)XBOX_PTR(cfg), ROAM_CFG_SIZE);
    memset(out, 0, 4);
    return roam_cfg_name(out);
}

/* A peer's car record (rec, len bytes: a CarCfg, or one + the donor trailer)
 * into blob; a car we do not have becomes a stock donor. 1: blob is usable. */
static int mods_net_peer_rec(uint32_t id, const uint8_t *rec, int len, uint8_t blob[ROAM_CFG_SIZE])
{
    static struct { uint32_t id; char car[32]; } said[16];
    static int nsaid;
    char hint[MN_NAME + 1] = "";
    const char *name, *got;
    int i;

    if (len != (int)ROAM_CFG_SIZE && !(len == (int)MN_REC && !memcmp(rec + ROAM_CFG_SIZE, MN_TAG, 4)))
        return 0;
    memcpy(blob, rec, ROAM_CFG_SIZE);
    if (!(name = roam_cfg_name(blob)))
        return 0;
    if (roam_car_index(name) >= 0)
        return 1;
    if (len == (int)MN_REC)
        memcpy(hint, rec + ROAM_CFG_SIZE + 4, MN_NAME);
    char car[32];
    snprintf(car, sizeof car, "%s", name);
    if (!(got = mn_stock_cfg(hint[0] ? hint : NULL, blob)))
        return 0;
    for (i = 0; i < nsaid && !(said[i].id == id && !strcmp(said[i].car, car)); i++) {}
    if (i == nsaid) {
        i = nsaid < 16 ? nsaid++ : (int)(id % 16u);
        said[i].id = id;
        snprintf(said[i].car, sizeof said[i].car, "%s", car);
        fprintf(stderr, "[MOD] peer %08X drives %s, not installed here: shown as %s\n", (unsigned)id, car, got);
    }
    return 1;
}

/* Our car record to the peers (blob: our CarCfg, vtable 0). A mod car: the
 * record + donor trailer to cores >= 19, the donor's stock CarCfg to older. */
static void mods_net_send_cfg(const uint8_t blob[ROAM_CFG_SIZE])
{
    static char said[32];
    uint8_t rec[MN_REC], old[ROAM_CFG_SIZE];
    const char *name = roam_cfg_name(blob), *donor, *got;

    if (!name || !mods_net_is_mod(name)) {
        roam_set_cfg(blob, ROAM_CFG_SIZE);
        return;
    }
    donor = mn_donor_of(name);
    memcpy(rec, blob, ROAM_CFG_SIZE);
    memcpy(rec + ROAM_CFG_SIZE, MN_TAG, 4);
    memset(rec + ROAM_CFG_SIZE + 4, 0, MN_NAME);
    memcpy(rec + ROAM_CFG_SIZE + 4, donor, strnlen(donor, MN_NAME - 1));
    if (!(got = mn_stock_cfg(donor, old))) {
        /* shortcut: no stock record for old builds: they get this one, which their 0x108-byte check
         * refuses (our car shows as theirs there); a real fix would need a stock car in our list */
        roam_set_cfg2(rec, MN_REC, NULL, 0);
        return;
    }
    if (strcmp(said, name)) {
        snprintf(said, sizeof said, "%s", name);
        fprintf(stderr, "[MOD] our car %s is a mod car: older builds are sent %s\n", name, got);
    }
    roam_set_cfg2(rec, MN_REC, old, ROAM_CFG_SIZE);
}

/* ── System Link (Online Races) ──
 * CAR_CONFIG (msg 0x44, 0x114 B: header, a whole CarCfg at +8 copied with
 * operator= sub_00272010 -> sub_00271E20, slot byte +0x110) is built and sent
 * by sub_00025D20 only (session vtable 0x449100 +0x13C, thiscall (CarCfg *src,
 * slot (-1 ours), a3, a4, mask (0xCAFEBABE = the host)), ret 0x14): the
 * lobby's car pick (sub_0015E3C0 / sub_0015F3F0, screen +0x378) and the host's
 * per-slot resend before each RACE_CONFIG (sub_00024420 <- sub_001AE040,
 * [[0x53F2E4]+slot*0x2C+0x120]). RACE_CONFIG 0x43 and PLAYER_AND_INPUT 0x42
 * carry no car. The receive sub_00026370 (vtable +0x1A0, (msg, from)) checks
 * only the size and copies the CarCfg into the slot's setup entry; the car
 * list lookups after it (sub_00272FA0 and others) use the -1 of an unknown
 * name as an index: a read before the car vector.
 * Both are reached through the vtable only, so recomp_lookup_manual takes
 * them (no relift). System Link has no version exchange: a mod car (index >=
 * the stock count) is always sent as its donor's stock CarCfg, except to
 * ourselves (the host applying its own pick); a peer's unknown car becomes
 * vp_350z_04's stock CarCfg before the game copies it. */
void sub_00025D20(void), sub_00026370(void);

static void mn_sl_send(void)
{
    uint32_t src = MEM32(g_esp + 4), mask = MEM32(g_esp + 0x14), self = MEM32(g_ecx + 0x10),
             host = MEM32(g_ecx + 0x18), slot = MEM32(g_esp + 8), cfg;
    char name[23];
    if (slot == 0xFFFFFFFFu)
        slot = self;
    if (mask == 0xCAFEBABEu)
        mask = 1u << (host & 31u);
    if ((s_mn_stock > 0 || s_mn_nmod) && online_ram(src) && MEM32(src) == ROAM_CFG_VT
        && !(self == host && self != 0xFFFFFFFFu && slot == self && mask == 1u << (host & 31u))) {
        memcpy(name, (const char *)XBOX_PTR(src + ROAM_CFG_NAME), 22);
        name[22] = 0;
        if (mods_net_is_mod(name) && (cfg = mn_showroom(mn_donor_of(name)))) {
            static char said[23];
            if (strcmp(said, name)) {
                snprintf(said, sizeof said, "%s", name);
                fprintf(stderr, "[MOD] System Link: slot %u's %s is a mod car: sent as %s\n", (unsigned)slot, name,
                        (const char *)XBOX_PTR(cfg + ROAM_CFG_NAME));
            }
            MEM32(g_esp + 4) = cfg;
        }
    }
    sub_00025D20();
}

static void mn_sl_recv(void)
{
    uint32_t msg = MEM32(g_esp + 4), cfg;
    char name[23];
    if (online_ram(msg) && MEM16(msg + 2) == 0x114u) {
        memcpy(name, (const char *)XBOX_PTR(msg + 8 + ROAM_CFG_NAME), 22);
        name[22] = 0;
        if (name[0] && roam_car_index(name) < 0 && (cfg = mn_showroom(NULL))) {
            memcpy((uint8_t *)XBOX_PTR(msg + 8), (uint8_t *)XBOX_PTR(cfg), ROAM_CFG_SIZE);
            fprintf(stderr, "[MOD] peer slot %u drives %s, not installed here: shown as %s\n", MEM8(msg + 0x110), name,
                    (const char *)XBOX_PTR(cfg + ROAM_CFG_NAME));
        }
    }
    sub_00026370();
}

static recomp_func_t mods_net_lookup(uint32_t va)
{
    return va == 0x00025D20u ? mn_sl_send : va == 0x00026370u ? mn_sl_recv : (recomp_func_t)0;
}
