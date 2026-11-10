# Online Races: the room and race-list protocol

MC3's own race list was Xbox Live's, which cannot be revived (nfsu2-web could revive
EA's lobby; MC3 has none of its own). So the page keeps the list: players make
**races** (rooms) on a server, others pick one from the page's Races list, and from
then on the game's own System Link (Online Races -> Host / Join Game) sees only the
players of that room.

This file is the contract for three implementations, which speak the same messages:

1. **Step 1 (now):** the relay server's `/lan` switch (`web/server/crates/lan`) keeps
   the rooms and routes the game's datagrams only inside a room.
2. **Step 2:** WebRTC data channels between the members of a room (peer to peer),
   the switch as fallback relay. It uses the `signal` messages below.
3. **Step 3:** the same list and signalling on a Cloudflare Worker (a Durable
   Object per server), the datagrams over WebRTC (or a relay).

Everything here is transport-neutral: JSON text messages, one object per message, a
`"t"` field naming it. Step 1 carries them on the `/lan` WebSocket next to the
switch's binary frames (crates/lan/src/lib.rs, header); another transport only has
to deliver the same objects in order per client.

## Identities

- **Client:** one connection. In step 1 its id is its virtual IP (`vip`, a u32 from
  `welcome`, 10.77.0.0/18); another transport hands out its own numeric id in
  `welcome.vip` and keeps using it in every message below.
- **Nick:** the `name` of the client's `hello` (cleaned: control and bidi characters
  out, trimmed, at most 32 characters, empty -> `Player`).
- **Room id:** 6 characters from `23456789abcdefghjkmnpqrstuvwxyz` (no 0/o, 1/l/i),
  random, unique on the server while the room lives. An invite link is the page's
  address with `?race=<id>`. Ids are not secrets: a locked room is protected by its
  password, not by its id.

## Messages

### Client -> server

| Message | Meaning |
|---|---|
| `{"t":"rooms","sub":1}` | Send me the list now and on every change (`"sub":0`: stop). |
| `{"t":"create","name":s,"pass":s?,"city":s?,"mode":s?,"max":n?}` | Make a room and enter it as its host. Leaves the current room first. |
| `{"t":"join","room":id,"pass":s?}` | Enter a room. Leaves the current room first. |
| `{"t":"leave"}` | Leave the room (no room afterwards). |
| `{"t":"beat"}` | Heartbeat, every 20 s while connected. Changes nothing. |
| `{"t":"signal","to":vip,"kind":"offer"\|"answer"\|"ice","data":any}` | Reserved for step 2: relayed to `to` if both are in the same room. |
| `{"t":"where","w":w}` | Worker switch only (web/lan-worker/src/switch.js): what this page is doing, kept private for the owner (GET /lan/activity below), not shown in the players list. `w` = `page` (start page, game not started), `loading` (Play pressed until the game's menus), `menu` (game menus, garage), `career`, `arcade` (offline races). Any other `w` is ignored. No reply. The page (web/lan.js `link.where`) sends it only on change, at most once per 5 s, and after the hello of every (re)connect. A socket that never sent one counts as `menu`. Free Roam (the hub knows it) and race rooms (the client's room) win over it: such a player is `roam` / `race` there. |

Fields: `name` the race's name (cleaned like a nick, at most 32 characters, empty ->
`<nick>'s race`); `city`, `mode` free text the page knows (at most 24 characters,
optional, shown in the list only); `max` 2..8 (default 8; System Link races hold 8);
`pass` optional, at most 64 characters, empty = no password.

### Server -> client

| Message | When |
|---|---|
| `{"t":"rooms","rooms":[room,...]}` | To subscribers, at `sub:1` and after every change of any room (created, closed, member count). |
| `{"t":"room","room":room,"members":[member,...]}` | To every member of a room, on entering it and on every change of its members. |
| `{"t":"left","room":id,"why":w}` | To a member that is no longer in the room: `why` = `"leave"` (asked), `"closed"` (the host left: the room is gone). |
| `{"t":"error","op":op,"code":c}` | A `create`, `join` or `signal` refused (codes below). |
| `{"t":"signal","from":vip,"kind":k,"data":any}` | Reserved for step 2: a member's signal to this client. |

```text
room   = {"id":id,"name":s,"host":nick,"hostVip":vip,"city":s,"mode":s,"n":members,"max":n,"locked":bool}
member = {"vip":vip,"vipStr":"10.77.x.y","name":nick,"host":bool}
```

`city`/`mode` are `""` when unknown. The list is sorted oldest room first. A
password, its hash or its verifier is never in any message.

Who is online (Worker switch only; `{"t":"players","sub":1}` subscribes, pushed on change, at most
every 2 s): `{"t":"players","online":n,"freeRoam":{...},"races":{...},"modes":{"roam","race","menu"},"list":[{"name","mode","city?","race?"}],"more":n}`.
`mode` is `roam` (in Free Roam; the hub knows it), `race` (in a race room) or `menu` (everybody else; the page
shows it as Career). The page's `where` is not in the push nor in GET `/lan/stats`.

