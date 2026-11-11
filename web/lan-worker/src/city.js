// The RoamCity Durable Object: free roam of one city (src/index.js routes wss .../lan/roam?city=N to the object named
// city-N after checking the page's ticket, src/switch.js). It holds the city's free-roam hub (src/roam.js, as the Switch's)
// and relays what its pages' games send each other on port 47210 (the pairs without a WebRTC channel, web/rtc.js) and
// their {"t":"signal",...,"roam":1}, so no single object carries every Free Roam player: each city's load is its own.
//
// Why a second socket per page (web/lan.js opens it when the game's HELLO to the hub names a city) and not the Switch
// forwarding to a city object: the Switch would still take every incoming message (billed 20:1, and its CPU), so the
// per-object rate would not fall; here the Switch sees none of free roam's traffic.
//
// The page keeps its vip from the Switch (the ticket proves it: HMAC under ROAM_SECRET, 60 s to open). One socket per
// vip: a newer one closes the older 4001. Frames: the Switch's (src/switch.js head), free roam's only (source or
// destination port 47210); to 10.77.0.253:47210 the hub, a broadcast to every other page here, a unicast to its vip if
// here (elsewhere: dropped, as another LAN). Same rate limits as the Switch (RATE, CTL; over budget for FLOOD_MS: 1008).
// The hub ticks from the incoming messages (every player sends a HELLO a second), and from an alarm every CITY_ALARM_MS
// while a socket is open, which also closes the sockets silent for CITY_IDLE_MS (4000) and keeps the report alive.
// Report: the players (vip, room) to the Switch's POST /lan/city (src/switch.js cityReport), for its players list and
// /lan/stats; on change at most every CITY.reportMs, else every CITY.keepMs while any are here, and at once when the
// last socket goes. Memory only: after an eviction the hub refills from the next HELLOs, the sockets from their
// attachments.
import { Hub, HUB, ROAM_PORT } from './roam.js';
import { LIMITS } from '../../races-worker/src/net.js';
import { RATE, CTL, FLOOD_MS, HDR, MAX_FRAME, BROADCAST, CITY, SWITCH_NAME, isVip, cityOk } from './switch.js';

export const CITY_IDLE_MS = 30e3, CITY_ALARM_MS = 15e3;
const SAVE_MS = 10e3;   // `last` reaches the attachment this often (the idle sweep after an eviction)

export class RoamCity {
  constructor(ctx, env, rate = RATE) {
    this.ctx = ctx;
    this.env = env;
    this.rate = rate;
    this.socks = new Map();   // vip -> {ws, a}, a = the attachment {vip, city, last}
    this.wsc = new Map();     // ws -> the same
    for (const ws of ctx.getWebSockets()) {
      const a = ws.deserializeAttachment();
      if (a?.vip) this.add(ws, a);
    }
    this.city = [...this.socks.values()][0]?.a.city ?? null;
    this.hub = new Hub(env?.ROAM_K ? { default_k: Number(env.ROAM_K) } : {});
    this.buckets = new Map();
    this.ticked = 0;
    this.sent = { at: -Infinity, body: '' };
    this.armed = false;
  }

  add(ws, a) { const c = { ws, a }; this.socks.set(a.vip, c); this.wsc.set(ws, c); return c; }

  async fetch(req, now = Date.now()) {
    if (req.headers.get('Upgrade') !== 'websocket') return new Response('websocket only', { status: 426 });
    const q = new URL(req.url).searchParams, vip = Number(q.get('vip')) >>> 0, city = Number(q.get('city'));
    if (!isVip(vip) || !cityOk(city)) return new Response('bad request', { status: 400 });   // the Worker checked the ticket
    const [client, server] = Object.values(new WebSocketPair());
    this.accept(server, vip, city, now);
    return new Response(null, { status: 101, webSocket: client });
  }

  /** A page's socket (its ticket checked by the Worker); an older one of the same vip goes. */
  accept(ws, vip, city, now = Date.now()) {
    this.city = city;
    const old = this.socks.get(vip);
    if (old) this.kick(old, 4001, 'replaced');
    this.ctx.acceptWebSocket(ws);
    const a = { vip, city, last: now };
    ws.serializeAttachment(a);
    this.add(ws, a);
    this.arm(now);
  }

  async webSocketMessage(ws, data, now = Date.now()) {
    const c = this.wsc.get(ws);
    if (!c) return;
    if (now - c.a.last > SAVE_MS) { c.a.last = now; this.save(c); } else c.a.last = now;
    if (typeof data !== 'string') this.frame(c, new Uint8Array(data), now);
    else this.control(c, data, now);
    if (this.hub.count && now - this.ticked >= this.hub.opts.tick_ms) this.tick(now);
    this.report(now);
  }

  webSocketClose(ws) { const c = this.wsc.get(ws); if (c) this.drop(c); try { ws.close(1000, 'bye'); } catch {} }
  webSocketError(ws) { const c = this.wsc.get(ws); if (c) this.drop(c); }

  async alarm(now) {
    if (typeof now !== 'number') now = Date.now();   // the runtime passes alarmInfo; tests a time
    this.armed = false;
    for (const c of [...this.socks.values()]) if (now - c.a.last > CITY_IDLE_MS) this.kick(c, 4000, 'idle');
    if (this.hub.count) this.tick(now);
    this.report(now);
    this.arm(now);
  }

  arm(now = Date.now()) {
    if (this.armed || !this.socks.size) return;
    this.armed = true;
    try { this.ctx.storage.setAlarm(now + CITY_ALARM_MS)?.catch?.(() => { this.armed = false; }); } catch { this.armed = false; }
  }

