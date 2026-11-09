// web/disc_cache.js: direct reads (signer + fetchBlocks) against a fake disc server and bucket.
//   node web/test/disc_sign.test.js
'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');

globalThis.location = { href: 'https://play.example/', hostname: 'play.example' };
const C = new Function(fs.readFileSync(path.join(__dirname, '../disc_cache.js'), 'utf8') + '\nreturn MC3DiscCache;')();
const DISC = 'https://disc.example/k/pw/discs/remix-usa', data = new Uint8Array(3 << 20).map((_, i) => i & 255);
let calls, bucket, signs;   // bucket: 'ok' | 403 | 'cors'
globalThis.fetch = async (u, o = {}) => {
  calls.push([u.split('?')[0], o.credentials]);
  if (u.endsWith('.sign')) {
    signs++;
    return new Response(JSON.stringify({ exp: Date.now() + 12 * 3600e3,
      urls: Object.fromEntries(o.body.split('\n').map(p => [p, `https://acct.r2.example/mc3/discs/remix-usa/${p}?sig=${signs}`])) }));
  }
  if (u.startsWith('https://acct.r2.example/')) {
    if (bucket === 'cors') throw new TypeError('Failed to fetch');
    if (bucket !== 'ok') return new Response('', { status: 403 });
  }
  const [, a, b] = /bytes=(\d+)-(\d+)/.exec(o.headers.Range);
  return new Response(data.slice(+a, +b + 1), { status: 206 });
};
const warns = [];
async function read(state, paths = ['a.bin']) {
  calls = []; bucket = state; signs = 0; warns.length = 0;
  const s = C.signer(DISC, paths, m => warns.push(m));
  const b = await C.fetchBlocks(s.url('a.bin'), 1, 2, data.length);
  assert.deepStrictEqual(b[0].subarray(0, 4), data.subarray(1 << 20, (1 << 20) + 4));
  return calls.filter(c => !c[0].endsWith('.sign'));
}

(async () => {
  // signed: one ranged read, at the bucket, no cookie
  assert.deepStrictEqual(await read('ok'), [['https://acct.r2.example/mc3/discs/remix-usa/a.bin', 'omit']]);
  assert.match(warns[0], /direct reads, 1 files signed/);
  // 250 files: three .sign calls of at most 100
  await read('ok', Array.from({ length: 250 }, (_, i) => i ? `f${i}` : 'a.bin'));
  assert.strictEqual(signs, 3);
  // the bucket refuses a fresh signature: off, the server's own URL with its cookie
  assert.deepStrictEqual(await read(403), [['https://acct.r2.example/mc3/discs/remix-usa/a.bin', 'omit'], [DISC + '/a.bin', 'include']]);
  assert.match(warns.at(-1), /direct reads off \(the bucket refused a fresh signature\)/);
  // no CORS on the bucket (no answer): off, the server
  assert.deepStrictEqual((await read('cors')).map(c => c[1]), ['omit', 'include']);
  // a server without signing: the server alone, nothing logged
  globalThis.fetch = (f => async (u, o) => u.endsWith('.sign') ? new Response('', { status: 404 }) : f(u, o))(globalThis.fetch);
  assert.deepStrictEqual(await read('ok'), [[DISC + '/a.bin', 'include']]);
  assert.strictEqual(warns.length, 0);
  console.log('disc_cache.js direct reads: ok');
})().catch(e => { console.error(e); process.exit(1); });