TURN credentials (Worker only): `GET /lan/ice` -> `{"iceServers":[{urls:[stun...]},{urls:[turn...],username,credential}]}`, Cloudflare
Realtime TURN, ttl 6 h, for the page's RTCPeerConnection (races, Free Roam P2P). Handled in the Worker's fetch (never wakes the
Durable Object). Origin must be in ALLOW_ORIGINS (no Origin: 403), CORS for it, `Cache-Control: private, no-store`. One generated set
is shared by everybody for 10 min (edge cache, per data center). Any API failure or missing secret: 200 with STUN only
`{"iceServers":[{"urls":"stun:stun.cloudflare.com:3478"}]}` (not cached). Secrets: `TURN_KEY_ID`, `TURN_KEY_API_TOKEN`
(`wrangler secret put`). No per-IP limit in the Worker.

Activity (private, Worker only): `GET /lan/activity` with `Authorization: Bearer <ADMIN_KEY>` (the Worker secret;
`?key=` also works but lands in logs, prefer the header). Without or with a wrong key, or with no secret set: 404.
No CORS, never cached. Answer: `{"online":n,"activities":{"roam","race","career","arcade","menu","loading","page":n},"cities":{"<room id>":n},"list":[{"name","activity","city?"}]}`.
`roam` and `race` come from the switch itself, the other five from the player's last `where`. The Durable Object also
prints one `console.log` line (`activity online=... {...} cities={...}`, `wrangler tail`) when the counts changed, at most
every 60 s, checked on each `where`. Set the key: `cd .deploy/lan-worker && XDG_CONFIG_HOME="$PWD/../xdg" npx wrangler secret put ADMIN_KEY`.
How the game side tells the activities: src/games/remix/recomp_manual.c `activity_update` (state at `[0x557ED0]+4`: 0/1/4 loading, 7 menu,
2/3/6 career when `[0x53F2D4]` == 2 else arcade, 5 garage keeps the activity it was entered from
when that was career or arcade, else menu), read by the page through `Module.mc3Activity`.

Error codes: `gone` (no such room), `full` (n == max), `password` (locked and no
or a wrong password), `limit` (too many rooms on the server or from this address),
`room` (a signal to someone outside your room, or while in none), `big` (a signal
over 16 KiB of JSON).

## Rules

- **One room at a time.** `create` and `join` leave the current room first (its
  members get `room`, or `left` with `closed` if it was yours).
- **The host owns the room.** When the host leaves (`leave`, `create`, `join`
  elsewhere, its connection gone or silent) the room closes: every member gets
  `left` with `why: "closed"` and is in no room. No host migration: System Link's
  host is the game that hosts, and it is gone too.
- **Expiry.** A client not heard from for 120 s is gone (step 1: the WebSocket's
  idle timeout, web/server/src/http.rs `IDLE`; any message counts, `beat` keeps an
  idle page alive). A transport without a persistent connection expires members
  the same way, from their last message.
- **Passwords.** Sent once, in `create`/`join`, over the page's own transport (wss
  on a public server). The server keeps only a verifier: SHA-1 of a per-room
  random 16-byte salt and the password, compared in constant time. The plaintext
  is never stored, logged or sent to anyone. (A room's lifetime is a race evening;
  the verifier dies with the room.)
- **Limits** (step 1 defaults, crates/lan `RoomLimits`): 8 members a room, 256
  rooms on a server, 2 rooms hosted from one IP address at a time, names 32
  characters, city/mode 24, password 64.
- **No room = the open LAN.** A client in no room is on the server's shared LAN,
  as before rooms existed: it sees the System Link games of every other client in
  no room, and nobody in a room. A room is its own LAN. So a page that never uses
  the list works as before.