  save(c) { try { c.ws.serializeAttachment(c.a); } catch {} }
  bin(q, b) { try { q.ws.send(b); return true; } catch { return false; } }
  raw(q, s) { try { q.ws.send(s); } catch {} }
  err(c, op, code) { this.raw(c, JSON.stringify({ t: 'error', op, code })); }
  kick(c, code, why) { try { c.ws.close(code, why); } catch {} this.drop(c); }

  drop(c) {
    if (this.socks.get(c.a.vip) !== c) { this.wsc.delete(c.ws); return; }
    this.socks.delete(c.a.vip);
    this.wsc.delete(c.ws);
    this.buckets.delete(c.a.vip);
    this.hub.leave(c.a.vip);
    this.report(Date.now(), !this.socks.size);
  }

  /** One frame (size > 0) or JSON message (size 0) of `c` over budget: true (drop). Empty for FLOOD_MS: closed 1008. */
  limited(c, size, now) {
    const r = this.rate;
    let k = this.buckets.get(c.a.vip);
    if (!k) this.buckets.set(c.a.vip, k = { frames: r.burstFrames, bytes: r.burstBytes, ctl: CTL.burst, at: now, over: 0 });
    const dt = Math.max(0, now - k.at) / 1000;
    k.at = now;
    k.frames = Math.min(k.frames + dt * r.framesPerS, r.burstFrames);
    k.bytes = Math.min(k.bytes + dt * r.bytesPerS, r.burstBytes);
    k.ctl = Math.min(k.ctl + dt * CTL.perS, CTL.burst);
    const out = size ? k.frames < 1 || k.bytes < size : k.ctl < 1;
    if (out) {
      if (!k.over) k.over = now;
      else if (now - k.over > FLOOD_MS) this.kick(c, 1008, 'rate limit');
      return true;
    }
    if (size) { k.frames -= 1; k.bytes -= size; } else k.ctl -= 1;
    if (size ? k.frames >= r.burstFrames / 2 : k.ctl >= CTL.burst / 2) k.over = 0;   // a flood's trickle does not reset the clock
    return false;
  }

  /** A binary frame: free roam's only; its source rewritten; to the hub, a broadcast, or a page of this city. */
  frame(c, b, now) {
    if (b.length < HDR || b.length > MAX_FRAME) { this.kick(c, 1009, b.length < HDR ? 'frame too short' : 'frame too long'); return 'size'; }
    if (this.limited(c, b.length, now)) return 'limited';
    const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
    b[1] = b[2] = b[3] = 0;
    dv.setUint32(4, c.a.vip);
    const dst = dv.getUint32(8), sport = dv.getUint16(12), dport = dv.getUint16(14);
    if (sport !== ROAM_PORT && dport !== ROAM_PORT) return 0;
    if (dst === HUB) {
      if (dport !== ROAM_PORT) return 0;
      this.hub.frame(c.a.vip, sport, b.subarray(HDR), now);
      return 1;
    }
    if (dst === BROADCAST) {
      let n = 0;
      for (const q of this.socks.values()) if (q !== c && this.bin(q, b)) n++;
      return n;
    }
    const q = this.socks.get(dst);
    return q && q !== c && this.bin(q, b) ? 1 : 0;
  }

  /** {"t":"signal",...,"roam":1} to another page of this city (web/rtc.js free roam); {"t":"ping"}; anything else ignored. */
  control(c, text, now) {
    if (text.length > LIMITS.msg || this.limited(c, 0, now)) return;
    let m;
    try { m = JSON.parse(text); } catch { return; }
    if (m?.t === 'ping') return this.raw(c, JSON.stringify({ t: 'pong', id: m.id ?? null }));
    if (m?.t !== 'signal' || m.roam !== 1) return;
    if (text.length > LIMITS.signal) return this.err(c, 'signal', 'big');
    const to = this.socks.get(Number(m.to) >>> 0);
    if (!to || to === c) return this.err(c, 'signal', 'roam');
    if (!['offer', 'answer', 'ice'].includes(m.kind)) return;
    this.raw(to, JSON.stringify({ t: 'signal', from: c.a.vip, kind: m.kind, data: m.data ?? null, roam: 1 }));
  }

  /** The hub's lists out, as frames from 10.77.0.253:47210 (src/switch.js tick). */
  tick(now) {
    this.ticked = now;
    for (const [dst, port, d] of this.hub.tick(now)) {
      const q = this.socks.get(dst);
      if (!q) continue;
      const f = new Uint8Array(HDR + d.length), dv = new DataView(f.buffer);
      f[0] = 3;
      dv.setUint32(4, HUB); dv.setUint32(8, dst); dv.setUint16(12, ROAM_PORT); dv.setUint16(14, port);
      f.set(d, HDR);
      this.bin(q, f);
    }
  }

  /** The players (vip, room) to the Switch (see the head). Returns the call when one was made (tests). */
  report(now = Date.now(), force = false) {
    const vips = new Map();
    for (const p of this.hub.players.values()) if (!vips.has(p.vip)) vips.set(p.vip, p.room >>> 0);
    const players = [...vips].sort((x, y) => x[0] - y[0]), body = JSON.stringify({ city: this.city, players });
    const since = now - this.sent.at;
    if (!force && (body === this.sent.body ? !players.length || since < CITY.keepMs : since < CITY.reportMs)) return null;
    if (force && body === this.sent.body) return null;
    this.sent = { at: now, body };
    const sw = this.env?.SWITCH;
    if (!sw || this.city == null) return null;
    const p = Promise.resolve().then(() => sw.get(sw.idFromName(SWITCH_NAME)).fetch('https://switch/lan/city', { method: 'POST', body })).catch(() => {});
    try { this.ctx.waitUntil?.(p); } catch {}
    return p;
  }
}
