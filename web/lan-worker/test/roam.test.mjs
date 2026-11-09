// The free-roam hub (src/roam.js): the tests of web/server/crates/roam/src/lib.rs, ported. node --test test/
import test from 'node:test';
import assert from 'node:assert/strict';
import { Hub, crc32, decode, encodePeers, ROAM_PORT, MSG_BYE, MSG_PEERS, MSG_FAR, ST_ROAM } from '../src/roam.js';
import { vipAt } from '../../races-worker/src/net.js';
import { dgram, hello, snap } from './wire.mjs';

const vip = vipAt;
const same = (a, b) => assert.deepEqual(a, b);

test('wire', () => {
  assert.equal(crc32(Buffer.from('123456789')), 0xCBF43926);
  same(decode(hello(5, 'Ann', 7)), { k: 'hello', sender: 5, status: ST_ROAM, name: 'Ann', links: 7, room: 0 });
  same(decode(hello(5, 'Ann', 0)), { k: 'hello', sender: 5, status: ST_ROAM, name: 'Ann', links: 0, room: 0 });
  const s = snap(6, [1, 2, 3], [4, 0, 0]);
  same(decode(s), { k: 'snap', sender: 6, status: ST_ROAM, p: [1, 2, 3], v: [4, 0, 0], q: [0, 0, 0, 0] });
  s[20] ^= 1;
  assert.equal(decode(s), null);
  same(decode(dgram(MSG_BYE, 6, 0, [2])), { k: 'bye', sender: 6 });
  assert.equal(decode(dgram(MSG_BYE, 0, 0, [2])), null);                      // sender 0
  assert.equal(decode(hello(5, 'A\x01n', 0)), null);                          // a control character in the name
  const p = encodePeers(9, 1, [[0x0A4D0102, ROAM_PORT, 77]]);
  same([...p.subarray(0, 4)], [...Buffer.from('MC3R')]);
  assert.equal(p.length, 16 + 1 + 10 + 4);
  same([...p.subarray(17, 21)], [10, 77, 1, 2]);
  assert.equal(Buffer.from(p).readUInt32LE(p.length - 4), crc32(p.subarray(0, p.length - 4)));
  same(decode(p), { k: 'other' });
});

test('nearest K: 20 players on a line 10 m apart, K = 3', () => {
  const h = new Hub();
  for (let i = 0; i < 20; i++) {
    h.frame(vip(i), ROAM_PORT, hello(100 + i, `P${i}`, 3), 0);
    h.frame(vip(i), ROAM_PORT, snap(100 + i, [10 * i, 0, 0], [0, 0, 0]), 0);
  }
  const out = h.tick(10);
  assert.equal(out.length, 20);
  for (let i = 0; i < 20; i++) {
    const l = h.linksOf(vip(i), ROAM_PORT);
    assert.ok(l.length && l.length <= 3, `${i}: ${l}`);
    for (const [v, port] of l) assert.ok(h.linksOf(v, port).some(([x, y]) => x === vip(i) && y === ROAM_PORT), 'both ways');
    for (const j of [i - 1, i + 1].filter(j => j >= 0 && j < 20))
      assert.ok(l.some(([x]) => x === vip(j)), `${i} not linked with its neighbour ${j}`);
  }
  const [, , d] = out.find(o => o[0] === vip(10));
  assert.equal(d[16], h.linksOf(vip(10), ROAM_PORT).length);
  assert.match(h.listJson(), /"P10"/);
});

test('a reconnect moves the player to its new endpoint', () => {
  const h = new Hub();
  for (let i = 0; i < 2; i++) {
    h.frame(vip(i), ROAM_PORT, hello(100 + i, 'x', 1), 0);
    h.frame(vip(i), ROAM_PORT, snap(100 + i, [i, 0, 0], [0, 0, 0]), 0);
  }
  h.frame(vip(9), ROAM_PORT, hello(101, 'x', 1), 100);
  h.frame(vip(9), ROAM_PORT, snap(101, [1, 0, 0], [0, 0, 0]), 100);
  h.tick(100);
  same(h.linksOf(vip(0), ROAM_PORT), [[vip(9), ROAM_PORT]]);
  same(h.linksOf(vip(1), ROAM_PORT), []);
});

