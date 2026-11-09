// mc3-races: the Online Races list, room membership and WebRTC signalling on a Cloudflare Worker (step 3 of
// docs/races-protocol.md; same messages as the /lan switch's rooms of step 1). The race traffic itself does not pass
// here: members connect peer to peer (WebRTC, step 2) or through a relay switch.
//
//   GET /races (WebSocket upgrade, Origin in ALLOW_ORIGINS)  -> the Durable Object "Lobby" (one for the whole server)
//
// One Lobby holds every client and room. Clients use the WebSocket Hibernation API, so an idle lobby is evicted from
// memory and costs nothing; `{"t":"beat"}` is answered by the runtime's auto-response (`{"t":"beat"}` echoed) without
// waking it. Nothing is kept in storage: each socket carries its client in its attachment, and a room's settings
// (and password verifier) ride in its host's attachment, since a room dies with its host. After a wake the
// constructor rebuilds everything from the sockets. Clients silent for 120 s (their last message, or the last
// auto-answered beat) are expired by a sweep: on any wake (a connect, a message, at most every 60 s), and by an alarm
// every 60 s only while there is work (a room exists; the /lan switch adds free roam). Clients that only sit in the
// menus (beats) set no alarm, so the object stays hibernated between real events.
// shortcut: one Lobby for all rooms (fine for hundreds of rooms / a few thousand sockets); shard by room id + an index
// DO when one object's CPU or 32k sockets become the limit.
//
// vars: ALLOW_ORIGINS "https://a.example,https://b.example" (exact scheme://host, comma-separated).

import { MASK, ADDRS, LIMITS, vipAt, vipStr, macStr } from './net.js';

const IDLE_MS = 120e3, SWEEP_MS = 60e3;
const ID_CHARS = '23456789abcdefghjkmnpqrstuvwxyz';
const BEAT = '{"t":"beat"}';

const hex = b => [...new Uint8Array(b)].map(x => x.toString(16).padStart(2, '0')).join('');

/** crates/lan clean_name: control and bidi/format characters out, at most `max` characters, trimmed. */
export function clean(s, max, empty = '') {
  s = [...String(s ?? '')].filter(c => !/[\p{Cc}​-‏‪-‮⁠-⁯﻿]/u.test(c)).slice(0, max).join('').trim();
  return s || empty;
}

/** The room's password verifier: SHA-1 of its 16-byte salt and the password's UTF-8, hex. */
async function verifier(saltHex, pass) {
  const salt = Uint8Array.from(saltHex.match(/../g), h => parseInt(h, 16));
  const p = new TextEncoder().encode(pass);
  const buf = new Uint8Array(salt.length + p.length);
  buf.set(salt); buf.set(p, salt.length);
  return hex(await crypto.subtle.digest('SHA-1', buf));
}
function sameHex(a, b) {   // constant time (both are 40 hex digits)
  let d = a.length ^ b.length;
  for (let i = 0; i < a.length && i < b.length; i++) d |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return d === 0;
}
const passOf = m => typeof m.pass === 'string' ? m.pass.slice(0, LIMITS.pass) : '';

