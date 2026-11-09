// web/rtc.js: the routing rule (plan) always; with node-datachannel (its WebRTC polyfill) also a loopback: three
// pages in one room on a fake switch, A and B direct, C on the relay (its signals are lost).
//   node web/test/rtc.test.js                       # plan only
//   NDC=<folder holding node_modules/node-datachannel> node web/test/rtc.test.js
'use strict';
const assert = require('assert');
const path = require('path');
const mc3Rtc = require('../rtc.js');
const B = 0xFFFFFFFF, ROAM = 47210;

// --- plan: [[vip, 'dc'|'ws'], ...]
const open = set => v => set.includes(v);
assert.deepStrictEqual(mc3Rtc.plan(20, 10, [20, 30], open([20])), [[20, 'dc']]);
assert.deepStrictEqual(mc3Rtc.plan(30, 10, [20, 30], open([20])), [[30, 'ws']]);
assert.deepStrictEqual(mc3Rtc.plan(99, 10, [20, 30], open([20])), [[99, 'ws']], 'not a member: switch');
assert.deepStrictEqual(mc3Rtc.plan(B, 10, [], open([])), [[B, 'ws']], 'no room: the switch broadcasts');
assert.deepStrictEqual(mc3Rtc.plan(B, 10, [20, 30], open([])), [[B, 'ws']], 'no channel: the switch broadcasts');
assert.deepStrictEqual(mc3Rtc.plan(B, 10, [10, 20, 30], open([20])), [[20, 'dc'], [30, 'ws']], 'mixed: split, no switch broadcast');
assert.deepStrictEqual(mc3Rtc.plan(B, 10, [20, 30], open([20, 30])), [[20, 'dc'], [30, 'dc']]);
console.log('plan: ok');

if (!process.env.NDC) { console.log('loopback: skipped (set NDC)'); return; }
const poly = require(require.resolve('node-datachannel/polyfill', { paths: [process.env.NDC] }));
globalThis.RTCPeerConnection = poly.RTCPeerConnection;

function frame(src, dst, sport, dport, text) {
  const d = Buffer.from(text), f = new Uint8Array(16 + d.length), v = new DataView(f.buffer);
  f[0] = 3; v.setUint32(4, src); v.setUint32(8, dst); v.setUint16(12, sport); v.setUint16(14, dport); f.set(d, 16);
  return f;
}
// the switch: frames (source rewritten to the sender, as crates/lan does), signals except C's, room messages
const sw = { links: {}, frames: 0, bcasts: 0 }, got = [];
function page(vip) {
  const fns = [], msg = [];
  const l = { vip, vipStr: '10.77.0.' + vip, state: 'online', sent: 0, recv: 0, onframe: null,
    on: f => { fns.push(f); f(l); }, onMsg: f => msg.push(f), deliver: m => msg.forEach(f => f(m)),
    sendMsg: m => { if (m.t == 'signal' && m.to != 30 && vip != 30) setTimeout(() => sw.links[m.to].deliver({ t: 'signal', from: vip, kind: m.kind, data: m.data })); },
    send: f => {
      const g = f.slice(), v = new DataView(g.buffer), dst = v.getUint32(8);
      v.setUint32(4, vip); sw.frames++; if (dst == B) sw.bcasts++;
      for (const k in sw.links) if (+k != vip && (dst == B || +k == dst)) sw.links[k].onframe(g);
      return true;
    } };
  l.onframe = f => { const v = new DataView(f.buffer, f.byteOffset); got.push([vip, v.getUint32(4), Buffer.from(f.subarray(16)).toString()]); };
  sw.links[vip] = l;
  return l;
}
const A = page(10), Bp = page(20), C = page(30), ra = mc3Rtc(A), rb = mc3Rtc(Bp), rc = mc3Rtc(C);
const members = [10, 20, 30].map(v => ({ vip: v, vipStr: '10.77.0.' + v, name: 'p' + v, host: v == 10 }));
for (const l of [A, Bp, C]) l.deliver({ t: 'room', room: { id: 'abcdef' }, members });

const t0 = Date.now();
(function wait() {
  if (!(ra.mode(20) == 'direct' && rb.mode(10) == 'direct')) {
    if (Date.now() - t0 > 15000) { console.error('loopback: no channel in 15 s'); process.exit(1); }
    return setTimeout(wait, 50);
  }
  assert.strictEqual(ra.mode(30), 'relay'); assert.strictEqual(rc.mode(10), 'relay');
  ra.send(frame(10, 20, 1000, 1000, 'uni-b'));              // channel
  ra.send(frame(10, 30, 1000, 1000, 'uni-c'));              // switch
  ra.send(frame(10, B, 1000, 1000, 'bcast'));               // B by channel, C by switch unicast, once each
  ra.send(frame(10, 20, ROAM, ROAM, 'roam'));               // free roam: switch
  rc.send(frame(30, B, 1000, 1000, 'c-bcast'));             // C has no channel: the switch's broadcast
  setTimeout(() => {
    const s = got.map(g => g.join(' ')).sort();
    assert.deepStrictEqual(s, ['10 30 c-bcast', '20 10 bcast', '20 10 roam', '20 10 uni-b', '20 30 c-bcast', '30 10 bcast', '30 10 uni-c'].sort(), s.join('|'));
    assert.strictEqual(ra.sent, 2, 'two frames over the channel');
    assert.strictEqual(rb.recv, 2);
    assert.strictEqual(sw.bcasts, 1, 'the switch broadcast only C\'s');
    assert.strictEqual(sw.frames, 4);
    // a channel frame's source is the channel's peer, whatever the frame says
    got.length = 0;
    rb.send(frame(77, 10, 1000, 1000, 'spoof'));
    setTimeout(() => {
      assert.deepStrictEqual(got, [[10, 20, 'spoof']]);
      console.log('loopback: ok (' + (Date.now() - t0) + ' ms)');
      [ra, rb, rc].forEach(r => r.close());
      setTimeout(() => process.exit(0), 200);
    }, 300);
  }, 500);
})();
