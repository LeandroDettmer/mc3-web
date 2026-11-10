# Online Races: player invites

**Status: designed, not implemented.** This file is the design of a planned feature.
Nothing here exists in the code yet; paths and line numbers below are where the
change would go (checked against the tree on 2026-11-10).

Extends the room contract in [races-protocol.md](races-protocol.md). Same rules: JSON
text messages, one object per message, a `"t"` field.

## Goals

- The host of a race room can invite any online player from the page's player list.
- The invited player gets a prompt, in the start page and in game, and can accept or
  decline. Accepting puts them in the room (no password: the invite is the host's
  authorization).
- Players can opt out of invites. Nobody can be spammed.
- No new Cloudflare cost worth noting, no timers, no stored state.

Non-goals: inviting into Free Roam, invites from room members that are not the host,
friends lists, offline invites.

## Where it runs

Invites are **Worker-only** (the switch in `web/lan-worker`, which extends the races
Lobby). The Rust server (`web/server/crates/lan/src/lib.rs`, `control()` :441) has no
`{"t":"players"}` push, so it has no player list to invite from. The page already
degrades (`liveStats()`, web/index.html:1883, needs `lan.players.list`): it hides the
invite UI when `lan.players` is missing. Rust parity is optional later: a `players`
push plus `invite`, about 150 lines.

The `players` list entries have no `vip` today (web/lan-worker/src/switch.js:72-83);
this feature adds `vip` and `inv`.

## Messages

### Client -> server

| Message | Rule |
|---|---|
| `{"t":"invite","to":vip}` | Sender must be the **host** of a room. Target must be live, have said hello and be opted in. |
| `{"t":"invite-reply","id":n,"ok":0\|1}` | Accept (1) or decline (0) a pending invite of mine. |
| `{"t":"invites","on":0\|1}` | Opt in or out, live. No broadcast to peers. `on:0` also drops my pending invites. |
| `hello` gains `"inv":0\|1` (default 1) | The opt-out survives a reconnect (the page sends it from `localStorage` `mc3.invites`). |

### Server -> client

| Message | When |
|---|---|
| `{"t":"invite","id":n,"from":vip,"name":nick,"room":id,"rname":s,"ttl":30000}` | To the target. `id` is unique per target. |
| `{"t":"error","op":"invite","code":c,"to":vip,"wait":s?}` | An `invite` refused. Codes: `off` `busy` `gone` `cooldown` `rate` `full` `host` `self`. `wait` (seconds) comes with `cooldown`. |
| `{"t":"error","op":"invite-reply","code":c}` | An accept refused. Codes: `expired` `gone` `full`. |
| players list entry `{name,mode,vip,inv:0\|1,...}` | `inv` is 1 only if the player is opted in. |

- **Decline is silent to the sender** (no pressure, no harassment). A decline still
  counts for the pair cooldown.
- **Accept:** the sender sees the normal `room` member update, nothing new.
- There is no "invite gone" push: an accept answers `gone` or `expired`, and the page
  drops the row at its `ttl`.

## Server rules and numbers

State lives in the socket attachments (survives hibernation, nothing in storage):

- target: `a.invs = [{id,from,room,at}]`, at most 3; `a.noinv` (opted out); `a.invN`
  (id counter);
- sender: `a.sent = [[toVip, ts], ...]`, at most 15, pruned to 10 minutes.

The attachment cap is 2 KiB; now about 400 B, plus about 210 B (`invs`) and 330 B
(`sent`): fits.

`invite` checks, in this order:

1. `self` (target is me);
2. sender hosts a room (`c.a.host`), else `host`;
3. room members < max, else `full`;
4. target live and said hello, else `gone`;
5. target opted out -> `off`;
6. same-target cooldown -> `cooldown` with `wait`;
7. sender rate -> `rate`;
8. target pending >= 3 (after dropping the expired ones) -> `busy`.

Then push to the target and log in the sender's `sent`.

