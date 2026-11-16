// web/cloud.js (cloud saves under a keyphrase, docs/cloud-saves-plan.md) in node: no browser, no server, no game.
//   node tools/test_cloud_saves.mjs
// After nfsu2-web's test/vault.test.mjs (th0mk, MIT): a mock of the saves endpoint (plan 1.3: GET/PUT/DELETE, ETag
// compare-and-swap, the copy of an earlier day, 412/404/429) and two "browsers" with in-memory saves, fake clock and
// timers. Checks: the keyphrase, the crypto, the box, the sync decisions (take, conflict both/load/older/bare/thin/
// newer/empty/fewer, lost answer, gone), the snapshot before every take, overlay only, and the anti-spam rules
// (one automatic send per 5 min, unchanged boxes never sent, backoff, Retry-After, pagehide keepalive).
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { createHmac, pbkdf2Sync, randomBytes, createHash } from 'node:crypto';

const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
vm.runInThisContext(fs.readFileSync(path.join(here, '../web/cloud.js'), 'utf8'), { filename: 'cloud.js' });
const C = globalThis.MC3Cloud;

let passed = 0, failed = 0;
const results = [];
async function test(name, fn) {
  try { await fn(); passed++; results.push(`ok   ${name}`); }
  catch (e) { failed++; results.push(`FAIL ${name}: ${e.stack || e}`); }
}
function eq(a, b, what = '') { if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error(`${what} ${JSON.stringify(a)} !== ${JSON.stringify(b)}`); }
function ok(v, what = '') { if (!v) throw new Error('expected true: ' + what); }

// ------------------------------------------------------------------------------------------------- save files --
const TITLE = 'UDATA/54540079/';
const STAMP = Buffer.from([0x47, 0x94, 0xF5, 0x02]);
function career(seed, size = 99162) {               // stamp + body + HMAC-SHA1 under 16 zero bytes (web_fs.c sig_ok)
  const b = Buffer.alloc(size);
  STAMP.copy(b, 0);
  for (let i = 4; i < size - 20; i += 4096) b.writeUInt32LE((seed * 2654435761 + i) >>> 0, i);   // mostly empty, like a real profile
  createHmac('sha1', Buffer.alloc(16)).update(b.subarray(0, size - 20)).digest().copy(b, size - 20);
  return new Uint8Array(b);
}
function meta(name) { return new Uint8Array(Buffer.from('﻿Name=Remix : ' + name + '\r\n', 'utf16le')); }
const BODIES = [0, 4 + 0x18340, 8 + 0x18340, 12 + 0x18340, 16 + 0x18340];
function crc32(b) { let c = ~0; for (const x of b) { c ^= x; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; } return ~c >>> 0; }
function oprof(money, ver = 4) {
  const body = Buffer.alloc(BODIES[4]);
  body.writeInt32LE(money, 0);
  const h = Buffer.alloc(20);
  h.writeUInt32LE(0x4F33434D, 0); h.writeUInt32LE(ver, 4); h.writeUInt32LE(BODIES[4], 8); h.writeUInt32LE(crc32(body), 12); h.writeUInt32LE(3, 16);
  return new Uint8Array(Buffer.concat([h, body]));
}
const prof = (folder, name, seed) => ({ [`${TITLE}${folder}/${name}.mc3Remix`]: career(seed), [`${TITLE}${folder}/SaveMeta.xbx`]: meta(name) });

