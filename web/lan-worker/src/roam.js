// The free-roam hub: a faithful port of web/server/crates/roam/src/lib.rs (itself after nfsu2-web's roam hub, MIT
// License, Copyright (c) 2026 th0mk).  Same wire (xboxrecomp/src/net/roam/roam_core.h), same link pick; see the Rust
// file for the rules.  The hub stands at 10.77.0.253 port 47210 on the switch (src/index.js), which feeds it the
// datagrams sent there (frame), the browsers that left (leave), and sends its PEERS lists out (tick).
//
// - player -> hub: HELLO every second (id, status, name, links wanted: its K), SNAP twice a second (place, velocity),
//   BYE when it stops.
// - hub -> player: every tick a PEERS list { virtual address, port, id } of the players linked to it; every far_ms
//   (2 s) a FAR list of the others it is not linked with, nearest first: { id, status, place, orientation, name }
//   (map markers; a core without FAR, type 9, drops it as an unknown type).

export const HUB = 0x0A4D00FD;   // 10.77.0.253
export const ROAM_PORT = 47210;
const MAGIC = [0x4D, 0x43, 0x33, 0x52];   // "MC3R"
const HDR = 16, CRC = 4;
export const MSG_HELLO = 1, MSG_PEERS = 2, MSG_SNAP = 3, MSG_BYE = 8, MSG_FAR = 9;
export const HF_CREW = 0x0200, ST_ROAM = 2;
const LIST_MAX = 96;
export const FAR_MAX = 20;   // 50 bytes each at worst: one datagram under 1200 bytes

export const OPTIONS = {
  default_k: 32, degree: 64, tick_ms: 500, stale_ms: 60_000, gone_ms: 10_000, soon_ms: 1000, look_ms: 4000,
  view_m: 150, away_m: 100, hold_ms: 5000, keep: 0.5, max_players: 64 * 250, far_ms: 2000,
};

const CRC_TABLE = Uint32Array.from({ length: 256 }, (_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ c >>> 1 : c >>> 1;
  return c >>> 0;
});
export function crc32(d) {
  let c = ~0;
  for (let i = 0; i < d.length; i++) c = CRC_TABLE[(c ^ d[i]) & 255] ^ c >>> 8;
  return ~c >>> 0;
}

/** A datagram to the hub: {k:'hello'|'snap'|'bye'|'other', ...} or null (not one of ours, or malformed). */
export function decode(d) {
  if (d.length < HDR + CRC || MAGIC.some((b, i) => d[i] !== b) || d[4] !== 1) return null;
  const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
  if (dv.getUint16(6, true) !== d.length) return null;
  const end = d.length - CRC;
  if (crc32(d.subarray(0, end)) !== dv.getUint32(end, true)) return null;
  let n = 8;
  const take = k => { if (n + k > end) throw 0; const s = n; n += k; return s; };
  const u8 = () => d[take(1)], u16 = () => dv.getUint16(take(2), true), u32 = () => dv.getUint32(take(4), true);
  const f32 = () => dv.getFloat32(take(4), true);
  try {
    const sender = u32(); u16(); const flags = u16();
    if (sender === 0) return null;
    switch (d[5]) {
      case MSG_HELLO: {
        u16(); u8(); const status = u8(); u16(); u16(); const room = u32();
        const nl = u8(), at = take(nl);
        const name = d.subarray(at, at + nl);
        if (nl > 24 || name.some(c => c < 0x20 || c >= 0x7F)) return null;
        take(4 * u8());
        let links = 0;
        if (flags & HF_CREW && n < end) {
          take(4 * u8());
          links = n < end ? u8() : 0;
        }
        return { k: 'hello', sender, status, name: String.fromCharCode(...name), links, room };
      }
      case MSG_SNAP: {
        const status = u8(); u8(); u16(); const cnt = u8(); u8();
        if (cnt === 0) return null;
        u32();
        const p = [f32(), f32(), f32()], v = [f32(), f32(), f32()];
        if ([...p, ...v].some(x => !Number.isFinite(x) || Math.abs(x) > 20000)) return null;
        const q = [0, 0, 0, 0].map(() => dv.getInt16(take(2), true));
        return { k: 'snap', sender, status, p, v, q };
      }
      case MSG_BYE: return { k: 'bye', sender };
      default: return { k: 'other' };
    }
  } catch { return null; }
}