Player invites (host invites a player from the page's list; planned, not implemented):
[invites.md](invites.md).

## Routing (step 1, the switch)

- A **broadcast** (destination 255.255.255.255) reaches the other clients with
  System Link open *in the same room* (or, from no room, in no room).
- A **unicast** reaches its destination only if it is in the same room (both in
  none counts).
- **Free roam is not a room's business:** frames to or from the free-roam port
  47210 (crates/roam `ROAM_PORT`) and the hub at 10.77.0.253 route as before,
  whatever the rooms.
- `peers` (the server-wide player list, crates/lan) is unchanged.

## Abuse guards (the Worker switch, web/lan-worker/src/switch.js `GUARD`)

All in memory or in the sockets' attachments; no IP is stored beyond the attachment or logged;
no new timer or alarm (they run on the wakes the object has anyway).

| Guard | Rule | Result |
|---|---|---|
| sockets per IP | at most 8 live sockets from one `CF-Connecting-IP` (IPv6: its /64) | upgrade refused: HTTP 429, `Retry-After: 60` |
| reconnect churn, IP | more than 20 upgrades a minute | 429 for 60 s |
| reconnect churn, browser | more than 20 `hello`s with one `cid` a minute | socket closed `4004 slow down`, cooled 60 s |
| sockets per browser | a `cid` holds 2 sockets; a third closes the oldest | `4002 too many tabs` |
| hello deadline | no `hello` within 10 s of connecting | `4003 no hello` (at the next wake, at most once a second) |

Close codes the page sees: `1008` rate limit, `1009` frame size, `4000` idle, `4001` a newer page
of this browser took the place (Rust switch), `4002`, `4003`, `4004` above. web/lan.js: 4001 and
4002 leave the link offline (`link.refused = 'tabs'` for 4002; the start page says to close a
tab), 4004 waits the longest backoff, anything else (4003, a 429 seen as 1006) backs off 1-15 s
and retries. The free-roam hub (web/lan-worker/src/roam.js) also keeps, in the Worker only, a
player in MENUS/LOADING for 60 s without a SNAP of another status (ROAM, SHOP, EVENT, PAUSED) out
of every PEERS and FAR list until one arrives; a browser joining more than 4 times a minute
starts that way; one browser holds at most 2 hub endpoints.

## Step 2: WebRTC inside a room (web/rtc.js)

Two members exchange `signal` messages through the list server: `kind: "offer"` and
`"answer"` carry an SDP (`data: {"sdp": "..."}`), `"ice"` a candidate (`data:
{"candidate": ..., "sdpMid": ..., "sdpMLineIndex": ...}`, or `null` for the end of
candidates). The server relays them untouched between members of the same room and
nowhere else (error `room`).

- **Pairs.** On every `room` message the page connects to each other member: the
  lower vip makes the offer (an offer from a higher vip is ignored: no glare). One
  `RTCPeerConnection` per pair with one data channel, `negotiated: true, id: 0`,
  `ordered: false, maxRetransmits: 0` (System Link is UDP: late is lost). ICE servers:
  config.json `iceServers` (for a TURN server later), else public STUN only
  (`stun:stun.l.google.com:19302`, `stun:stun.cloudflare.com:3478`).
- **Frames.** Over the channel goes the switch's frame (16-byte header, kind 3, the
  datagram after it). The receiver checks kind and length, drops the free-roam port,
  and **writes the channel's peer vip as the source** (the frame's own is never
  trusted), then feeds it to the same receive path as the switch's frames (net.js).
- **Unicast.** To a member with an open channel: over it. Anything else (no channel
  yet, failed, a non-member, no room): the switch, as in step 1.
- **Broadcast** (255.255.255.255), chosen so the switch needs no change and never
  delivers twice. No open channel in the room (or no room): one frame to the switch,
  which broadcasts as in step 1. At least one open channel: the page splits it, over
  each open channel and **as a unicast through the switch** (destination rewritten to
  that member) to each member without one; the switch sees no broadcast. The game
  cannot tell the difference (xnet_web.c's receive slot carries the source, not the
  destination). A member who entered a moment ago and is not yet in the page's list
  misses a broadcast or two; System Link's discovery repeats them.
- **Free roam** (port 47210, the hub 10.77.0.253) never uses a channel: net.js sends
  only System Link's mailbox through rtc.js.
- **Failure and leaving.** A channel that closes or a connection that fails goes back
  to the relay at once; the offerer tries again after 5, 10 and 15 s, then the pair
  stays on the relay until the next `room` message. A member gone from `room`, a
  `left`, or the link going offline closes the pairs.
- **Page.** The Online tab lists the room's other members with `direct` (channel open)
  or `relay`. `Module.mc3Net()` counts `p2pSent`/`p2pRecv` beside the switch's.
- **Test.** `node web/test/rtc.test.js` checks the routing rule; with
  `NDC=<folder holding node_modules/node-datachannel>` also a loopback of three pages
  (two direct, one relayed) on a fake switch.

## Step 3: the Worker (web/races-worker)

The same messages over a WebSocket to a Worker at `wss://<host>/races` (Origin in
its `ALLOW_ORIGINS`, else 403); one Durable Object `Lobby` holds every room (and the
list) and the members' sockets (hibernation API), and relays `signal`. On
connecting it sends `welcome` like the switch (`vip`, `vipStr`, `mask`, `mac`,
addresses from the same 10.77.0.0/18 pool); it answers `hello` (the nick) and
`ping` as the switch does. `beat` must be sent exactly as `{"t":"beat"}`: the
runtime answers it with the same text without waking the object (pages ignore
it). Expiry is the idle rule above: an alarm every 60 s drops clients silent for
120 s (last message or last beat). Same limits and password verifier as step 1.

How the page picks the endpoint: config.json `racesUrl` set -> the list and the
signalling go to that Worker; absent -> the `/lan` switch's own control messages
(step 1). The Worker carries no race datagrams: members use WebRTC (step 2), and a
pair whose WebRTC fails falls back to the relay switch (`lanUrl`, mc3-server). A
deploy with the Worker alone has no relay: such a pair cannot race.
Open: with both `racesUrl` and a relay, a client has two vips (the Worker's and
the switch's); the page must pick one as the game's address (the relay's while
it is connected, so fallback frames route) and use it consistently.
