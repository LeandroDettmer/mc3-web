// Free roam per city (src/city.js RoamCity, the Switch's tickets and reports, the Worker's /lan/roam, web/lan.js's city
// socket), with fake hibernatable WebSockets as in switch.test.mjs. node --test test/
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
import worker from '../src/index.js';
import { RoamCity, CITY_IDLE_MS } from '../src/city.js';
import { Switch, ticket, ticketOk, TICKET_MS, CITY, CITY_TTL } from '../src/switch.js';
import { HUB, ROAM_PORT, MSG_PEERS } from '../src/roam.js';
import { vipAt } from '../../races-worker/src/net.js';
import { hello, snap, dgram } from './wire.mjs';

class FakeWs {
  constructor() { this.out = []; this.closed = null; this.att = null; }
  send(s) { if (this.closed) throw new Error('closed'); this.out.push(typeof s === 'string' ? JSON.parse(s) : Uint8Array.from(s)); }
  close(code, why) { this.closed = [code, why]; }
  serializeAttachment(a) { this.att = JSON.stringify(a); }
  deserializeAttachment() { return this.att && JSON.parse(this.att); }
  take() { const o = this.out; this.out = []; return o; }
  bins() { return this.take().filter(m => m instanceof Uint8Array); }
  texts() { return this.take().filter(m => !(m instanceof Uint8Array)); }
}
function fakeCtx() {
  const ctx = { sockets: [], alarm: null };
  ctx.acceptWebSocket = ws => ctx.sockets.push(ws);
  ctx.getWebSockets = () => ctx.sockets.filter(ws => !ws.closed);
  ctx.setWebSocketAutoResponse = () => {};
  ctx.getWebSocketAutoResponseTimestamp = () => null;
  ctx.storage = { getAlarm: async () => ctx.alarm, setAlarm: async t => { ctx.alarm = t; } };
  return ctx;
}
const be32 = n => [n >>> 24, n >>> 16 & 255, n >>> 8 & 255, n & 255], be16 = n => [n >>> 8, n & 255];
const udp = (src, dst, data, sport = ROAM_PORT, dport = ROAM_PORT) => [3, 0, 0, 0, ...be32(src), ...be32(dst), ...be16(sport), ...be16(dport), ...data];
const SECRET = 'test-secret-0123456789';

/** A Switch and its city objects, wired as the Worker wires them (env.SWITCH, env.ROAM). */
function world() {
  const calls = [];
  const env = { ROAM_SECRET: SECRET };
  const sw = new Switch(fakeCtx(), env);
  env.SWITCH = { idFromName: n => n, get: () => ({ fetch: (u, init) => { calls.push(JSON.parse(init.body)); return sw.fetch(new Request(u, init)); } }) };
  const cities = new Map();
  env.ROAM = { idFromName: n => n, get: n => { if (!cities.has(n)) cities.set(n, new RoamCity(fakeCtx(), env)); return cities.get(n); } };
  const city = n => env.ROAM.get(`city-${n}`);
  const page = (name, now = Date.now()) => {
    const ws = new FakeWs();
    assert.ok(sw.connect(ws, '1.1.1.' + (sw.clients.size + 1), now));
    const w = ws.take()[0];
    return { ws, vip: w.vip, w, name };
  };
  // the page's city socket (what web/lan.js opens with its ticket; the Worker checked it)
  const enter = (p, n, now = Date.now()) => { const ws = new FakeWs(); city(n).accept(ws, p.vip, n, now); return (p.cs = { ws, n }); };
  const send = (p, f, now = Date.now()) => city(p.cs.n).webSocketMessage(p.cs.ws, Uint8Array.from(f).buffer, now);
  const roam = async (p, id, room, at, now = Date.now()) => {
    await send(p, udp(p.vip, HUB, [...hello(id, p.name, 7, room)]), now);
    await send(p, udp(p.vip, HUB, [...snap(id, at, [0, 0, 0])]), now);
  };
  return { env, sw, city, cities, page, enter, send, roam, calls };
}
const peersOf = ws => ws.bins().filter(b => b[16 + 5] === MSG_PEERS).map(b => {
  const n = b[16 + 16], out = [];
  for (let i = 0; i < n; i++) out.push(new DataView(b.buffer, b.byteOffset).getUint32(16 + 17 + 10 * i));
  return out;
});
const settle = () => new Promise(r => setTimeout(r, 0));

