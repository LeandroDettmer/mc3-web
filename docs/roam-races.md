# Free roam races: AI races, online races and street challenges from the map

Design for three Free Roam (Online cruise) features in the Remix web build:

- **3. AI race from a map icon.** Start the race at a race icon against the game's
  AI, then come back to the Cruise at the same spot.
- **4. Online race from the map.** Create or join a System Link race from the map.
  Both players go into it without touching the System Link menus, and come back to
  free roam after it.
- **2. Street challenge (later).** Two nearby players flash headlights, the other
  accepts with a button, and they run a short race.

Everything below was read from the Remix disassembly
(`work-remix/analysis/disasm/asm/text.asm`, strings.json, the XBE's data). Nothing
here was run in the game. "Confirmed" means it was seen in the code. "To trace" means
one run with a trace is needed before building on it.

## 1. What the game has

### 1.1 Globals and objects

| Address | What |
|---|---|
| `[0x557ED0]` | mcGameState. State at +4, request queue at +0xC (10 slots, ring +0x34/+0x38/+0x3C). vt+4 `sub_00020E40` = Request(id), vt+8 `sub_00020EC0` = run the requests, vt+0x18 `sub_00021D30` = EnterState(n). |
| `[0x53F2D4]` | game mode (2 = career; Arcade path `sub_0013A400` writes 1; 3/4/5 other modes, not needed here) |
| `[0x53F2E0]` | race setup **in use** (the loaded race or cruise) |
| `[0x53F2E4]` | race setup **to load next**. State 2's enter (`sub_00021170`) builds the level from it. |
| `[0x53F2E8]` | Arcade setup screen's settings (same class) |
| `[0x50F14C]` | network kind: 0 = none, 1 = System Link (`sub_001801C0` sets it), 2 = Xbox Live |
| `[0x557EFC]` | mcNetSession (game session). Vtable 0x466560 (base); the game's derived vtable has its listener part at 0x449100 (OnState `sub_00025E30` at +0x30, OnPeerJoined `sub_000244C0` +0x34, member-left `sub_00026FD0` +0x38, per-frame `sub_00027490` +0x3C). `+0x10` < 0 = no session. |
| `[0x557EE4]` | city table, 0x50 B a city: +0x14 event count, +0x18 names, +0x20 events (0x30 B) |
| `[0x557F0C]` | career manager (progress +0xC) |
| `[0x50F148]` | front end. `[[+0xC]+0xC]` = screen manager (+0xE0 = screen asked for last). |

The race setup object (fields confirmed in `sub_0026B8E0`, `sub_0013B110`,
`sub_00184FD0`, `sub_00155AF0`):

| Offset | Field |
|---|---|
| +0x00 | city index |
| +0x04 | time of day (event +0xD) |
| +0x08 | weather (event +0xC) |
| +0x14 / +0x1C | set to 1 by SetEvent (+0x1C only for types 0xA/0xB/0xF/0x14) |
| +0x18 | RaceType (0 Arcade cruise, 1 career cruise, 4 unordered, 7 ordered/circuit, 8/9 blitz, 0x15 autocross, 0x16 track, the rest battle modes; labels `sub_0013A690`) |
| +0x40 | (DoStartRace caps it at 3 with >1 player) |
| +0x47 / +0x48 | 0xFF / custom race flag (0 = event race) |
| +0x4C | local players (entries used) |
| +0x54 | split-screen layout |
| +0x68 | event name (Arcade's lookup key, `sub_0026B010`) |
| +0x108 | network race flag (`sub_00155AF0` sets 1) |
| +0x10D | hookmen on (career cruise only; host path clears it) |
| +0x114 + i·0x2C | entries: +0x0C CarCfg*, +0x10 pad, +0x14 viewport (roam_game.c) |

Event record (0x30 B, reader `sub_0026DA70` from `tune/race/<city>.loc`; also in
roam_races.h): +0x00 RaceType, +0x04 AmbientDensity, +0x08 PedDensity, +0x0C Weather,
+0x0D Time, +0x0F **OpponentCount**, +0x10 NumLaps, +0x14 StartBeaconPosition (vec3),
+0x27 cutscene mask, +0x28..+0x2B non-competitor car indices.

### 1.2 States and requests

The state table comes from `sub_00021D30`'s jump table at 0x21E50. Request numbers
come from `sub_00020EC0`'s table at 0x2108C. A real trace (.scratch/touchtest) shows
boot 1 -> 4 (movies) -> 7 (front end) -> request 6 -> 2.

| State | Enter function | Meaning |
|---|---|---|
| 2 | `sub_00021170` | **in game**: loads the level from `[0x53F2E4]` (race or cruise) |
| 4 | `sub_00021390` | movies |
| 5 | `sub_00021A60` | garage |
| 6 | `sub_000217B0` | level reload (restart) |
| 7 | `sub_00021410` | front end (menus) |

| Request | Does |
|---|---|
| 6 | EnterState(2): load `[0x53F2E4]` |
| 7 | EnterState(2) (game session's restart) |
| 8 | `sub_00023AB0` on `[0x4CBAD8]` (end of current race events; career sends 8 before 6) |
| 0x12 | EnterState(5) garage |
| 0x15 | EnterState(6) reload |
| 0x16 | EnterState(7) front end |
| 1 / 2 | pause / unpause in game |

**From cruise straight into a race, with no menus.** Career Progress()
(`sub_002779D0`) does `Request(8); Request(6)` from the cruise (state 2) after it puts
the next race in `[0x53F2E4]`. The hookman street races use this path
(`DoMoveToNextHookmanRace` `sub_0016B730`). The **way back** is
DoQuitToCruise (`sub_0016A340`): it sets `[0x53F2E4]+0x18 = 0` (and +0x48 = 0), then
`Request(6)`.

### 1.3 Arcade: menu to race (feature 3)

1. The Arcade setup screen 0x19 (mcMenuArcade). Builder `sub_0013BA60`: rows for city,
   race, mode, players, game version and car. **There is no opponents row and no
   difficulty row.** Opponents come from the event (+0x0F OpponentCount). The AI's
   difficulty comes from the race's own tune data (`DifficultyModifier`
   `sub_0026F0F7`, rubber-banding `sub_001E3460`) and the career's relative
   difficulty (`sub_0027D590`). Arcade takes the defaults.
2. Go (`sub_0013B110`): with a stale name, the event is looked up by name
   (`sub_0026B010`). Then SetEvent `sub_0026B8E0` (thiscall (city, eventIdx), ret 8)
   fills +0/+4/+8/+0x18/+0x68. Each player's CarCfg is copied into the entries
   (`sub_00272010`). Then the next screen opens (`sub_00163830`).
3. mcMenuCustomRaceType::DoStartRace (`sub_00184FD0`, the StartRace item):
   - mode `sub_0013A400` (`[0x53F2D4]`)
   - per-entry fields, the viewport layout +0x54
   - for an event race: the career manager's current event `sub_00276EC0(city, idx)`
   - copy the settings to the next setup: `sub_00268E50(dst = [0x53F2E4], src = [0x53F2E8])`
   - **`Request(6)`**
4. State 2 builds the race. The race object comes from the factory `sub_001C11E0`,
   switched on +0x18 (byte table 0x1C13AC). Type 0 = `sub_001E1A30` (plain cruise,
   0x1250 B). Type 1 = `sub_001D6D70` (career cruise with hookmen, vtable 0x46A7D8,
   0x1418 B). The rest are the race kinds. AI opponents are made by the race from
   the event (`ExpectNumRacers`/`RacerData` `sub_001CE170`, `numopponents`
   `sub_001D4F20`). Only the human players come from setup +0x4C.

### 1.4 The race-over menu and the way back

The race-over and pause menu builder is `sub_0016ADD0` (items Replay, ContinueRace,
ContinueCruise, RestartRace, RestartCruise, NextRace, ReturnToLobby, CM_ChangeVehicle,
SD_Garage, ...). Most actions go through `sub_001642C0` -> `sub_00164480`. Quit to
cruise is `sub_0016A340`. In Arcade, the race ends on that menu, and its items lead to
the front end (Request 0x16), not back to a cruise. **So feature 3 needs its own way
back**: the DoQuitToCruise recipe from §1.2.

Spawn: a cruise starts at the city's start spot. Free Roam already moves our car after
a load: `roam_set_pose` (SetMatrix `sub_002A35A0` + SetVel `sub_003432B0`) followed by
`roam_teleport()`, at the first cruise frame (roam_game.c ~:1224). No game state holds
"resume here" for the Arcade cruise. The career keeps `CruiseStartData`
(`sub_00280073`, the save), and the debug menu "StartPoint: CurrentSpot"
(`[0x50A1B4]+0x18`, `sub_00187BD0`) is read by nothing else.

### 1.5 System Link: host, join, start (feature 4)

- **Enter:** Online Races (LAN_Play `sub_001801C0`). It sets `[0x50F14C] = 1`. If
  `[[0x523994]+0x9B8] == 0` it calls the net init `sub_001B0AD0`. Then Go(0x20) and
  session `sub_001ADB80(0)`, `sub_001AD480(0)`.
- **Host:** the custom race screen (`sub_00185A90`) in network mode adds HostGame
  (`sub_00185800`, callback `sub_00184FC0` -> **`sub_00155AF0`**, ecx =
  `[0x50F148]`). sub_00155AF0 is the whole "host a game":
  - `sub_00268E50([0x53F2E4], [0x53F2E8])`, +0x10D = 0, +0x108 = 1 (network race)
  - it picks a game-type code from +0x18
  - if there is no session yet (`[[0x557EFC]+0x10] < 0`): FE to the lobby
    (`sub_00155920`), then session vt+0x94(type), vt+0x98(...) and `sub_00047F20`
    (System Link). Then our address (`sub_00047F00`), `sub_001ADB80(live)`,
    `sub_001AD480(live)`, `sub_001ADB40(1)`, and `sub_0005C790(name, players, &addr,
    3, 3)` (create and advertise, HOST_INFO 0x5F)
  - else it updates the host info.
- **Join:** the join list screen (around 0x173000-0x174400: HS_Host, HS_City,
  HS_Mode, HS_NumPlayers, Refresh) calls **`sub_001730A0`** (thiscall (resultIdx, x,
  0), ret 0xC). It reads a discovery result, opens the lobby (`sub_00155920`), calls
  session `sub_001ADB00`, `sub_001ADB80(1)`, `sub_001AD480(1)`, `sub_001ADB40(1)`, and
  joins: `sub_0005C880(session id, XNADDR, key, ...)`. Discovery results:
  `[0x529CA8 + 4·i]` = slot, info 0x80 B at `0x5239A0 + slot·0x80`
  (`sub_001B12B0`), address 0x54 B at `0x527C88 + slot·0x54` (`sub_001B1330`: +4
  session id 8 B, +0xC XNADDR 0x24 B, +0x30 key 16 B). The list's count and the
  search start are near the net manager's globals (`0x529C78..0x529D38`, functions
  0x46000-0x4B000). **To trace.**
- **Lobby and start:** the lobby screen is mcLobby (0x15B000-0x15D000:
  LOBBY_WaitingForPlayerToJoin, LOBBY_Starting3/2/1/Go, `sub_0015BE30`). The message
  ids come from the name table `sub_001AFBB0`, `id - 0x40` (0x40 INPUT, 0x42
  PLAYER_AND_INPUT, 0x43 RACE_CONFIG, 0x44 CAR_CONFIG, 0x47 LOAD_RACE, 0x49
  GAME_STATE, 0x5A CUSTOM_RACE, 0x5B START_COUNTDOWN, 0x5C STOP_COUNTDOWN, 0x5F
  HOST_INFO). The senders sit in the session vtable 0x466560: +0x88 SendRaceConfig
  `sub_001AE040`, +0xC4 `sub_001AEF80` (0x5A), +0xC8 `sub_001AF030` (0x5B). The
  game session's per-frame `sub_00027490` follows the session state `[this+0x24]`
  after a 3 s delay: 5 -> Request(7) (restart), 9 -> Request(0x16) (front end).
  **Who presses "start" in the lobby, and where LOAD_RACE turns into Request(6), is
  still to trace.**
- **After the race:** the race-over menu (§1.4) has ReturnToLobby and NextRace. The
  host's choices reach the peers as RACE_CONFIG states (OnState `sub_00025E30`:
  host_selected_next_race / restart / frontend).
