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

// --- pick (free roam): busiest first, at least ROAM.minFrames, within max channels and n new ones, skip()
const none = () => false;
assert.deepStrictEqual(mc3Rtc.pick({ 20: 30, 30: 5, 40: 18 }, [], 12, 2, none), [20, 40], 'two new a tick, busiest first');
assert.deepStrictEqual(mc3Rtc.pick({ 20: 30, 30: 5, 40: 18 }, [20], 2, 2, none), [40], 'max channels in all');
assert.deepStrictEqual(mc3Rtc.pick({ 20: 30 }, [20, 30], 2, 2, none), [], 'full');
assert.deepStrictEqual(mc3Rtc.pick({ 20: 1, 30: 2 }, [], 12, 2, none), [30], 'HELLO only (1 a second): no channel');
assert.deepStrictEqual(mc3Rtc.pick({ 20: 9, 30: 9, 40: 9 }, [], 12, 3, v => v == 30), [20, 40], 'skip: lower vip, backing off');
console.log('pick: ok');

// --- ICE servers from the switch: wss://host/lan -> https://host/lan/ice, asked once at the start
{
  const urls = [], logs = [], f0 = globalThis.fetch;
  globalThis.fetch = u => { urls.push(u); return Promise.resolve({ ok: true, json: () => ({ iceServers: [{ urls: ['stun:s'] }, { urls: ['turn:t'], username: 'u', credential: 'c' }] }) }); };
  const l = { url: 'wss://lan.example/lan', vip: 1, state: 'online', onMsg() {}, on() {}, sendMsg() {}, send() {} };
  const r = mc3Rtc(l);
  r.log = s => logs.push(s);
  globalThis.fetch = f0;
  assert.deepStrictEqual(urls, ['https://lan.example/lan/ice']);
  setTimeout(() => { assert.deepStrictEqual(logs, ['[rtc] ICE servers from the switch: STUN + TURN']); r.close(); console.log('ice: ok'); });
}

function frame(src, dst, sport, dport, text) {
  const d = Buffer.from(text), f = new Uint8Array(16 + d.length), v = new DataView(f.buffer);
  f[0] = 3; v.setUint32(4, src); v.setUint32(8, dst); v.setUint16(12, sport); v.setUint16(14, dport); f.set(d, 16);
  return f;
}
// a roam core datagram after the switch header: 'MC3R', version 1, type (1 HELLO, 3 SNAP, 8 BYE)
function roamFrame(dst, type) {
  const f = frame(100, dst, ROAM, ROAM, 'MC3R\x01\x00........');
  f[16 + 5] = type;
  return f;
}