test('tickets: signed for one vip, 60 s, constant-time check', async () => {
  const now = Date.now(), v = vipAt(3), t = await ticket(SECRET, v, now);
  assert.equal(t.exp, now + TICKET_MS);
  assert.ok(await ticketOk(SECRET, v, t.exp, t.sig, now));
  assert.ok(!await ticketOk(SECRET, vipAt(4), t.exp, t.sig, now), 'another vip');
  assert.ok(!await ticketOk('other', v, t.exp, t.sig, now), 'another secret');
  assert.ok(!await ticketOk(SECRET, v, t.exp, t.sig, t.exp), 'expired');
  assert.ok(!await ticketOk(SECRET, v, t.exp + 1, t.sig, now), 'exp changed');
  assert.ok(!await ticketOk(SECRET, v, t.exp, t.sig.slice(1), now) && !await ticketOk(SECRET, v, t.exp, null, now));
  assert.ok(!await ticketOk('', v, t.exp, t.sig, now), 'no secret: none');
});

test('worker: /lan/roam checks Origin, ticket and city, then the object city-N; 404 without ROAM or ROAM_SECRET', async () => {
  let got = null;
  const env = { ALLOW_ORIGINS: 'https://game.example', ROAM_SECRET: SECRET, ROAM: { idFromName: n => n, get: id => ({ fetch: r => (got = [id, r], new Response('ok')) }) } };
  const v = vipAt(7), t = await ticket(SECRET, v);
  const req = (q, origin = 'https://game.example', up = 'websocket') => new Request('https://lan.example/lan/roam' + q, { headers: { Origin: origin, Upgrade: up } });
  const ok = `?city=2&vip=${v}&exp=${t.exp}&sig=${t.sig}`;
  assert.equal((await worker.fetch(req(ok), env)).status, 200);
  assert.equal(got[0], 'city-2');
  assert.equal((await worker.fetch(req(ok, 'https://evil.example'), env)).status, 403);
  assert.equal((await worker.fetch(req(ok, undefined, 'h2c'), env)).status, 426);
  assert.equal((await worker.fetch(req(`?city=2&vip=${vipAt(8)}&exp=${t.exp}&sig=${t.sig}`), env)).status, 403, 'another vip');
  assert.equal((await worker.fetch(req(`?city=14&vip=${v}&exp=${t.exp}&sig=${t.sig}`), env)).status, 403, 'no such city');
  assert.equal((await worker.fetch(req(`?city=x&vip=${v}&exp=${t.exp}&sig=${t.sig}`), env)).status, 403);
  assert.equal((await worker.fetch(req(ok), { ...env, ROAM_SECRET: '' })).status, 404);
  assert.equal((await worker.fetch(req(ok), { ...env, ROAM: undefined })).status, 404);
});

test('switch: welcome says roamCity only with ROAM + ROAM_SECRET; a ticket takes the vip out of its own hub; off without', async () => {
  const { sw, page } = world();
  const a = page('Ann');
  assert.equal(a.w.roamCity, 1);
  await sw.webSocketMessage(a.ws, Uint8Array.from(udp(a.vip, HUB, [...hello(1, 'Ann', 7, 2)])).buffer);
  assert.equal(sw.hub.count, 1, 'the fallback path: the switch hub');
  await sw.webSocketMessage(a.ws, '{"t":"ticket"}');
  const t = a.ws.texts().find(m => m.t === 'ticket');
  assert.equal(t.vip, a.vip);
  assert.ok(await ticketOk(SECRET, a.vip, t.exp, t.sig));
  assert.equal(sw.hub.count, 0);
  const off = new Switch(fakeCtx(), {}), ws = new FakeWs();
  off.connect(ws, '1.1.1.1');
  assert.equal(ws.take()[0].roamCity, undefined);
  await off.webSocketMessage(ws, '{"t":"ticket"}');
  assert.deepEqual(ws.texts().find(m => m.t === 'error'), { t: 'error', op: 'ticket', code: 'off' });
});

