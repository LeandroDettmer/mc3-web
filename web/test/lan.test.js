// web/lan.js: reconnection.  A fake WebSocket and fake timers; the switch's side is played by hand.
//   node web/test/lan.test.js
'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');

let socks = [], timers = [], now = 1e6;
class FakeWS {
  constructor(url) { this.url = url; this.readyState = 0; this.sent = []; socks.push(this); }
  send(d) { assert.strictEqual(this.readyState, 1, 'send on an open socket only'); this.sent.push(d); }
  close() { if (this.readyState == 3) return; this.readyState = 3; this.onclose && this.onclose({}); }
  // the switch's side
  open() { this.readyState = 1; this.onopen(); }
  msg(m) { this.onmessage({ data: JSON.stringify(m) }); }
  bin(b) { this.onmessage({ data: new Uint8Array(b).buffer }); }
  drop() { this.readyState = 3; this.onclose({}); }   // the server, a proxy or the network cut it
  json() { return this.sent.filter(d => typeof d == 'string').map(d => JSON.parse(d)); }
}
const g = globalThis;
g.window = g;
g.location = { protocol: 'https:', host: 'mc3.example' };
g.WebSocket = FakeWS;
g.setTimeout = (f, ms) => { timers.push({ f, ms, at: now + ms, iv: false }); return timers.length; };
g.setInterval = (f, ms) => { timers.push({ f, ms, at: now + ms, iv: true }); return timers.length; };
g.clearTimeout = g.clearInterval = id => { if (timers[id - 1]) timers[id - 1].dead = true; };
Date.now = () => now;
function run(ms) {   // advance the clock, firing what falls due in order
  const end = now + ms;
  for (;;) {
    const t = timers.filter(t => !t.dead && t.at <= end).sort((a, b) => a.at - b.at)[0];
    if (!t) break;
    now = t.at;
    if (t.iv) t.at += t.ms; else t.dead = true;
    t.f();
  }
  now = end;
}
eval(fs.readFileSync(path.join(__dirname, '..', 'lan.js'), 'utf8'));

const A = 0x0A4D0102, B = 0x0A4D0203, HOST = 0x0A4D0505;
const link = mc3Lan('Ann');
const seen = [];
link.on(l => seen.push(l.state + ':' + l.vip));
const frames = [];
link.onframe = f => frames.push(f);   // web/net.js sets this once, at attach

// first connection: hello, rooms, a race room joined by hand (as a member, with a password)
let ws = socks[0];
assert.strictEqual(ws.url, 'wss://mc3.example/lan');
ws.open();
ws.msg({ t: 'welcome', vip: A, vipStr: '10.77.1.2', mac: 'x' });
assert.strictEqual(link.state, 'online');
assert.deepStrictEqual(ws.json().map(m => m.t), ['hello', 'rooms', 'players', 'beat']);   // players: the live list sub; beat: the auto-answered keepalive
link.join('r1', 'pw');
ws.msg({ t: 'room', room: { id: 'r1', hostVip: HOST }, members: [{ vip: HOST }, { vip: A }] });
link.rename('Annie');

// keepalive: a ping every 20 s
run(20000);
assert.strictEqual(ws.json().filter(m => m.t == 'ping').length, 1, 'ping after 20 s');
ws.msg({ t: 'pong', id: 1 });

// the cut: offline at once (rtc.js and net.js hear it), then connecting; nothing sent meanwhile
ws.drop();
assert.strictEqual(link.state, 'connecting');
assert.ok(seen.includes('offline:' + A), 'offline seen before the retry');
assert.strictEqual(link.room, null);
assert.strictEqual(link.send(new Uint8Array(20)), false);
run(700);
assert.strictEqual(socks.length, 1, 'no retry before 750 ms (1 s - 25%)');
run(600);
assert.strictEqual(socks.length, 2, 'retry within 1.25 s');
ws = socks[1];
assert.strictEqual(ws.url, 'wss://mc3.example/lan?vip=' + A, 'asks for its address back');