- **Rooms vs free roam:** the /lan switch routes System Link datagrams only inside a
  room. Free roam's port routes whatever the room
  (web/server/crates/lan/src/lib.rs:579). So a player can be in a race room and stay
  in the free-roam mesh. In Free Roam our code drops the session's datagrams while
  `[0x50F14C] == 0` (`fr_sl_detached`, recomp_manual.c). A race sets it to 1, so they
  flow again.

### 1.6 The headlight challenge (feature 2)

- The original MC3 street challenge is the **career cruise's hookmen**: race type 1,
  class at vtable 0x46A7D8, ctor `sub_001D6D70`. The flag is `[setup+0x10D]`. The
  hint "flash to begin" / "circle to begin" (PS2 pad scheme `sub_002CA830`) comes
  from `sub_001D7BF0` / `sub_001D7F00`. The race is `mcRaceHookmen`, with messages
  CC_looking_for_a_challenger / CC_new_race_available `sub_001D7460`. Their races are
  career data (HookmanIdx, CruiseRaceName, the career save).
- **The Remix's Arcade cruise does not have it.** Free Roam loads race type 0
  (`sub_001E1A30`), which has no hookmen. Type 1 needs the career (`[0x53F2D4] == 2`,
  career manager state, hookman tables). Bringing it into Free Roam would mean faking
  the career. That is not a route.
