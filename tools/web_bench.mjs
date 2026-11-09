// web_bench.mjs -- frame-rate benchmark of a browser build (Windows host, Chrome with the GPU).
//
//   BUILD_DIR=work-remix/build-web node tools/web_bench.mjs [title|cruise|both]
//
// Serves the build on its own port (tools/web_serve.py), starts a fresh Chrome
// (own profile under .scratch/bench/, CDP), presses Play with MC3_PERF=5 and
// MC3_SKIP_MOVIES=hard, and measures a fixed scene:
//   title  -- the title screen's city flythrough (no input), from the first
//             [PATH] ...bg_savegame + SETTLE s;
//   cruise -- MC3_AUTOSTART=cruise into Arcade, Cruise (the profile in SAVE_DIR
//             is written to OPFS first), gas held from "[AUTOSTART] in Cruise".
// For SECS seconds it averages the [perf] reports (frames presented/s, guest
// lock wait, GL thread idle, GL ms per frame), the Chrome GPU process's CPU and
// its 3D engine use (Windows counters), then (PROFILE=1, default) samples every
// worker for PSECS s: busy share per thread and the hottest functions. Function
// names need a build linked with --profiling-funcs; without, wasm-function[n].
// Everything it started (Chrome, server) is killed at the end.
//
// Env: BUILD_DIR (work-remix/build-web), DISC (game-remix-usa), PORT (3011),
// CDP (9311), SECS (30), SETTLE (10), PROFILE (1), PSECS (10),
// SAVE_DIR (.scratch/bench/save: a UDATA/ tree), QUERY (extra page query, a=b&c=d),
// CHROME (chrome.exe path), OUT (.scratch/bench/<time>).
import { spawn, execFileSync } from 'node:child_process';
import { writeFileSync, mkdirSync, rmSync, readdirSync, readFileSync, statSync, existsSync } from 'node:fs';
import { join, relative, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const E = process.env;
const BUILD = resolve(ROOT, E.BUILD_DIR || 'work-remix/build-web');
const PORT = Number(E.PORT || 3011), CDP = Number(E.CDP || 9311);
const SECS = Number(E.SECS || 30), SETTLE = Number(E.SETTLE || 10), PSECS = Number(E.PSECS || 10);
const PROFILE = E.PROFILE !== '0';
const SAVE_DIR = resolve(ROOT, E.SAVE_DIR || '.scratch/bench/save');
const CHROME = E.CHROME || 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const OUT = resolve(ROOT, E.OUT || `.scratch/bench/${new Date().toISOString().replace(/[:.]/g, '-')}`);
const PROF_DIR = resolve(ROOT, `.scratch/bench/chrome-${CDP}`);
const PHASES = { title: ['title'], cruise: ['cruise'], both: ['title', 'cruise'] }[process.argv[2] || 'both'];
if (!PHASES) { console.error('usage: node tools/web_bench.mjs [title|cruise|both]'); process.exit(2); }
if (!existsSync(join(BUILD, 'mcdub3remix.wasm')) && !existsSync(join(BUILD, 'mcdub3.wasm'))) { console.error(`no build in ${BUILD}`); process.exit(2); }
mkdirSync(OUT, { recursive: true });

const sleep = ms => new Promise(r => setTimeout(r, ms));
const ps = cmd => execFileSync('powershell', ['-NoProfile', '-Command', cmd], { encoding: 'utf8' });
const say = s => { console.log(s); summary.push(s); };
const summary = [];

// --- processes we own
let server, browserPid;
function chromeProcs() {      // [{pid, type, cpu (s)}] of our Chrome: the browser and its children
  const tag = PROF_DIR.replace(/\\/g, '\\\\').split(/[\\/]/).pop();
  const j = ps(`$a = Get-CimInstance Win32_Process -Filter "Name='chrome.exe'"; $b = $a | Where-Object { $_.CommandLine -match '${tag}' -and $_.CommandLine -notmatch '--type=' };` +
    ` $a | Where-Object { $b.ProcessId -contains $_.ProcessId -or $b.ProcessId -contains $_.ParentProcessId } | ForEach-Object {` +
    ` [pscustomobject]@{ pid = $_.ProcessId; ppid = $_.ParentProcessId; type = if ($_.CommandLine -match '--type=([a-z-]+)') { $matches[1] } else { 'browser' };` +
    ` cpu = ($_.KernelModeTime + $_.UserModeTime) / 1e7 } } | ConvertTo-Json -Compress`).trim();
  return j ? [].concat(JSON.parse(j)) : [];
}
function cleanup() {
  try { const b = chromeProcs().filter(p => p.type === 'browser'); for (const p of b) execFileSync('taskkill', ['/PID', String(p.pid), '/T', '/F'], { stdio: 'ignore' }); if (b.length) console.log(`killed chrome ${b.map(p => p.pid)}`); } catch {}
  if (server) { try { execFileSync('taskkill', ['/PID', String(server.pid), '/T', '/F'], { stdio: 'ignore' }); console.log(`killed server ${server.pid}`); } catch {} server = null; }
}
process.on('SIGINT', () => { cleanup(); process.exit(130); });

// --- CDP over the page target; workers auto-attached (flatten) as sessions
let ws, cid = 0;
const pend = new Map(), sessions = new Map();
const send = (method, params = {}, sessionId) => new Promise(r => {
  const i = ++cid; pend.set(i, r);
  ws.send(JSON.stringify(sessionId ? { id: i, method, params, sessionId } : { id: i, method, params }));
});
const evaluate = async (expression, awaitPromise = false) =>
  (await send('Runtime.evaluate', { expression, awaitPromise, returnByValue: true, userGesture: true })).result?.result?.value;

async function connect() {
  let page;
  for (let i = 0; i < 100 && !page; i++) {
    await sleep(200);
    try { page = (await (await fetch(`http://127.0.0.1:${CDP}/json/list`)).json()).find(x => x.type === 'page'); } catch {}
  }
  if (!page) throw new Error('Chrome did not open its CDP port');
  ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise(r => ws.addEventListener('open', r));
  ws.addEventListener('message', ev => {
    const m = JSON.parse(ev.data);
    if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); return; }
    if (m.method === 'Target.attachedToTarget') {
      const s = m.params.sessionId;
      sessions.set(s, m.params.targetInfo);
      send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, s);
      send('Runtime.runIfWaitingForDebugger', {}, s);
    }
    if (m.method === 'Target.detachedFromTarget') sessions.delete(m.params.sessionId);
  });
  await send('Page.enable');
  await send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: true });
}