// ----------------------------------------------------------------------------------------------- the server --
function server(clock) {
  const store = new Map();         // sha256(token) -> { rev, saved, body, earlier: { rev, saved, body } | null }
  const s = { store, requests: [], fail: null, url: 'https://saves.test/v' };
  const day = t => Math.floor(t / 86400e3);
  const res = (status, body, h = {}) => new Response(status === 304 || status === 204 ? null : body ?? null, { status, headers: h });
  const heads = o => ({ etag: o.rev, 'x-vault-saved': String(o.saved), ...o.earlier ? { 'x-vault-earlier': String(o.earlier.saved) } : {} });
  s.fetch = async (url, init = {}) => {
    const method = init.method || 'GET', u = new URL(url), h = Object.fromEntries(Object.entries(init.headers || {}).map(([k, v]) => [k.toLowerCase(), v]));
    s.requests.push({ method, keepalive: !!init.keepalive, ifMatch: h['if-match'], len: init.body?.length || 0 });
    if (s.fail) { const f = s.fail; if (!f.sticky) s.fail = null; if (f.network) throw new TypeError('fetch failed'); return res(f.status, '', f.headers || {}); }
    const auth = /^Bearer ([A-Za-z0-9_-]{43})$/.exec(h.authorization || '');
    if (!auth) return res(401, '');
    const key = createHash('sha256').update(auth[1]).digest('hex'), o = store.get(key);
    if (method === 'GET') {
      if (!o) return res(404, '');
      if (u.searchParams.has('earlier')) return o.earlier ? res(200, o.earlier.body, { etag: o.earlier.rev, 'x-vault-saved': String(o.earlier.saved) }) : res(404, '');
      if (h['if-none-match'] === o.rev) return res(304, null, heads(o));
      return res(200, o.body, heads(o));
    }
    if (method === 'PUT') {
      if (!h['if-match'] && h['if-none-match'] !== '*') return res(428, '');
      if (h['if-none-match'] === '*' ? !!o : (!o || o.rev !== h['if-match'])) return res(412, '', o ? heads(o) : {});
      const now = clock.t, rev = '"' + randomBytes(8).toString('hex') + '"', body = new Uint8Array(init.body);
      const earlier = o ? (day(o.saved) < day(now) ? { rev: o.rev, saved: o.saved, body: o.body } : o.earlier) : null;
      const n = { rev, saved: now, body, earlier };
      store.set(key, n);
      return res(o ? 200 : 201, '', heads(n));
    }
    if (method === 'DELETE') { if (!o) return res(404, ''); store.delete(key); return res(204); }
    return res(405, '');
  };
  s.writes = () => s.requests.filter(r => r.method === 'PUT').length;
  return s;
}

// ---------------------------------------------------------------------------------------------- a browser --
function browser(srv, clock, files = {}) {
  const b = { files: new Map(), baks: new Map(), snaps: [], link: null, playing: false, logs: [], timers: [] };
  for (const [p, d] of Object.entries(files)) { b.files.set(p, d); b.baks.set(p, d); }
  b.deps = {
    url: srv.url, fetch: (u, i) => srv.fetch(u, i), now: () => clock.t,
    read: async () => ({ files: [...b.files].map(([path, data]) => ({ path, data, mtime: clock.t })), refs: Object.fromEntries(b.baks) }),
    apply: async list => {
      if (b.files.size) b.snaps.push(new Map(b.files));                       // index.html: a dated snapshot first
      for (const f of list) { b.files.set(f.path, f.data); b.baks.set(f.path, f.data); }   // overlay, never a delete
    },
    load: () => b.link, save: l => { b.link = l && JSON.parse(JSON.stringify(l)); },
    log: s => b.logs.push(s), playing: () => b.playing,
    setTimer: (fn, ms) => { const t = { at: clock.t + ms, fn }; b.timers.push(t); return t; },
    clearTimer: t => { b.timers = b.timers.filter(x => x !== t); },
  };
  b.sync = new C.Sync(b.deps);
  b.write = (p, d) => { b.files.set(p, d); b.baks.set(p, d); };          // the game's save (and its rotating copy)
  b.advance = async ms => {                                               // the clock on, timers run as they fall due
    const end = clock.t + ms;
    for (;;) {
      b.timers.sort((x, y) => x.at - y.at);
      const t = b.timers[0];
      if (!t || t.at > end) break;
      b.timers.shift(); clock.t = Math.max(clock.t, t.at);
      await t.fn(); await b.sync.queue;
    }
    clock.t = end;
  };
  return b;
}
function world(filesA = {}) {
  const clock = { t: Date.UTC(2026, 10, 16, 10) }, srv = server(clock);
  return { clock, srv, A: browser(srv, clock, filesA), mk: f => browser(srv, clock, f) };
}
const P1 = prof('AE1A4832EEF5', 'Player 1', 1), P2 = prof('ED509E14024E', 'ferez', 2);
const ON = { 'online/profile.bin': oprof(2000000) };