test('city: routing inside one city only; hub links per city; free-roam ports only; signals; one socket per vip', async () => {
  const { city, page, enter, send, roam } = world();
  const now = Date.now();
  const a = page('Ann'), b = page('Bob'), c = page('Cy');
  enter(a, 0); enter(b, 0); enter(c, 1);
  await roam(a, 1, 0, [0, 0, 0], now); await roam(b, 2, 0, [10, 0, 0], now); await roam(c, 3, 1, [5, 0, 0], now);
  assert.equal(city(0).hub.count, 2);
  assert.equal(city(1).hub.count, 1);
  for (const p of [a, b, c]) p.cs.ws.take();
  city(0).tick(now + 500); city(1).tick(now + 500);
  assert.deepEqual(peersOf(a.cs.ws).at(-1), [b.vip]);
  assert.deepEqual(peersOf(c.cs.ws).at(-1) ?? [], [], 'alone in its city');
  b.cs.ws.take();
  // relay: a unicast to b arrives, its source rewritten; to c (another city) and off the roam port: nothing
  await send(a, udp(0, b.vip, [1, 2, 3]), now + 600);
  const got = b.cs.ws.bins();
  assert.equal(got.length, 1);
  assert.deepEqual([...got[0].subarray(4, 8)], be32(a.vip));
  await send(a, udp(a.vip, c.vip, [1]), now + 600);
  await send(a, udp(a.vip, b.vip, [1], 1000, 1000), now + 600);
  await send(a, udp(a.vip, 0xFFFFFFFF, [9]), now + 600);
  assert.equal(c.cs.ws.bins().length, 0);
  assert.equal(b.cs.ws.bins().length, 1, 'only the broadcast');
  // signals: inside the city
  await city(0).webSocketMessage(a.cs.ws, JSON.stringify({ t: 'signal', to: b.vip, kind: 'offer', data: { sdp: 'x' }, roam: 1 }), now);
  assert.deepEqual(b.cs.ws.texts().at(-1), { t: 'signal', from: a.vip, kind: 'offer', data: { sdp: 'x' }, roam: 1 });
  await city(0).webSocketMessage(a.cs.ws, JSON.stringify({ t: 'signal', to: c.vip, kind: 'offer', data: { sdp: 'x' }, roam: 1 }), now);
  assert.deepEqual(a.cs.ws.texts().at(-1), { t: 'error', op: 'signal', code: 'roam' });
  // a second socket of the same vip replaces the first
  const old = a.cs.ws;
  enter(a, 0);
  assert.deepEqual(old.closed, [4001, 'replaced']);
  assert.equal(city(0).hub.count, 1, "the old socket's endpoint left the hub");
});

test('city change: the old city drops the player, the new one takes it; reports move it in the players list', async () => {
  const { sw, city, page, enter, roam, calls } = world();
  const now = Date.now();
  const a = page('Ann'), b = page('Bob');
  await sw.webSocketMessage(a.ws, JSON.stringify({ t: 'players', sub: 1 }), now);
  enter(a, 0); enter(b, 0);
  await roam(a, 1, 0, [0, 0, 0], now); await roam(b, 2, 0, [1, 0, 0], now);
  await settle();
  assert.deepEqual(calls.at(-1), { city: 0, players: [[a.vip, 0, 2]] }, 'the first change at once; the next waits CITY.reportMs');
  const t1 = now + CITY.reportMs;
  await roam(a, 1, 0, [0, 0, 0], now + 5000); await roam(b, 2, 0, [1, 0, 0], now + 5000);   // within reportMs: no report
  await settle();
  assert.deepEqual(calls.at(-1), { city: 0, players: [[a.vip, 0, 2]] });
  await roam(b, 2, 0, [1, 0, 0], t1);   // the next message after reportMs reports
  await settle();
  assert.deepEqual(calls.at(-1), { city: 0, players: [[a.vip, 0, 2], [b.vip, 0, 2]].sort((x, y) => x[0] - y[0]) });
  assert.deepEqual(sw.stats().freeRoam, { total: 2, rooms: { 0: 2 } });
  // Ann drives to Atlanta: lan.js closes city 0's socket and opens city 1's
  city(0).webSocketClose(a.cs.ws);
  enter(a, 1, t1);
  await roam(a, 1, 1, [0, 0, 0], t1);
  await settle();
  assert.deepEqual(city(0).hub.linksOf(a.vip, ROAM_PORT), []);
  assert.equal(city(0).hub.count, 1);
  assert.deepEqual(sw.stats().freeRoam, { total: 2, rooms: { 0: 1, 1: 1 } }, "city 1's first report at once; city 0's still lists Ann (its next waits reportMs)");
  await roam(b, 2, 0, [1, 0, 0], t1 + CITY.reportMs);
  await settle();
  assert.deepEqual(sw.stats().freeRoam, { total: 2, rooms: { 0: 1, 1: 1 } });
  assert.deepEqual(sw.players().list.filter(e => e.mode === 'roam').map(e => e.city), [0, 1]);
  assert.equal(sw.activity().cities[1], 1);
  // the last socket of a city goes: reported at once, empty
  city(0).webSocketClose(b.cs.ws);
  await settle();
  assert.deepEqual(calls.at(-1), { city: 0, players: [] });
  assert.deepEqual(sw.stats().freeRoam, { total: 1, rooms: { 1: 1 } });
});