// --- the page log (Module.print/printErr), drained into `log` as [ms, text]
let log = [];
async function drain() {
  const v = await evaluate("(window.__L || []).splice(0)");
  for (const l of v || []) { const k = l.indexOf('\t'); log.push([Number(l.slice(0, k)), l.slice(k + 1)]); }
}
async function waitLog(re, bad, secs) {
  for (let t = 0; t < secs; t++) {
    await drain();
    const hit = log.find(([, s]) => re.test(s)); if (hit) return hit[0];
    const b = bad && log.find(([, s]) => bad.test(s)); if (b) throw new Error(`stopped: ${b[1]}`);
    await sleep(1000);
  }
  throw new Error(`timeout waiting for ${re}`);
}

function saveFiles(dir) {       // {"UDATA/...": base64}
  const out = {};
  const walk = d => { for (const n of readdirSync(d)) { const p = join(d, n); statSync(p).isDirectory() ? walk(p) : out[relative(dir, p).replace(/\\/g, '/')] = readFileSync(p).toString('base64'); } };
  walk(dir); return out;
}

async function key(type, k, code) {
  await send('Input.dispatchKeyEvent', { type, windowsVirtualKeyCode: code, code: k, key: k });
}

// --- GPU 3D engine use of one process (Windows counters), averaged over secs
function gpuSampler(pid, secs) {
  return new Promise(res => {
    const p = spawn('powershell', ['-NoProfile', '-Command',
      `$end = (Get-Date).AddSeconds(${secs}); $v = @(); while ((Get-Date) -lt $end) {` +
      ` $v += (Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUEngine | Where-Object { $_.Name -match '^pid_${pid}_.*engtype_3D$' } | Measure-Object UtilizationPercentage -Sum).Sum;` +
      ` Start-Sleep -Milliseconds 700 }; ($v | Measure-Object -Average).Average`]);
    let o = ''; p.stdout.on('data', d => o += d); p.on('close', () => res(parseFloat(o)));
  });
}