// --------------------------------------------------------------------------------------------------- tests --
await test('keyphrase: new ones read back in any case, separators, prefix optional', async () => {
  const p = C.newPhrase();
  ok(/^mc3(-[a-z]+){8}$/.test(p), p);
  eq(C.readPhrase(p).phrase, p);
  eq(C.readPhrase(p.toUpperCase().replace(/-/g, '  ')).phrase, p);
  eq(C.readPhrase(p.slice(4).replace(/-/g, '.')).phrase, p);
  ok(C.readPhrase('mc3-acid-acorn').bad, 'too short');
  ok(/zzzz/.test(C.readPhrase('mc3 zzzz acid acid acid acid acid acid acid').bad), 'unknown word');
  eq(C.WORDS.length, 1296);
});

await test('PBKDF2 key and token match node:crypto', async () => {
  const p = 'mc3-acid-acorn-acre-acts-afar-affix-aged-agent';
  const bits = pbkdf2Sync(p, 'mc3-web keyphrase saves v1', 100000, 64, 'sha256');
  const k = await C.keysOf(p);
  eq(k.token, bits.subarray(32).toString('base64url'));
  eq(k.token.length, 43);
});

await test('seal/unseal round trip; another keyphrase cannot open it; a newer box version asks for a reload', async () => {
  const a = await C.keysOf(C.newPhrase()), b = await C.keysOf(C.newPhrase());
  const files = [{ path: TITLE + 'AE1A4832EEF5/x.mc3Remix', data: career(5) }, { path: 'online/profile.bin', data: oprof(5) }];
  const box = await C.seal(a.key, await C.pack(files));
  const back = await C.unpack(await C.unseal(a.key, box));
  eq(await C.contentHash(back), await C.contentHash(files));
  let e = null; try { await C.unseal(b.key, box); } catch (x) { e = x; }
  eq(e?.code, 'damaged');
  const newer = box.slice(); newer[0] = 2;
  e = null; try { await C.unseal(a.key, newer); } catch (x) { e = x; }
  eq(e?.code, 'newer-box');
});

await test('box: only career folders and online/profile.bin go in; a real-sized folder packs small', async () => {
  const files = [...Object.entries({ ...P1, ...P2, ...ON }).map(([path, data]) => ({ path, data })),
    { path: 'backup/UDATA/x.bak1', data: new Uint8Array(9) }, { path: 'TDATA/54540079/opt', data: new Uint8Array(9) },
    { path: TITLE + 'AE1A4832EEF5/x.mc3Remix.tmp', data: new Uint8Array(9) }, { path: TITLE + '../evil', data: new Uint8Array(9) }];
  eq(files.filter(f => C.keeps(f.path)).length, 5);
  const k = await C.keysOf(C.newPhrase()), box = await C.seal(k.key, await C.pack(files.filter(f => C.keeps(f.path))));
  ok(box.length < C.MAX_BOX, `box ${box.length}`);
  results.push(`     (2 synthetic careers + online profile, mostly zeros: ${files.slice(0, 5).reduce((s, f) => s + f.data.length, 0)} B raw -> ${box.length} B sealed; real size: plan phase 0)`);
});