test('counts: reports from several cities add up with the fallback hub, once per vip; stale reports and gone pages drop', async () => {
  const { sw, page } = world();
  const now = Date.now();
  const ps = Array.from({ length: 5 }, (_, i) => page('P' + i));
  assert.ok(sw.cityReport({ city: 0, players: [[ps[0].vip, 0], [ps[1].vip, 0]] }, now));
  assert.ok(sw.cityReport({ city: 3, players: [[ps[2].vip, 3]] }, now));
  assert.ok(!sw.cityReport({ city: 99, players: [] }, now) && !sw.cityReport({ city: 1 }, now) && !sw.cityReport(null, now));
  await sw.webSocketMessage(ps[3].ws, Uint8Array.from(udp(ps[3].vip, HUB, [...hello(4, 'P3', 7, 1)])).buffer, now);
  await sw.webSocketMessage(ps[1].ws, Uint8Array.from(udp(ps[1].vip, HUB, [...hello(5, 'P1', 7, 0)])).buffer, now);   // both paths for a moment
  assert.deepEqual(sw.stats(now).freeRoam, { total: 4, rooms: { 0: 2, 1: 1, 3: 1 } });
  const res = await sw.fetch(new Request('https://switch/lan/city', { method: 'POST', body: JSON.stringify({ city: 2, players: [[ps[4].vip, 2]] }) }));
  assert.equal(res.status, 204);
  assert.equal(sw.players().modes.roam, 5);
  assert.equal(sw.activity().cities[2], 1);
  sw.drop(sw.clients.get(ps[0].vip));
  assert.deepEqual(sw.stats().freeRoam.rooms[0], 1, 'a page gone from the switch no longer counts');
  assert.deepEqual(sw.stats(now + CITY_TTL + 5000).freeRoam.rooms, { 0: 1, 1: 1 }, 'reports older than CITY_TTL drop; the own hub keeps its players');
  assert.equal((await sw.fetch(new Request('https://switch/lan/city', { method: 'POST', body: 'junk' }))).status, 400);
});

test('activity: roam status per player from city reports (3-element entries), unknown from old 2-element ones, public push unchanged', async () => {
  const { sw, page } = world();
  const now = Date.now();
  const ps = Array.from({ length: 4 }, (_, i) => page('P' + i));
  assert.ok(sw.cityReport({ city: 0, players: [[ps[0].vip, 0, 2], [ps[1].vip, 0, 4], [ps[2].vip, 0, 3], [ps[3].vip, 0]] }, now));
  const act = sw.activity();
  assert.deepEqual(act.roamStatus, { menus: 0, loading: 0, drive: 1, race: 1, garage: 1, paused: 0 });
  assert.deepEqual(act.list.map(e => e.roam), ['drive', 'garage', 'race', undefined]);
  assert.equal(act.activities.roam, 4);
  assert.ok(!('roam' in sw.players().list[0]) && !('roamStatus' in sw.stats()) && !('roamStatus' in sw.players()));
});

test('city: idle sockets close at the alarm; reports keep alive every keepMs, none while nothing changed and empty', async () => {
  const { city, page, enter, roam, calls } = world();
  const now = Date.now();
  const a = page('Ann');
  enter(a, 2, now);
  await roam(a, 1, 2, [0, 0, 0], now);
  await settle();
  const n = calls.length;
  await city(2).alarm(now + 1000);
  await settle();
  assert.equal(calls.length, n, 'unchanged: no report');
  await roam(a, 1, 2, [0, 0, 0], now + CITY.keepMs);
  await settle();
  assert.equal(calls.length, n + 1, 'keep-alive');
  await city(2).alarm(now + CITY.keepMs + CITY_IDLE_MS + 1);
  await settle();
  assert.deepEqual(a.cs.ws.closed, [4000, 'idle']);
  assert.deepEqual(calls.at(-1), { city: 2, players: [] });
  assert.equal(city(2).socks.size, 0);
});

test('city: after an eviction the sockets come back from their attachments, the hub from the next HELLO', async () => {
  const { env, page } = world();
  const ctx = fakeCtx(), c1 = new RoamCity(ctx, env), a = page('Ann'), ws = new FakeWs();
  c1.accept(ws, a.vip, 1);
  const c2 = new RoamCity(ctx, env);
  assert.equal(c2.city, 1);
  assert.ok(c2.socks.has(a.vip));
  await c2.webSocketMessage(ws, Uint8Array.from(udp(a.vip, HUB, [...hello(1, 'Ann', 7, 1)])).buffer);
  assert.equal(c2.hub.count, 1);
});

