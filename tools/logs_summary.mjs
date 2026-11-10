// Summary of downloaded player logs: counts by tag, build and browser, and the first fatal line of each.
// Usage: node tools/logs_summary.mjs <dir>      (node tools/logs_summary.mjs --self-test  checks the parsers)
// Names: logs/<yyyymmdd>-<tag>-<code>.txt (new); old logs <yyyymmdd>-<code>.txt count as tag 'fatal'.
// This script never downloads and never reads credentials. Fetch the logs yourself first, e.g.
//   rclone copy "$MC3_LOGS_REMOTE" ./logs --include "*.txt"     (MC3_LOGS_REMOTE: your rclone remote, set in your shell)
// FATAL mirrors FATAL in web/index.html (~:1202); keep both in step.

import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

const dir = process.argv[2];
if (!dir) {
  console.error('usage: node tools/logs_summary.mjs <dir>');
  process.exit(2);
}

const FATAL = [
  /\[WGPU\] device lost/,
  /\[GL\] WebGL context lost/,
  /\[GL\] WebGL2 context on #canvas failed|\[GL\] (window|context):/,
  /worker sent an error!.*UnknownError/,
  /worker sent an error!/,
  /out of memory|Cannot enlarge memory|could not allocate|\[WASM\] (no storage|commit of)/i,
];

const NAMED = /^(\d{8})-([a-z0-9][a-z0-9-]*)-([A-Z0-9]+)\.txt$/;
const OLD = /^(\d{8})-([A-Z0-9]+)\.txt$/;

const mask = s => s.replace(/\/k\/[^/\s"']+/g, '/k/<key>');
const clip = (s, n) => (s.length > n ? s.slice(0, n - 1) + '…' : s);
const norm = s => clip(mask(s).replace(/blob:\S+/g, 'blob:<id>').replace(/\b\d+(\.\d+)?\b/g, 'N').replace(/\s+/g, ' '), 110);

function browserOf(ua) {
  let m;
  if ((m = ua.match(/Edg\/(\d+)/))) return 'Edge ' + m[1];
  if ((m = ua.match(/OPR\/(\d+)/))) return 'Opera ' + m[1];
  if ((m = ua.match(/CriOS\/(\d+)/))) return 'Chrome iOS ' + m[1];
  if ((m = ua.match(/Chrome\/(\d+)/))) return 'Chrome ' + m[1];
  if ((m = ua.match(/FxiOS\/(\d+)|Firefox\/(\d+)/))) return 'Firefox ' + (m[1] || m[2]);
  const v = (ua.match(/Version\/(\d+)/) || [])[1];
  if (/iPhone|iPad|iPod/.test(ua)) return 'iOS Safari' + (v ? ' ' + v : '');
  if (/Safari\//.test(ua)) return 'Safari' + (v ? ' ' + v : '');
  return 'unknown';
}

// Build: the boot banner ('... package, lifted from the ... built <time>'), without the build time.
function buildOf(lines) {
  const l = lines.find(x => / package, lifted from the /.test(x));
  if (!l) return 'unknown build';
  return l.replace(/^build: /, '').replace(/\s+built \S+\s*$/, '').trim();
}

// First fatal line: the '[WEB] failure:' line, else the first FATAL match, else the last line.
function firstFatal(lines) {
  const failure = lines.find(l => l.startsWith('[WEB] failure: '));
  if (failure) return ['failure', failure.trim()];
  const hit = lines.find(l => !/^\[WEB\] (memory|opfs):/.test(l) && FATAL.some(re => re.test(l)));
  if (hit) return ['pattern', hit.trim()];
  const last = lines.filter(l => l.trim()).pop();
  return ['last', (last ?? '(empty log)').trim()];
}

function selfTest() {
  const eq = (a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error(`expected ${JSON.stringify(b)}, got ${JSON.stringify(a)}`); };
  eq(NAMED.test('20261110-freeze-2AXBDF.txt'), true);
  eq(NAMED.test('20261110-2AXBDF.txt'), false);
  eq(NAMED.exec('20261110-previous-crash-2AXBDF.txt')[2], 'previous-crash');
  eq(OLD.test('20261110-2AXBDF.txt'), true);
  eq(browserOf('Mozilla/5.0 (Windows NT 10.0) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/156.0.0.0 Safari/537.36'), 'Chrome 156');
  eq(browserOf('Mozilla/5.0 (iPhone; CPU iPhone OS 18_7) Version/18.7.5 Mobile/15E148 Safari/604.1'), 'iOS Safari 18');
  eq(firstFatal(['x', '[WEB] failure: A: b', 'out of memory']), ['failure', '[WEB] failure: A: b']);
  eq(firstFatal(['[WEB] memory: out of memory', 'worker sent an error! x']), ['pattern', 'worker sent an error! x']);
  eq(firstFatal(['a', 'b']), ['last', 'b']);
  eq(mask('/k/abc123/discs'), '/k/<key>/discs');
}

if (dir === '--self-test') {
  selfTest();
  console.log('self-test ok');
  process.exit(0);
}

const files = readdirSync(dir).filter(f => f.endsWith('.txt')).sort();
const rows = [];
let skipped = 0;
for (const f of files) {
  const named = f.match(NAMED), old = f.match(OLD);
  if (!named && !old) { skipped++; continue; }
  let lines;
  try { lines = readFileSync(join(dir, f), 'utf8').split(/\r?\n/); } catch { skipped++; continue; }
  const uaLine = lines.find(l => l.includes('Mozilla/5.0')) ?? '';
  const [kind, line] = firstFatal(lines);
  rows.push({
    code: named ? named[3] : old[2],
    tag: named ? named[2] : 'fatal',
    build: buildOf(lines),
    browser: browserOf(uaLine.slice(uaLine.indexOf('Mozilla/5.0'))),
    kind, line,
  });
}

function tally(keyOf) {
  const m = new Map();
  for (const r of rows) {
    const k = keyOf(r);
    if (!m.has(k)) m.set(k, []);
    m.get(k).push(r.code);
  }
  return [...m].sort((a, b) => b[1].length - a[1].length || a[0].localeCompare(b[0]));
}

const fatalShow = new Map();
for (const r of rows) {
  r.fatalKey = `[${r.kind}] ${norm(r.line)}`;
  if (!fatalShow.has(r.fatalKey)) fatalShow.set(r.fatalKey, `[${r.kind}] ${clip(mask(r.line), 120)}`);
}

const sections = [
  ['by tag', tally(r => r.tag), k => k],
  ['by build', tally(r => r.build), k => k],
  ['by browser', tally(r => r.browser), k => k],
  ['first fatal line', tally(r => r.fatalKey), k => fatalShow.get(k)],
];

console.log(`${rows.length} logs in ${dir} (${skipped} skipped: not named as logs)`);
for (const [title, entries, show] of sections) {
  console.log(`\n${title}`);
  for (const [k, codes] of entries) console.log(`  ${String(codes.length).padStart(4)}  ${clip(show(k), 120)}`);
}
console.log('\ncodes');
for (const [title, entries, show] of sections) {
  console.log(` ${title}`);
  for (const [k, codes] of entries) console.log(`  ${clip(show(k), 60)}: ${codes.join(' ')}`);
}