await test('content hash ignores times, sees bytes and paths', async () => {
  const f = [{ path: 'a', data: new Uint8Array([1]), mtime: 1 }], g = [{ path: 'a', data: new Uint8Array([1]), mtime: 2 }];
  eq(await C.contentHash(f), await C.contentHash(g));
  ok(await C.contentHash(f) !== await C.contentHash([{ path: 'a', data: new Uint8Array([2]) }]));
});

await test('checks: career against its copy (stamp, size, signature); profile.bin magic/version/CRC', async () => {
  const good = career(1), torn = good.slice(); torn[500] ^= 1;
  ok(await C.careerOk(good, good)); ok(!(await C.careerOk(torn, good)), 'unsigned while copy signed');
  ok(!(await C.careerOk(good.subarray(0, 1000), good)), 'shorter'); ok(!(await C.careerOk(new Uint8Array(0), null)), 'empty');
  ok(await C.careerOk(torn, null), 'no copy: not empty is enough');
  eq(C.oprofState(oprof(1)), 'ok'); eq(C.oprofState(oprof(1, 9)), 'newer');
  const bad = oprof(1); bad[100] ^= 1; eq(C.oprofState(bad), 'bad');
});

await test('create in A, load in an empty B: taken; B changes, A takes it at its next pull', async () => {
  const { A, mk, clock } = world({ ...P1, ...ON });
  const phrase = await A.sync.create();
  ok(phrase && A.link.rev, 'linked');
  const B = mk({});
  eq(await B.sync.load(phrase.toUpperCase()), {});
  eq(B.sync.state.s, 'synced');
  eq(await C.contentHash([...B.files].map(([path, data]) => ({ path, data }))), A.link.hash);
  eq(B.snaps.length, 0, 'nothing to snapshot');
  B.write(`${TITLE}AE1A4832EEF5/Player 1.mc3Remix`, career(77));
  await B.sync.push({});
  eq(B.sync.state.s, 'synced');
  clock.t += 120e3;
  await A.sync.pull({ auto: true });
  eq(A.sync.state.s, 'synced');
  eq(Buffer.from(A.files.get(`${TITLE}AE1A4832EEF5/Player 1.mc3Remix`)).equals(Buffer.from(career(77))), true);
  eq(A.snaps.length, 1, 'snapshot before the take');
  ok(A.snaps[0].get(`${TITLE}AE1A4832EEF5/Player 1.mc3Remix`), 'the replaced file is in the snapshot');
});

await test('load in a B with saves: conflict "load", nothing sent or written until the player picks', async () => {
  const { A, mk, srv } = world({ ...P1 });
  const phrase = await A.sync.create(), B = mk({ ...P2 }), writes = srv.writes();
  await B.sync.load(phrase);
  eq(B.sync.state.s, 'conflict'); eq(B.sync.state.why, 'load');
  eq(B.sync.state.cloud.profiles, ['Player 1']); eq(B.sync.state.here.profiles, ['ferez']);
  await B.sync.push({}); await B.sync.pull({});
  eq(srv.writes(), writes, 'no PUT while the conflict waits');
  // a reload before the choice: the link alone never sends over the cloud's
  const B2 = browser(srv, { t: 0 }, {}); B2.files = B.files; B2.baks = B.baks; B2.link = B.link; B2.sync = new C.Sync(B2.deps);
  await B2.sync.push({});
  eq(B2.sync.state.s, 'conflict', 'push after reload finds the conflict again');
  // Use the cloud's: overlay, B's own profile stays, a snapshot first
  B.sync.state = { s: 'conflict', why: 'load', theirs: null }; await B.sync.takeTheirs();
  eq(B.sync.state.s, 'synced');
  ok(B.files.has(`${TITLE}ED509E14024E/ferez.mc3Remix`), 'local-only profile kept');
  ok(B.files.has(`${TITLE}AE1A4832EEF5/Player 1.mc3Remix`), 'cloud profile in');
  eq(B.snaps.length, 1);
});

