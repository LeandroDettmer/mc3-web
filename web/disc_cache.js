// The disc cache: the disc's files kept in the browser (OPFS), read in 1 MiB
// blocks. After the design of nfsu2-web's game data cache (web/src/host/
// datacache.ts, io-worker.ts, predownload-worker.ts; MIT, th0mk).
//
// OPFS folder "mc3-disc-cache": one sparse file per file of a disc, named by
// the disc, its path and size (not mtime: the same disc from either server,
// or copied, keeps its cache; a same-size change needs "Clear"), and next to
// it a ".have" map of one byte per block. A block's data is written before
// its byte in the map. A host on this machine or the LAN reads the disc
// straight over HTTP, no cache (enabled); MC3_DISC_CACHE=1 / =0 forces it.
// The predownload runs only on the owner's click, uses 3 connections and
// stops when a game announces itself on the 'mc3-disc' channel. Every disc
// request carries credentials: a deploy's disc server on another origin
// (config.json, web/index.html) may sit behind a sign-in cookie.
//
// One file, three users:
//   - the game (--pre-js, CMakeLists.txt): the WASMFS disc backend
//     (web/wasmfs_fetch.js) reads through the cache on its own thread;
//   - the start page (<script>): what of a disc is kept, clearing it;
//   - the predownload worker (new Worker('disc_cache.js', {name:
//     'mc3-predownload'})): every block of a disc into the cache.
//     Page -> worker { t: 'start', disc: <disc url> } | { t: 'stop' };
//     worker -> page { t: 'progress' | 'done', have, total } (bytes),
//     { t: 'stopped' }, { t: 'error', msg }. What was written stays.
var MC3DiscCache = (() => {
  const BLOCK = 1 << 20, DIR = 'mc3-disc-cache';
  const blockCount = size => Math.ceil(size / BLOCK);
  const blockLen = (size, bi) => Math.min(BLOCK, size - bi * BLOCK);
  /** a disc's name in cache keys: the last part of its URL's path */
  const discName = url => decodeURIComponent(new URL(url, location.href).pathname.replace(/\/+$/, '').split('/').pop());
  /** a manifest file {p, s, m} of the disc named `disc` -> its cache file's name (path and size, not mtime) */
  const key = (disc, f) => `${disc}/${f.p}`.replace(/[^A-Za-z0-9._-]/g, '_') + `.${f.s}`;
  const fileUrl = (disc, p) => disc.replace(/\/+$/, '') + '/' + p.split('/').map(encodeURIComponent).join('/');
  const LANES = 3, FETCH_MS = 15000, TRIES = 6;   // predownload connections; a read with no answer for FETCH_MS is tried again
  const SCHEME = '2', SCHEME_FILE = 'scheme', MODE_FILE = 'mode';
  // localhost, *.local, loopback, LAN: the disc is already on this machine
  const LOCAL = /^(localhost|.*\.local|127(\.\d{1,3}){3}|\[?::1\]?|10(\.\d{1,3}){3}|192\.168(\.\d{1,3}){2}|172\.(1[6-9]|2\d|3[01])(\.\d{1,3}){2})$/i;
  /** is the disc cache used? `force` is MC3_DISC_CACHE ('1' on, '0' off), else the host decides */
  function enabled(force, host = location.hostname) {
    if (force === '1' || force === '0') return force === '1';
    return !LOCAL.test(host);
  }

  /** the disc's files [{p, s, m}], from the server's <disc>.manifest */
  async function manifest(disc) {
    const r = await fetch(disc.replace(/\/+$/, '') + '.manifest', { cache: 'no-cache', credentials: 'include' });
    if (!r.ok) throw new Error(`no manifest for ${disc} (HTTP ${r.status})`);
    return (await r.json()).files;
  }
  async function dir(create) {
    const d = await (await navigator.storage.getDirectory()).getDirectoryHandle(DIR, { create });
    if (create) await upgrade(d);
    return d;
  }
  /** a folder of an older key scheme (keys with mtimes) is emptied, once; the scheme file marks the current one */
  async function upgrade(d) {
    try { if (await (await (await d.getFileHandle(SCHEME_FILE)).getFile()).text() === SCHEME) return; } catch { /* none yet */ }
    const names = [];
    for await (const [n] of d) names.push(n);
    for (const n of names) {
      try { await d.removeEntry(n); }
      catch (e) { if (e.name !== 'NotFoundError') throw e; }   // a file in use: the next start tries again
    }
    const w = await (await d.getFileHandle(SCHEME_FILE, { create: true })).createWritable();
    await w.write(SCHEME); await w.close();
  }
  /** the page's choice for this run, for the game's disc thread (it cannot read the page's settings): '1', '0', or '' for the host's default */
  async function setForce(force) {
    const d = await dir(true), f = force === '1' || force === '0' ? force : '';
    if (!f) { try { await d.removeEntry(MODE_FILE); } catch (e) { if (e.name !== 'NotFoundError') throw e; } return; }
    const w = await (await d.getFileHandle(MODE_FILE, { create: true })).createWritable();
    await w.write(f); await w.close();
  }
  /** is the cache used in this run: setForce's choice, else the host of this thread decides */
  async function use() {
    let force = '';
    try { force = await (await (await (await dir(false)).getFileHandle(MODE_FILE)).getFile()).text(); } catch { /* none: the host decides */ }
    return enabled(force);
  }

  // Sync handles: dedicated workers only. 'readwrite-unsafe' lets the game and
  // the predownload worker hold the same file (Chrome 121+; elsewhere a second
  // open fails and that reader goes to the network).
  async function syncHandle(fh) {
    try { return await fh.createSyncAccessHandle({ mode: 'readwrite-unsafe' }); }
    catch (e) { if (e.name !== 'TypeError') throw e; return fh.createSyncAccessHandle(); }
  }
  /** a cache file's handles and map {h, haveH, have}; the map made empty if it does not fit the file */
  async function open(d, k, size) {
    const h = await syncHandle(await d.getFileHandle(k, { create: true }));
    let haveH;
    try { haveH = await syncHandle(await d.getFileHandle(k + '.have', { create: true })); }
    catch (e) { h.close(); throw e; }
    const have = new Uint8Array(blockCount(size));
    if (haveH.getSize() === have.length) haveH.read(have, { at: 0 });
    else { haveH.truncate(0); haveH.write(have, { at: 0 }); }
    return { h, haveH, have };
  }
  function close(cf) { if (cf) try { cf.h.close(); cf.haveH.close(); } catch { /* closed */ } }
  /** block bi is in the cache file now (throws if not: no room, say) */
  function put(cf, bi, b) {
    const n = cf.h.write(b, { at: bi * BLOCK });
    if (n !== b.length) throw new Error(`${n} of ${b.length} bytes written`);
    cf.have[bi] = 1; cf.haveH.write(cf.have.subarray(bi, bi + 1), { at: bi });
  }

  /** block bi of a file of `size` bytes at `url`, over HTTP. A try with no answer for FETCH_MS is abandoned and tried again, so a stalled connection never blocks a read; warn(msg) logs each retry */
  async function fetchBlock(url, bi, size, warn = () => {}) {
    const start = bi * BLOCK, len = blockLen(size, bi);
    for (let attempt = 1; ; attempt++) {
      const ac = new AbortController(), timer = setTimeout(() => ac.abort(), FETCH_MS);
      try {
        const r = await fetch(url, { headers: { Range: `bytes=${start}-${start + len - 1}` }, credentials: 'include', signal: ac.signal });
        if (r.status !== 206 && r.status !== 200) throw Object.assign(new Error(`HTTP ${r.status}`), { status: r.status });
        let b = new Uint8Array(await r.arrayBuffer());
        if (r.status === 200) b = b.slice(start, start + len);       // a server without ranges: the whole file
        if (b.length !== len) throw new Error(`${b.length} of ${len} bytes`);
        return b;
      } catch (e) {
        const why = ac.signal.aborted ? `no data for ${FETCH_MS / 1000} s` : String(e.message || e);
        if (attempt >= TRIES || (e.status && e.status < 500)) throw new Error(why);
        warn(`${why}: try ${attempt} of ${TRIES} again`);
        await new Promise(res => setTimeout(res, 250 * 2 ** attempt));
      } finally { clearTimeout(timer); }
    }
  }

  /** What of the disc the cache holds: { total, have } bytes, files [{f, k, have: map | null}]. Async reads only. */
  async function scan(disc) {
    const files = await manifest(disc), name = discName(disc), s = { total: 0, have: 0, files: [] };
    const kept = new Map();
    try { for await (const [n, h] of await dir(false)) if (n.endsWith('.have')) kept.set(n, h); } catch { /* none yet */ }
    for (const f of files) {
      const k = key(name, f);
      let have = null;
      s.total += f.s;
      if (kept.has(k + '.have')) {
        const m = new Uint8Array(await (await kept.get(k + '.have').getFile()).arrayBuffer());
        if (m.length === blockCount(f.s)) have = m;
      }
      if (have) for (let bi = 0; bi < have.length; bi++) if (have[bi]) s.have += blockLen(f.s, bi);
      s.files.push({ f, k, have });
    }
    return s;
  }
  /** the whole cache, every disc (not while a game or the predownload has it open) */
  async function clear() {
    try { await (await navigator.storage.getDirectory()).removeEntry(DIR, { recursive: true }); }
    catch (e) { if (e.name !== 'NotFoundError') throw e; }
  }

  // ---- the predownload worker
  async function predownload(disc, post, stopped) {
    const s = await scan(disc), d = await dir(true);
    let have = s.have, shown = 0;
    const progress = now => { const t = Date.now(); if (now || t - shown >= 250) { shown = t; post({ t: 'progress', have, total: s.total }); } };
    progress(true);
    const jobs = [];
    for (const x of s.files) {
      x.left = 0;
      for (let bi = 0; bi < blockCount(x.f.s); bi++) if (!x.have || !x.have[bi]) { jobs.push([x, bi]); x.left++; }
    }
    let next = 0;
    const lane = async () => {
      while (!stopped() && next < jobs.length) {
        const [x, bi] = jobs[next++];
        try {
          const cf = await (x.cf ??= open(d, x.k, x.f.s));
          if (!cf.have[bi]) {                     // the game may have read it meanwhile
            put(cf, bi, await fetchBlock(fileUrl(disc, x.f.p), bi, x.f.s, m => console.warn('[WEB] predownload: ' + m)));
            have += blockLen(x.f.s, bi); progress();
          }
        } finally { if (--x.left === 0 && x.cf) close(await x.cf.catch(() => null)); }
      }
    };
    try { await Promise.all(Array.from({ length: LANES }, lane)); }
    finally { for (const x of s.files) if (x.left > 0 && x.cf) close(await x.cf.catch(() => null)); }
    progress(true);
    return { have, total: s.total };
  }
  if (typeof WorkerGlobalScope !== 'undefined' && self.name === 'mc3-predownload') {
    let stop = false, busy = false;
    const halt = () => { stop = true; if (!busy) postMessage({ t: 'stopped' }); };
    // a game reading the disc (any tab) announces itself: the download stops, the game gets the connections
    new BroadcastChannel('mc3-disc').onmessage = e => { if (e.data && e.data.t === 'game') halt(); };
    self.onmessage = e => {
      if (e.data.t === 'stop') { halt(); return; }
      if (busy) return;
      busy = true;
      predownload(e.data.disc, m => postMessage(m), () => stop).then(
        r => postMessage(stop ? { t: 'stopped' } : { t: 'done', ...r }),
        err => postMessage(stop ? { t: 'stopped' } : { t: 'error', msg: err.name === 'QuotaExceededError' ? 'the browser has no more room for it' : String(err.message || err) }),
      ).finally(() => { busy = false; });
    };
  }

  return { BLOCK, blockLen, discName, key, manifest, dir, open, close, put, fetchBlock, scan, clear, enabled, setForce, use };
})();
