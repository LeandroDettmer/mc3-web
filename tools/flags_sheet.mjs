// The players' country flags (start page's "You" card, in-game name tags, players panel) as one picture, web/flags.png:
// 48x36 each, 16 a row, in the order of web/index.html's FLAG_CODES (the page and web/pre.js find a flag by its index).
// Made from nfsu2-web's runtime/flags.dat (th0mk, MIT; its web/tools/mkflags.mjs drew flag-icons 7.6.0's 4:3 SVGs,
// lipis/flag-icons, MIT, LICENSES/flag-icons.txt; Quebec from Wikimedia Commons, public domain). flagSheet below is
// nfsu2-web's web/build.mjs flagSheet (MIT, th0mk), reading the codes from index.html.
//
//   node tools/flags_sheet.mjs <nfsu2-web>/runtime/flags.dat
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const dat = process.argv[2];
if (!dat) { console.error('usage: node tools/flags_sheet.mjs <nfsu2-web>/runtime/flags.dat'); process.exit(2); }
const out = path.join(REPO, 'web/flags.png');
fs.writeFileSync(out, flagSheet(fs.readFileSync(dat), fs.readFileSync(path.join(REPO, 'web/index.html'), 'utf8'), 16));
console.log(`${out}: ${fs.statSync(out).size} bytes`);

function flagSheet(bin, src, cols) {
  const bad = what => { throw new Error(`flags.dat: ${what}`); };
  if (bin.toString('latin1', 0, 4) !== 'FLG1') bad('not FLG1');
  const n = bin.readUInt16LE(4), w = bin[6], h = bin[7];
  const codes = [...(/FLAG_CODES\s*=\s*\[([^\]]*)\]/.exec(src)?.[1] ?? '').matchAll(/'([a-z-]+)'/g)].map(m => m[1]);
  if (codes.length !== n) bad(`${n} flags, web/index.html FLAG_CODES has ${codes.length}`);
  const W = cols * w, H = Math.ceil(n / cols) * h, px = Buffer.alloc(W * H * 3);
  for (let i = 0; i < n; i++) {
    // { char code[8], u32 offset, u32 length } after the 8-byte head; the pixels in runs (mkflags.mjs)
    const e = 8 + 16 * i, code = bin.toString('latin1', e, e + 8).replace(/\0+$/, '');
    if (code !== codes[i]) bad(`flag ${i} is ${code}, web/index.html has ${codes[i]} there`);
    let p = bin.readUInt32LE(e + 8), k = 0;
    const end = p + bin.readUInt32LE(e + 12), x0 = (i % cols) * w, y0 = Math.floor(i / cols) * h;
    const put = at => { if (k < w * h) bin.copy(px, ((y0 + Math.floor(k / w)) * W + x0 + k % w) * 3, at, at + 3); k++; };
    while (p < end) {
      const r = bin[p++];
      if (r < 128) for (let j = 0; j <= r; j++, p += 3) put(p);
      else { for (let j = 0; j < r - 126; j++) put(p); p += 3; }
    }
    if (k !== w * h) bad(`${code}: ${k} pixels, not ${w * h}`);
  }
  // PNG, 8-bit RGB, each row unfiltered
  const raw = Buffer.alloc(H * (1 + W * 3));
  for (let y = 0; y < H; y++) px.copy(raw, y * (1 + W * 3) + 1, y * W * 3, (y + 1) * W * 3);
  const chunk = (type, data) => {
    const head = Buffer.alloc(8), tail = Buffer.alloc(4);
    head.writeUInt32BE(data.length, 0); head.write(type, 4, 'latin1');
    tail.writeUInt32BE(zlib.crc32(Buffer.concat([head.subarray(4), data])) >>> 0, 0);
    return Buffer.concat([head, data, tail]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4); ihdr[8] = 8; ihdr[9] = 2;
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]);
}
