// The Switch Durable Object: the /lan switch (System Link, race rooms, free roam) with the same wire contract as
// web/server's (crates/lan + crates/roam): the page (web/lan.js) and the game cannot tell them apart. The Worker's
// entry is src/index.js.
//
//   GET /lan[?vip=n] (WebSocket upgrade, Origin in ALLOW_ORIGINS) -> the Durable Object "Switch" (one for the server)
//
// Switch extends the races Lobby (web/races-worker: rooms, signalling, hello/ping, idle expiry, hibernation) with
// what crates/lan adds: binary frames routed between members (unicast, broadcast to System Link peers, a room is its
// own LAN, free roam's port and the hub route whatever the rooms), the peers list, {"t":"lan"}, {"t":"roam"}, ?vip=
// on reconnect, a per-sender token bucket; and the free-roam hub (src/roam.js) at 10.77.0.253:47210. Beyond crates/lan:
// {"t":"signal",...,"roam":1} between two free-roam players of one city (signal() below; web/rtc.js free roam).
//
// Free roam per city (src/city.js, 2026-11-10): with the ROAM binding and the ROAM_SECRET secret set, the welcome says
// roamCity: 1 and a page in free roam asks {"t":"ticket"} -> {"t":"ticket", vip, exp, sig} (HMAC of its vip, TICKET_MS),
// then opens a second socket wss .../lan/roam?city=N&vip&exp&sig straight to that city's RoamCity object, which carries
// its hub, its relay frames and its roam signalling from then on. This object keeps the lobby, races, the players list
// and its own hub as the fallback (old pages, a city socket that failed). The city objects report their players to
// POST /lan/city (internal: the Worker routes no such path) when they change, at most every CITY.reportMs, and every
// CITY.keepMs while any are there; a report older than CITY_TTL is dropped (its object died).
//
// Frames (big-endian): [0] kind, [1..3] 0, [4..7] source vip (rewritten to the sender's), [8..11] destination vip
// (0xFFFFFFFF broadcast), [12..13] source port, [14..15] destination port, [16..] datagram (<= 1472 bytes).
//
// Cost (free tier): incoming WebSocket messages bill 20 to a request, outgoing are free. The hub's 500 ms tick is not
// an alarm (two alarms a second would be 172,800 requests a day by themselves): it runs from the incoming messages,
// at most every tick_ms, and every hub player sends at least a HELLO a second (roam_core.c T_HUB_HELLO), well inside
// its 2.5 s grace (T_HUB_GRACE). The Lobby's 60 s alarm (idle expiry) also ticks; it runs only while there is work
// (a race room or a hub player). Idle pages keep their link with {"t":"beat"} every 20 s, answered by the runtime's
// auto-response without waking the object (web/lan.js), so with nobody in free roam or a race the object hibernates
// and only real events (connects, closes, renames, room changes) wake it; each wake also sweeps the silent clients.
// The hub and the rate buckets live in memory: lost when the object hibernates, which needs 10 s without a message,
// so never during free roam; the hub refills from the next HELLOs.
//
// Who is online (the start page's "Playing now" box and its View players list): {"t":"players","sub":1} subscribes;
// the subscriber gets the list at once, then again whenever it changed (joins, leaves, renames, mode or city), at most
// every WHO_MS: {"t":"players", online, freeRoam, races (as GET /lan/stats), modes: {roam, race, menu},
// list: [{name, mode: 'roam'|'race'|'menu', city?: free-roam room id, race?: room name}] (at most PLAYERS_MAX, free
// roam by city first, then races, then the menus, by name), more: the players beyond the list}. 'menu' is everybody
// else. The public push stays this coarse on purpose: what each page really does (below) is the owner's, GET /lan/activity.
//
// Activity (private): a page says {"t":"where","w":"page|loading|menu|career|arcade"} (web/lan.js link.where: the start
// page, Play pressed until the game's menus, game menus, career, arcade; stored in the client's attachment `w`, anything
// else ignored, no reply); no 'where' yet (an old page, a new socket) counts as 'menu'. The page sends it only on change
// (at most every 5 s) and after each hello of a (re)connect: ~10 incoming messages per session, 1/20 request each.
// activity() (GET /lan/activity, src/index.js, behind ADMIN_KEY) gives the counts per activity and per free-roam city and
// the list {name, activity, city?, roam?, hub?}; a free-roam player or one in a race room is 'roam' / 'race' (the switch knows it).
// A free-roam entry also has `roam`: 'drive'|'garage'|'race'|'paused'|'menus'|'loading' (the hub's in-game status, which the
// city objects report as [vip, room, status]; up to CITY.reportMs late), and the response `roamStatus` counts them.
// `hub`: 'city' (a city object's hub) or 'switch' (this object's fallback hub); players of one city on different hubs
// never see each other (web/lan.js keeps retrying its city socket, so 'switch' lasts seconds on a current page).
// Private like the rest: the players push and /lan/stats do not carry it.
// logActivity() prints one console.log line (wrangler tail) when that changed, at most every ACTIVITY_LOG_MS. Nicks are cleaned
// and cut to NICK characters on hello, and unique among the live clients (any case; 'Player', the empty name's
// fallback, excepted): a hello with a held nick gets {"t":"error","op":"hello","code":"nick-taken"} and keeps its old name.
//
// Abuse guards (GUARD below; all in memory or in the sockets' attachments, nothing stored, no IP ever logged; no timer
// and no alarm of their own: they run on the wakes the object has anyway, so a silent socket costs nothing):
//  - per IP: at most GUARD.perIp live sockets from one CF-Connecting-IP (IPv6: its /64); one more upgrade -> HTTP 429.
//    The DO reads the address Cloudflare's edge put on the request (a client cannot set it); `ip` is in the attachment.
//  - reconnect churn: more than GUARD.churnMax upgrades a minute from one IP, or hellos a minute of one browser (cid),
//    and it is refused for GUARD.coolMs: 429 + Retry-After at the upgrade; a cid's socket is closed 4004 'slow down'.
//  - per browser: a cid (hello) may hold GUARD.perCid sockets (a second tab, or a ghost of a dropped link); a newer one
//    closes the OLDEST with 4002 'too many tabs' (lan.js does not reconnect on it).
//  - hello deadline: a socket with no hello within GUARD.helloMs is closed 4003 'no hello' (swept on the next wake, at
//    most once a second; a socket that never sends a thing is also swept by the next upgrade or message of anyone).
// Close codes: 1008 rate limit, 1009 frame size, 4000 idle (or the page's own 'no answer'), 4001 a newer page of this
// browser took the place (Rust switch), 4002/4003/4004 above.
//
// vars: ALLOW_ORIGINS "https://a.example,https://b.example" (scheme://host, comma-separated, any case); empty = any.

