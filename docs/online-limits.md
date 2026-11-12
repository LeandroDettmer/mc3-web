# Online limits: what caps Free Roam, what was hit, what can be raised

Study of 2026-11-12, from player logs at peaks of 20, 25 and 30 players in
Free Roam (26 in San Diego), 120-132 online. Numbers marked *estimate* were not
measured. Design rule from the owner: a player only needs to see the players
near him; nothing should be raised just to show far players.

## 1. Game-side limits (per client)

| Limit | Value | Where | Hit? | Notes |
|---|---|---|---|---|
| Game car cache | 16 entries | `[0x50A08C]`, ctor `sub_00103B40`, get `sub_00105B80` | was hit: 17th car of any kind froze the game | Fixed 3ff3f9a: standalone cars take entries from our own caches (now 80). Player builder `sub_002CB2D0` asks key 0, so no sharing by model. |
| Remote car caches (ours) | 80 entries | `roam_pool.c` | 30 at peak | Plenty. |
| Vehicle registry | 42 (raised from 10) | `roam_cars.c` (`sub_002BE2C0` override) | 33 at peak | Raised at every cruise load since ebb9294 (phones stayed at 10 before). The registry list holds 4 entries per car; count cars as `ceil(count / (k/4))`. |
| Particle emitters | 256, ~9 per car | particle manager | **hit**: car 27 broke (XQUE58, PC, 33 cars) | Being fixed (scout icall-35dc7b). Cap remote cars below it, or give each standalone car fewer emitters. |
| Remote cars shown | `MC3_ROAM_CARS`: 32 on PC (128 MB), 7 on phones/low-mem | `roam_cars.c` | 32 once (PC) | Slots included. |
| Car radius | 250 m (`MC3_ROAM_RADIUS`, 0 off), give back after 3 s beyond 300 m | `roam_pool.c` ROAM_RADIUS | n/a | Matches the game's own traffic fade (250 m, `0x4AFD18`). Players beyond it are map blips only (no car, no draw cost). Lowering it saves a few draws and MB, but more players cross the edge, and each crossing builds a car (a short hitch). |
| Player slots | 8 (us + 7) | `GetPlayer sub_002BF570` (209 callers), `sub_002BDC00` unrolled 8x | n/a | Not worth raising; standalone cars have replaced slots for more cars. |
| Main heap | 47-95 MB (`MC3_HEAP_MB`), ~0.8-1.1 MB per car | `roam_pool.c` heap check | PC: 33 MB free at 33 cars. Phones: 3.9-4.8 MB free, so ~4 cars | Phones are heap-bound. Rebuilds keep 3 MB, new cars 6 MB (370f417). |
| Cars heap | 48 MB of the contiguous window at 128 MB (`MC3_ROAM_HEAP_MB`) | `roam_cheap` | off on the page today (index.html), main heap used | Can be turned on for PCs if the main heap runs short. |
| Race ghosts | 6 at race load, 128 MB only | `roam_pool.c` roam_ghost_* | n/a | No builds mid-race. |
| Minimap far blips | 20 nearest + any within 400 m | `roam_map.c` RMAP_MINI_FAR_N | cosmetic | The full map draws everyone. |

## 2. Network limits

| Limit | Value | Where | Hit? | Notes |
|---|---|---|---|---|
| Hub links per player (K) | 32 nearest (client asks max(cars, 32)) | `roam_game.c` roam_set_k; hub default_k 32 | 25 linked at 30 players | A pair is linked only while both are under their K. At 33+ players in one city the farthest pairs stay unlinked (dimmed far blips). Kept at 32 on purpose: the car radius only shows near players anyway; 48 would add ~40% city-DO load. |
| Roam core list | RC_LIST_MAX 96, RC_MAX_PLAYERS 65, group cap 97 | `xboxrecomp/src/net/roam/roam_core.h` | no | |
| FAR list | 256 players, parts of 20 per datagram | hub msg 9 | no | Reassembled by the core. |
| P2P channels | 32 busiest destinations (rtc roamMax; 6 on low-mem) | `web/rtc.js` | 14-16 open | The rest goes through the city DO. |
| TURN relays | relayMax 8 per client | `web/rtc.js` | 21 refusals in 1 log | Could go to 16; TURN egress is ~11 GB/month (free 1 TB). |
| Players panel list | PLAYERS_MAX 200 | `web/lan-worker/src/switch.js` | no | |
| Tabs per address | perIp 8, perCid 2 | `switch.js` GUARD | only shared networks (LAN house, campus) | Real refusal (HTTP 429 / 'too many tabs'). |
| Rates | RATE 2000 frames/s per client, CTL 40/s burst 400 | `switch.js` | no | |

## 3. Server and cost limits (Cloudflare, Firebase)

Analytics could not be read (the token lacks Account Analytics: Read and
Billing: Read), so these are *estimates* at 132 online / 25 in Free Roam, for 24 h.

| Item | Estimate / day | Free limit / day | If paid |
|---|---|---|---|
| Durable Object requests (city + switch; 20 WebSocket messages = 1 request) | 1.7-9.5 M | 100 k | ~$8-43 / month |
| Durable Object duration | 35-54 k GB-s | 13 k GB-s | ~$15 / month |
| Worker requests | 20-55 k | 100 k | within |
| Firebase Hosting transfer | 300-400 MB (config.json poll was ~150 MB) | 360 MB (Spark) | Blaze: pay as you go |
| TURN egress | ~0.37 GB (owner's dashboard: 366 MB egress, 619 MB ingress) | 1 TB / month | $0.05 / GB |

The city DOs kept answering past the free request limit, so the account is
probably on Workers Paid; confirm in the dashboard (Workers & Pages -> Plans).
Over a free limit, Cloudflare and Firebase Spark do not bill: requests fail
until the daily reset (00:00 UTC), i.e. the online or the site goes down.

City DO CPU: San Diego with 26 players takes ~2.7 k incoming messages/s
(~100 relayed frames/s per player without P2P); ~6.8 k at 40 players with
K 32. One DO is one thread: near one core at ~40 players in one city.

## 4. What can be done, by priority

1. **Particle emitters (256)**: cap so no client passes ~26 remote cars, or
   fewer emitters per standalone car. In progress (icall-35dc7b).
2. **Firebase transfer**: done in 6dfea33 (config.json poll 5 min, visible tab
   only); also check whether the project is on Spark or Blaze.
3. **City DO load at 40+ players in one city** (Worker deploy, drops everyone):
   - bundle the frames a client sends per tick into one WebSocket message
     (KIND_BUNDLE), and HELLO + SNAP in one message: fewer DO requests and less CPU;
   - raise the P2P share (more direct channels, relayMax 8 -> 16) so fewer frames go through the DO;
   - split a crowded city across two DOs by area (the hub already picks links by distance).
4. **Phones**: heap-bound at ~4 cars. Gains only by freeing heap (fewer
   systems, smaller cars); keep the radius and the nearest-gets-the-car rule.
5. **Analytics token** (Account Analytics: Read + Billing: Read) in
   `.deploy/.cf.env`, so cost can be read instead of estimated.

Not worth doing: raising K, the 8 player slots, or the far-blip count; the
owner's rule is that only near players matter.

Sources: scouts cap-20-check, limits-at-30, peak-25-health, cost-now,
view-distance-facts (2026-11-12); logs in `.scratch/logs-20261112*`.