// --- sample every worker (and the page); per thread: busy share, top self, key inclusive
const IDLE = /^\((idle|program|garbage collector)\)$|futex_wait|_emscripten_thread_mailbox_await|^wait$|nanosleep/;
const KEY = ['gil_lock', 'xbox_gil_resume', 'kernel_thunk_dispatch', 'q_gl_run', 'gl_draw_raw', 'merge_flush', 'tex_upload', 'nv2a_pb_exec_method', 'q_draw', 'q_flip', 'sub_00012150', 'sub_0001EDB0', '_emscripten_get_now'];
async function profile(secs) {
  const targets = [[{ type: 'page', targetId: 'page' }, undefined], ...[...sessions].filter(([, t]) => t.type === 'worker').map(([s, t]) => [t, s])];
  for (const [, s] of targets) { await send('Profiler.enable', {}, s); await send('Profiler.setSamplingInterval', { interval: 500 }, s); }
  await Promise.all(targets.map(([, s]) => send('Profiler.start', {}, s)));
  await sleep(secs * 1000);
  const res = await Promise.all(targets.map(([, s]) => Promise.race([send('Profiler.stop', {}, s), sleep(30000).then(() => null)])));
  const raw = [];
  res.forEach((r, k) => {
    const p = r?.result?.profile; if (!p) return;
    const byId = new Map(p.nodes.map(n => [n.id, n])), par = new Map();
    for (const n of p.nodes) for (const c of n.children || []) par.set(c, n.id);
    const cnt = new Map(); for (const s of p.samples) cnt.set(s, (cnt.get(s) || 0) + 1);
    const self = new Map(), inc = new Map(), caller = new Map(); let total = 0;
    for (const [nid, c] of cnt) {
      const f = byId.get(nid).callFrame.functionName || '(anonymous)';
      total += c;
      self.set(f, (self.get(f) || 0) + c);
      for (let x = par.get(nid); x != null; x = par.get(x)) { const g = byId.get(x).callFrame.functionName; if (g && g !== f && !/^(wasm-to-js|js-to-wasm|(anonymous))$/.test(g)) { const m = caller.get(f) || new Map(); m.set(g, (m.get(g) || 0) + c); caller.set(f, m); break; } }
      const seen = new Set();
      for (let x = nid; x != null; x = par.get(x)) { const g = byId.get(x).callFrame.functionName; if (!seen.has(g)) { seen.add(g); inc.set(g, (inc.get(g) || 0) + c); } }
    }
    if (total) raw.push({ k, self, inc, caller, total });
  });
  // A build without names shows the futex wait as wasm-function[n]: the one most
  // parked threads (pool workers) sit in, >90% self, is taken for it.
  const parked = new Map();
  for (const t of raw) for (const [f, c] of t.self) if (/^wasm-function/.test(f) && c / t.total > 0.9) parked.set(f, (parked.get(f) || 0) + 1);
  const wait = [...parked].filter(([, n]) => n >= 3).map(([f]) => f);
  const isIdle = f => IDLE.test(f) || wait.includes(f);
  const threads = [], global = new Map();
  for (const { k, self, inc, caller, total } of raw) {
    let idle = 0; for (const [f, c] of self) if (isIdle(f)) idle += c;
    const has = f => (inc.get(f) || 0) / total;
    const role = has('sub_00012150') > 0.3 ? 'game main' : has('q_gl_run') > 0.05 || has('bufferSubData') + has('drawElements') + has('texSubImage2D') > 0.02 ? 'GL' :
      has('q_draw') + has('q_flip') > 0.05 ? 'pushbuffer exec' : targets[k][0].type;
    const busy = (total - idle) / total;
    threads.push({ role, id: targets[k][0].targetId.slice(0, 8), busy, total, self, inc, caller, has });
    for (const [f, c] of self) if (!isIdle(f)) global.set(f, (global.get(f) || 0) + c / total);   // in thread-seconds share
  }
  threads.sort((a, b) => b.busy - a.busy);
  return { threads, global };
}