- **Input:** "Flash headlights" is the **White** button (web/index.html:641,
  web/touch.js). The car controller's button word `[c+0x28]` (24 bits) already goes to
  peers in rc_state.ext (`roam_ctl_pack`, roam_game.c:944). **Which bit is White is
  to trace** (one run with MC3_ROAM_TRACE, pressing White).

## 2. Recommended approach per feature

### Feature 3: AI race from a map icon (game-native race). Effort: about 3 days + 1 test round

Use the game's own race, loaded from the cruise the way the career does it. There is
no front end, so there are no menus to drive.

1. **Pick:** at a race icon, `roam_races_nearest(city, x, z, 30 m)` (roam_races.h,
   being built by the roam-map-icons scout) gives `{city, index, type}`. A prompt says
   "White: start <race>". The type must be a single-player kind (not 0/1, not the
   battle modes). Skip races of another city.
2. **Leave the cruise:**
   - save our pose (the last `s_roam_hist` entry)
   - roam status RC_ST_LOADING, so peers park and hide our car
   - in `[0x53F2E4]`: copy `[0x53F2E0]` (`sub_00268E50`), then SetEvent
     `sub_0026B8E0(ecx = E4; city, index)`, +0x4C = **1**, +0x48 = 0, +0x10D = 0,
     +0x108 = 0
   - `Request(8)`, then `Request(6)`. That is Career Progress()'s pair.