/** A PEERS datagram: [address, port, id] each, the address big-endian. */
export function encodePeers(sender, seq, list) {
  const n = Math.min(list.length, LIST_MAX), len = HDR + 1 + 10 * n + CRC;
  const b = new Uint8Array(len), dv = new DataView(b.buffer);
  b.set(MAGIC); b[4] = 1; b[5] = MSG_PEERS;
  dv.setUint16(6, len, true); dv.setUint32(8, sender, true); dv.setUint16(12, seq, true);
  b[16] = n;
  for (let i = 0; i < n; i++) {
    const [ip, port, id] = list[i], o = 17 + 10 * i;
    dv.setUint32(o, ip, false); dv.setUint16(o + 4, port, true); dv.setUint32(o + 6, id, true);
  }
  dv.setUint32(len - CRC, crc32(b.subarray(0, len - CRC)), true);
  return b;
}

/** A FAR datagram: [id, status, place, orientation as the SNAP carried it, name] each, at most FAR_MAX. */
export function encodeFar(sender, seq, list) {
  const l = list.slice(0, FAR_MAX).map(([id, status, p, q, name]) => [id, status, p, q, name.slice(0, 24)]);
  const len = HDR + 1 + l.reduce((s, e) => s + 26 + e[4].length, 0) + CRC;
  const b = new Uint8Array(len), dv = new DataView(b.buffer);
  b.set(MAGIC); b[4] = 1; b[5] = MSG_FAR;
  dv.setUint16(6, len, true); dv.setUint32(8, sender, true); dv.setUint16(12, seq, true);
  b[16] = l.length;
  let o = 17;
  for (const [id, status, p, q, name] of l) {
    dv.setUint32(o, id, true); b[o + 4] = status;
    p.forEach((x, i) => dv.setFloat32(o + 5 + 4 * i, x, true));
    q.forEach((x, i) => dv.setInt16(o + 17 + 2 * i, x, true));
    b[o + 25] = name.length;
    for (let i = 0; i < name.length; i++) b[o + 26 + i] = name.charCodeAt(i);
    o += 26 + name.length;
  }
  dv.setUint32(len - CRC, crc32(b.subarray(0, len - CRC)), true);
  return b;
}

const dist = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
const since = (now, t) => Math.max(0, now - t);
const key = (vip, port) => vip * 65536 + port;

export class Hub {
  constructor(opts = {}) {
    this.opts = { ...OPTIONS, ...opts };
    this.players = new Map();   // key(vip, port) -> {vip, port, id, name, status, k, pos: {at, v, t}|null, last, links: Map key -> since}
    this.id = 0x48554231;
    this.seq = 0;
  }

  get count() { return this.players.size; }

  /** A datagram to the hub from src:sport, at `now` ms. */
  frame(src, sport, data, now) {
    const kk = key(src, sport), m = decode(data);
    if (!m) return;
    let p = this.players.get(kk);
    if (m.k === 'hello') {
      if (!p) {
        // the same player at another address (the page reconnected, web/lan.js): its old endpoint goes
        for (const [k2, q] of this.players) if (q.id === m.sender) this.players.delete(k2);
        if (this.players.size >= this.opts.max_players) return;
      }
      const k = Math.min(m.links === 0 ? this.opts.default_k : m.links, this.opts.degree);
      if (!p) this.players.set(kk, p = { vip: src, port: sport, id: m.sender, name: '', room: m.room, status: m.status, k, pos: null, last: now, links: new Map() });
      if (p.id !== m.sender) { p.id = m.sender; p.pos = null; p.links.clear(); }   // a restart at the same endpoint
      p.name = m.name; p.room = m.room; p.status = m.status; p.k = k; p.last = now;
    } else if (m.k === 'snap') {
      if (p && p.id === m.sender) { p.status = m.status; p.pos = { at: m.p, v: m.v, t: now }; p.q = m.q; p.last = now; }
    } else if (m.k === 'bye') {
      if (p && p.id === m.sender) this.players.delete(kk);
    }
  }

  /** Every endpoint of a browser that left the switch. */
  leave(vip) { for (const [k, p] of this.players) if (p.vip === vip) this.players.delete(k); }