function parsePerf(t0, t1) {
  const L = log.filter(([t, s]) => t >= t0 + 4000 && t <= t1 + 1000 && s.startsWith('[perf]')).map(([, s]) => s);
  const avg = (re, g = 1) => { const v = L.map(s => s.match(re)).filter(Boolean).map(m => parseFloat(m[g])); return v.length ? v.reduce((a, b) => a + b) / v.length : NaN; };
  const glms = L.map(s => s.match(/GL \d+ frames, per frame: (\d+) draws.*?(\d+) draw calls.*?ms: (.*?); uniforms/)).filter(Boolean);
  return {
    reports: L.filter(s => /main loop:/.test(s)).length,
    fps: avg(/main loop: [\d.]+ passes\/s, ([\d.]+) frames presented/),
    passes: avg(/main loop: ([\d.]+) passes\/s/),
    lockWait: avg(/main thread: (\d+)% of the time waiting for the guest lock/),
    handovers: avg(/guest lock: (\d+) hand-overs\/s/),
    lockHeld: avg(/guest lock held (\d+)%/),
    glIdle: avg(/GL thread idle (\d+)%/),
    execHeld: avg(/executor held back by it (\d+)%/),
    draws: glms.length ? glms.reduce((a, m) => a + +m[1], 0) / glms.length : NaN,
    calls: glms.length ? glms.reduce((a, m) => a + +m[2], 0) / glms.length : NaN,
    glPhaseMs: glms.length ? glms.reduce((a, m) => a + [...m[3].matchAll(/[\d.]+/g)].reduce((x, y) => x + +y, 0), 0) / glms.length : NaN,
    glPhases: glms.length ? glms[glms.length - 1][3] : '',
  };
}

