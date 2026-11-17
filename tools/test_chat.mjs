// web/chat.js's ChatStore in node (docs/chat-plan.md section 4): no browser, no server. A stub localStorage (and one
// that throws, as a blocked one does). Checks the caps (session lines 200, 40 a thread, 30 threads, 60 friends), thread
// keys (a stranger by nick, a friend by fid; the stranger's thread moves to the friend's), blocks by name and fid,
// unread counts, renames, and that nothing breaks without storage.
//   node tools/test_chat.mjs
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';

const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
let data = {};
globalThis.localStorage = {
  getItem: k => (k in data ? data[k] : null),
  setItem: (k, v) => { data[k] = String(v); },
  removeItem: k => { delete data[k]; },
};
vm.runInThisContext(fs.readFileSync(path.join(here, '../web/chat.js'), 'utf8'), { filename: 'chat.js' });
const { ChatStore } = globalThis.MC3Chat;

let passed = 0, failed = 0;
function test(name, fn) {
  data = {};
  try { fn(); passed++; console.log(`ok   ${name}`); } catch (e) { failed++; console.log(`FAIL ${name}: ${e.stack || e}`); }
}
function eq(a, b, what = '') { if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error(`${what} ${JSON.stringify(a)} !== ${JSON.stringify(b)}`); }
function ok(v, what = '') { if (!v) throw new Error('expected true: ' + what); }
const FID = n => n.toString(16).padStart(16, '0');

test('session lines: 200 kept, newest last, ids grow', () => {
  const s = new ChatStore();
  for (let i = 0; i < 250; i++) s.line({ kind: 'c', name: 'A', text: 't' + i });
  eq(s.ring.length, 200); eq(s.ring[0].text, 't50'); eq(s.ring[199].text, 't249');
  ok(s.ring[199].id > s.ring[0].id);
  eq(data['mc3.chat.w'], undefined, 'lines are never stored');
});

test('whisper thread: 40 messages, text cleaned and cut to 140, unread counts in only', () => {
  const s = new ChatStore();
  for (let i = 0; i < 45; i++) s.whisper('Bravo', i % 2 ? 'out' : 'in', ' m  ' + i + ' ', i + 1);
  const t = s.threads['name:bravo'];
  eq(t.msgs.length, 40); eq(t.msgs[0].text, 'm 5'); eq(t.unread, 23);
  s.whisper('BRAVO', 'in', 'x'.repeat(300), 99);
  eq(s.threads['name:bravo'].msgs.at(-1).text.length, 140, 'same thread whatever the case');
  eq(s.unread(), 24);
  s.read('name:bravo'); eq(s.unread(), 0);
  eq(JSON.parse(data['mc3.chat.w'])['name:bravo'].msgs.length, 40, 'stored');
});

test('threads: 30 kept, the least recent go', () => {
  const s = new ChatStore();
  for (let i = 0; i < 35; i++) s.whisper('P' + i, 'in', 'hi', 1000 + i);
  eq(Object.keys(s.threads).length, 30);
  ok(!s.threads['name:p0'] && !s.threads['name:p4'] && s.threads['name:p5'] && s.threads['name:p34']);
  const l = s.list(); eq(l[0].name, 'P34', 'newest first among unread');
});

test('list: unread first, then newest', () => {
  const s = new ChatStore();
  s.whisper('Old', 'in', 'a', 1); s.whisper('New', 'out', 'b', 5); s.whisper('Mid', 'in', 'c', 3);
  s.read('name:mid');
  eq(s.list().map(x => x.name), ['Old', 'New', 'Mid']);
});

test('friends: thread keyed by fid, stranger thread moves over, renames keep it', () => {
  const s = new ChatStore();
  s.whisper('Bravo', 'in', 'before', 1);
  s.addAsk(7, 'Bravo');
  const f = s.addFriend(FID(1), 'Bravo');
  ok(f && f.fid === FID(1));
  eq(s.asks.length, 0, 'ask gone with the friendship');
  ok(!s.threads['name:bravo'] && s.threads[FID(1)].msgs[0].text === 'before');
  eq(s.keyOf('bravo'), FID(1));
  s.whisper('Bravo', 'out', 'after', 2);
  eq(s.threads[FID(1)].msgs.length, 2);
  ok(s.rename(FID(1), 'Bravo2'));
  eq(s.keyOf('Bravo2'), FID(1)); eq(s.threads[FID(1)].name, 'Bravo2');
  eq(s.addFriend('nothex', 'X'), null, 'bad fid refused');
  eq(JSON.parse(data['mc3.friends']).length, 1);
  s.removeFriend(FID(1)); eq(s.friends.length, 0);
});