3. **Guard the free-roam slots.** `roam_build` (roam_game.c:816) adds remote slots to
   any one-player, non-network load. It must also require `+0x18 == 0` (cruise), or
   the race gets N undriven "racers". Check `roam_cars_load` the same way.
4. **Race end:** hook the race-over menu builder `sub_0016ADD0`. In the Online
   session it adds "Back to Free Roam" or replaces Quit. A simpler first cut: after
   the race result (the race's ReportRaceResult, or the menu's build), do it
   automatically after N s or on A. That runs DoQuitToCruise's recipe:
   - in `[0x53F2E4]`: `fr_cruise()` (the city's first type-0 event; recomp_manual.c),
     +0x48 = 0, +0x4C = 1 (roam_build adds the slots again)
   - `Request(8)`, `Request(6)`
5. **Same spot:** a flag "restore pose" makes the first cruise frame (the existing
   `!s_roam_cfg_loaded` branch) call `roam_set_pose(player 0's vehicle, saved m,
   zero vel)` and then `roam_teleport()`. Streaming around the new spot is the same
   as after the garage exit.

Risks:
- Request 8's `sub_00023AB0` on an Arcade cruise is untested. It was seen only on the
  career path. If it misbehaves, try Request(6) alone (DoQuitToCruise sends only 6).
- Race music and HUD state from Arcade mode `[0x53F2D4]` may need mode 1
  (`sub_0013A400`), as DoStartRace sets it.
- Money, rewards and the career are untouched because Online skips saves. The
  results screen may try to report to the career manager (`sub_0024F8C0` path): check
  that it is mode-gated.
- Heap: a race loads opponents' cars. Remote slots are not built (step 3), so the
  heap margin is better than in the cruise.
- Pose restore far from the city's start: a short pop-in while streaming.

### Feature 4: online race from the map (game-native System Link race). Effort: about 6-8 days, with a 1-day trace spike first

Only the game's own System Link carries a real multiplayer race (grid, AI, results,
remote cars). Use it, and skip its menus by calling the front-end functions it uses.

