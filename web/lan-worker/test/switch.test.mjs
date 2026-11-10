// The Switch Durable Object driven with fake hibernatable WebSockets: the tests of web/server/crates/lan/src/lib.rs
// (routing, resume, ping, rate limit, hub, rooms) ported. No Cloudflare runtime needed. node --test test/
import test from 'node:test';
import assert from 'node:assert/strict';
import worker from '../src/index.js';
import { Switch, originOk, isVip, BROADCAST, HDR, MAX_DATA, CTL, GUARD, CLOSE } from '../src/switch.js';
import { vipAt, vipStr, ADDRS, MASK, NET } from '../../races-worker/src/net.js';
import { HUB, ROAM_PORT } from '../src/roam.js';
import { hello, snap } from './wire.mjs';

class FakeWs {
  constructor() { this.out = []; this.closed = null; this.att = null; }
  send(s) {
    if (this.closed) throw new Error('closed');
    this.out.push(typeof s === 'string' ? JSON.parse(s) : Uint8Array.from(s));
  }
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
function setup(rate) {
  const ctx = fakeCtx(), sw = new Switch(ctx, {}, rate);
  const peer = (want = 0, ip = '1.1.1.1') => {
    const ws = new FakeWs();
    if (!sw.connect(ws, ip, Date.now(), want)) return null;
    const w = ws.take()[0];
    assert.equal(w.t, 'welcome');
    const p = { ws, vip: w.vip, w };
    p.say = (m, now) => sw.webSocketMessage(ws, typeof m === 'string' ? m : JSON.stringify(m), now);
    p.frame = (f, now) => sw.webSocketMessage(ws, Uint8Array.from(f).buffer, now);
    return p;
  };
  const named = async (name, lan = 1, ip) => { const p = peer(0, ip); await p.say({ t: 'hello', name, lan }); p.ws.take(); return p; };
  return { ctx, sw, peer, named };
}
const be32 = n => [n >>> 24, n >>> 16 & 255, n >>> 8 & 255, n & 255], be16 = n => [n >>> 8, n & 255];
const udp = (src, dst, data, sport = 1000, dport = 1000) => [3, 9, 9, 9, ...be32(src), ...be32(dst), ...be16(sport), ...be16(dport), ...data];
const bytes = s => [...Buffer.from(s)];
const last = (msgs, t) => msgs.filter(m => m.t === t).at(-1);

test('addresses', () => {
  assert.equal(vipStr(vipAt(0)), '10.77.0.2');
  assert.equal(vipStr(vipAt(1)), '10.77.1.2');
  assert.equal(vipStr(vipAt(64)), '10.77.0.3');
  assert.equal(vipStr(vipAt(ADDRS - 1)), '10.77.63.251');
  const all = new Set(Array.from({ length: ADDRS }, (_, i) => vipAt(i)));
  assert.equal(all.size, ADDRS);
  for (const v of all) assert.ok(isVip(v) && (v & MASK) >>> 0 === NET && v !== HUB && v !== 0x0A4D0001);
});

test('welcome, resume: the old address when free, never a reserved one', async () => {
  const { sw, peer } = setup();
  const a = peer();
  assert.deepEqual(a.w, { t: 'welcome', vip: a.vip, vipStr: '10.77.0.2', mask: 0xFFFFC000, mac: '00:50:F2:4D:00:02' });
  assert.notEqual(peer(a.vip).vip, a.vip, 'held: another address');
  sw.drop(sw.clients.get(a.vip));
  assert.equal(peer(a.vip).vip, a.vip, 'free again: the same one');
  for (const bad of [HUB, 0x0A4D0001, BROADCAST, 0x0A4E0002]) assert.notEqual(peer(bad).vip, bad);
});

test('routing: broadcast to System Link peers, unicast to the holder, sizes, leave', async () => {
  const { sw, peer } = setup();
  const a = peer(), b = peer(), c = peer();
  await a.say({ t: 'hello', name: 'Ann', lan: 1 });
  await b.say({ t: 'hello', name: 'Bob\u0007', lan: 1 });
  await c.say({ t: 'hello', name: 'Cy' });   // System Link shut
  const list = last(c.ws.texts(), 'peers').peers;
  assert.deepEqual(list.map(p => p.name), ['Ann', 'Bob', 'Cy']);
  assert.deepEqual(list[0], { vip: a.vip, vipStr: vipStr(a.vip), name: 'Ann' });
  a.ws.take(); b.ws.take();

  await a.frame(udp(0x01020304, BROADCAST, bytes('hi')));
  const got = b.ws.bins();
  const want = udp(a.vip, BROADCAST, bytes('hi')); want[1] = want[2] = want[3] = 0;
  assert.deepEqual([...got[0]], want);
  assert.equal(got.length, 1);
  assert.equal(c.ws.bins().length + a.ws.bins().length, 0);

  await c.say({ t: 'lan', on: 1 });
  await a.frame(udp(a.vip, BROADCAST, bytes('x')));
  assert.equal(c.ws.bins().length, 1);
  b.ws.take();

  await b.frame(udp(b.vip, c.vip, bytes('u')));
  const u = c.ws.bins()[0];
  assert.deepEqual([...u.subarray(4, 8)], be32(b.vip));
  assert.equal(Buffer.from(u.subarray(16)).toString(), 'u');
  assert.equal(a.ws.bins().length, 0);
  await b.frame(udp(b.vip, b.vip, bytes('u')));
  assert.equal(b.ws.bins().length, 0, 'not to oneself');

  await a.frame(udp(a.vip, b.vip, new Array(MAX_DATA).fill(0)));
  assert.equal(b.ws.bins().length, 1);
  await a.frame(udp(a.vip, b.vip, new Array(MAX_DATA + 1).fill(0)));
  assert.deepEqual(a.ws.closed, [1009, 'frame too long']);
  assert.ok(!sw.clients.has(a.vip));
  assert.deepEqual(last(c.ws.texts(), 'peers').peers.map(p => p.name), ['Bob', 'Cy']);
  await c.frame(new Array(HDR - 1).fill(3));
  assert.deepEqual(c.ws.closed, [1009, 'frame too short']);
});

test('ping, junk', async () => {
  const { peer } = setup();
  const a = peer();
  await a.say({ t: 'ping', id: 7 });
  await a.say('not json');
  assert.deepEqual(a.ws.take(), [{ t: 'pong', id: 7 }]);
});

test('rate limit: a token bucket per sender', async () => {
  const { peer } = setup({ framesPerS: 10, burstFrames: 5, bytesPerS: 1 << 20, burstBytes: 2 << 20 });
  const a = peer(), b = peer();
  const t0 = 1e6;
  for (let i = 0; i < 7; i++) await a.frame(udp(a.vip, b.vip, bytes('z')), t0);
  assert.equal(b.ws.bins().length, 5);
  for (let i = 0; i < 3; i++) await a.frame(udp(a.vip, b.vip, bytes('z')), t0 + 200);   // two frames' worth later
  assert.equal(b.ws.bins().length, 2);
});

test('rate limit: normal burst passes, flood dropped, sustained flood closes 1008', async () => {
  const { peer } = setup();
  const a = peer(), b = peer();
  let t = 1e6;
  for (let i = 0; i < 700; i++) await a.frame(udp(a.vip, b.vip, bytes('z')), t);   // a roam peak in one instant
  assert.equal(b.ws.bins().length, 700);
  for (let i = 0; i < 80; i++) await a.say({ t: 'ping', id: i }, t);   // a connect burst: subs + rtc.js signalling for 7 peers
  assert.equal(a.ws.texts().length, 80);
  for (let i = 0; i < 2 * CTL.burst; i++) await a.say({ t: 'ping', id: i }, t);   // JSON flood: the rest of the burst, then dropped
  assert.equal(a.ws.texts().length, CTL.burst - 80);
  assert.equal(a.ws.closed, null);
  for (let s = 0; s < 12 && !a.ws.closed; s++) { t += 1000; for (let i = 0; i < 5 * CTL.perS; i++) await a.say({ t: 'ping', id: i }, t); }
  assert.deepEqual(a.ws.closed, [1008, 'rate limit']);
  const c = peer();   // frame flood
  t += 1e6;
  for (let s = 0; s < 700 && !c.ws.closed; s++) { t += 20; for (let i = 0; i < 100; i++) await c.frame(udp(c.vip, b.vip, bytes("z")), t); }   // 5000 frames/s
  assert.deepEqual(c.ws.closed, [1008, 'rate limit']);
});

test('hub: HELLO + SNAP in, PEERS out as frames from 10.77.0.253:47210; {"t":"roam"}; leave', async () => {
  const { sw, peer } = setup();
  const ps = [peer(), peer()];
  let now = 1e6;
  for (const [i, p] of ps.entries())
    for (const d of [hello(10 + i, 'p' + i, 7), snap(10 + i, [i, 0, 0], [0, 0, 0])])
      await p.frame(udp(p.vip, HUB, [...d], ROAM_PORT, ROAM_PORT), now);
  assert.equal(sw.hub.count, 2);
  now += 500;
  await ps[1].say({ t: 'roam' }, now);   // any message ticks once tick_ms has passed
  const got = ps[0].ws.bins().at(-1);
  assert.deepEqual([...got.subarray(4, 8)], be32(HUB));
  assert.deepEqual([...got.subarray(12, 16)], [...be16(ROAM_PORT), ...be16(ROAM_PORT)]);
  assert.equal(got[16 + 16], 1);
  assert.deepEqual([...got.subarray(16 + 17, 16 + 21)], be32(ps[1].vip));
  assert.deepEqual(last(ps[1].ws.texts(), 'roam').players, [{ name: 'p0', status: 2 }, { name: 'p1', status: 2 }]);
  // no tick before tick_ms
  await ps[0].say({ t: 'ping', id: 1 }, now + 100);
  assert.equal(ps[0].ws.bins().length, 0);
  // the alarm ticks too
  await sw.alarm(now + 600);
  assert.equal(ps[0].ws.bins().filter(b => b[16 + 5] === 2).length, 1, 'one PEERS (a FAR list too: far_ms passed)');
  sw.drop(sw.clients.get(ps[0].vip));
  assert.equal(sw.hub.count, 1);
});

test('alarm only while someone plays: menu pages none, a hub player arms it', async () => {
  const { ctx, sw, named } = setup();
  const a = await named('Ann');
  await named('Bob');
  await a.say({ t: 'players', sub: 1 });
  assert.equal(ctx.alarm, null);   // menu clients (their beats are auto-answered): the object may hibernate
  await a.frame(udp(a.vip, HUB, [...hello(10, 'Ann', 7)], ROAM_PORT, ROAM_PORT));
  assert.equal(sw.hub.count, 1);
  await Promise.resolve();
  assert.ok(ctx.alarm);
});

test('rooms: a room is its own LAN; free roam crosses rooms; signal inside only', async () => {
  const { named } = setup();
  const a = await named('Ann'), b = await named('Bob'), c = await named('Cy'), d = await named('Di'), e = await named('Ed');
  await a.say({ t: 'create', name: 'Night run', pass: 's3cret', city: 'Atlanta', max: 4 });
  const id = last(a.ws.texts(), 'room').room.id;
  await b.say({ t: 'join', room: id });
  assert.equal(last(b.ws.texts(), 'error').code, 'password');
  await b.say({ t: 'join', room: id, pass: 's3cret' });
  assert.equal(last(b.ws.texts(), 'room').members.length, 2);
  await c.say({ t: 'create', name: '' });
  assert.equal(last(c.ws.texts(), 'room').room.name, "Cy's race");
  for (const p of [a, b, c, d, e]) p.ws.take();

  await a.frame(udp(a.vip, BROADCAST, bytes('a')));
  assert.equal(b.ws.bins().length, 1);
  await c.frame(udp(c.vip, BROADCAST, bytes('c')));
  await d.frame(udp(d.vip, BROADCAST, bytes('d')));
  assert.equal(e.ws.bins().length, 1);
  assert.equal(a.ws.bins().length + c.ws.bins().length, 0);
  await b.frame(udp(b.vip, a.vip, bytes('u')));
  assert.equal(a.ws.bins().length, 1);
  await a.frame(udp(a.vip, c.vip, bytes('u')));
  await d.frame(udp(d.vip, a.vip, bytes('u')));
  assert.equal(c.ws.bins().length + a.ws.bins().length, 0);
  await a.frame(udp(a.vip, c.vip, bytes('MC3R'), 1000, ROAM_PORT));
  assert.equal(c.ws.bins().length, 1, 'free roam goes through');

  await a.say({ t: 'signal', to: b.vip, kind: 'offer', data: { sdp: 'x' } });
  assert.deepEqual(last(b.ws.texts(), 'signal'), { t: 'signal', from: a.vip, kind: 'offer', data: { sdp: 'x' } });
  await a.say({ t: 'signal', to: c.vip, kind: 'ice', data: null });
  assert.equal(last(a.ws.texts(), 'error').code, 'room');

  // the host goes: the room closes, b lands on the open LAN with d and e
  await a.say({ t: 'leave' });
  assert.equal(last(b.ws.texts(), 'left').why, 'closed');
  await b.frame(udp(b.vip, BROADCAST, bytes('b')));
  assert.equal(d.ws.bins().length + e.ws.bins().length, 2);
});

test('origins', () => {
  const list = ['https://game.example', 'https://game2.example'];
  assert.ok(originOk([], null) && originOk([], 'http://x'));
  assert.ok(originOk(list, 'https://GAME2.example'));
  assert.ok(!originOk(list, 'https://evil.example') && !originOk(list, null));
});

test('worker: /lan only, Origin checked, then the one Switch', async () => {
  let got = null;
  const env = { ALLOW_ORIGINS: 'https://game.example', SWITCH: { idFromName: n => n, get: id => ({ fetch: r => (got = [id, r], new Response('ok')) }) } };
  const req = (path, origin, up = 'websocket') => new Request('https://lan.example' + path, { headers: { Origin: origin, Upgrade: up } });
  assert.equal((await worker.fetch(req('/races', 'https://game.example'), env)).status, 404);
  assert.equal((await worker.fetch(req('/lan', 'https://evil.example'), env)).status, 403);
  assert.equal((await worker.fetch(req('/lan', 'https://game.example', 'h2c'), env)).status, 426);
  assert.equal((await worker.fetch(req('/lan?vip=5', 'https://game.example'), env)).status, 200);
  assert.equal(got[0], 'switch-2');
});

test('stats: counts only, per room, CORS by the allowlist', async () => {
  const { sw, named } = setup();
  const a = await named('Ann'), b = await named('Bob'), c = await named('Cy');
  await sw.webSocketMessage(a.ws, Uint8Array.from(udp(0, HUB, hello(1, 'Ann', 0, 0), 1, ROAM_PORT)).buffer);
  await sw.webSocketMessage(b.ws, Uint8Array.from(udp(0, HUB, hello(2, 'Bob', 0, 2), 1, ROAM_PORT)).buffer);
  await sw.webSocketMessage(c.ws, Uint8Array.from(udp(0, HUB, hello(3, 'Cy', 0, 2), 1, ROAM_PORT)).buffer);
  const s = sw.stats();
  assert.deepEqual(s, { online: 3, freeRoam: { total: 3, rooms: { 0: 1, 2: 2 } }, races: { rooms: 0 } });
  assert.ok(!JSON.stringify(s).includes('Ann'));
  const env = { ALLOW_ORIGINS: 'https://game.example', SWITCH: { idFromName: n => n, get: () => ({ fetch: () => Response.json(s) }) } };
  const get = origin => worker.fetch(new Request('https://lan.example/lan/stats', origin ? { headers: { Origin: origin } } : {}), env);
  const ok = await get('https://game.example');
  assert.equal(ok.status, 200);
  assert.equal(ok.headers.get('Access-Control-Allow-Origin'), 'https://game.example');
  assert.deepEqual(await ok.json(), s);
  assert.equal((await get('https://evil.example')).status, 403);
  assert.equal((await get(null)).status, 200);   // same-origin: the browser sends no Origin
});

test('players: pushed to subscribers on change, at most every WHO_MS; modes, cities, nick cap, list cap', async () => {
  const { sw, named, peer } = setup();
  const a = await named('Ann'), b = await named('Bob\u0007 with a very long name');
  let now = Date.now() + 10_000;
  await a.say({ t: 'players', sub: 1 }, now);
  let got = last(a.ws.texts(), 'players');
  assert.equal(got.online, 2);
  assert.deepEqual(got.list, [{ name: 'Ann', mode: 'menu' }, { name: 'Bob with a very', mode: 'menu' }]);
  assert.equal(got.more, 0);
  assert.equal(last(b.ws.texts(), 'players'), undefined, 'not subscribed: nothing');
  // b drives free roam in room 2 and renames: one batched update after WHO_MS, none before
  await b.frame(udp(b.vip, HUB, [...hello(2, 'Bob', 0, 2)], ROAM_PORT, ROAM_PORT), now + 100);
  await b.say({ t: 'hello', name: 'Bo', lan: 1 }, now + 200);
  assert.equal(last(a.ws.texts(), 'players'), undefined, 'throttled');
  await b.say({ t: 'ping', id: 1 }, now += 2000);
  got = last(a.ws.texts(), 'players');
  assert.deepEqual(got.list, [{ name: 'Bo', mode: 'roam', city: 2 }, { name: 'Ann', mode: 'menu' }]);
  assert.deepEqual(got.modes, { roam: 1, race: 0, menu: 1 });
  assert.deepEqual(got.freeRoam, { total: 1, rooms: { 2: 1 } });
  // nothing changed: nothing sent
  await b.say({ t: 'ping', id: 2 }, now += 2000);
  assert.equal(last(a.ws.texts(), 'players'), undefined);
  // a race room
  await b.say({ t: 'create', name: 'Night' }, now += 2000);
  const c = await named('Cy');
  await c.say({ t: 'join', room: sw.rooms()[0].a.host.id }, now += 2000);
  got = last(a.ws.texts(), 'players');
  assert.deepEqual(got.list.find(e => e.name === 'Cy'), { name: 'Cy', mode: 'race', race: 'Night' });
  // a leave
  sw.drop(sw.clients.get(c.vip));
  await a.say({ t: 'ping', id: 3 }, now += 2000);
  assert.equal(last(a.ws.texts(), 'players').online, 2);
  // the cap: 250 online, 200 listed
  for (let i = 0; i < 248; i++) await named('p' + i);   // (no hello within helloMs would be kicked)
  await a.say({ t: 'ping', id: 4 }, now += 2000);
  got = last(a.ws.texts(), 'players');
  assert.equal(got.online, 250);
  assert.equal(got.list.length, 200);
  assert.equal(got.more, 50);
  // unsubscribe
  await a.say({ t: 'players', sub: 0 }, now += 2000);
  peer();
  await a.say({ t: 'ping', id: 5 }, now += 2000);
  assert.equal(last(a.ws.texts(), 'players'), undefined);
  clearTimeout(sw.whoTimer);
});

test('where: private. The push stays coarse; activity() has the detail; validated, no reply, free roam and race rooms win, old pages are menu', async () => {
  const { sw, named } = setup();
  const [a, b, c, d, e, f] = [await named('Ann'), await named('Bob'), await named('Cy'), await named('Di'), await named('Ed'), await named('Flo')];
  let now = Date.now() + 10_000;
  await a.say({ t: 'players', sub: 1 }, now);
  assert.deepEqual(sw.activity().activities, { roam: 0, race: 0, career: 0, arcade: 0, menu: 6, loading: 0, page: 0 }, 'never said: menu (old pages)');
  b.ws.take();
  for (const [p, w] of [[a, 'page'], [b, 'loading'], [c, 'career'], [d, 'arcade'], [e, 'bogus']]) await p.say({ t: 'where', w }, now);
  assert.deepEqual(b.ws.texts(), [], 'no reply to where');
  await a.say({ t: 'ping', id: 1 }, now += 2000);
  let got = last(a.ws.texts(), 'players');
  assert.deepEqual(got.modes, { roam: 0, race: 0, menu: 6 }, 'the push does not tell');
  assert.ok(got.list.every(p => p.mode === 'menu' && Object.keys(p).join() === 'name,mode'));
  assert.deepEqual(sw.activity().activities, { roam: 0, race: 0, career: 1, arcade: 1, menu: 2, loading: 1, page: 1 });
  assert.deepEqual(sw.stats(), { online: 6, freeRoam: { total: 0, rooms: {} }, races: { rooms: 0 } });
  // a bad value keeps the old one; a rename (hello) keeps it too
  await c.say({ t: 'where', w: 5 }, now);
  await c.say({ t: 'hello', name: 'Cyd', lan: 1 }, now);
  assert.equal(sw.clients.get(c.vip).a.w, 'career');
  // free roam beats it; so does a race room
  await d.frame(udp(d.vip, HUB, [...hello(1, 'Di', 0, 1)], ROAM_PORT, ROAM_PORT), now + 100);
  await e.say({ t: 'create', name: 'Night' }, now);
  await f.say({ t: 'join', room: sw.rooms()[0].a.host.id }, now);
  const act = sw.activity();
  assert.equal(act.online, 6);
  assert.deepEqual(act.activities, { roam: 1, race: 2, career: 1, arcade: 0, menu: 0, loading: 1, page: 1 });
  assert.deepEqual(act.cities, { 1: 1 });
  assert.deepEqual(act.list.find(p => p.name === 'Di'), { name: 'Di', activity: 'roam', city: 1 });
  assert.deepEqual(act.list.find(p => p.name === 'Bob'), { name: 'Bob', activity: 'loading' });
  // the choice survives the attachment (a hibernation wake rebuilds the clients from it)
  assert.equal(sw.clients.get(b.vip).ws.deserializeAttachment().w, 'loading');
  clearTimeout(sw.whoTimer);
});

test('logActivity: one line when the counts changed, at most every 60 s', async () => {
  const { sw, named } = setup();
  const a = await named('Ann'), logs = [], real = console.log;
  console.log = l => logs.push(l);
  try {
    let now = 1e6;
    sw.logActivity(now);
    assert.equal(logs.length, 1);
    assert.match(logs[0], /^activity online=1 {.*"menu":1.*} cities={}$/);
    await a.say({ t: 'where', w: 'career' }, now += 1000);   // changed, but inside the minute
    assert.equal(logs.length, 1);
    await a.say({ t: 'where', w: 'arcade' }, now += 60e3);
    assert.equal(logs.length, 2);
    assert.match(logs[1], /"arcade":1/);
    sw.logActivity(now += 61e3);   // a minute on, nothing changed
    assert.equal(logs.length, 2);
  } finally { console.log = real; }
  clearTimeout(sw.whoTimer);
});

test('GET /lan/activity: ADMIN_KEY (header or ?key=), 404 without or unset, no CORS, not cached', async () => {
  const { default: worker } = await import('../src/index.js');
  const sw = { activity: () => ({ online: 0 }) }, calls = [];
  const env = k => ({ ADMIN_KEY: k, SWITCH: { idFromName: n => n, get: () => ({ fetch: async r => (calls.push(new URL(r.url).pathname), Response.json({ online: 0 })) }) } });
  const get = (e, path, headers) => worker.fetch(new Request('https://x.test' + path, { headers }), e, undefined);
  const e = env('s3cret');
  assert.equal((await get(e, '/lan/activity')).status, 404);
  assert.equal((await get(e, '/lan/activity', { Authorization: 'Bearer nope' })).status, 404);
  assert.equal((await get(e, '/lan/activity?key=nope')).status, 404);
  assert.equal((await get(env(undefined), '/lan/activity?key=undefined')).status, 404);
  assert.equal((await get(env(''), '/lan/activity', { Authorization: 'Bearer ' })).status, 404);
  assert.equal(calls.length, 0, 'the object is never woken without the key');
  for (const r of [await get(e, '/lan/activity', { Authorization: 'Bearer s3cret' }), await get(e, '/lan/activity?key=s3cret', { Origin: 'https://evil.test' })]) {
    assert.equal(r.status, 200);
    assert.equal(r.headers.get('Cache-Control'), 'no-store');
    assert.equal(r.headers.get('Access-Control-Allow-Origin'), null);
    assert.deepEqual(await r.json(), { online: 0 });
  }
  assert.deepEqual(calls, ['/lan/activity', '/lan/activity']);
});

test('nicks: unique among live clients, any case, trimmed; refused keeps the old name; freed on close; Player shared', async () => {
  const { sw, named, peer } = setup();
  const a = await named('Ann');
  const b = peer();
  await b.say({ t: 'hello', name: '  aNN ', lan: 1 });
  assert.deepEqual(last(b.ws.texts(), 'error'), { t: 'error', op: 'hello', code: 'nick-taken' });
  assert.equal(sw.clients.get(b.vip).a.name, 'Player', 'kept its old name');
  assert.ok(sw.clients.get(b.vip).a.hi && sw.clients.get(b.vip).a.lan, 'still in System Link');
  await b.say({ t: 'hello', name: 'Bob', lan: 1 });
  assert.equal(last(b.ws.texts(), 'error'), undefined);
  await b.say({ t: 'hello', name: 'Ann', lan: 1 });   // a rename onto a held nick: refused, Bob stays
  assert.equal(last(b.ws.texts(), 'error').code, 'nick-taken');
  assert.equal(sw.clients.get(b.vip).a.name, 'Bob');
  await a.say({ t: 'hello', name: 'ANN', lan: 1 });   // its own nick in another case: fine
  assert.equal(last(a.ws.texts(), 'error'), undefined);
  assert.equal(sw.clients.get(a.vip).a.name, 'ANN');
  sw.drop(sw.clients.get(a.vip));
  await b.say({ t: 'hello', name: 'ann', lan: 1 });   // freed on close
  assert.equal(last(b.ws.texts(), 'error'), undefined);
  assert.equal(sw.clients.get(b.vip).a.name, 'ann');
  const c = peer(), d = peer();   // empty names fall back to 'Player', which many may hold
  await c.say({ t: 'hello', name: '', lan: 1 });
  await d.say({ t: 'hello', name: 'player', lan: 1 });
  assert.equal(last(c.ws.texts(), 'error'), undefined);
  assert.equal(last(d.ws.texts(), 'error'), undefined);
  clearTimeout(sw.whoTimer);
});

test('nicks: the same browser (cid) may hold its nick on another socket (a second tab keeps the game online); another cid is refused', async () => {
  const { sw, peer } = setup();
  const ghost = peer(), fresh = peer(), other = peer();
  await ghost.say({ t: 'hello', name: 'Ann', lan: 1, cid: 'abcdefgh1234' });
  await other.say({ t: 'hello', name: 'Ann', lan: 1, cid: 'zzzzzzzz9999' });
  assert.equal(last(other.ws.texts(), 'error').code, 'nick-taken');
  await fresh.say({ t: 'hello', name: 'ann', lan: 1, cid: 'abcdefgh1234' });
  assert.equal(last(fresh.ws.texts(), 'error'), undefined);
  assert.equal(sw.clients.get(fresh.vip).a.name, 'ann');
  assert.equal(sw.clients.has(ghost.vip), true, 'the older socket (maybe the game tab) stays');
  clearTimeout(sw.whoTimer);
});

test('guard: sockets per IP (IPv6 by /64), 429 beyond; other IPs and a freed slot are fine', async () => {
  const { sw, peer } = setup();
  const mine = Array.from({ length: GUARD.perIp }, () => peer(0, '9.9.9.9'));
  assert.equal(sw.admit('9.9.9.9'), false);
  assert.equal(sw.admit('9.9.9.8'), true);
  sw.drop(sw.clients.get(mine[0].vip));
  assert.equal(sw.admit('9.9.9.9', Date.now() + 1), true);
  for (let i = 0; i < GUARD.perIp; i++) peer(0, '2001:db8:1:2:' + i + '::1');
  assert.equal(sw.admit('2001:db8:1:2:ffff::1'), false, 'same /64');
  assert.equal(sw.admit('2001:db8:1:3::1'), true);
  assert.equal(sw.admit(''), true, 'no address (local run): no per-IP guard');
  clearTimeout(sw.whoTimer);
});

test('guard: reconnect churn from one IP cools down 60 s, then lets it in', () => {
  const { sw } = setup();
  let t = 5e6;
  for (let i = 0; i < GUARD.churnMax; i++) assert.equal(sw.admit('7.7.7.7', t += 100), true);
  assert.equal(sw.admit('7.7.7.7', t += 100), false);
  assert.equal(sw.admit('7.7.7.7', t + GUARD.coolMs - 500), false, 'still cooling');
  assert.equal(sw.admit('7.7.7.6', t), true, 'another IP is not touched');
  assert.equal(sw.admit('7.7.7.7', t + GUARD.coolMs + 200), true);
  sw.helloSweep(t + 10 * GUARD.churnMs);
  assert.equal(sw.churn.size, 0, 'cooled entries are forgotten');
});

test('guard: a third socket of one browser (cid) closes the oldest 4002; two are fine; no loop through the same cid', async () => {
  const { sw, peer } = setup();
  const cid = 'abcdefgh1234', t = Date.now();
  const mk = async (name, at) => { const p = peer(); sw.clients.get(p.vip).a.at = at; await p.say({ t: 'hello', name, lan: 1, cid }); return p; };
  const a = await mk('A1', t - 3000), b = await mk('A2', t - 2000);
  assert.equal(a.ws.closed, null);
  assert.equal(b.ws.closed, null);
  const c = await mk('A3', t - 1000);
  assert.deepEqual(a.ws.closed, [CLOSE.tabs, 'too many tabs']);
  assert.equal(b.ws.closed, null);
  assert.equal(c.ws.closed, null);
  assert.ok(!sw.clients.has(a.vip) && sw.clients.has(b.vip) && sw.clients.has(c.vip));
  await b.say({ t: 'hello', name: 'A2b', lan: 1, cid });   // a rename on a held cid evicts nobody
  assert.equal(c.ws.closed, null);
  const other = peer();   // another browser: untouched
  await other.say({ t: 'hello', name: 'Zed', lan: 1, cid: 'zzzzzzzz9999' });
  assert.equal(other.ws.closed, null);
  clearTimeout(sw.whoTimer);
});

test('guard: one cid saying hello over GUARD.churnMax times a minute: 4004, then cooled', async () => {
  const { sw, peer } = setup();
  let t = Date.now();
  for (let i = 0; i < GUARD.churnMax; i++) { const p = peer(); await p.say({ t: 'hello', name: 'n' + i, cid: 'churnchurn99' }, t += 10); assert.equal(p.ws.closed?.[0] === CLOSE.churn, false); sw.drop(sw.clients.get(p.vip)); }
  const p = peer();
  await p.say({ t: 'hello', name: 'again', cid: 'churnchurn99' }, t += 10);
  assert.deepEqual(p.ws.closed, [CLOSE.churn, 'slow down']);
  assert.ok(!sw.clients.has(p.vip));
  const q = peer();   // the cool-down holds for that cid only
  await q.say({ t: 'hello', name: 'other', cid: 'otherotherot' }, t += 10);
  assert.equal(q.ws.closed, null);
  clearTimeout(sw.whoTimer);
});

test('guard: no hello within helloMs closes 4003 at the next wake; a hello in time keeps the socket', async () => {
  const { sw, peer, named } = setup();
  const t = Date.now();
  const mute = peer(), good = await named('Ann');
  await good.say({ t: 'ping', id: 1 }, t + GUARD.helloMs - 500);
  assert.equal(mute.ws.closed, null, 'not yet');
  await good.say({ t: 'ping', id: 2 }, t + GUARD.helloMs + 1500);
  assert.deepEqual(mute.ws.closed, [CLOSE.hello, 'no hello']);
  assert.equal(good.ws.closed, null);
  assert.ok(!sw.clients.has(mute.vip) && sw.clients.has(good.vip));
  const late = peer();   // its first message is the hello, but too late
  await late.say({ t: 'hello', name: 'Late', lan: 1 }, t + 5 * GUARD.helloMs);
  assert.deepEqual(late.ws.closed, [CLOSE.hello, 'no hello']);
  clearTimeout(sw.whoTimer);
});

test('guard: the upgrade of a refused IP gets 429 with Retry-After (fetch)', async () => {
  const { sw, peer } = setup();
  for (let i = 0; i < GUARD.perIp; i++) peer(0, '5.5.5.5');
  const res = await sw.fetch(new Request('https://lan.example/lan', { headers: { Upgrade: 'websocket', 'CF-Connecting-IP': '5.5.5.5' } }));
  assert.equal(res.status, 429);
  assert.equal(res.headers.get('Retry-After'), '60');
});