export class Lobby {
  constructor(ctx, env) {
    this.ctx = ctx;
    this.env = env;
    this.clients = new Map();   // vip -> {ws, a}, a = the attachment: {vip, name, ip, sub, room, last, host?}
    // host = {id, name, city, mode, max, salt, ver, at}: the room it hosts (settings + verifier)
    for (const ws of ctx.getWebSockets()) {
      const a = ws.deserializeAttachment();
      if (a?.vip) this.clients.set(a.vip, { ws, a });
    }
    if (typeof WebSocketRequestResponsePair === 'function')
      ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair(BEAT, BEAT));
  }

  async fetch(req) {
    if (req.headers.get('Upgrade') !== 'websocket') return new Response('websocket only', { status: 426 });
    this.lazySweep();   // a ghost's address and room go before the newcomer's welcome
    const [client, server] = Object.values(new WebSocketPair());
    if (!this.connect(server, req.headers.get('CF-Connecting-IP') || '')) return new Response('server full', { status: 503 });
    return new Response(null, { status: 101, webSocket: client });
  }

  /** A new client: accept its socket, give it a vip and say welcome. */
  connect(ws, ip, now = Date.now()) {
    let i = 0;
    while (i < ADDRS && this.clients.has(vipAt(i))) i++;
    if (i === ADDRS) return false;
    const vip = vipAt(i);
    this.ctx.acceptWebSocket(ws);
    const a = { vip, name: 'Player', ip, sub: false, room: null, last: now };
    ws.serializeAttachment(a);
    this.clients.set(vip, { ws, a });
    this.send(vip, { t: 'welcome', vip, vipStr: vipStr(vip), mask: MASK >>> 0, mac: macStr(vip) });
    return true;
  }

  async webSocketMessage(ws, data, now = Date.now()) {
    this.lazySweep(now);
    const c = this.byWs(ws);
    if (!c || typeof data !== 'string' || data.length > LIMITS.msg) return;
    c.a.last = Date.now();
    let m;
    try { m = JSON.parse(data); } catch { return this.save(c); }
    if (!m || typeof m !== 'object') return this.save(c);
    const vip = c.a.vip;
    switch (m.t) {
      case 'hello': c.a.name = clean(m.name, LIMITS.name, 'Player'); this.save(c); if (c.a.room) { this.roomChanged(c.a.room); this.listChanged(); } return;
      case 'ping': this.save(c); return this.send(vip, { t: 'pong', id: m.id ?? null });
      case 'rooms': c.a.sub = !!m.sub; this.save(c); if (c.a.sub) this.send(vip, this.list()); return;
      case 'create': return this.create(c, m);
      case 'join': return this.join(c, m);
      case 'leave': this.save(c); return this.leaveRoom(c, 'leave');
      case 'signal': this.save(c); return this.signal(c, m, data.length);
      default: this.save(c);   // beat (when not auto-answered) and anything unknown: only counts as heard
    }
  }

  webSocketClose(ws) { const c = this.byWs(ws); if (c) this.drop(c); try { ws.close(1000, 'bye'); } catch {} }
  webSocketError(ws) { const c = this.byWs(ws); if (c) this.drop(c); }

  async alarm(now) {
    if (typeof now !== 'number') now = Date.now();   // the runtime passes alarmInfo {retryCount, isRetry}; tests a time
    this.sweep(now);
    this.armed = false;
    await this.arm(true);
  }

  /** Drops the clients silent for IDLE_MS: no message and no auto-answered beat. */
  sweep(now = Date.now()) {
    this.swept = now;
    for (const c of [...this.clients.values()]) {
      let beat = 0;
      try { beat = this.ctx.getWebSocketAutoResponseTimestamp(c.ws)?.getTime() || 0; } catch {}
      if (now - Math.max(c.a.last, beat) > IDLE_MS) {
        this.drop(c);
        try { c.ws.close(4000, 'idle'); } catch {}
      }
    }
  }
  /** The sweep on a wake, at most every SWEEP_MS (no alarm runs while only menu clients are connected). */
  lazySweep(now = Date.now()) { if (!(now - (this.swept || 0) < SWEEP_MS)) this.sweep(now); }
  /** Work that needs the alarm: a room (its members' liveness). */
  busy() { for (const c of this.clients.values()) if (c.a.room) return true; return false; }

  // --- rooms ---

  async create(c, m) {
    const pass = passOf(m);
    let salt = '', ver = '';
    if (pass) { salt = hex(crypto.getRandomValues(new Uint8Array(16))); ver = await verifier(salt, pass); }
    const rooms = this.rooms().filter(r => r !== c);   // its own room closes on success
    if (rooms.length >= LIMITS.rooms || rooms.filter(r => r.a.ip === c.a.ip).length >= LIMITS.roomsPerIp) return this.err(c, 'create', 'limit');
    this.leaveRoom(c, null, true);
    let id;
    do id = [...crypto.getRandomValues(new Uint8Array(6))].map(b => ID_CHARS[b % ID_CHARS.length]).join('');
    while (rooms.some(r => r.a.host.id === id));
    const max = Number.isInteger(m.max) && m.max >= 2 && m.max <= LIMITS.members ? m.max : LIMITS.members;
    c.a.host = { id, name: clean(m.name, LIMITS.name, `${c.a.name}'s race`), city: clean(m.city, LIMITS.text), mode: clean(m.mode, LIMITS.text), max, salt, ver, at: Date.now() };
    c.a.room = id;
    this.save(c);
    this.roomChanged(id);
    this.listChanged();
    this.arm();
  }

  async join(c, m) {
    const pass = passOf(m);
    let h = this.host(m.room);
    if (!h) return this.err(c, 'join', 'gone');
    if (h.a.host.ver) {
      const got = pass ? await verifier(h.a.host.salt, pass) : '';
      h = this.host(m.room);   // the await let other messages run
      if (!h) return this.err(c, 'join', 'gone');
      if (!got || !sameHex(got, h.a.host.ver)) return this.err(c, 'join', 'password');
    }
    if (c.a.room === h.a.host.id) return this.send(c.a.vip, this.roomMsg(h.a.host.id));
    if (this.members(h.a.host.id).length >= h.a.host.max) return this.err(c, 'join', 'full');
    this.leaveRoom(c, null, true);
    c.a.room = h.a.host.id;
    this.save(c);
    this.roomChanged(c.a.room);
    this.listChanged();
    this.arm();
  }

  /** Out of the current room (why: the `left` reason to send it, null = none). quiet: the caller announces the list. */
  leaveRoom(c, why, quiet = false) {
    const id = c.a.room;
    if (!id) return;
    c.a.room = null;
    if (c.a.host) {
      delete c.a.host;
      for (const o of this.members(id)) { o.a.room = null; this.save(o); this.send(o.a.vip, { t: 'left', room: id, why: 'closed' }); }
    } else this.roomChanged(id);
    this.save(c);
    if (why) this.send(c.a.vip, { t: 'left', room: id, why });
    if (!quiet) this.listChanged();
  }

  signal(c, m, size) {
    if (size > LIMITS.signal) return this.err(c, 'signal', 'big');
    const to = this.clients.get(Number(m.to) >>> 0);
    if (!c.a.room || !to || to === c || to.a.room !== c.a.room) return this.err(c, 'signal', 'room');
    if (!['offer', 'answer', 'ice'].includes(m.kind)) return;
    this.send(to.a.vip, { t: 'signal', from: c.a.vip, kind: m.kind, data: m.data ?? null });
  }

  drop(c) {
    if (this.clients.get(c.a.vip) !== c) return;
    const had = !!c.a.room;
    this.leaveRoom(c, null, true);
    this.clients.delete(c.a.vip);
    if (had) this.listChanged();
  }

  // --- views ---

  rooms() { return [...this.clients.values()].filter(c => c.a.host).sort((x, y) => x.a.host.at - y.a.host.at || (x.a.host.id < y.a.host.id ? -1 : 1)); }
  host(id) { for (const c of this.clients.values()) if (c.a.host && c.a.host.id === id) return c; }
  members(id) { return [...this.clients.values()].filter(c => c.a.room === id); }
  roomView(h, n = this.members(h.a.host.id).length) {
    const r = h.a.host;
    return { id: r.id, name: r.name, host: h.a.name, hostVip: h.a.vip, city: r.city, mode: r.mode, n, max: r.max, locked: !!r.ver };
  }
  list() {
    const n = new Map();   // one pass over the clients, not one per room
    for (const c of this.clients.values()) if (c.a.room) n.set(c.a.room, (n.get(c.a.room) || 0) + 1);
    return { t: 'rooms', rooms: this.rooms().map(h => this.roomView(h, n.get(h.a.host.id) || 0)) };
  }
  roomMsg(id) {
    const h = this.host(id);
    const members = this.members(id).sort((x, y) => (y === h) - (x === h) || x.a.vip - y.a.vip)
      .map(o => ({ vip: o.a.vip, vipStr: vipStr(o.a.vip), name: o.a.name, host: o === h }));
    return { t: 'room', room: this.roomView(h), members };
  }
  roomChanged(id) { if (!this.host(id)) return; const msg = JSON.stringify(this.roomMsg(id)); for (const o of this.members(id)) this.raw(o, msg); }
  listChanged() { const msg = JSON.stringify(this.list()); for (const o of this.clients.values()) if (o.a.sub) this.raw(o, msg); }

  // --- plumbing ---

  byWs(ws) { const a = ws.deserializeAttachment(); const c = a && this.clients.get(a.vip); return c && c.ws === ws ? c : undefined; }
  save(c) { try { c.ws.serializeAttachment(c.a); } catch {} }
  send(vip, obj) { const c = this.clients.get(vip); if (c) this.raw(c, JSON.stringify(obj)); }
  raw(c, text) { try { c.ws.send(text); } catch { /* closing: its close event drops it */ } }
  err(c, op, code) { this.send(c.a.vip, { t: 'error', op, code }); }
  async arm(force = false) {
    if (this.armed && !force) return;
    if (!this.busy()) return;
    this.armed = true;
    if (force || !(await this.ctx.storage.getAlarm())) await this.ctx.storage.setAlarm(Date.now() + SWEEP_MS);
  }
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (url.pathname !== '/races') return new Response('not found', { status: 404 });
    const allowed = (env.ALLOW_ORIGINS || '').split(',').map(s => s.trim()).filter(Boolean);
    if (!allowed.includes(req.headers.get('Origin'))) return new Response('origin not allowed', { status: 403 });
    if (req.headers.get('Upgrade') !== 'websocket') return new Response('websocket only', { status: 426 });
    return env.LOBBY.get(env.LOBBY.idFromName('lobby')).fetch(req);
  },
};
