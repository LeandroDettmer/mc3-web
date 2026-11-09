// The Switch Durable Object: the /lan switch (System Link, race rooms, free roam) with the same wire contract as
// web/server's (crates/lan + crates/roam): the page (web/lan.js) and the game cannot tell them apart. The Worker's
// entry is src/index.js.
//
//   GET /lan[?vip=n] (WebSocket upgrade, Origin in ALLOW_ORIGINS) -> the Durable Object "Switch" (one for the server)
//
// Switch extends the races Lobby (web/races-worker: rooms, signalling, hello/ping, idle expiry, hibernation) with
// what crates/lan adds: binary frames routed between members (unicast, broadcast to System Link peers, a room is its
// own LAN, free roam's port and the hub route whatever the rooms), the peers list, {"t":"lan"}, {"t":"roam"}, ?vip=
// on reconnect, a per-sender token bucket; and the free-roam hub (src/roam.js) at 10.77.0.253:47210.
//
// Frames (big-endian): [0] kind, [1..3] 0, [4..7] source vip (rewritten to the sender's), [8..11] destination vip
// (0xFFFFFFFF broadcast), [12..13] source port, [14..15] destination port, [16..] datagram (<= 1472 bytes).
//
// Cost (free tier): incoming WebSocket messages bill 20 to a request, outgoing are free. The hub's 500 ms tick is not
// an alarm (two alarms a second would be 172,800 requests a day by themselves): it runs from the incoming messages,
// at most every tick_ms, and every hub player sends at least a HELLO a second (roam_core.c T_HUB_HELLO), well inside
// its 2.5 s grace (T_HUB_GRACE). The Lobby's 60 s alarm (idle expiry, armed only while clients exist) also ticks.
// The hub and the rate buckets live in memory: lost when the object hibernates, which needs 10 s without a message,
// so never during free roam; the hub refills from the next HELLOs.
//
// vars: ALLOW_ORIGINS "https://a.example,https://b.example" (scheme://host, comma-separated, any case); empty = any.

import { Lobby, clean } from '../../races-worker/src/index.js';
import { vipAt, vipStr, macStr, NET, MASK, ADDRS, LIMITS } from '../../races-worker/src/net.js';
import { Hub, HUB, ROAM_PORT } from './roam.js';

export const BROADCAST = 0xFFFFFFFF, HDR = 16, MAX_DATA = 1472, MAX_FRAME = HDR + MAX_DATA;
export const RATE = { framesPerS: 2000, burstFrames: 4000, bytesPerS: 1 << 20, burstBytes: 2 << 20 };   // crates/lan Limits
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
  }

  /** GET /lan/stats: counts only (no names, no vips): connected sockets, free roam by room id, race rooms. */
  stats() {
    return { online: this.clients.size, freeRoam: this.hub.stats(), races: { rooms: this.rooms().length } };
  }

  async fetch(req) {
    if (new URL(req.url).pathname === '/lan/stats') return Response.json(this.stats());
    if (req.headers.get('Upgrade') !== 'websocket') return new Response('websocket only', { status: 426 });
    const want = Number(new URL(req.url).searchParams.get('vip')) >>> 0;
    const [client, server] = Object.values(new WebSocketPair());
    if (!this.connect(server, req.headers.get('CF-Connecting-IP') || '', Date.now(), want)) return new Response('LAN full', { status: 503 });
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
    const a = { vip, name: 'Player', ip, sub: false, room: null, last: now, hi: false, lan: false };
    ws.serializeAttachment(a);
    const c = { ws, a };
    this.clients.set(vip, c);
    this.wsc.set(ws, c);
    this.send(vip, { t: 'welcome', vip, vipStr: vipStr(vip), mask: MASK >>> 0, mac: macStr(vip) });
    this.arm();
    return true;
  }

  async webSocketMessage(ws, data, now = Date.now()) {
    const c = this.byWs(ws);
    if (c) {
      if (typeof data !== 'string') this.frame(c, new Uint8Array(data), now);
      else if (!this.control(c, data, now)) await super.webSocketMessage(ws, data);
    }
    if (this.hub.count && now - this.ticked >= this.hub.opts.tick_ms) this.tick(now);
  }

  /** The switch's own control messages (hello with lan, lan, roam); false: the Lobby's (rooms, ping, ...). */
  control(c, text, now) {
    if (text.length > LIMITS.msg || !/"(hello|lan|roam)"/.test(text)) return false;
    let m;
    try { m = JSON.parse(text); } catch { return false; }
    if (m?.t === 'hello') {
      c.a.name = clean(m.name, LIMITS.name, 'Player'); c.a.hi = true; c.a.lan = on(m.lan);
    } else if (m?.t === 'lan') c.a.lan = on(m.on);
    else if (m?.t !== 'roam') return false;
    c.a.last = now;
    this.save(c);
    if (m.t === 'roam') this.raw(c, this.hub.listJson());
    if (m.t === 'hello') {
      this.peersChanged();
      if (c.a.room) { this.roomChanged(c.a.room); this.listChanged(); }
    }
    return true;
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
    let k = this.buckets.get(vip);
    if (!k) this.buckets.set(vip, k = { frames: r.burstFrames, bytes: r.burstBytes, at: now });
    const dt = Math.max(0, now - k.at) / 1000;
    k.at = now;
    k.frames = Math.min(k.frames + dt * r.framesPerS, r.burstFrames);
    k.bytes = Math.min(k.bytes + dt * r.bytesPerS, r.burstBytes);
    if (k.frames < 1 || k.bytes < b.length) return 'limited';
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
  }

  drop(c) {
    const live = this.clients.get(c.a.vip) === c;
    super.drop(c);
    this.wsc.delete(c.ws);
    if (!live) return;
    this.buckets.delete(c.a.vip);
    this.hub.leave(c.a.vip);
    if (c.a.hi) this.peersChanged();
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