import { Lobby, clean } from '../../races-worker/src/index.js';
import { vipAt, vipStr, macStr, NET, MASK, ADDRS, LIMITS } from '../../races-worker/src/net.js';
import { Hub, HUB, ROAM_PORT, ROAM_STATUS } from './roam.js';

// The one switch object's name (src/index.js). A new name starts a fresh object on the deploy's code: a live object
// keeps its old code until it restarts, which a busy one may not do for long (2026-11-10: 'switch' -> 'switch-2', then 'switch-3' with the city objects; its
// sockets stay there until their pages reload).
export const SWITCH_NAME = 'switch-3';
// Free roam per city (src/city.js): the game's room ids are city indexes below 14 (src/games/remix/roam_game.c).
export const CITIES = 14, cityOk = n => Number.isInteger(n) && n >= 0 && n < CITIES, cityName = n => `city-${n}`;
export const TICKET_MS = 60e3, CITY = { reportMs: 10e3, keepMs: 60e3 }, CITY_TTL = 3 * CITY.keepMs;

const enc = new TextEncoder();
async function hmac(secret, msg) {
  const k = await crypto.subtle.importKey('raw', enc.encode(secret), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  return [...new Uint8Array(await crypto.subtle.sign('HMAC', k, enc.encode(msg)))].map(b => b.toString(16).padStart(2, '0')).join('');
}
/** A city-socket ticket for `vip`: {vip, exp, sig}, good for TICKET_MS. */
export async function ticket(secret, vip, now = Date.now()) {
  const exp = now + TICKET_MS;
  return { vip, exp, sig: await hmac(secret, `${vip}.${exp}`) };
}
/** Is (vip, exp, sig) a live ticket of this secret? Constant time over the signature. */
export async function ticketOk(secret, vip, exp, sig, now = Date.now()) {
  if (!secret || !isVip(vip) || !(exp > now) || exp - now > TICKET_MS || typeof sig !== 'string' || sig.length !== 64) return false;
  const want = await hmac(secret, `${vip}.${exp}`);
  let d = 0;
  for (let i = 0; i < 64; i++) d |= want.charCodeAt(i) ^ sig.charCodeAt(i);
  return d === 0;
}

export const BROADCAST = 0xFFFFFFFF, HDR = 16, MAX_DATA = 1472, MAX_FRAME = HDR + MAX_DATA;
export const RATE = { framesPerS: 2000, burstFrames: 4000, bytesPerS: 1 << 20, burstBytes: 2 << 20 };   // crates/lan Limits
// Measured legit peaks (client code, 2026-11-09): binary per client ~700 frames/s worst case = free roam's 20 Hz SNAP to
// K = 32 peers (640) + 1 Hz HELLO/PING to them; an 8-player race ~60 Hz x 7 = 420. framesPerS 2000 is ~3x that.
// JSON: web/lan.js sends a ping every 20 s plus a handful of subs/joins/renames at connect: < 1/s, but web/rtc.js signals
// through it too: an offer/answer + ~10 ICE candidates per peer, ~80 at once joining an 8-player room. 20/s, burst 200.
// Free roam's P2P (rtc.js, "roam":1) shares that budget (an estimate, not measured live): a pair costs each end ~6-11
// (offer or answer + its candidates); a page opens at most 2 pairs a second and holds 12, so entering a busy city is
// ~70-130 at most over 6 s, and driving through it a pair or two every few seconds; failed pairs back off 30 s .. 5 min.
// Over budget: dropped silently; empty for FLOOD_MS in a row: closed with 1008 (lan.js reconnects with backoff).
export const CTL = { perS: 20, burst: 200 }, FLOOD_MS = 10e3;
export const NICK = 15, PLAYERS_MAX = 200, WHO_MS = 2000, ACTIVITY_LOG_MS = 60e3;   // the page's name field is 15 long
// What a player not in free roam or a race room is doing ({"t":"where","w"} from the page, web/lan.js link.where), in the
// list's order; a player whose page never said (an old page, or not yet) counts as 'menu'. Free roam and race rooms are
// the switch's own knowledge (the hub, the client's room) and win over it.
export const WHERE = ['career', 'arcade', 'menu', 'loading', 'page'];
// shortcut: the churn counters live in memory (lost when the object hibernates: needs 10 s without a message, so a
// reconnect storm never loses them); the cap per IP, counted from the live sockets, is what is left if it did.
export const GUARD = { perCid: 2, perIp: 8, helloMs: 10e3, churnMax: 20, churnMs: 60e3, coolMs: 60e3 };
export const CLOSE = { tabs: 4002, hello: 4003, churn: 4004 };
const ipKey = ip => ip.includes(':') ? ip.split(':').slice(0, 4).join(':') : ip;   // IPv6: a /64 is one household
const SAVE_MS = 30e3;   // a frame-only client's `last` reaches its attachment this often (idle expiry after a wake)

export const isVip = v => (v & MASK) >>> 0 === NET && (v & 255) >= 2 && (v & 255) < 252;
const on = v => v === 1 || v === true;

export class Switch extends Lobby {
  constructor(ctx, env, rate = RATE) {
    super(ctx, env);
    this.rate = rate;
    this.next = 0;
    this.wsc = new Map();     // ws -> client (no attachment read per frame)
    for (const c of this.clients.values()) this.wsc.set(c.ws, c);
    this.buckets = new Map(); // vip -> {frames, bytes, at}
    this.hub = new Hub(env?.ROAM_K ? { default_k: Number(env.ROAM_K) } : {}); // links for a HELLO asking none
    this.ticked = 0;
    this.whoAt = 0; this.whoLast = ''; this.whoTimer = 0;
    this.churn = new Map();   // 'ip' | 'c:<cid>' -> {n, at, until}
    this.helloAt = 0;
    this.cities = new Map();  // city -> {at, vips: Map vip -> room}: the city objects' reports (POST /lan/city)
  }

  /** Are the city objects there (the ROAM binding and its ticket secret)? */
  cityOn() { return !!(this.env?.ROAM && this.env?.ROAM_SECRET); }

  /** vip -> free-roam room: the city objects' players and this object's own hub's (pages on the fallback path). */
  // `st`, when given, is filled vip -> in-game status code (roam_api.h; absent for an old city report without one).
  // shortcut: rebuilt per call (players push, stats, activity); a few thousand entries at most.
  roamers(now = Date.now(), st = null) {
    const m = new Map();
    for (const [city, r] of this.cities) {
      if (now - r.at > CITY_TTL) { this.cities.delete(city); continue; }
      for (const [v, room] of r.vips) { m.set(v, room); if (st && r.st.has(v)) st.set(v, r.st.get(v)); }
    }
    for (const p of this.hub.players.values()) if (!m.has(p.vip)) { m.set(p.vip, p.room >>> 0); st?.set(p.vip, p.status); }
    return m;
  }

  /** POST /lan/city from a city object: {city, players: [[vip, room, status], ...]} (src/city.js report; status optional). */
  // shortcut: in memory, lost if this object hibernates; the next report (CITY.keepMs at most) brings it back.
  cityReport(m, now = Date.now()) {
    const city = Number(m?.city);
    if (!cityOk(city) || !Array.isArray(m.players)) return false;
    const vips = new Map(), st = new Map();
    for (const e of m.players.slice(0, 1 << 14)) {
      if (!Array.isArray(e) || !isVip(e[0] >>> 0)) continue;
      vips.set(e[0] >>> 0, e[1] >>> 0);
      if (Number.isInteger(e[2])) st.set(e[0] >>> 0, e[2]);
    }
    if (vips.size) this.cities.set(city, { at: now, vips, st }); else this.cities.delete(city);
    this.who(now);
    return true;
  }

  /** {"t":"ticket"}: a ticket for this page's city socket, or {"t":"error","op":"ticket","code":"off"}. Its free roam
   *  leaves this hub now (it moves; a refused or failed move comes back with its next HELLO, a second later). */
  async ticket(c, now) {
    if (!this.cityOn()) return this.err(c, 'ticket', 'off');
    this.hub.leave(c.a.vip);
    const t = await ticket(this.env.ROAM_SECRET, c.a.vip, now);
    if (this.clients.get(c.a.vip) === c) this.raw(c, JSON.stringify({ t: 'ticket', ...t }));
  }

  /** One more connect or hello of `key`: true while it is cooling down (over GUARD.churnMax in churnMs: refused for coolMs). */
  hot(key, now) {
    let k = this.churn.get(key);
    if (k && now < k.until) return true;
    if (!k || now - k.at > GUARD.churnMs) this.churn.set(key, k = { n: 0, at: now, until: 0 });
    if (++k.n <= GUARD.churnMax) return false;
    k.until = now + GUARD.coolMs;
    return true;
  }

  /** May this upgrade proceed? false: answer 429. Counts the attempt (churn) and the sockets of the same IP. */
  admit(ip, now = Date.now()) {
    this.helloSweep(now);   // a bot's silent sockets go before they are counted
    if (!ip) return true;
    const key = ipKey(ip);
    if (this.hot(key, now)) return false;
    let n = 0;
    for (const q of this.clients.values()) if (q.a.ip && ipKey(q.a.ip) === key) n++;
    return n < GUARD.perIp;
  }

  /** Closes the sockets with no hello after GUARD.helloMs, forgets cooled churn entries; at most once a second. */
  helloSweep(now) {
    if (now - this.helloAt < 1000 && now >= this.helloAt) return;
    this.helloAt = now;
    for (const c of [...this.clients.values()]) if (!c.a.hi && now - c.a.at > GUARD.helloMs) this.kick(c, CLOSE.hello, 'no hello');
    for (const [k, v] of this.churn) if (now - v.at > GUARD.churnMs && now >= v.until) this.churn.delete(k);
  }

  kick(c, code, why) { try { c.ws.close(code, why); } catch {} this.drop(c); }

  /** The browser `c.a.cid` holds more than GUARD.perCid sockets: the oldest others go. */
  capCid(c) {
    const old = [...this.clients.values()].filter(q => q !== c && q.a.cid === c.a.cid).sort((x, y) => (x.a.at || 0) - (y.a.at || 0));
    while (old.length >= GUARD.perCid) this.kick(old.shift(), CLOSE.tabs, 'too many tabs');
  }

  /** GET /lan/stats: counts only (no names, no vips): connected sockets, free roam by room id, race rooms. */
  stats(now = Date.now()) {
    const rooms = {};
    let total = 0;
    for (const [v, r] of this.roamers(now)) if (this.clients.has(v)) { rooms[r] = (rooms[r] || 0) + 1; total++; }
    return { online: this.clients.size, freeRoam: { total, rooms }, races: { rooms: this.rooms().length } };
  }

  /** {"t":"players"} (see the head of this file): the stats, every client's mode, the first PLAYERS_MAX names. */
  players() {
    const roam = this.roamers(), races = new Map(), modes = { roam: 0, race: 0, menu: 0 }, rank = { roam: 0, race: 1, menu: 2 };
    for (const h of this.rooms()) races.set(h.a.host.id, h.a.host.name);
    const all = [...this.clients.values()].map(({ a }) => {
      const e = { name: String(a.name).slice(0, NICK), mode: 'menu' };
      if (roam.has(a.vip)) { e.mode = 'roam'; e.city = roam.get(a.vip); }
      else if (a.room) { e.mode = 'race'; e.race = races.get(a.room) ?? ''; }
      modes[e.mode]++;
      return e;
    });
    all.sort((x, y) => rank[x.mode] - rank[y.mode] || (x.city ?? 0) - (y.city ?? 0) || (x.name < y.name ? -1 : x.name > y.name ? 1 : 0));
    const list = all.slice(0, PLAYERS_MAX);
    return { t: 'players', ...this.stats(), modes, list, more: all.length - list.length };
  }

  /** GET /lan/activity (private): what every client does: {online, activities: {<activity>: n}, cities: {<room id>: n},
   *  roamStatus: {<status>: n} (free roam players by in-game status), list: [{name, activity, city?, roam?}]}; `roam` =
   *  'drive'|'garage'|'race'|'paused'|'menus'|'loading', absent while unknown (a city object that reports no status);
   *  `hub` = 'city' (its city object's hub) | 'switch' (this object's fallback hub): the two never see each other. */
  activity() {
    const st = new Map(), roam = this.roamers(Date.now(), st), activities = {}, cities = {}, roamStatus = {}, inCity = new Set();
    for (const r of this.cities.values()) for (const v of r.vips.keys()) inCity.add(v);   // roamers() dropped the stale reports
    for (const m of ['roam', 'race', ...WHERE]) activities[m] = 0;
    for (const s of ROAM_STATUS) roamStatus[s] = 0;
    const list = [...this.clients.values()].map(({ a }) => {
      const e = { name: String(a.name).slice(0, NICK), activity: WHERE.includes(a.w) ? a.w : 'menu' };
      if (roam.has(a.vip)) {
        e.activity = 'roam'; e.city = roam.get(a.vip); e.hub = inCity.has(a.vip) ? 'city' : 'switch'; cities[e.city] = (cities[e.city] || 0) + 1;
        const s = ROAM_STATUS[st.get(a.vip)];
        if (s) { e.roam = s; roamStatus[s]++; }
      }
      else if (a.room) e.activity = 'race';
      activities[e.activity]++;
      return e;
    });
    return { online: list.length, activities, cities, roamStatus, list };
  }

  /** One console.log line (wrangler tail) with the counts when they changed, at most every ACTIVITY_LOG_MS; called on each 'where'. */
  logActivity(now = Date.now()) {
    const wait = (this.actAt ?? -Infinity) + ACTIVITY_LOG_MS - now;
    if (wait > 0 && wait <= ACTIVITY_LOG_MS) return;   // beyond: the clock went back (tests' times), log now
    const { online, activities, cities } = this.activity(), line = `activity online=${online} ${JSON.stringify(activities)} cities=${JSON.stringify(cities)}`;
    this.actAt = now;
    if (line !== this.actLast) { this.actLast = line; console.log(line); }
  }

  /** The list to its subscribers when it changed; at most every WHO_MS, a timer sends what came in between. */
  // shortcut: rebuilt whole (O(n log n)) at most once a second while anything happens; fine to a few thousand clients.
  who(now = Date.now()) {
    const wait = this.whoAt + WHO_MS - now;
    if (wait > 0 && wait <= WHO_MS) {   // beyond: the clock went back (tests' times), send now
      if (!this.whoTimer) this.whoTimer = setTimeout(() => { this.whoTimer = 0; this.who(); }, wait);
      return;
    }
    this.whoAt = now;
    const subs = [...this.clients.values()].filter(q => q.a.who);
    if (!subs.length) { this.whoLast = ''; return; }
    const msg = JSON.stringify(this.players());
    if (msg === this.whoLast) return;
    this.whoLast = msg;
    for (const q of subs) this.raw(q, msg);
  }

  async fetch(req) {
    this.lazySweep();
    const path = new URL(req.url).pathname;
    if (path === '/lan/stats') return Response.json(this.stats());
    if (path === '/lan/activity') return Response.json(this.activity());
    if (path === '/lan/city' && req.method === 'POST') {   // internal: only the city objects reach it (src/index.js routes no such path)
      let m = null;
      try { m = await req.json(); } catch {}
      return new Response(null, { status: this.cityReport(m) ? 204 : 400 });
    }
    if (req.headers.get('Upgrade') !== 'websocket') return new Response('websocket only', { status: 426 });
    const want = Number(new URL(req.url).searchParams.get('vip')) >>> 0, ip = req.headers.get('CF-Connecting-IP') || '';
    if (!this.admit(ip)) return new Response('too many connections', { status: 429, headers: { 'Retry-After': String(GUARD.coolMs / 1000) } });
    const [client, server] = Object.values(new WebSocketPair());
    if (!this.connect(server, ip, Date.now(), want)) return new Response('LAN full', { status: 503 });
    return new Response(null, { status: 101, webSocket: client });
  }

  /** crates/lan join_from: the address it asks for (a reconnect) when free, else the next free one in turn. */
  connect(ws, ip, now = Date.now(), want = 0) {
    let vip = isVip(want) && !this.clients.has(want) ? want : 0;
    if (!vip) {
      const start = this.next;
      for (let k = 0; k < ADDRS && !vip; k++) { const v = vipAt((start + k) % ADDRS); if (!this.clients.has(v)) vip = v; }
      if (!vip) return false;
      this.next = (start + 1) % ADDRS;
    }
    this.ctx.acceptWebSocket(ws);
    const a = { vip, name: 'Player', ip, sub: false, room: null, last: now, at: now, hi: false, lan: false };
    ws.serializeAttachment(a);
    const c = { ws, a };
    this.clients.set(vip, c);
    this.wsc.set(ws, c);
    this.send(vip, { t: 'welcome', vip, vipStr: vipStr(vip), mask: MASK >>> 0, mac: macStr(vip), ...(this.cityOn() ? { roamCity: 1 } : {}) });
    this.who(now);
    return true;
  }

  async webSocketMessage(ws, data, now = Date.now()) {
    this.lazySweep(now);
    this.helloSweep(now);
    const c = this.byWs(ws);
    if (c) {
      if (typeof data !== 'string') this.frame(c, new Uint8Array(data), now);
      else if (this.ctlLimited(c, now)) { /* dropped */ }
      else if (data.length <= 64 && /^\{\s*"t"\s*:\s*"ticket"\s*\}$/.test(data)) await this.ticket(c, now);
      else if (!this.control(c, data, now)) await super.webSocketMessage(ws, data, now);
    }
    if (this.hub.count) {
      if (!this.armed) this.arm();
      if (now - this.ticked >= this.hub.opts.tick_ms) this.tick(now);
    }
    this.who(now);
  }

  /** The bucket of `c` (frames, bytes, JSON tokens). */
  bucket(vip, now) {
    let k = this.buckets.get(vip);
    if (!k) this.buckets.set(vip, k = { frames: this.rate.burstFrames, bytes: this.rate.burstBytes, at: now, ctl: CTL.burst, ctlAt: now, over: 0 });
    return k;
  }

  /** Over budget: true (drop). Empty for FLOOD_MS in a row: the socket is closed with 1008. `over` is the time it ran dry. */
  flood(c, k, limited, calm, now) {
    if (calm) k.over = 0;   // refilled to half: the few messages a flood still lets through do not reset the clock
    if (limited) {
      if (!k.over) k.over = now;
      else if (now - k.over > FLOOD_MS) {
        try { c.ws.close(1008, 'rate limit'); } catch {}
        this.drop(c);
      }
    }
    return limited;
  }

  /** A JSON message's token (separate, small budget: ping/subs/join/rename are < 1/s). */
  ctlLimited(c, now) {
    const k = this.bucket(c.a.vip, now);
    k.ctl = Math.min(k.ctl + Math.max(0, now - k.ctlAt) / 1000 * CTL.perS, CTL.burst);
    k.ctlAt = now;
    const limited = k.ctl < 1;
    if (!limited) k.ctl -= 1;
    return this.flood(c, k, limited, k.ctl >= CTL.burst / 2, now);
  }

  /** The switch's own control messages (hello with lan, lan, roam, players, where); false: the Lobby's (rooms, ping, ...). */
  control(c, text, now) {
    if (text.length > LIMITS.msg || !/"(hello|lan|roam|players|where)"/.test(text)) return false;
    let m;
    try { m = JSON.parse(text); } catch { return false; }
    if (m?.t === 'hello') {
      const name = clean(m.name, NICK, 'Player');
      if (typeof m.cid === 'string' && /^[\w-]{8,40}$/.test(m.cid) && m.cid !== c.a.cid) {   // a new socket's first hello
        if (this.hot('c:' + m.cid, now)) { this.kick(c, CLOSE.churn, 'slow down'); return true; }
        c.a.cid = m.cid;
        this.capCid(c);
      }
      if (this.nickTaken(c, name)) this.err(c, 'hello', 'nick-taken');   // the old name stays
      else c.a.name = name;   // the same browser (cid) may hold its nick on two sockets: a second tab, a ghost (the idle sweep drops it)
      c.a.hi = true; c.a.lan = on(m.lan);
    } else if (m?.t === 'lan') c.a.lan = on(m.on);
    else if (m?.t === 'players') c.a.who = on(m.sub);
    else if (m?.t === 'where') { if (WHERE.includes(m.w)) { c.a.w = m.w; this.logActivity(now); } }   // anything else: ignored, the old one stays
    else if (m?.t !== 'roam') return false;
    c.a.last = now;
    this.save(c);
    if (m.t === 'roam') this.raw(c, this.hub.listJson());
    if (m.t === 'players' && c.a.who) this.raw(c, JSON.stringify(this.players()));
    if (m.t === 'hello') {
      this.peersChanged();
      if (c.a.room) { this.roomChanged(c.a.room); this.listChanged(); }
    }
    return true;
  }

  /** Does another live client hold `name` (any case)? 'Player', the fallback for an empty name, may be shared. */
  // shortcut: a scan of the clients per hello (hellos are rare and rate limited; no index to keep across hibernation);
  // a Map of names if a switch ever holds tens of thousands.
  nickTaken(c, name) {
    const k = name.toLowerCase();
    if (k === 'player') return false;
    for (const q of this.clients.values()) if (q !== c && String(q.a.name).toLowerCase() === k && !(c.a.cid && q.a.cid === c.a.cid)) return true;
    return false;
  }

  /** A binary frame from `c`: checked, its source rewritten, routed (crates/lan Switch::frame). */
  frame(c, b, now) {
    if (b.length < HDR || b.length > MAX_FRAME) {
      try { c.ws.close(1009, b.length < HDR ? 'frame too short' : 'frame too long'); } catch {}
      this.drop(c);
      return 'size';
    }
    const vip = c.a.vip, r = this.rate;
    if (now - c.a.last > SAVE_MS) { c.a.last = now; this.save(c); } else c.a.last = now;
    const k = this.bucket(vip, now);
    const dt = Math.max(0, now - k.at) / 1000;
    k.at = now;
    k.frames = Math.min(k.frames + dt * r.framesPerS, r.burstFrames);
    k.bytes = Math.min(k.bytes + dt * r.bytesPerS, r.burstBytes);
    if (this.flood(c, k, k.frames < 1 || k.bytes < b.length, k.frames >= r.burstFrames / 2, now)) return 'limited';
    k.frames -= 1;
    k.bytes -= b.length;

    const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
    b[1] = b[2] = b[3] = 0;
    dv.setUint32(4, vip);
    const dst = dv.getUint32(8), sport = dv.getUint16(12), dport = dv.getUint16(14);
    if (dst === HUB) {
      if (dport !== ROAM_PORT) return 0;
      this.hub.frame(vip, sport, b.subarray(HDR), now);
      return 1;
    }
    // a room is its own LAN; free roam's traffic is not a room's business
    const room = c.a.room, roam = sport === ROAM_PORT || dport === ROAM_PORT;
    const reach = q => q.a.room === room || roam;
    let n = 0;
    if (dst === BROADCAST) {
      for (const q of this.clients.values()) if (q !== c && q.a.lan && reach(q) && this.bin(q, b)) n++;
    } else if (dst !== vip) {
      const q = this.clients.get(dst);
      if (q && reach(q) && this.bin(q, b)) n = 1;
    }
    return n;
  }

  /** {"t":"signal",...,"roam":1}: WebRTC between two free-roam players of the same city (web/rtc.js free roam), relayed
   *  with "roam":1; refused as 'roam' otherwise. Without "roam" it is the Lobby's race-room signal. */
  signal(c, m, size) {
    if (m.roam !== 1) return super.signal(c, m, size);
    if (size > LIMITS.signal) return this.err(c, 'signal', 'big');
    const to = this.clients.get(Number(m.to) >>> 0);
    if (!to || to === c || !this.sameCity(c.a.vip, to.a.vip)) return this.err(c, 'signal', 'roam');
    if (!['offer', 'answer', 'ice'].includes(m.kind)) return;
    this.send(to.a.vip, { t: 'signal', from: c.a.vip, kind: m.kind, data: m.data ?? null, roam: 1 });
  }

  /** Are vips a and b both hub players of one free-roam room (city)? */
  // shortcut: a scan of the hub per signal (a pair's opening sends ~10); an index by vip if the hub holds thousands.
  sameCity(a, b) {
    const ra = new Set(), rb = new Set();
    for (const p of this.hub.players.values()) { if (p.vip === a) ra.add(p.room); else if (p.vip === b) rb.add(p.room); }
    for (const r of ra) if (rb.has(r)) return true;
    return false;
  }

  /** The hub's lists out, as frames from 10.77.0.253:47210 (crates/lan Switch::inject). */
  tick(now) {
    this.ticked = now;
    for (const [dst, port, d] of this.hub.tick(now)) {
      const q = this.clients.get(dst);
      if (!q) continue;
      const f = new Uint8Array(HDR + d.length), dv = new DataView(f.buffer);
      f[0] = 3;
      dv.setUint32(4, HUB); dv.setUint32(8, dst); dv.setUint16(12, ROAM_PORT); dv.setUint16(14, port);
      f.set(d, HDR);
      this.bin(q, f);
    }
  }

  async alarm(now) {
    if (typeof now !== 'number') now = Date.now();   // the runtime passes alarmInfo; tests a time
    if (this.hub.count) this.tick(now);
    await super.alarm(now);
    this.who(now);
  }

  busy() { return this.hub.count > 0 || super.busy(); }

  drop(c) {
    const live = this.clients.get(c.a.vip) === c;
    super.drop(c);
    this.wsc.delete(c.ws);
    if (!live) return;
    this.buckets.delete(c.a.vip);
    this.hub.leave(c.a.vip);
    if (c.a.hi) this.peersChanged();
    this.who();
  }

  /** The whole peer list to everyone who said hello (crates/lan send_list). */
  peersChanged() {
    const list = [...this.clients.values()].filter(q => q.a.hi).sort((x, y) => x.a.vip - y.a.vip);
    const msg = JSON.stringify({ t: 'peers', peers: list.map(q => ({ vip: q.a.vip, vipStr: vipStr(q.a.vip), name: q.a.name })) });
    for (const q of list) this.raw(q, msg);
  }

  byWs(ws) { return this.wsc.get(ws) ?? super.byWs(ws); }
  bin(q, b) { try { q.ws.send(b); return true; } catch { return false; } }
}

/** May a page of this Origin join? No list: any. A list: only its origins, any case (web/server/src/http.rs origin_ok). */
export function originOk(allow, origin) {
  return !allow.length || (!!origin && allow.some(a => a.toLowerCase() === origin.toLowerCase()));
}
