// The Lobby Durable Object driven with fake hibernatable WebSockets (node --test test/). No Cloudflare runtime needed.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import worker, { Lobby, clean } from '../src/index.js';

class FakeWs {
  constructor() { this.out = []; this.closed = null; this.att = null; }
  send(s) { if (this.closed) throw new Error('closed'); this.out.push(JSON.parse(s)); }
  close(code, why) { this.closed = [code, why]; }
  serializeAttachment(a) { this.att = JSON.stringify(a); }
  deserializeAttachment() { return this.att && JSON.parse(this.att); }
  take() { const o = this.out; this.out = []; return o; }
  last(t) { return this.out.filter(m => m.t === t).at(-1); }
}
function fakeCtx() {
  const ctx = { sockets: [], alarm: null, beats: new Map() };
  ctx.acceptWebSocket = ws => ctx.sockets.push(ws);
  ctx.getWebSockets = () => ctx.sockets.filter(ws => !ws.closed);
  ctx.setWebSocketAutoResponse = () => {};
  ctx.getWebSocketAutoResponseTimestamp = ws => ctx.beats.get(ws) || null;
  ctx.storage = { getAlarm: async () => ctx.alarm, setAlarm: async t => { ctx.alarm = t; } };
  return ctx;
}
function setup() {
  const ctx = fakeCtx(), lobby = new Lobby(ctx, {});
  const join = async (name, ip = '1.1.1.1') => {
    const ws = new FakeWs();
    lobby.connect(ws, ip);
    const vip = ws.out[0].vip;
    await lobby.webSocketMessage(ws, JSON.stringify({ t: 'hello', name }));
    return { ws, vip, say: m => lobby.webSocketMessage(ws, JSON.stringify(m)) };
  };
  return { ctx, lobby, join };
}

test('welcome gives a 10.77.0.0/18 address', async () => {
  const { join } = setup();
  const a = await join('Ann'), b = await join('Bob');
  const w = a.ws.out[0];
  assert.equal(w.t, 'welcome');
  assert.equal(w.vipStr, '10.77.0.2');
  assert.equal(w.mac, '00:50:F2:4D:00:02');
  assert.equal(b.ws.out[0].vipStr, '10.77.1.2');   // crates/lan vip_at: across the /24s first
});

test('create, list, join, leave, host leaves closes the room', async () => {
  const { join } = setup();
  const a = await join('Ann'), b = await join('Bob'), w = await join('Watcher');
  await w.say({ t: 'rooms', sub: 1 });
  assert.deepEqual(w.ws.last('rooms').rooms, []);
  await a.say({ t: 'create', name: '', city: 'Atlanta', mode: 'Circuit', max: 4 });
  const r = a.ws.last('room');
  assert.equal(r.room.name, "Ann's race");
  assert.deepEqual(r.members, [{ vip: a.vip, vipStr: r.members[0].vipStr, name: 'Ann', host: true }]);
  const listed = w.ws.last('rooms').rooms;
  assert.deepEqual(listed, [{ id: r.room.id, name: "Ann's race", host: 'Ann', hostVip: a.vip, city: 'Atlanta', mode: 'Circuit', n: 1, max: 4, locked: false }]);
  assert.match(r.room.id, /^[2-9a-hjkmnp-z]{6}$/);

  await b.say({ t: 'join', room: r.room.id });
  assert.deepEqual(a.ws.last('room').members.map(m => [m.name, m.host]), [['Ann', true], ['Bob', false]]);
  assert.equal(b.ws.last('room').room.n, 2);
  assert.equal(w.ws.last('rooms').rooms[0].n, 2);

  await b.say({ t: 'leave' });
  assert.deepEqual(b.ws.last('left'), { t: 'left', room: r.room.id, why: 'leave' });
  assert.equal(a.ws.last('room').members.length, 1);

  await b.say({ t: 'join', room: r.room.id });
  await a.say({ t: 'leave' });
  assert.deepEqual(b.ws.last('left'), { t: 'left', room: r.room.id, why: 'closed' });
  assert.deepEqual(w.ws.last('rooms').rooms, []);
  await b.say({ t: 'join', room: r.room.id });
  assert.deepEqual(b.ws.last('error'), { t: 'error', op: 'join', code: 'gone' });

  await w.say({ t: 'rooms', sub: 0 });
  w.ws.take();
  await a.say({ t: 'create', name: 'x' });
  assert.equal(w.ws.out.length, 0);   // unsubscribed
});