await test('both change: conflict "both"; keep mine sends over; the same change on both: no conflict', async () => {
  const { A, mk, clock } = world({ ...P1 });
  const phrase = await A.sync.create(), B = mk({}); await B.sync.load(phrase);
  A.write(`${TITLE}AE1A4832EEF5/Player 1.mc3Remix`, career(10)); B.write(`${TITLE}AE1A4832EEF5/Player 1.mc3Remix`, career(11));
  await A.sync.push({}); await B.sync.push({});
  eq(B.sync.state.why, 'both');
  await B.sync.keepMine();
  eq(B.sync.state.s, 'synced');
  // the same bytes on both: linked, not a conflict
  clock.t += 3600e3;
  A.write(`${TITLE}AE1A4832EEF5/Player 1.mc3Remix`, career(12)); B.write(`${TITLE}AE1A4832EEF5/Player 1.mc3Remix`, career(12));
  await B.sync.push({}); await A.sync.push({});
  eq(A.sync.state.s, 'synced', A.sync.state.why);
});

await test('pull: older, bare, thin, newer profile.bin and a torn file are never taken by themselves', async () => {
  for (const [why, change] of [
    ['older', (B, A) => { B.write(`${TITLE}AE1A4832EEF5/Player 1.mc3Remix`, career(98)); A.link = { ...A.link, saved: A.link.saved + 999e3 }; A.sync.link = A.link; }],
    ['bare', B => { B.files.clear(); B.sync.intended(); }],
    ['thin', B => { for (const p of [...B.files.keys()]) if (p.includes('ED509E14024E')) B.files.delete(p); B.sync.intended(); }],
    ['newer', B => B.write('online/profile.bin', oprof(7, 9))],
    ['bad', B => { const t = career(3); t[700] ^= 1; B.files.set(`${TITLE}AE1A4832EEF5/Player 1.mc3Remix`, t); }],
  ]) {
    const { A, mk, clock } = world({ ...P1, ...P2, ...ON });
    const phrase = await A.sync.create(), B = mk({}); await B.sync.load(phrase);
    clock.t += 1000;
    change(B, A);
    await B.sync.push({});
    if (why === 'newer') eq(B.sync.state.s, 'conflict', 'B asks before sending a newer it cannot judge');
    if (B.sync.state.s === 'conflict') { B.sync.state = { s: 'conflict', why: 'x', theirs: null }; await B.sync.keepMine(); }
    const before = [...A.files];
    await A.sync.pull({});
    eq([A.sync.state.s, A.sync.state.why], ['conflict', why]);
    eq(A.snaps.length, 0, why + ': nothing written');
    eq([...A.files].length, before.length);
  }
});

await test('push asks: "empty" (all profiles gone) and "fewer" (one torn outside a game); torn mid-game just waits', async () => {
  const { A, srv } = world({ ...P1, ...P2 });
  await A.sync.create();
  const t = career(1); t[900] ^= 1;
  A.files.set(`${TITLE}AE1A4832EEF5/Player 1.mc3Remix`, t);
  A.playing = true; const w = srv.writes();
  await A.sync.push({ playing: true });
  eq(A.sync.state.s, 'synced'); eq(srv.writes(), w, 'no send mid-game'); ok(A.sync.dirty, 'tried again later');
  A.playing = false;
  await A.sync.push({});
  eq(A.sync.state.why, 'fewer');
  A.sync.reopen();
  A.files.clear();
  await A.sync.push({});
  eq(A.sync.state.why, 'empty');
  eq(srv.writes(), w, 'nothing sent');
  // deleted on purpose (the Saves tab): no question
  A.sync.reopen(); A.sync.intended();
  await A.sync.push({});
  eq(A.sync.state.s, 'synced'); eq(srv.writes(), w + 1);
});