`invite-reply` with `ok=1`: find `id` in my `invs` (else `expired`); age > 30 s ->
`expired`; the host is still there and `host.id == inv.room`, else `gone`; members >=
max -> `full`. Then **skip the password** (the invite is the host's authorization) and
enter the room as `join` does: leave the current room first. If I host a room, it
closes, so the page asks `confirm()` first. Remove the invite.

| Number | Value | Why |
|---|---|---|
| TTL | 30 s | Time to read or alt-tab; shorter than the cooldown. |
| Pair cooldown | 60 s | Longer than the TTL: never two live invites for one pair; also after a decline. |
| Sender rate | 6 per 60 s and 15 per 10 min | Fills an 8-player room in a minute; a sweep of 200 players is impossible. |
| Target pending | 3 | A flooded player never sees a wall of prompts. |
| Control bucket | 20/s (existing `CTL`) | Still applies on top. |
| Room max | unchanged (2..8) | See "Players per room". |

Other cases:

- **Target reconnects:** a new socket is a new attachment, so pending invites are
  gone. Fine with a 30 s TTL.
- **Addressing by vip** is safe: a vip is reused only by `?vip=` resume of the same
  client. The invite also stores `from` and the room id, and the accept re-verifies
  the room's host. A different client that takes a freed vip could receive an invite
  meant for the old holder only if the sender invited in that gap; accepted (the
  sender's name is shown).
- **Host leaves:** the room closes, so an accept answers `gone`. No scan needed.

**Cost:** an invite is one incoming message (billed 1/20 request) plus free outgoing
messages; an accept one more. 1000 invites a day is about 100 request-equivalents of
the 100k free. The opt-out toggle only calls `who()` (at most one push per 2 s). The
players list grows about 14 B per entry (outgoing, free). No new alarms or timers.

## Opt-out

The page's Online tab has an **Allow invites** checkbox, kept in `localStorage`
`mc3.invites` (`'0'` = off). It is sent as `hello.inv` on every connect and as
`{"t":"invites","on":n}` when toggled. An opted-out player shows `inv:0` in the list,
the Invite button is disabled for them, and a forced `invite` gets `off`.

## Accept flow

1. Host clicks Invite on a player. If the host is in no room, the page creates
   `<nick>'s race` (no password), waits for `room`, then sends the queued invites.
2. Target gets `invite`, the page shows a prompt with a countdown to `until = now +
   ttl`.
3. Accept -> `invite-reply ok:1`. The page shows the toast `Joined <race>. Open Online
   Races -> Join Game.` The player is in the room (the game's System Link sees only
   its players) but the game itself is not driven: see the next section.
4. Decline -> `invite-reply ok:0`, the row goes. Ignoring it: the row goes at the ttl.

## Taking the game to the room screen

**Designed, unverified in game** (static read of the Remix code, nothing run). Until it
is built, Accept only joins the page's room and the player opens **Online Races -> Join
Game** by hand.

Mechanism: direct calls from a per-frame hook in the game; pad injection only as a
fallback.

- **Path:** main menu -> Online `sub_0018C5A0` -> Go(0x1E) -> LAN_Play `sub_001801C0`
  (`[0x50F14C]=1`; net init `sub_001B0AD0` if `[[0x523994]+0x9B8]==0`) -> Go(0x20)
  Host/Join -> the join list, screen id 0x27 (MenuNetworkHostSelect,
  screen = `[[[0x50F148]+0xC]+0xC]+0x90`). Go = `sub_00163830(mgr, id)` with
  mgr = `[[[0x50F148]+0xC]+0xC]`, the call Free Roam's Arcade path already uses.
- **Pick the inviter's session:** results count `[0x524854]`; row i's XNADDR =
  `sub_001B1330(i)+0xC`, whose first dword is the host's vip (xnet_hle.c:185); session
  id = `sub_00046710(i)`.
- **Join:** `sub_00172FD0(screen, i)` (thiscall, ret 4; needs `[0x50F14C]==1`; sets
  `[scr+0x1EC]=2`, the id at +0x1F0/+0x1F4). The screen's own per-frame machine then
  runs `sub_001730A0` -> `sub_0005C880` and the game opens the lobby.

| Player is in | Then |
|---|---|
| Title screen, no profile | Wait for the main menu (joining needs a profile) |
| Menus, garage | Go now |
| Free Roam | Request(8) + Request(0x16) back to the menus (as roam_race_ai.c:209), then go |
| Another lobby | State 2 leaves the old session, then join |
| A race, or a save running | Queue until the menus |

Risks: the lobby step is unverified; the session list needs ~1-2 s of discovery before
the row exists; Free Roam's money and unlocks may leak into the race session.
Estimate: a 1-day trace spike, then ~3 days.

## UI

**Start page, Online tab** (cards about 420 px wide; one column under 600 px):

```text
[x] Allow invites
+-- Invites for you ------+ +-- Players online -------+
| Ann invites you to      | | Ann        [Invite]     |
| "Ann's race"  24 s      | | Bob (off)  [Invite] dis |
| [Accept]  [Decline]     | | You                     |
+-------------------------+ +-------------------------+
```

**In game** (the stage is visible and the canvas owns input), two non-modal pieces,
never a `<dialog>` so the game keeps the keyboard:

- **Left: toast stack** at `top:56px; left:16px` (under the stage bar, clear of the
  touch pad in the bottom corners). `#races-panel` and `#keys-panel` also sit at the
  left (`top:64px`, web/index.html:273): shift the toasts when one is open, or stack
  above it. Each toast has Accept / Decline and the countdown, `pointer-events` only
  on the buttons, `role="status"`.
- **Right: panel** opened by a stage-bar button `Invites (n)` (the badge is the
  pending count), same lists as the start page, over the game, `top:56px;
  right:16px`, `touch-action: pan-y`.

Buttons at least 44 px for touch. The tab title flashes `(1) invite` while the page is
hidden. A gamepad drives the game and cannot focus DOM buttons, so accept is by mouse
or touch only (hotkeys are an open decision).

## User-facing errors

Shown as a toast in the existing `raceMsg` style:

| Code | Text |
|---|---|
| `off` | `<Name> is not accepting invites.` |
| `cooldown` | `You invited <Name> a moment ago. Try again in <n> s.` |
| `rate` | `Too many invites. Wait a minute.` |
| `busy` | `<Name> has several invites waiting.` |
| `gone` (invite) | `<Name> left.` |
| `full` (invite) | `Your race is full.` |
| `host` | `Only the race host can invite.` |
| accept `expired` | `That invite expired.` |
| accept `gone` | `That race is gone.` |
| accept `full` | `That race is full.` |

Internal only (`console.warn`): `self`, malformed JSON, control-bucket drops, `signal`
errors.

## Players per room

Why a room holds at most 8 and cannot simply be raised (static study, nothing run):

- **The game seats 8, hard-wired.** System Link's session constructor
  (`sub_0001DC80`, `push 8` @0x1DD17 -> `sub_00024300` -> `sub_0005CD20`; the peer
  array is heap-sized and patchable), the network helper object (0xA18 B,
  `sub_001B0540`, `[0x523994]`: inline 8-wide arrays at +0x9C0..+0x9FC), the player
  manager (`[0x558814]`, ctor `sub_002BE100`, loop of 8 @0x2BE1F5, `players[8]` at +8,
  `GetPlayer` `sub_002BD910` with no bounds check and 209 callers) and the race setup
  (entries `0x114 + i*0x2C`, reset loop of 8 @0x26B61D in `sub_0026B510`, fields after
  +0x3D4). The registry `[0x5638E4]` has capacity 10 (already enlarged by
  roam_cars.c).
- **The server caps match:** `web/races-worker/src/net.js:6` `LIMITS.members = 8`,
  `web/races-worker/src/index.js:145` clamps `max` to it; Rust
  `crates/lan/src/lib.rs:187` `RoomLimits.members`, :490 clamp, :512 full check; the
  page creates rooms with `max: 8` (web/lan.js:112).
- **Verdict:** raising only the server cap would admit players the game cannot seat.
  A native raise is about 3-6 weeks, high risk (relayouts and relifts), not
  recommended.
- **Route past 8:** a "ghost race" on the Free Roam path: standalone cars from
  roam_cars.c (up to 32 at `RECOMP_RAM_MB=128`), `roam_challenge.c` generalised to N
  players, one shared grid, countdown and checkpoints, standings in a page overlay.
  About 1-2 weeks. Risks: no native results or rewards screen, collisions only within
  20 m, heap and 30 fps with 32 cars.
- **Unverified:** race racer-record capacity (`sub_001CE170`), `.rac` grid sizes, the
  lobby Flash rows, the position HUD.

## Tests to add

`web/lan-worker/test/switch.test.mjs` (after the existing rooms test; use
`setup().named` and `p.say(m, now)`):

1. Happy path: A creates a locked room and invites B; B gets `invite` (id, from, name,
   room, ttl); accept puts B in the room without the password and A gets `room` with
   2 members.
2. Opt-out: `hello inv:0` or `{"t":"invites","on":0}`: invite -> `off`; the list entry
   has `inv:0`; toggling off clears pending invites.
3. Cooldown: same target within 60 s -> `cooldown` with `wait`; fine at +60 s.
4. Global cap: 6 ok, the 7th `rate`, fine after the window; the 15 per 10 min cap.
5. Expiry: accept at +31 s -> `expired`, invite removed.
6. Pending cap 3 -> `busy`.
7. Not host / self / unknown vip / target closed -> `host` / `self` / `gone`.
8. Room full at accept -> `full`; host left -> `gone`; target hosting a room: the
   accept closes it.
9. `players()` entries carry `vip` and `inv`.
10. Hibernation: a new `Switch(ctx)` over the same sockets keeps pending invites and
    the sent log.

`web/races-worker/test/lobby.test.mjs`: no new test; the existing join tests guard
the `enter()` refactor (run them). `web/lan.js` has no harness: check by hand in two
tabs.

## Change plan, per file

- `web/lan-worker/src/switch.js`: header doc (add an invite section); a constant
  `INV = {ttl:30e3, pair:60e3, per:6, perWin:60e3, per10:15, pending:3}` near
  `CTL` (:46); `players()` (:72) adds `vip: a.vip, inv: a.noinv ? 0 : 1`;
  `control()` (:181): the regex at :182 gains `invite|invites|invite-reply`, the
  `hello` branch (:185) sets `c.a.noinv = m.inv === 0 || m.inv === false`, new
  branches after :193, `this.who()` after an opt-out change; new methods `invite()`
  and `inviteReply()`.
- `web/races-worker/src/index.js`: `join()` (:154-172): keep the password check in
  `join`, split the tail (:164-171: same-room check, full check, `leaveRoom`, set
  room, `roomChanged`, `listChanged`, `arm`) into `enter(c, h, why)` so
  `invite-reply` reuses it. About 10 lines moved.
- `web/lan.js`: `hello()` (:36) adds `inv` from `localStorage` `mc3.invites`;
  `link.invites = []`; message handling (:72) takes `m.t == 'invite'` (push
  `{id,from,name,room,rname,until: Date.now()+ttl}`); `lost()` (:87) clears
  invites; new `link.invite(to)`, `link.reply(id, ok)`, `link.setInvites(on)` through
  `ctl()` (:111); header doc (:8-18).
- `web/index.html`: CSS near :273; the Online tab: the `Allow invites` checkbox and
  the two columns; overlays `#inv-left`, `#inv-right` after `#races-panel` (:478); a
  stage-bar button `Invites (n)` beside :468; `lanShow` (:1604) calls a new
  `invitesShow(l)`; `RACE_ERR` (:1613) gains the new codes; `renderOnline` (:1884) and
  `playersList` (:1892) reuse `l.players.list` for the right panel; the dialog close
  selector (:1840) and the key-capture exclusion (:1963) gain `#inv-left, #inv-right`.
- `docs/races-protocol.md`: one pointer line (done).
- `web/server/crates/lan/src/lib.rs`: no change (the page feature-detects).

## Open decisions

1. Host-only invites OK, or may any member invite?
2. Accept cannot drive the game's menus: add a game-side automatic Join Game later?
   (A study is running; see above.)
3. Accepting while hosting a room closes it: `confirm()` in the page, or refuse?
4. In-game hotkeys for accept and decline (gamepad users), e.g. Alt+Y / Alt+N for the
   newest invite?
5. Rust server parity (needs the `players` push first)?
6. A longer cooldown after a decline (e.g. 5 min)?
7. Decline silent to the sender: OK?