  /** The links again, and each player's list: [[address, port, PEERS datagram], ...]. */
  // shortcut: every pair is weighed (O(n^2)), as the Rust hub: fine to a couple of thousand players.
  tick(now) {
    const o = this.opts;
    for (const [k, p] of this.players) if (since(now, p.last) >= o.gone_ms) this.players.delete(k);
    const keys = [...this.players.keys()], ps = keys.map(k => this.players.get(k));
    const places = ps.map(p => {
      if (!p.pos) return null;
      const { at, v, t } = p.pos;
      if (since(now, t) >= o.stale_ms) return null;
      const roam = p.status === ST_ROAM, age = since(now, t) / 1000;
      const go = s => roam ? [at[0] + v[0] * s, at[1] + v[1] * s, at[2] + v[2] * s] : at;
      const soon = go(age + o.soon_ms / 1000), later = go(age + o.look_ms / 1000);
      const speed = dist(v, [0, 0, 0]);
      let view = null;
      if (roam && speed > 1) { const k = o.view_m / speed; view = [soon[0] + v[0] * k, soon[1] + v[1] * k, soon[2] + v[2] * k]; }
      return { soon, later, view, away: roam ? 0 : o.away_m };
    });
    const pairs = [];
    for (let i = 0; i < keys.length; i++) {
      const a = places[i];
      if (!a) continue;
      for (let j = i + 1; j < keys.length; j++) {
        const b = places[j];
        if (!b || ps[i].vip === ps[j].vip) continue;   // two endpoints of one browser
        if (ps[i].room !== ps[j].room) continue;       // another room (MC3: another city)
        let d = Math.min(dist(a.soon, b.soon), dist(a.later, b.later));
        if (a.view) d = Math.min(d, dist(a.view, b.soon));
        if (b.view) d = Math.min(d, dist(b.view, a.soon));
        d += a.away + b.away;
        const s = ps[i].links.get(keys[j]);
        if (s !== undefined) d = since(now, s) < o.hold_ms ? -1 : d * o.keep;
        pairs.push([d, i, j]);
      }
    }
    pairs.sort((x, y) => x[0] - y[0] || x[1] - y[1] || x[2] - y[2]);
    const deg = keys.map(() => 0), linked = keys.map(() => new Map());
    for (const [, i, j] of pairs) {
      if (deg[i] >= ps[i].k || deg[j] >= ps[j].k) continue;
      deg[i]++; deg[j]++;
      const s = ps[i].links.get(keys[j]) ?? now;
      linked[i].set(keys[j], s);
      linked[j].set(keys[i], s);
    }
    this.seq = this.seq + 1 & 0xFFFF;
    const out = ps.map((p, i) => {
      p.links = linked[i];
      const list = [...p.links.keys()].map(l => { const q = this.players.get(l); return [q.vip, q.port, q.id]; });
      return [p.vip, p.port, encodePeers(this.id, this.seq, list)];
    });
    if (since(now, this.farAt ?? 0) >= o.far_ms) {
      this.farAt = now;
      // each placed player: the placed ones it is not linked with (nor its own browser), nearest first
      // shortcut: a sort per player every far_ms (O(n^2 log n)), as the Rust hub's.
      ps.forEach((p, i) => {
        if (!places[i]) return;
        const list = ps.map((q, j) => [j, q])
          .filter(([j, q]) => j !== i && places[j] && q.vip !== p.vip && q.room === p.room && !p.links.has(keys[j]))
          .map(([j, q]) => [dist(places[i].soon, places[j].soon), j, q])
          .sort((x, y) => x[0] - y[0] || x[1] - y[1])
          .slice(0, FAR_MAX)
          .map(([, , q]) => [q.id, q.status, q.pos.at, q.q ?? [0, 0, 0, 0], q.name]);
        out.push([p.vip, p.port, encodeFar(this.id, this.seq, list)]);
      });
    }
    return out;
  }

  /** The players as the page's Online tab shows them: {"t":"roam","players":[{name, status}]}, by name. */
  listJson() {
    const v = [...this.players.values()].sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : a.id - b.id));
    return JSON.stringify({ t: 'roam', players: v.map(p => ({ name: p.name, status: p.status })) });
  }

  /** Counts only, no names: {total, rooms: {<room id>: n}} (the stats endpoint). */
  stats() {
    const rooms = {};
    for (const p of this.players.values()) rooms[p.room] = (rooms[p.room] || 0) + 1;
    return { total: this.players.size, rooms };
  }

  /** The players linked with vip:port, as [vip, port] sorted (tests, logs). */
  linksOf(vip, port) {
    const p = this.players.get(key(vip, port));
    return p ? [...p.links.keys()].sort((a, b) => a - b).map(k => [Math.floor(k / 65536), k % 65536]) : [];
  }
}