test('password: verifier only, wrong/missing refused, right accepted', async () => {
  const { join } = setup();
  const a = await join('Ann'), b = await join('Bob');
  await a.say({ t: 'create', name: 'Locked', pass: 'hunter2' });
  const id = a.ws.last('room').room.id;
  assert.equal(a.ws.last('room').room.locked, true);
  assert.ok(!a.ws.att.includes('hunter2'));                    // only salt + verifier kept
  assert.match(JSON.parse(a.ws.att).host.ver, /^[0-9a-f]{40}$/);
  await b.say({ t: 'join', room: id });
  assert.deepEqual(b.ws.last('error'), { t: 'error', op: 'join', code: 'password' });
  await b.say({ t: 'join', room: id, pass: 'hunter3' });
  assert.equal(b.ws.last('error').code, 'password');
  assert.equal(b.ws.last('room'), undefined);
  await b.say({ t: 'join', room: id, pass: 'hunter2' });
  assert.equal(b.ws.last('room').room.n, 2);
  for (const ws of [a.ws, b.ws]) assert.ok(!JSON.stringify(ws.out).includes('hunter'));
});

test('limits: full room, rooms per IP', async () => {
  const { join } = setup();
  const h = await join('Host', '9.9.9.9');
  await h.say({ t: 'create', name: 'Duel', max: 2 });
  const id = h.ws.last('room').room.id;
  const b = await join('B'), c = await join('C');
  await b.say({ t: 'join', room: id });
  await c.say({ t: 'join', room: id });
  assert.deepEqual(c.ws.last('error'), { t: 'error', op: 'join', code: 'full' });

  const x = await join('X', '5.5.5.5'), y = await join('Y', '5.5.5.5'), z = await join('Z', '5.5.5.5');
  await x.say({ t: 'create', name: '1' });
  await y.say({ t: 'create', name: '2' });
  await z.say({ t: 'create', name: '3' });
  assert.deepEqual(z.ws.last('error'), { t: 'error', op: 'create', code: 'limit' });
  await x.say({ t: 'create', name: '1b' });                 // replacing its own room is fine
  assert.equal(x.ws.last('error'), undefined);
  assert.equal(x.ws.last('room').room.name, '1b');
});

test('signal relayed only within a room', async () => {
  const { join } = setup();
  const a = await join('Ann'), b = await join('Bob'), o = await join('Out');
  await a.say({ t: 'signal', to: b.vip, kind: 'offer', data: { sdp: 'v=0' } });
  assert.deepEqual(a.ws.last('error'), { t: 'error', op: 'signal', code: 'room' });
  await a.say({ t: 'create', name: 'r' });
  await b.say({ t: 'join', room: a.ws.last('room').room.id });
  await a.say({ t: 'signal', to: b.vip, kind: 'offer', data: { sdp: 'v=0' } });
  assert.deepEqual(b.ws.last('signal'), { t: 'signal', from: a.vip, kind: 'offer', data: { sdp: 'v=0' } });
  await b.say({ t: 'signal', to: a.vip, kind: 'ice', data: null });
  assert.deepEqual(a.ws.last('signal'), { t: 'signal', from: b.vip, kind: 'ice', data: null });
  a.ws.take();
  await a.say({ t: 'signal', to: o.vip, kind: 'offer', data: {} });
  assert.equal(o.ws.last('signal'), undefined);
  assert.equal(a.ws.last('error').code, 'room');
  await a.say({ t: 'signal', to: b.vip, kind: 'offer', data: { sdp: 'x'.repeat(17000) } });
  assert.equal(a.ws.last('error').code, 'big');
});

test('expiry: silent clients dropped by the alarm, beats keep them', async () => {
  const { ctx, lobby, join } = setup();
  const a = await join('Ann'), b = await join('Bob'), c = await join('Cy');
  assert.equal(ctx.alarm, null);   // menu clients only: no alarm, the object may hibernate
  await a.say({ t: 'create', name: 'r' });
  assert.ok(ctx.alarm);            // a room: the sweep alarm runs
  await b.say({ t: 'join', room: a.ws.last('room').room.id });
  const now = Date.now() + 130e3;
  ctx.beats.set(b.ws, new Date(now - 10e3));   // b's beats were auto-answered
  ctx.beats.set(c.ws, new Date(now - 10e3));
  ctx.alarm = null;                // fired
  await lobby.alarm(now);
  assert.deepEqual(a.ws.closed, [4000, 'idle']);
  assert.deepEqual(b.ws.last('left'), { t: 'left', room: a.ws.last('room').room.id, why: 'closed' });
  assert.equal(b.ws.closed, null);
  assert.equal(lobby.clients.size, 2);
  assert.equal(ctx.alarm, null);   // the room went: not re-armed
});