async function phase(name) {
  log = [];
  const q = new URLSearchParams({ MC3_PERF: '5', MC3_SKIP_MOVIES: 'hard', ...(name === 'cruise' ? { MC3_AUTOSTART: 'cruise' } : {}) });
  for (const [k, v] of new URLSearchParams(E.QUERY || '')) q.set(k, v);
  await send('Page.navigate', { url: `http://localhost:${PORT}/?${q}` });
  for (let i = 0; i < 60 && !(await evaluate("!!document.getElementById('play') && !document.getElementById('play').disabled")); i++) await sleep(500);
  if (name === 'cruise') {
    if (!existsSync(SAVE_DIR)) throw new Error(`cruise needs a profile: ${SAVE_DIR} (a UDATA/ tree)`);
    const n = await evaluate(`(async files => { const root = await navigator.storage.getDirectory();
      for (const [p, b] of Object.entries(files)) { const parts = p.split('/'); let d = root;
        for (const n of parts.slice(0, -1)) d = await d.getDirectoryHandle(n, { create: true });
        const w = await (await d.getFileHandle(parts.at(-1), { create: true })).createWritable();
        await w.write(Uint8Array.from(atob(b), c => c.charCodeAt(0))); await w.close(); } return Object.keys(files).length; })(${JSON.stringify(saveFiles(SAVE_DIR))})`, true);
    if (!n) throw new Error(`could not write the profile from ${SAVE_DIR} to OPFS`);
    say(`seeded ${n} save files into OPFS`);
  }
  await evaluate("(document.getElementById('play').click(), window.__L = [], (() => { const o = Module.printErr; Module.print = Module.printErr = t => { __L.push(Date.now() + '\\t' + t); o(t); }; })(), 1)");
  const tBoot = Date.now();
  const tReady = name === 'title'
    ? await waitLog(/\[PATH\].*bg_savegame/, /Stopped:|Aborted/, 240)
    : await waitLog(/\[AUTOSTART\] in Cruise/, /\[AUTOSTART\] (stopped|in game: stopped)|Aborted/, 300);
  say(`\n== ${name}: ready ${((tReady - tBoot) / 1000).toFixed(0)} s after Play; settle ${SETTLE} s, measure ${SECS} s`);
  for (let t = 0; t < SETTLE; t++) { await sleep(1000); await drain(); }
  await evaluate("document.getElementById('log').hidden = true; document.getElementById('canvas').focus(); 1");
  if (name === 'cruise') await key('keyDown', 'ArrowUp', 38);
  const gpuPid = chromeProcs().find(p => p.type === 'gpu-process')?.pid;
  const c0 = chromeProcs(), t0 = Date.now();
  const gpuP = gpuPid ? gpuSampler(gpuPid, SECS) : Promise.resolve(NaN);
  for (let t = 0; t < SECS; t++) { await sleep(1000); await drain(); }
  const c1 = chromeProcs(), t1 = Date.now(), gpu3d = await gpuP;
  await sleep(1500); await drain();
  const cpu = type => { const d = c1.filter(p => p.type === type).map(p => p.cpu - (c0.find(q => q.pid === p.pid)?.cpu ?? p.cpu)); return d.length ? Math.max(...d) / ((t1 - t0) / 1000) : NaN; };
  const P = parsePerf(t0, t1), fr = 1000 / P.fps;
  say(`avg fps ${P.fps.toFixed(1)} (${P.reports} reports; main loop ${P.passes.toFixed(1)} passes/s) -> ${fr.toFixed(1)} ms/frame`);
  say(`guest lock: main waits ${P.lockWait.toFixed(0)}% (${(P.lockWait / 100 * fr).toFixed(1)} ms/frame), lock held ${P.lockHeld.toFixed(0)}%, ${P.handovers.toFixed(0)} hand-overs/s`);
  say(`GL thread: busy ${(100 - P.glIdle).toFixed(0)}% (${((100 - P.glIdle) / 100 * fr).toFixed(1)} ms/frame), executor held back ${P.execHeld.toFixed(0)}%; ${P.draws.toFixed(0)} draws -> ${P.calls.toFixed(0)} GL draw calls/frame; phases ${P.glPhaseMs.toFixed(1)} ms/frame [${P.glPhases}]`);
  say(`GPU: 3D engine ${gpu3d.toFixed(0)}% (${(gpu3d / 100 * fr).toFixed(1)} ms/frame); GPU process CPU ${(100 * cpu('gpu-process')).toFixed(0)}% of a core; renderer CPU ${(100 * cpu('renderer')).toFixed(0)}% (all workers, of a core)`);
  const res = { name, ...P, gpu3d, gpuProcCpu: cpu('gpu-process'), rendererCpu: cpu('renderer') };
  if (PROFILE) {
    const { threads, global } = await profile(PSECS);
    say(`profile ${PSECS} s (busy = not idle and not in a futex wait):`);
    const txt = [];
    for (const t of threads.filter(t => t.busy > 0.05)) {
      say(`  ${t.role.padEnd(16)} ${t.id} busy ${(100 * t.busy).toFixed(0)}% (${(t.busy * fr).toFixed(1)} ms/frame)  incl: ${KEY.filter(f => t.has(f) > 0.005).map(f => `${f} ${(100 * t.has(f)).toFixed(0)}%`).join(', ')}`);
      say(`      self: ${[...t.self].filter(([f]) => !IDLE.test(f)).sort((a, b) => b[1] - a[1]).slice(0, 5).map(([f, c]) => `${f} ${(100 * c / t.total).toFixed(0)}% <- ${[...(t.caller.get(f) || [])].sort((a, b) => b[1] - a[1])[0]?.[0] ?? '?'}`).join(', ')}`);
      txt.push(`== ${t.role} ${t.id} busy ${(100 * t.busy).toFixed(1)}% (${t.total} samples)`,
        ...[...t.self].sort((a, b) => b[1] - a[1]).slice(0, 30).map(([f, c]) => `  ${(100 * c / t.total).toFixed(1)}% ${f}  <- ${[...(t.caller.get(f) || [])].sort((a, b) => b[1] - a[1]).slice(0, 2).map(([g, n]) => `${g} ${(100 * n / c).toFixed(0)}%`).join(', ')}`));
    }
    const top = [...global].sort((a, b) => b[1] - a[1]);
    say(`  top functions (self, % of one thread's time): ${top.slice(0, 12).map(([f, s]) => `${f} ${(100 * s).toFixed(1)}%`).join(', ')}`);
    const lifted = top.filter(([f]) => /^sub_[0-9A-F]{8}/.test(f));
    if (lifted.length) say(`  top lifted: ${lifted.slice(0, 10).map(([f, s]) => `${f} ${(100 * s).toFixed(1)}%`).join(', ')}`);
    writeFileSync(join(OUT, `prof-${name}.txt`), txt.join('\n') + '\n');
  }
  if (name === 'cruise') { await key('keyUp', 'ArrowUp', 38); writeFileSync(join(OUT, 'cruise.jpg'), Buffer.from((await send('Page.captureScreenshot', { format: 'jpeg', quality: 60 })).result.data, 'base64')); }
  else writeFileSync(join(OUT, 'title.jpg'), Buffer.from((await send('Page.captureScreenshot', { format: 'jpeg', quality: 60 })).result.data, 'base64'));
  writeFileSync(join(OUT, `log-${name}.txt`), log.map(([t, s]) => `${t}\t${s}`).join('\n') + '\n');
  return res;
}

