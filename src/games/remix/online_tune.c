/* ── Free Roam: the pause menu's Tune Your Vehicle (browser only) ──
 *
 * Included by recomp_manual.c after roam_cars.c (needs s_online, roam_send_cfg,
 * roam_free_cruise, online_garage_left).
 *
 * The pause menu's item list is built by sub_0018D610 (reached through the
 * menu's vtable only, so recomp_lookup_manual takes it, no relift). Its first
 * item, CM_TuningOptions (screen 0x3A, mcMenuIGTune vtable 0x4638B8, list
 * sub_0018FC70: Drift Factor / Traction / Friction Bias, Reset, Test, Exit),
 * is added only when all of these hold:
 *   state [0x557ED0]+4 not 7 (main menu) and not 3;
 *   race flow phase [[0x557EEC]+4] == 5 (the Free Roam cruise drives in 6);
 *   local players [[0x53F2E0]+0x4C] == 1 (Free Roam puts 1 + remote slots there);
 *   car from the garage: byte 0x53F2FC == 0 (1 = a showroom car, Online's usual);
 *   page byte [[[[0x50F148]+0xC]+0xC]+0xE9] == 0.
 * In the Online Free Roam cruise (race type 0, not an AI or group race) the
 * last four are made to hold for the build only and put back after.
 *
 * Exit (sub_0018FBF0, a menu item callback) copies the tuned CarCfg (setup
 * entry 0, [0x53F2E0]+0x120) over garage car [0x53F80D] (0x53F810 + i*0x108,
 * sub_00271E20), sets the profile dirty byte 0x557648 and copies it into the
 * next setup [0x53F2E4]. Online the garage blob is the Online profile's (the
 * career's is kept aside until online_leave, and sub_00022040 never writes the
 * career while Online), so:
 *   garage car: the slot written is the pick's (0x53F2FE), and the Online
 *     profile is saved at once (online_garage_left);
 *   showroom car: the garage slot is put back as it was (it is another car);
 *     the tune holds for this cruise and its reloads, until Free Roam is
 *     entered again (sub_0013AB20 copies the showroom's CarCfg then).
 * The dirty byte is put back (no career save prompt from an Online tune).
 * Peers get our CarCfg once, at Exit (roam_send_cfg sends a changed record
 * only; they rebuild our car once). The sliders send nothing. */

#ifdef __EMSCRIPTEN__
#define TUNE_CAR_FROM   0x0053F2FCu     /* 1 showroom car, 0 garage car (ONLINE_CAR_FROM) */
#define TUNE_CAR_INDEX  0x0053F2FEu     /* int16, its index */
#define TUNE_GARAGE_CUR 0x0053F80Du     /* byte: the garage's current car */
#define TUNE_GARAGE_CAR 0x0053F810u     /* + i * 0x108: the garage's CarCfgs */
#define TUNE_CFG_SIZE   0x108u
#define TUNE_DIRTY      0x00557648u     /* the profile's dirty byte (sub_000197F0) */

void sub_0018D610(void);                /* lifted: the pause menu's items */
void sub_0018FBF0(void);                /* lifted: the tune menu's Exit */

static int online_tune_here(void)
{
    uint32_t setup = MEM32(ROAM_SETUP_VA);
    return roam_free_cruise() && online_ram(setup) && MEM32(setup + 0x18u) == 0u;
}

static void online_tune_menu(void)
{
    static uint32_t said[5] = { ~0u, ~0u, ~0u, ~0u, ~0u };
    uint32_t setup = MEM32(ROAM_SETUP_VA), flow = MEM32(ROAM_FLOW_VA), fe = MEM32(0x0050F148u), page = 0;
    uint32_t now[5], players, phase, from, e9;
    if (!online_tune_here() || !online_ram(flow) || !online_ram(fe) || !online_ram(MEM32(fe + 0xCu))
        || !online_ram(page = MEM32(MEM32(fe + 0xCu) + 0xCu))) {
        sub_0018D610();
        return;
    }
    players = MEM32(setup + 0x4Cu);
    phase = MEM32(flow + 4u);
    from = MEM8(TUNE_CAR_FROM);
    e9 = MEM8(page + 0xE9u);
    now[0] = MEM32(0x00557ED0u) ? MEM32(MEM32(0x00557ED0u) + 4u) : ~0u;
    now[1] = phase; now[2] = players; now[3] = from; now[4] = e9;
    if (memcmp(now, said, sizeof now)) {
        memcpy(said, now, sizeof now);
        fprintf(stderr, "[ONLINE] tune: pause menu with Tune Your Vehicle (state %u, race flow phase %u -> 5, "
                        "players %u -> 1, %s car, page +0xE9 %u -> 0 for the build)\n",
                (unsigned)now[0], (unsigned)phase, (unsigned)players, from ? "showroom" : "garage", (unsigned)e9);
    }
    MEM32(flow + 4u) = 5u;
    MEM32(setup + 0x4Cu) = 1u;
    MEM8(TUNE_CAR_FROM) = 0;
    MEM8(page + 0xE9u) = 0;
    sub_0018D610();
    MEM8(page + 0xE9u) = (uint8_t)e9;
    MEM8(TUNE_CAR_FROM) = (uint8_t)from;
    MEM32(setup + 0x4Cu) = players;
    MEM32(flow + 4u) = phase;
}

static void online_tune_exit(void)
{
    static uint8_t keep[TUNE_CFG_SIZE];
    uint32_t setup = MEM32(ROAM_SETUP_VA), cur, slot, from, idx;
    uint8_t dirty;
    if (!s_online || !online_tune_here()) {
        sub_0018FBF0();
        return;
    }
    cur = MEM8(TUNE_GARAGE_CUR);
    from = MEM8(TUNE_CAR_FROM);
    idx = MEM16(TUNE_CAR_INDEX);
    dirty = MEM8(TUNE_DIRTY);
    if (!from && idx < 30u && idx < MEM8(TUNE_GARAGE_CUR - 1u))   /* [0x53F80C]: the garage's car count */
        MEM8(TUNE_GARAGE_CUR) = (uint8_t)idx;                    /* the pick's slot gets the tune */
    else
        from = 1;                                                /* not a garage car: its slot stays */
    slot = TUNE_GARAGE_CAR + MEM8(TUNE_GARAGE_CUR) * TUNE_CFG_SIZE;
    memcpy(keep, (uint8_t *)XBOX_PTR(slot), TUNE_CFG_SIZE);
    sub_0018FBF0();
    if (from)
        memcpy((uint8_t *)XBOX_PTR(slot), keep, TUNE_CFG_SIZE);
    MEM8(TUNE_GARAGE_CUR) = (uint8_t)cur;
    MEM8(TUNE_DIRTY) = dirty;
    fprintf(stderr, "[ONLINE] tune: exit, %s\n", from ? "showroom car: tune kept for this Free Roam, garage untouched"
                                                       : online_garage_left() ? "garage car: Online profile saved"
                                                                              : "garage car: Online profile not saved");
    roam_send_cfg(MEM32(setup + ROAM_ENTRY + 0x0Cu));
}
#endif