test('web/lan.js: the HELLO picks the city socket, frames and signals go over it, a new city moves it, BYE closes it, fallback to the switch', async () => {
  const sockets = [];
  class WS {
    constructor(url) { this.url = url; this.readyState = 0; this.sent = []; sockets.push(this); }
    send(d) { this.sent.push(d); }
    close() { this.readyState = 3; }
    open() { this.readyState = 1; this.onopen?.(); }
    msg(m) { this.onmessage?.({ data: typeof m === 'string' ? m : JSON.stringify(m) }); }
    shut() { this.readyState = 3; this.onclose?.({ code: 1006 }); }
  }
  const win = { location: { protocol: 'https:', host: 'play.example', href: 'https://play.example/' }, localStorage: { getItem: () => 'cid-12345678', setItem() {} } };
  const g = { window: win, WebSocket: WS, location: win.location, localStorage: win.localStorage, URL, JSON, Math, Date, setTimeout: () => 0, clearTimeout() {}, setInterval: () => 0, clearInterval() {}, console };
  vm.runInNewContext(await readFile(new URL('../../lan.js', import.meta.url), 'utf8'), g);
  const link = win.mc3Lan('Ann', 'wss://lan.example/lan');
  const main = sockets[0];
  main.open();
  main.msg({ t: 'welcome', vip: vipAt(0), vipStr: '10.77.0.2', mac: '', roamCity: 1 });
  const vip = vipAt(0);
  const hi = room => Uint8Array.from(udp(vip, HUB, [...hello(9, 'Ann', 7, room)]));
  const tickets = () => main.sent.filter(s => s === '{"t":"ticket"}').length;
  main.sent.length = 0;
  assert.ok(link.roamSend(hi(2)));
  assert.equal(tickets(), 1);
  assert.equal(main.sent.filter(s => typeof s !== 'string').length, 1, 'until the city socket is open: the switch');
  main.msg({ t: 'ticket', vip, exp: 123, sig: 'ab' });
  const c2 = sockets[1];
  assert.equal(c2.url, `wss://lan.example/lan/roam?city=2&vip=${vip}&exp=123&sig=ab`);
  c2.open();
  assert.equal(link.city, 2);
  link.roamSend(hi(2));
  link.roamSend(Uint8Array.from(udp(vip, vipAt(1), [1])));
  link.roamMsg({ t: 'signal', to: vipAt(1), kind: 'ice', data: null, roam: 1 });
  assert.equal(c2.sent.length, 3);
  assert.equal(tickets(), 1, 'the same city: no new ticket');
  const msgs = [];
  link.onMsg(m => msgs.push(m));
  c2.msg({ t: 'signal', from: vipAt(1), kind: 'offer', data: { sdp: 'x' }, roam: 1 });
  assert.equal(msgs.at(-1).from, vipAt(1), "the city's signals reach web/rtc.js");
  // another city: the old socket closes, a new ticket
  link.roamSend(hi(3));
  assert.equal(c2.readyState, 3);
  assert.equal(link.city, null);
  assert.equal(tickets(), 2);
  main.msg({ t: 'ticket', vip, exp: 124, sig: 'cd' });
  const c3 = sockets[2];
  assert.match(c3.url, /city=3&/);
  c3.open();
  // BYE: sent, then closed
  link.roamSend(Uint8Array.from(udp(vip, HUB, [...dgram(8, 9, 0, [2])])));
  assert.equal(c3.sent.length, 1);
  assert.equal(c3.readyState, 3);
  assert.equal(link.city, null);
  // refused (never opened): the switch, and no new ticket before the backoff
  link.roamSend(hi(1));
  main.msg({ t: 'ticket', vip, exp: 125, sig: 'ef' });
  sockets[3].shut();
  const before = tickets();
  main.sent.length = 0;
  assert.ok(link.roamSend(hi(1)));
  assert.equal(tickets(), 0, 'backing off');
  assert.equal(main.sent.length, 1, 'through the switch');
  assert.ok(before >= 3);
  // a switch without city objects: never a ticket
  main.msg({ t: 'welcome', vip, vipStr: '10.77.0.2', mac: '' });
  main.sent.length = 0;
  link.roamSend(hi(0));
  assert.equal(tickets(), 0);
});