// the server is still down: backoff grows 2, 4, 8, 15, 15 s (+-25%)
const gaps = [];
for (let k = 0; k < 5; k++) {
  const t0 = now, n = socks.length;
  socks[n - 1].drop();
  while (socks.length == n) run(50);
  gaps.push(now - t0);
}
[2000, 4000, 8000, 15000, 15000].forEach((d, i) => assert.ok(gaps[i] >= d * 0.75 - 50 && gaps[i] <= d * 1.25 + 50, `gap ${i}: ${gaps[i]} vs ${d}`));

// back: the old address was taken, the switch gives B; hello with the new nick, rooms, the room again
ws = socks[socks.length - 1];
ws.open();
ws.msg({ t: 'welcome', vip: B, vipStr: '10.77.2.3', mac: 'y' });
assert.strictEqual(link.state, 'online');
assert.strictEqual(link.vip, B);
const sent = ws.json();
assert.deepStrictEqual(sent[0], { t: 'hello', name: 'Annie', lan: 1 });
assert.deepStrictEqual(sent[1], { t: 'rooms', sub: 1 });
assert.deepStrictEqual(sent[2], { t: 'players', sub: 1 });
assert.deepStrictEqual(sent[3], { t: 'join', room: 'r1', pass: 'pw' });
ws.msg({ t: 'room', room: { id: 'r1', hostVip: HOST }, members: [{ vip: HOST }, { vip: B }] });
assert.strictEqual(link.room.id, 'r1');
// the game's frames flow on the new socket, both ways, through the same onframe
assert.strictEqual(link.send(new Uint8Array(20)), true);
assert.strictEqual(ws.sent.filter(d => typeof d != 'string').length, 1);
ws.bin([3, 0, 0, 0]);
assert.strictEqual(frames.length, 1);
// the backoff starts over after a welcome
ws.drop();
const t1 = now;
while (socks[socks.length - 1] === ws) run(50);
assert.ok(now - t1 <= 1300, 'backoff reset: ' + (now - t1));

// the room went while we were away: the refused rejoin is quiet, and not tried again
ws = socks[socks.length - 1];
ws.open();
ws.msg({ t: 'welcome', vip: B, vipStr: '10.77.2.3', mac: 'y' });
assert.strictEqual(ws.json()[3].t, 'join');
ws.msg({ t: 'error', op: 'join', code: 'gone' });
assert.strictEqual(link.error, null, 'no error shown for an automatic rejoin');
ws.drop();
while (socks[socks.length - 1] === ws) run(50);
ws = socks[socks.length - 1];
ws.open();
assert.ok(!ws.json().some(m => m.t == 'join'), 'no rejoin after a refused one');

// a host does not rejoin (its leaving closed the room)
ws.msg({ t: 'welcome', vip: B, vipStr: '10.77.2.3', mac: 'y' });
link.create({ name: 'mine' });
ws.msg({ t: 'room', room: { id: 'r2', hostVip: B }, members: [{ vip: B }] });
ws.drop();
while (socks[socks.length - 1] === ws) run(50);
ws = socks[socks.length - 1];
ws.open();
assert.ok(!ws.json().some(m => m.t == 'join'), 'host: no rejoin');

// a dead link (no close event): no answer for 50 s after pongs were seen -> reconnect
ws.msg({ t: 'welcome', vip: B, vipStr: '10.77.2.3', mac: 'y' });
run(20000); ws.msg({ t: 'pong', id: 2 });
const n = socks.length;
run(80000);
assert.ok(socks.length > n, 'watchdog reconnects a silent link');

// a held nick: the switch refuses the hello; the page sees link.error (op hello) and renames
ws = socks[socks.length - 1];
ws.open();
ws.msg({ t: 'welcome', vip: B, vipStr: '10.77.2.3', mac: 'y' });
ws.msg({ t: 'error', op: 'hello', code: 'nick-taken' });
assert.deepStrictEqual(link.error, { t: 'error', op: 'hello', code: 'nick-taken' });
link.rename('Ann2');
assert.deepStrictEqual(ws.json().at(-1), { t: 'hello', name: 'Ann2', lan: 1 });

// close(): offline for good
link.close();
assert.strictEqual(link.state, 'offline');
const m = socks.length;
run(60000);
assert.strictEqual(socks.length, m, 'no retry after close()');
console.log('lan reconnect: ok');