await test('lost answer: the cloud holds our own unanswered send -> relinked, no conflict', async () => {
  const { A, srv } = world({ ...P1 });
  await A.sync.create();
  A.write(`${TITLE}AE1A4832EEF5/Player 1.mc3Remix`, career(40));
  const real = srv.fetch;                          // the PUT lands, its answer is lost
  srv.fetch = async (u, i) => { const r = await real(u, i); if (i?.method === 'PUT') { srv.fetch = real; throw new TypeError('fetch failed'); } return r; };
  await A.sync.push({});
  eq(A.sync.state.s, 'error');
  A.sync.serverUntil = A.sync.waitUntil = 0;
  A.write(`${TITLE}AE1A4832EEF5/Player 1.mc3Remix`, career(41));
  await A.sync.push({});
  eq(A.sync.state.s, 'synced', JSON.stringify(A.sync.state));
  eq(A.link.sent, undefined);
});

await test('gone: deleted elsewhere -> state gone, nothing sent by itself; Send again re-creates', async () => {
  const { A, mk, srv } = world({ ...P1 });
  const phrase = await A.sync.create(), B = mk({}); await B.sync.load(phrase);
  await B.sync.remove();
  eq(B.link, null);
  A.write(`${TITLE}AE1A4832EEF5/Player 1.mc3Remix`, career(50));
  await A.sync.push({});
  eq(A.sync.state.s, 'gone');
  const w = srv.writes(); await A.sync.push({}); eq(srv.writes(), w, 'gone sends nothing');
  await A.sync.resend();
  eq(A.sync.state.s, 'synced'); eq(srv.store.size, 1);
});

await test('go back a day: the earlier copy overlaid (snapshot first) and sent as current', async () => {
  const { A, clock } = world({ ...P1 });
  await A.sync.create();
  clock.t += 86400e3;
  A.write(`${TITLE}AE1A4832EEF5/Player 1.mc3Remix`, career(60));
  await A.sync.push({});
  ok(A.link.earlier, 'earlier copy exists');
  await A.sync.goBack();
  eq(A.sync.state.s, 'synced');
  ok(Buffer.from(A.files.get(`${TITLE}AE1A4832EEF5/Player 1.mc3Remix`)).equals(Buffer.from(career(1))), 'day-1 bytes back');
  eq(A.snaps.length, 1);
});

await test('debounce: save signals send at most once per 5 min; unchanged files never sent; pagehide keepalive', async () => {
  const { A, srv } = world({ ...P1 });
  await A.sync.create();
  A.playing = true;
  const w0 = srv.writes();
  for (let i = 0; i < 30; i++) {                   // a save every 20 s for 10 minutes
    A.write(`${TITLE}AE1A4832EEF5/Player 1.mc3Remix`, career(100 + i));
    A.sync.saved();
    await A.advance(20e3);
  }
  const sends = srv.writes() - w0;
  ok(sends >= 1 && sends <= 3, `sends in 10 min: ${sends}`);
  await A.advance(10 * 60e3);                      // the last change goes out, once
  const w1 = srv.writes();
  ok(w1 - w0 <= 4, `total ${w1 - w0}`);
  A.sync.saved(); await A.advance(10 * 60e3);      // a signal with nothing changed: no PUT
  eq(srv.writes(), w1, 'unchanged box not sent');
  // pagehide: a change prepared during the 5-minute wait goes with keepalive (small box)
  A.write(`${TITLE}AE1A4832EEF5/SaveMeta.xbx`, meta('Player 1 renamed'));
  A.sync.lastTry = A.deps.now();                   // just sent: the next send waits 5 min
  A.sync.saved(); await A.advance(10e3);
  ok(A.sync.ready, 'box ready during the wait');
  const flushed = A.sync.flush();
  results.push(`     (box ${A.sync.ready ? A.sync.ready.box.length : 'sent'}; flush ${flushed})`);
  const last = srv.requests.at(-1);
  if (flushed) ok(last.method === 'PUT' && last.keepalive, 'keepalive PUT');
  else ok(true);
});