test('look-ahead and hold', () => {
  const h = new Hub();
  const k1 = (i, p, v) => { h.frame(vip(i), ROAM_PORT, hello(100 + i, 'x', 1), 0); h.frame(vip(i), ROAM_PORT, snap(100 + i, p, v), 0); };
  k1(0, [0, 0, 0], [30, 0, 0]);    // drives along +x at 30 m/s
  k1(1, [-60, 0, 0], [0, 0, 0]);   // 60 m behind it
  k1(2, [200, 0, 0], [0, 0, 0]);   // 200 m ahead: in 4 s 80 m off, its view point on it
  h.tick(0);
  same(h.linksOf(vip(0), ROAM_PORT), [[vip(2), ROAM_PORT]]);
  h.frame(vip(1), ROAM_PORT, snap(101, [1, 0, 0], [30, 0, 0]), 1000);
  h.frame(vip(0), ROAM_PORT, snap(100, [0, 0, 0], [30, 0, 0]), 1000);
  h.tick(1000);
  same(h.linksOf(vip(0), ROAM_PORT), [[vip(2), ROAM_PORT]]);   // the link holds for hold_ms
  for (const t of [6000, 6500]) for (let i = 0; i < 3; i++) h.frame(vip(i), ROAM_PORT, hello(100 + i, 'x', 1), t);
  h.frame(vip(1), ROAM_PORT, snap(101, [1, 0, 0], [30, 0, 0]), 6500);
  h.frame(vip(0), ROAM_PORT, snap(100, [0, 0, 0], [30, 0, 0]), 6500);
  h.frame(vip(2), ROAM_PORT, snap(102, [2000, 0, 0], [0, 0, 0]), 6500);
  h.tick(6500);
  same(h.linksOf(vip(0), ROAM_PORT), [[vip(1), ROAM_PORT]]);
});

test('far list: every far_ms the placed players not linked, nearest first, with name and place', () => {
  const h = new Hub();
  for (let i = 0; i < 4; i++) {
    h.frame(vip(i), ROAM_PORT, hello(100 + i, `P${i}`, 1), 0);
    h.frame(vip(i), ROAM_PORT, snap(100 + i, [100 * i, 0, 5], [0, 0, 0]), 0);
  }
  h.frame(vip(9), ROAM_PORT, hello(109, 'nowhere', 1), 0);   // no place yet: in nobody's list, gets none
  assert.ok(h.tick(500).every(o => o[2][5] === MSG_PEERS), 'not due yet');
  const far = h.tick(2000).filter(o => o[2][5] === MSG_FAR);
  assert.equal(far.length, 4);
  const d = Buffer.from(far.find(o => o[0] === vip(0))[2]);
  assert.equal(d.readUInt16LE(6), d.length);
  assert.equal(d.readUInt32LE(d.length - 4), crc32(d.subarray(0, d.length - 4)));
  same(h.linksOf(vip(0), ROAM_PORT), [[vip(1), ROAM_PORT]]);   // K = 1: 2 and 3 are far, nearest first
  assert.equal(d[16], 2);
  assert.equal(d.readUInt32LE(17), 102);
  assert.equal(d[21], ST_ROAM);
  assert.equal(d.readFloatLE(22), 200);
  assert.equal(d.readFloatLE(30), 5);
  same([...d.subarray(42, 45)], [2, 0x50, 0x32]);
  assert.equal(d.readUInt32LE(45), 103);
  same(decode(d), { k: 'other' });
  assert.ok(h.tick(2500).every(o => o[2][5] === MSG_PEERS), 'next one at 4000');
});

test('gone: BYE, leave, silence', () => {
  const h = new Hub();
  h.frame(vip(0), ROAM_PORT, hello(1, 'a', 0), 0);
  h.frame(vip(1), ROAM_PORT, hello(2, 'b', 0), 0);
  h.frame(vip(1), ROAM_PORT, dgram(MSG_BYE, 2, 0, [2]), 0);
  assert.equal(h.count, 1);
  h.leave(vip(0));
  assert.equal(h.count, 0);
  h.frame(vip(2), ROAM_PORT, hello(3, 'c', 0), 0);
  h.tick(20000);
  assert.equal(h.count, 0, 'silent players go after gone_ms');
});

test('rooms (MC3: cities): linked and far-listed within a room only; no room = 0', () => {
  const h = new Hub();
  [0, 1, 1, 0].forEach((room, i) => {
    h.frame(vip(i), ROAM_PORT, hello(100 + i, 'x', 7, room), 0);
    h.frame(vip(i), ROAM_PORT, snap(100 + i, [i, 0, 0], [0, 0, 0]), 0);
  });
  const out = h.tick(2000);
  same(h.linksOf(vip(0), ROAM_PORT), [[vip(3), ROAM_PORT]]);
  same(h.linksOf(vip(1), ROAM_PORT), [[vip(2), ROAM_PORT]]);
  assert.ok(out.filter(o => o[2][5] === MSG_FAR).every(o => o[2][16] === 0), 'nobody far in its own room');
  h.frame(vip(3), ROAM_PORT, hello(103, 'x', 7, 1), 2100);   // player 3 drives to Atlanta
  h.tick(2500);
  same(h.linksOf(vip(0), ROAM_PORT), []);
  same(h.linksOf(vip(3), ROAM_PORT), [[vip(1), ROAM_PORT], [vip(2), ROAM_PORT]]);
});
