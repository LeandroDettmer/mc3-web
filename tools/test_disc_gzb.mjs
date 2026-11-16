// web/disc_cache.js fetchBlocks with packed copies (tools/disc_pack.py), against a mocked fetch: no server, no disc.
//   node tools/test_disc_gzb.mjs [<disc folder> <path>]   (with a disc: that file and its <disc>.gzb copy, first 40 blocks)
// Checks: runs from the packed copy decode to the raw bytes, one request per run, a 200 without ranges, and a 404
// on the .gzb (or a bad block) falling back to the raw file with z.dead set.
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import zlib from 'node:zlib';
import { randomBytes } from 'node:crypto';

const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'));
const BLOCK = 1 << 20;
let raw, z, packedBuf;
if (process.argv[3]) {
  const [disc, rel] = process.argv.slice(2);
  const fd = fs.openSync(path.join(disc, rel), 'r'), n = Math.min(40 * BLOCK, fs.fstatSync(fd).size);
  raw = Buffer.alloc(n); fs.readSync(fd, raw, 0, n, 0);
  z = JSON.parse(fs.readFileSync(path.join(disc + '.gzb', 'index.json')))[rel].z.slice(0, Math.ceil(n / BLOCK));
  const gzb = fs.readFileSync(path.join(disc + '.gzb', rel + '.gzb'));
  packedBuf = gzb.subarray(0, z.reduce((a, b) => a + b, 0));
} else {                                  // synthetic: zero, text and random blocks, the last one short
  const parts = [];
  for (let i = 0; i < 9; i++) parts.push(i % 3 === 0 ? Buffer.alloc(BLOCK) : i % 3 === 1 ? Buffer.alloc(BLOCK, `block ${i} `) : randomBytes(BLOCK));
  parts.push(Buffer.alloc(12345, 'tail'));
  raw = Buffer.concat(parts);
  const stored = parts.map(b => { const g = zlib.gzipSync(b, { level: 9 }); return g.length > b.length * 0.97 ? b : g; });
  z = stored.map(b => b.length);
  packedBuf = Buffer.concat(stored);
}
let requests = 0, gzbStatus = 206, rawRanges = true;
globalThis.location = { href: 'https://example.test/', hostname: 'example.test' };
globalThis.fetch = async (u, o) => {
  requests++;
  const body = u.endsWith('.gzb') ? packedBuf : raw;
  if (u.endsWith('.gzb') && gzbStatus === 404) return new Response('', { status: 404 });
  if (!u.endsWith('.gzb') && !rawRanges) return new Response(body, { status: 200 });
  const [, a, b] = /bytes=(\d+)-(\d+)/.exec(o.headers.Range);
  return new Response(body.subarray(+a, +b + 1), { status: 206 });
};
const ctx = vm.createContext(globalThis);
const C = vm.runInContext(fs.readFileSync(path.join(here, '../web/disc_cache.js'), 'utf8') + ';MC3DiscCache', ctx);
const size = raw.length, nb = Math.ceil(size / BLOCK), f = { p: 'x', s: size, z };
const same = (bi, out) => out.every((b, i) => Buffer.compare(Buffer.from(b), raw.subarray((bi + i) * BLOCK, Math.min(size, (bi + i + 1) * BLOCK))) === 0);
let fails = 0;
const check = (what, ok) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) fails++; };

const zobj = { off: C.packed(f), src: 'https://example.test/d/x.gzb' };
check('packed() offsets', zobj.off && zobj.off.length === nb + 1 && zobj.off[nb] === packedBuf.length);
for (const [bi, n] of [[0, 1], [0, nb], [2, 3], [nb - 1, 1], [nb - 4, 4]]) {
  requests = 0;
  let wire = 0;
  const out = await C.fetchBlocks('https://example.test/d/x', bi, n, size, { z: zobj, onBlock: (j, b, w) => { wire += w; } });
  check(`blocks ${bi}..${bi + n - 1}: decoded, 1 request, ${wire} wire bytes`, same(bi, out) && requests === 1 && wire === zobj.off[bi + n] - zobj.off[bi]);
}
check('no z: raw as before', same(1, await C.fetchBlocks('https://example.test/d/x', 1, 3, size)));
rawRanges = false;
check('raw server without ranges (200)', same(3, await C.fetchBlocks('https://example.test/d/x', 3, 2, size)));
rawRanges = true;
gzbStatus = 404;
const dead = { off: zobj.off, src: zobj.src }, warns = [];
const out = await C.fetchBlocks('https://example.test/d/x', 1, 4, size, { z: dead, warn: w => warns.push(w) });
check(`404 on .gzb: raw, z.dead (${warns[0]})`, same(1, out) && dead.dead === true);
gzbStatus = 206;
const bad = { off: zobj.off, src: zobj.src };
const good = packedBuf; packedBuf = Buffer.from(good); packedBuf.fill(0x55, 0, 64);
const out2 = await C.fetchBlocks('https://example.test/d/x', 0, 2, size, { z: bad, warn: () => {} });
check('bad packed block: raw, z.dead', same(0, out2) && bad.dead === true);
packedBuf = good;
console.log(fails ? `${fails} failed` : 'all passed');
process.exit(fails ? 1 : 0);