test('friends: 60 kept, the oldest go; a friend added again keeps its place', () => {
  const s = new ChatStore();
  for (let i = 0; i < 65; i++) s.addFriend(FID(i), 'F' + i);
  eq(s.friends.length, 60); ok(!s.friendBy(null, FID(4)) && s.friendBy(null, FID(5)));
  s.addFriend(FID(10), 'F10b');
  eq(s.friends.length, 60); eq(s.friendBy(null, FID(10)).name, 'F10b');
});

test('block: by name (any case) and by fid; drops asks; unblock', () => {
  const s = new ChatStore();
  s.addFriend(FID(9), 'Zed');
  s.addAsk(3, 'Mean');
  s.block('mean');
  ok(s.blocked('MEAN')); eq(s.asks.length, 0);
  s.block('Zed');
  ok(s.blocked('Renamed', FID(9)), 'a blocked friend stays blocked by fid after a rename');
  ok(!s.blocked('Nice'));
  s.unblock('MEAN'); ok(!s.blocked('mean'));
  eq(JSON.parse(data['mc3.chat.block']).length, 1);
});

test('stored data comes back; damaged data is ignored', () => {
  let s = new ChatStore();
  s.addFriend(FID(2), 'Kept'); s.whisper('Kept', 'in', 'hello', 1); s.block('Bad');
  s = new ChatStore();
  ok(s.friendBy('kept')); eq(s.threads[FID(2)].msgs[0].text, 'hello'); ok(s.blocked('bad'));
  data['mc3.friends'] = '{"x":1}'; data['mc3.chat.w'] = '[1,2]'; data['mc3.chat.block'] = 'not json';
  s = new ChatStore();
  eq(s.friends, []); eq(s.threads, {}); eq(s.blocks, []);
});

test('no storage (it throws): everything works for the session', () => {
  const real = globalThis.localStorage;
  globalThis.localStorage = { getItem() { throw new Error('denied'); }, setItem() { throw new Error('denied'); } };
  try {
    const s = new ChatStore();
    s.whisper('A', 'in', 'x', 1); s.addFriend(FID(3), 'A'); s.block('B');
    eq(s.threads[FID(3)].msgs.length, 1); ok(s.blocked('b')); eq(s.unread(), 1);
  } finally { globalThis.localStorage = real; }
});

test('asks: one per name, 20 at most, counted as unread', () => {
  const s = new ChatStore();
  s.addAsk(1, 'A'); s.addAsk(2, 'a');
  eq(s.asks.length, 1); eq(s.asks[0].vip, 2);
  for (let i = 0; i < 25; i++) s.addAsk(i, 'N' + i);
  eq(s.asks.length, 20); eq(s.unread(), 20);
});

test('attach without a page: every incoming chat message is taken without throwing', () => {
  const fns = [], ons = [], link = { state: 'online', vip: 1, peers: [{ vip: 1, name: 'Me' }, { vip: 2, name: 'Bravo' }],
    onMsg: f => fns.push(f), on: f => { ons.push(f); f(link); }, chat: () => true };
  const logs = [];
  globalThis.MC3Chat.attach({ lan: link, playing: () => true, log: l => logs.push(l), settings: () => ({ on: true, city: true }) });
  for (const m of [
    { t: 'chat', op: 'say', ch: 'c', from: 2, name: 'Bravo', text: 'hi', at: 1 },
    { t: 'chat', op: 'say', ch: 'w', from: 2, name: 'Bravo', text: 'psst', at: 1 },
    { t: 'chat', op: 'ask', from: 2, name: 'Bravo', at: 1 },
    { t: 'chat', op: 'friend', vip: 2, name: 'Bravo', fid: FID(5) },
    { t: 'chat', op: 'seen', id: 1, on: [{ f: FID(5), vip: 2, name: 'Bravo', w: 'roam', city: 1 }] },
    { t: 'chat', op: 'done', id: 99, done: 'sent', n: 1 },
    { t: 'chat', op: 'say', ch: 'c', name: '<b>x</b>', text: 42 },
  ]) fns.forEach(f => f(m));
  ok(JSON.parse(data['mc3.friends'])[0].name === 'Bravo', 'friend stored');
  ok(logs.length && logs.every(l => !/hi|psst/.test(l)), 'log lines carry no text');
});

console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