try {
  say(`web_bench: ${relative(ROOT, BUILD)} on :${PORT}, ${PHASES.join(' + ')}, out ${relative(ROOT, OUT)}`);
  server = spawn('py', [join(ROOT, 'tools/web_serve.py'), '--root', join(ROOT, 'web'), '--root', BUILD, '--disc', join(ROOT, E.DISC || 'game-remix-usa'), '--port', String(PORT)], { cwd: ROOT, stdio: 'ignore' });
  rmSync(PROF_DIR, { recursive: true, force: true });
  spawn(CHROME, [`--remote-debugging-port=${CDP}`, '--remote-allow-origins=*', `--user-data-dir=${PROF_DIR}`,
    '--use-angle=d3d11', '--enable-gpu', '--ignore-gpu-blocklist', '--window-size=1320,900', '--window-position=40,40',
    '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows',
    '--no-first-run', '--no-default-browser-check', '--autoplay-policy=no-user-gesture-required', 'about:blank'],
    { stdio: 'ignore', detached: true }).unref();
  await connect();
  const results = [];
  for (const p of PHASES) {
    for (let tries = 1; ; tries++) {
      try { results.push(await phase(p)); break; }
      catch (e) { if (p !== 'cruise' || tries >= 3 || !/stopped/.test(e.message)) throw e; say(`cruise try ${tries}: ${e.message}; again`); }
    }
  }
  writeFileSync(join(OUT, 'results.json'), JSON.stringify(results, null, 1));
} catch (e) {
  say(`FAILED: ${e.message}`); process.exitCode = 1;
} finally {
  writeFileSync(join(OUT, 'summary.txt'), summary.join('\n') + '\n');
  cleanup();
  process.exit();
}