**Page side** (web/lan.js already has `link.create/join/leave`): the game asks the
page through a small SAB box (like `roam_room_box`) for "host race <city,index>" or
"join room <id>". The page makes or joins the race room and returns ok or an error.
Peers learn about the race through the free-roam hub, for example a map icon
"<nick>'s race" (needs a new roam message kind or the FAR list). That is our own
protocol, not the game's discovery.

**Game side.** System Link needs the front end (screens, lobby), so:
1. **Leave the cruise to the front end:** roam status, `online_leave()` must not run
   (the session and garage stay). Request(8) + Request(0x16), or the pause menu's
   quit.
2. **Host,** from a per-frame front-end hook once the screen manager is up:
   - `[0x50F14C] = 1`, net init if needed (as `sub_001801C0`)
   - fill `[0x53F2E8]`: SetEvent(city, idx), +0x4C = 1, car = the profile's
   - call `sub_00155AF0` (ecx = `[0x50F148]`). That creates the session and opens the
     lobby.
3. **Join:** `[0x50F14C] = 1`, open the join list (FE Go to its screen, which starts
   discovery), wait until result 0 exists (only the room's host is visible), then
   call `sub_001730A0(screen; 0, ...)`.
4. **Start:** when the room's members are all in the lobby, the host's start. **To
   trace:** the lobby's Start handler (mcLobby `sub_0015BE30` and its callbacks) and
   the LOAD_RACE -> Request(6) path. If it can't be called cleanly, press the pad
   button (A/Start) through the input path MC3_AUTOSTART uses (src/autostart.c),
   gated on the lobby screen being shown.
5. **Back to free roam:** on the race-over menu, the host's front end
   (`host_selected_frontend`) brings everyone to state 7. Our hook then runs Free
   Roam's entry (`sub_00180160`: online_enter, roam_begin, Arcade's path) and Go
   (`sub_0013B110` on screen 0x19), with "restore pose" set as in feature 3. Reset
   `[0x50F14C] = 0` and leave the session (`sub_00049A80`) before that.

Risks:
- The lobby internals are the biggest unknown. Mitigation: trace first (step plan
  F4.0).
- The front end trip costs two loads (cruise -> FE -> race -> FE -> cruise),
  ~10-20 s each on the web. That is acceptable for an online race.
- The Online session's overrides (money, unlocks, no saves, `fr_net_drop` of state 5
  and the restart path) must allow a System Link game. They test
  `[0x50F14C] == 0`, so they step aside. Verify that `online_leave` is not triggered
  by the Arcade/LAN entry points (`sub_0018C530` is Arcade's, so call
  `sub_0018C530_gen`, as Free Roam already does).
- A peer that never joins: time out in the lobby and return to free roam.
- Version check (HS_IncompatibleVersion): both are the same build.

**Cheaper alternative,** if 4 is too heavy: run feature 2's own race (below) with a
start point from the race list. No game-native AI or results, but no loads and no
menus. Effort: about 3 days on top of feature 2.

### Feature 2: headlight challenge (our own lightweight race in the cruise). Effort: about 4-5 days

The hookmen can't come to the Arcade cruise (§1.6), so the race is ours. Everyone
stays in the cruise: no load, no slots rebuilt, and the cars are already shared.

