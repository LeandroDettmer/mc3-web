// Two clients against a running switch (the Worker under `wrangler dev`, or mc3-server): welcome, hello/peers, a
// System Link broadcast and a unicast, the hub's PEERS list, ?vip= resume. Skipped unless LAN_URL is set, e.g.
//   LAN_URL=ws://127.0.0.1:8787/lan [LAN_ORIGIN=https://game.example] node --test test/live.test.mjs
// Uses Node's own WebSocket (Node 22+). The vips are not assumed: other clients may be on the switch.
import test from 'node:test';
import assert from 'node:assert/strict';
import { hello, snap } from './wire.mjs';

const URL_ = process.env.LAN_URL, ORIGIN = process.env.LAN_ORIGIN;
const be32 = n => [n >>> 24, n >>> 16 & 255, n >>> 8 & 255, n & 255], be16 = n => [n >>> 8, n & 255];
const udp = (src, dst, data, sport = 1000, dport = 1000) => Uint8Array.from([3, 0, 0, 0, ...be32(src), ...be32(dst), ...be16(sport), ...be16(dport), ...data]);
const HUB = 0x0A4D00FD, ROAM = 47210;

function open(q = '') {
  const ws = new WebSocket(URL_ + q, ORIGIN ? { headers: { Origin: ORIGIN } } : undefined);
  ws.binaryType = 'arraybuffer';
  const msgs = [], waiters = [];
  ws.onmessage = e => { msgs.push(typeof e.data === 'string' ? JSON.parse(e.data) : new Uint8Array(e.data)); waiters.splice(0).forEach(w => w()); };
  const next = async (pred, ms = 5000) => {
    for (const t0 = Date.now(); Date.now() - t0 < ms;) {
      const i = msgs.findIndex(pred);
      if (i >= 0) return msgs.splice(i, 1)[0];
      await new Promise(r => { waiters.push(r); setTimeout(r, 100); });
    }
    throw new Error('timed out');
  };
  return next(m => m.t === 'welcome').then(w => ({ ws, next, vip: w.vip, welcome: w }));
}

test('two clients through a running switch', { skip: !URL_ && 'LAN_URL not set' }, async () => {
  const a = await open(), b = await open();
  assert.match(a.welcome.vipStr, /^10\.77\.\d+\.\d+$/);
  assert.equal(a.welcome.mask, 0xFFFFC000);
  a.ws.send(JSON.stringify({ t: 'hello', name: 'TestAnn', lan: 1 }));
  b.ws.send(JSON.stringify({ t: 'hello', name: 'TestBob', lan: 1 }));
  await a.next(m => m.t === 'peers' && m.peers.some(p => p.name === 'TestBob'));

  a.ws.send(udp(0, 0xFFFFFFFF, [1, 2, 3]));
  let f = await b.next(m => m instanceof Uint8Array && m[16] === 1);
  assert.deepEqual([...f.subarray(4, 8)], be32(a.vip));
  assert.deepEqual([...f.subarray(16)], [1, 2, 3]);
  b.ws.send(udp(b.vip, a.vip, [9]));
  f = await a.next(m => m instanceof Uint8Array && m[16] === 9);
  assert.deepEqual([...f.subarray(4, 8)], be32(b.vip));

  for (const [p, id, x] of [[a, 0x7E57A, 0], [b, 0x7E57B, 5]])
    for (const d of [hello(id, 'p', 7), snap(id, [x, 0, 0], [0, 0, 0])]) p.ws.send(udp(p.vip, HUB, [...d], ROAM, ROAM));
  const until = Date.now() + 3000;   // HELLO a second, as roam_core.c: the ticks run from incoming messages
  let list;
  while (!list && Date.now() < until) {
    a.ws.send(udp(a.vip, HUB, [...hello(0x7E57A, 'p', 7)], ROAM, ROAM));
    list = await a.next(m => m instanceof Uint8Array && m[16 + 5] === 2 && m[16 + 16] >= 1, 600).catch(() => null);
  }
  assert.ok(list, 'a PEERS list');
  assert.deepEqual([...list.subarray(4, 8)], be32(HUB));
  assert.deepEqual([...list.subarray(16 + 17, 16 + 21)], be32(b.vip));

  a.ws.send(JSON.stringify({ t: 'ping', id: 42 }));
  assert.equal((await a.next(m => m.t === 'pong')).id, 42);

  b.ws.close(1000, 'bye');
  await a.next(m => m.t === 'peers' && !m.peers.some(p => p.name === 'TestBob'));
  const b2 = await open('?vip=' + b.vip);
  assert.equal(b2.vip, b.vip, 'resume: the same address back');
  b2.ws.close(1000, 'bye');
  a.ws.close(1000, 'bye');
});