await test('pagehide keepalive with a small box; its answer unread is covered by link.sent', async () => {
  const small = { [`${TITLE}AE1A4832EEF5/SaveMeta.xbx`]: meta('P'), [`${TITLE}AE1A4832EEF5/P.mc3Remix`]: career(1, 4000) };
  const { A, srv } = world(small);
  await A.sync.create();
  A.write(`${TITLE}AE1A4832EEF5/P.mc3Remix`, career(2, 4000));
  A.sync.lastTry = A.deps.now();
  A.sync.saved(); await A.advance(6e3);
  ok(A.sync.ready && A.sync.ready.box.length <= C.KEEPALIVE_MAX, 'ready small');
  ok(A.sync.flush(), 'flushed');
  await new Promise(r => setTimeout(r, 20));
  const last = srv.requests.at(-1);
  ok(last.method === 'PUT' && last.keepalive, 'keepalive PUT');
  ok(A.link.sent?.length === 1, 'hash remembered');
  // next visit: the pull sees its own version and relinks, no conflict
  const A2 = browser(srv, { t: A.deps.now() + 3600e3 }, {}); A2.files = A.files; A2.baks = A.baks; A2.link = A.link; A2.sync = new C.Sync(A2.deps);
  await A2.sync.pull({ auto: true });
  eq(A2.sync.state.s, 'synced'); eq(A2.link.sent, undefined);
});

await test('backoff: a failure waits 1 min doubling, a 429 Retry-After blocks manual tries too, no retry loop', async () => {
  const { A, srv } = world({ ...P1 });
  await A.sync.create();
  srv.fail = { status: 500, sticky: true };
  A.playing = true;
  const n0 = srv.requests.length;
  for (let i = 0; i < 60; i++) {                   // an hour of saves every minute against a broken server
    A.write(`${TITLE}AE1A4832EEF5/Player 1.mc3Remix`, career(200 + i));
    A.sync.saved();
    await A.advance(60e3);
  }
  const tries = srv.requests.length - n0;
  ok(tries <= 12, `tries in an hour: ${tries}`);
  srv.fail = { status: 429, headers: { 'retry-after': '600' } };
  A.sync.serverUntil = A.sync.waitUntil = 0; A.sync.backoff = 0;
  await A.sync.push({});
  eq(A.sync.state.s, 'error');
  const n1 = srv.requests.length;
  await A.sync.push({});                           // manual, within the 10 minutes
  eq(srv.requests.length, n1, 'nothing sent while the server asked to wait');
  ok(/wait/.test(A.sync.state.msg), A.sync.state.msg);
  await A.advance(11 * 60e3);
  srv.fail = null;
  await A.sync.push({});
  eq(A.sync.state.s, 'synced');
});

await test('automatic pulls: one per minute at most (page open + Play)', async () => {
  const { A, srv } = world({ ...P1 });
  await A.sync.create();
  const n = srv.requests.length;
  await A.sync.pull({ auto: true }); await A.sync.pull({ auto: true, timeoutMs: 4000 });
  eq(srv.requests.length - n, 1);
});

await test('a take never deletes a local file and never runs under a game', async () => {
  const { A, mk } = world({ ...P1 });
  const phrase = await A.sync.create(), B = mk({ ...P1, ...P2 });
  await B.sync.load(phrase);
  B.playing = true;
  await B.sync.takeTheirs();
  eq(B.sync.state.s, 'error'); ok(/before Play/.test(B.sync.state.msg));
  eq(B.snaps.length, 0);
  B.playing = false; B.sync.state = { s: 'conflict', why: 'load', theirs: null };
  await B.sync.takeTheirs();
  ok(B.files.has(`${TITLE}ED509E14024E/ferez.mc3Remix`)); eq(B.snaps.length, 1);
});

for (const r of results) console.log(r);
console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