1. **Challenge:** A presses White within 25 m of B, roughly facing B. A sends a roam
   message CHALLENGE{to, race id}. This needs a new KIND in xboxrecomp/src/net/roam
   plus a hub pass-through; the White bit in the shared ctl word alone is not
   explicit enough.
2. **Accept:** B sees "<nick> challenges you, White: accept" (the page overlay
   roam_tags.c already draws text over cars). B presses White within 8 s and sends
   ACCEPT. A timeout or driving away means decline.
3. **Race:** the finish is the nearest race start point 600-1500 m away
   (`roam_races_get`), or a point straight ahead on the road net if no such point
   exists. Show a map icon for the finish, a 3-2-1 countdown on both (the overlay),
   and freeze inputs during it (zero the controller's +0x0C/+0x10 as the pad, or
   only show "GO" and trust players).
4. **Finish:** the first car within 20 m of the finish sends FINISH{t}. The race is
   decided on the sender's clock, with the hub's relay order as tiebreak. Show the
   result.
5. **Traffic:** stays as it is (local per player).

Risks:
- Cheating and fairness: peer-reported finishes. Fine for a casual mode.
- The finish point is not on a road: use race start beacons only, which are on roads.

## 3. Step plan

**Shared, first (0.5 day):**
- S0. In `roam_build` / `roam_cars_load`, require race type 0 (`[setup+0x18] == 0`)
  before adding slots. Log '[ROAM] race load: no slots'.

**Feature 3 (AI race):**
- F3.1 `roam_race_start(const roam_race *)`: save the pose, status LOADING, fill
  `[0x53F2E4]` (copy, SetEvent, counts), Request(8) + Request(6). Log '[RACE] start
  <name> (type, opponents)'. MC3_ROAM_RACE=0 turns it off.
- F3.2 Trigger: White near an icon while driving slowly (the prompt is in roam_map.c's
  HUD, or the page overlay). Use the input layer's button state, not the car
  controller.
- F3.3 Way back: hook `sub_0016ADD0`, and in the Online session add "Free Roam". The
  first cut is automatic after the results. Then `[0x53F2E4]` = fr_cruise,
  Request(8) + Request(6).
- F3.4 Restore the pose at the first cruise frame (roam_game.c's
  `!s_roam_cfg_loaded` branch), then `roam_teleport()`.
- F3.5 Compile check; the owner tests one sprint and one circuit, then checks
  repeated round trips for the heap margin ('[ROAM] heap' lines).

**Feature 4 (online race):**
- F4.0 **Trace spike (1 day, the owner plays one System Link race between two
  browsers):** add a tracer for MC3_STATE_TRACE plus calls to `sub_00155AF0`,
  `sub_001730A0`, `sub_0005C790`, `sub_0005C880`, session vt+0xC8/+0xC4/+0x88, and
  incoming message ids (`sub_001AFBB0`'s id at the receive dispatch). It records
  which function starts the countdown and which turns LOAD_RACE into Request(6).
- F4.1 Page: the game -> page box (host / join / leave), lan.js room calls, and the
  result back. Peers learn about open races over the hub (a map icon).
- F4.2 Cruise -> FE without ending the Online session. FE hook: host
  (`sub_00155AF0`) or join (open the join list, `sub_001730A0` on result 0), with a
  timeout.
- F4.3 Start the lobby automatically (the call found in F4.0, else a pad press on
  the lobby screen).
- F4.4 Race end -> FE -> the Free Roam entry + Go + pose restore. `[0x50F14C] = 0`,
  leave the session and the room.
- F4.5 Compile check; the owner tests 2 browsers, then 3.

**Feature 2 (challenge), after 3 (shares the pose and prompt pieces):**
- F2.0 Find White's bit in `[c+0x28]` (MC3_ROAM_TRACE run), or read White from the
  pad layer.
- F2.1 Roam KIND CHALLENGE / ACCEPT / FINISH in xboxrecomp/src/net/roam (own commit
  `xboxrecomp:`) and hub pass-through (crates/roam, lan-worker).
- F2.2 Game: proximity and facing test, prompt, finish point from roam_races, finish
  detection, result text (page overlay).
- F2.3 Compile check; the owner tests in two browsers.