// --- free roam cost caps with fake peer connections: at most ROAM.relayMax channels through TURN (the busiest stay),
// the rest through the server; there SNAPs to a peer outside the ROAM.near busiest go 1 in ROAM.thin.
function capTests() {
  const R = mc3Rtc.ROAM, saved = { perTick: R.perTick, statsMs: R.statsMs }, pcs = [];
  class FakePC {
    constructor() { this.connectionState = 'new'; this.signalingState = 'stable'; pcs.push(this); }
    createDataChannel() { return this.dc = { readyState: 'connecting', bufferedAmount: 0, sent: 0, send() { this.sent++; }, close() {} }; }
    createOffer() { return Promise.resolve({ type: 'offer', sdp: 'o' }); }
    createAnswer() { return Promise.resolve({ type: 'answer', sdp: 'a' }); }
    setLocalDescription(d) { this.localDescription = d; return Promise.resolve(); }
    setRemoteDescription() { return Promise.resolve(); }
    addIceCandidate() { return Promise.resolve(); }
    getStats() {   // every pair relayed: the selected pair's local candidate is a TURN one
      return Promise.resolve(new Map([['T', { type: 'transport', selectedCandidatePairId: 'P' }],
        ['P', { type: 'candidate-pair', localCandidateId: 'L', remoteCandidateId: 'Q' }], ['L', { candidateType: 'relay' }], ['Q', { candidateType: 'host' }]]));
    }
    close() { this.closed = true; }
  }
  const pc0 = globalThis.RTCPeerConnection;
  globalThis.RTCPeerConnection = FakePC;
  R.perTick = 32;
  const out = [], logs = [], msg = [];
  const l = { vip: 100, state: 'online', onMsg: f => msg.push(f), on() {}, sendMsg() {}, send: f => { out.push(new DataView(f.buffer).getUint32(8) + ':' + f[21]); return true; } };
  const r = mc3Rtc(l);
  r.log = s => logs.push(s);
  r.roamMax = 32;
  const peers = Array.from({ length: 12 }, (_, i) => 101 + i);   // 101 the busiest ... 112 the least busy
  const second = () => { peers.forEach((v, i) => { for (let n = 0; n < 30 - i; n++) r.roamSend(roamFrame(v, 3)); }); r.tick(); };
  const settle = () => new Promise(res => setTimeout(res, 0));
  const open = pc => { pc.dc.readyState = 'open'; pc.connectionState = 'connected'; pc.dc.onopen(); };
  second();                                  // ranks the peers, offers to all 12
  assert.strictEqual(pcs.length, 12, 'one offer per peer');
  pcs.forEach(open);
  return settle().then(() => {
    const direct = peers.filter(v => r.roamMode(v) == 'direct');
    assert.deepStrictEqual(direct, peers.slice(0, 8), 'the 8 busiest keep their TURN channel');
    assert.strictEqual(logs.filter(s => s.includes('via TURN beyond the 8 allowed')).length, 4, logs.join('|'));
    assert.ok(logs.includes('[rtc] free roam 0.0.0.108: via TURN (8 of 8)'), logs.join('|'));
    second();                                // capped peers back off: no new offer
    assert.strictEqual(pcs.length, 12, 'no retry while backing off');
    // a lower vip offers; its TURN channel is the least busy: closed, and its next offer refused
    const offer = () => msg.forEach(f => f({ t: 'signal', from: 50, kind: 'offer', data: { sdp: 'o' }, roam: 1 }));
    offer();
    return settle().then(() => { open(pcs[12]); return settle(); }).then(() => {
      assert.strictEqual(r.roamMode(50), 'relay', 'answerer side capped too');
      offer();
      assert.strictEqual(pcs.length, 13, 'offer refused while backing off');
      // thinning through the server: SNAPs to a far peer 1 in 4; HELLO, BYE, the hub's and near peers' never
      out.length = 0;
      for (let n = 0; n < 8; n++) r.roamSend(roamFrame(112, 3));
      assert.strictEqual(out.length, 2, 'far SNAP: 1 in 4');
      out.length = 0;
      for (let n = 0; n < 4; n++) { r.roamSend(roamFrame(112, 1)); r.roamSend(roamFrame(112, 8)); r.roamSend(roamFrame(0x0A4D00FD, 3)); }
      assert.strictEqual(out.length, 12, 'HELLO, BYE, hub: all');
      r.roamP2P = false;                     // a near peer through the server: not thinned
      out.length = 0;
      for (let n = 0; n < 8; n++) r.roamSend(roamFrame(101, 3));
      assert.strictEqual(out.length, 8, 'near SNAP through the server: all');
      r.roamP2P = true;
      R.statsMs = 0;
      r.tick();                              // forget the extra frames above, then one plain second
      logs.length = 0; second();
      assert.ok(/\[rtc\] free roam: 0 direct, 8 via TURN, 4 via the server \(thinned 4\); .*thinned out \d+/.test(logs.join('|')), logs.join('|'));
      r.close();
      Object.assign(R, saved);
      globalThis.RTCPeerConnection = pc0;
      console.log('relay cap + thinning: ok');
    });
  });
}

capTests().then(() => {
  if (!process.env.NDC) { console.log('loopback: skipped (set NDC)'); return; }
  loopback();
}).catch(e => { console.error(e); process.exit(1); });