test('beats: the page sends the exact text the runtime auto-answers', async () => {
  const pairs = [];
  globalThis.WebSocketRequestResponsePair = class { constructor(req, res) { this.req = req; this.res = res; } };
  try {
    const ctx = fakeCtx();
    ctx.setWebSocketAutoResponse = p => pairs.push(p);
    new Lobby(ctx, {});
  } finally { delete globalThis.WebSocketRequestResponsePair; }
  assert.deepEqual(pairs.map(p => [p.req, p.res]), [['{"t":"beat"}', '{"t":"beat"}']]);
  const page = await readFile(new URL('../../lan.js', import.meta.url), 'utf8');
  assert.ok(page.includes(`var BEAT = '{"t":"beat"}';`), 'web/lan.js BEAT must match the auto-response');
});

test('lazy sweep: a wake drops clients silent past IDLE_MS (no alarm needed)', async () => {
  const { ctx, lobby, join } = setup();
  const a = await join('Ann'), b = await join('Bob');
  const now = Date.now() + 130e3;
  ctx.beats.set(b.ws, new Date(now - 5e3));   // b only beats (auto-answered, never delivered)
  lobby.swept = 0;
  lobby.lazySweep(now);
  assert.deepEqual(a.ws.closed, [4000, 'idle']);
  assert.equal(b.ws.closed, null);
  assert.equal(lobby.clients.size, 1);
  lobby.clients.get(b.vip).a.last = 0; ctx.beats.delete(b.ws);
  lobby.lazySweep(now + 1000);   // within SWEEP_MS of the last sweep: skipped
  assert.equal(lobby.clients.size, 1);
  assert.equal(ctx.alarm, null);
});

test('socket close drops the client; state survives hibernation', async () => {
  const { ctx, lobby, join } = setup();
  const a = await join('Ann'), b = await join('Bob'), w = await join('W');
  await a.say({ t: 'create', name: 'Night', pass: 'pw' });
  const id = a.ws.last('room').room.id;
  await b.say({ t: 'join', room: id, pass: 'pw' });
  await w.say({ t: 'rooms', sub: 1 });
  const before = w.ws.last('rooms');

  const woke = new Lobby(ctx, {});                          // evicted and rebuilt from the attachments
  w.ws.take();
  await woke.webSocketMessage(w.ws, '{"t":"rooms","sub":1}');
  assert.deepEqual(w.ws.last('rooms'), before);
  const c = new FakeWs();
  woke.connect(c, '2.2.2.2');
  assert.ok(![a.vip, b.vip, w.vip].includes(c.out[0].vip));  // no vip handed out twice
  await woke.webSocketMessage(c, '{"t":"join","room":"' + id + '","pass":"pw"}');
  assert.equal(c.last('room').room.n, 3);

  woke.webSocketClose(b.ws, 1001, '', true);
  assert.equal(a.ws.last('room').members.length, 2);
  woke.webSocketClose(a.ws, 1001, '', true);
  assert.equal(c.last('left').why, 'closed');
  assert.deepEqual(w.ws.last('rooms').rooms, []);
});

test('worker: origin allow-list and path', async () => {
  const env = { ALLOW_ORIGINS: 'https://play.example, https://b.example', LOBBY: { idFromName: n => n, get: () => ({ fetch: async () => new Response('lobby') }) } };
  const req = (path, origin) => new Request('https://races.example' + path, { headers: { Upgrade: 'websocket', ...(origin && { Origin: origin }) } });
  assert.equal((await worker.fetch(req('/races', 'https://evil.example'), env)).status, 403);
  assert.equal((await worker.fetch(req('/races'), env)).status, 403);
  assert.equal((await worker.fetch(req('/x', 'https://play.example'), env)).status, 404);
  assert.equal(await (await worker.fetch(req('/races', 'https://b.example'), env)).text(), 'lobby');
});

test('names cleaned like crates/lan', () => {
  assert.equal(clean(' Bo\u0007b‮ ', 32, 'Player'), 'Bob');
  assert.equal(clean('', 32, 'Player'), 'Player');
  assert.equal(clean('x'.repeat(40), 32).length, 32);
});