function loopback() {
const poly = require(require.resolve('node-datachannel/polyfill', { paths: [process.env.NDC] }));
globalThis.RTCPeerConnection = poly.RTCPeerConnection;
// the switch: frames (source rewritten to the sender, as crates/lan does), signals except C's, room messages
const sw = { links: {}, frames: 0, bcasts: 0 }, got = [];
function page(vip) {
  const fns = [], msg = [];
  const l = { vip, vipStr: '10.77.0.' + vip, state: 'online', sent: 0, recv: 0, onframe: null,
    on: f => { fns.push(f); f(l); }, onMsg: f => msg.push(f), deliver: m => msg.forEach(f => f(m)),
    sendMsg: m => { if (m.t == 'signal' && m.to != 30 && vip != 30) setTimeout(() => sw.links[m.to].deliver({ t: 'signal', from: vip, kind: m.kind, data: m.data, roam: m.roam })); },
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
      setTimeout(roamLoop, 200);
    }, 300);
  }, 500);
})();

// Free roam: A (110) holds 1 channel at most and sends 20 frames a second to B (120), 5 to D (140): it opens B only
// (the busiest), sends B's frames over it, D's and the hub's through the switch; B writes A's vip as the source
// whatever the frame says; the channel closes once idle.
function roamLoop() {
  const HUB = 0x0A4D00FD;
  mc3Rtc.ROAM.idleMs = 1500;
  const PA = page(110), PB = page(120), PD = page(140);
  const xa = mc3Rtc(PA), xb = mc3Rtc(PB), xd = mc3Rtc(PD);
  const logs = []; xa.log = s => { logs.push(s); console.log('  A ' + s); }; xb.log = s => console.log('  B ' + s);
  xa.roamMax = 1;
  got.length = 0;
  let n = 0;
  const pump = setInterval(() => {
    xa.roamSend(frame(110, 120, ROAM, ROAM, 'snap-b'));
    if (n++ % 4 == 0) xa.roamSend(frame(110, 140, ROAM, ROAM, 'snap-d'));
  }, 50);
  const t1 = Date.now();
  (function wait() {
    if (xa.roamMode(120) != 'direct' || xb.roamMode(110) != 'direct') {
      if (Date.now() - t1 > 15000) { console.error('roam loopback: no channel in 15 s'); process.exit(1); }
      return setTimeout(wait, 50);
    }
    clearInterval(pump);
    assert.strictEqual(xa.roamMode(140), 'relay', 'roamMax 1: D stays on the switch');
    setTimeout(() => {
      got.length = 0; sw.frames = 0;
      const s0 = xa.roamSent, r0 = xa.roamRelay;
      xa.roamSend(frame(110, 120, ROAM, ROAM, 'p2p'));
      xa.roamSend(frame(110, 140, ROAM, ROAM, 'relay'));
      xa.roamSend(frame(110, HUB, ROAM, ROAM, 'hub'));
      xb.roamSend(frame(77, 110, ROAM, ROAM, 'spoof'));
      setTimeout(() => {
        const s = got.map(g => g.join(' ')).sort();
        assert.deepStrictEqual(s, ['110 120 spoof', '120 110 p2p', '140 110 relay'], s.join('|'));
        assert.strictEqual(xa.roamSent - s0, 1); assert.strictEqual(xa.roamRelay - r0, 1);
        assert.strictEqual(sw.frames, 2, 'D and the hub through the switch');
        assert.ok(logs.some(l => l.includes('free roam 0.0.0.120: direct (P2P)')), logs.join('|'));
        // idle: no frames for idleMs, the next ticks close it
        setTimeout(() => {
          assert.strictEqual(xa.roamMode(120), 'relay', 'idle channel closed');
          console.log('roam loopback: ok (' + (Date.now() - t1) + ' ms; A sent ' + xa.roamSent + ' over P2P, ' + xa.roamRelay + ' via the switch)');
          [xa, xb, xd].forEach(r => r.close());
          setTimeout(() => process.exit(0), 200);
        }, 3500);
      }, 300);
    }, 200);
  })();
}
}
