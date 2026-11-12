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
// The predownload runs only on the owner's click, uses 3 connections (runs of up to 16 missing blocks a request) and
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
  // The saves (OPFS UDATA) share this origin with up to ~4.4 GiB of cache: best-effort storage is evicted whole under
  // disk pressure, saves included. Persistent storage is not (Chrome grants it silently or not; Firefox asks once).
  if (typeof document == 'object' && navigator.storage?.persist)
    navigator.storage.persisted().then(p => p || navigator.storage.persist()).catch(() => {});
  const blockCount = size => Math.ceil(size / BLOCK);
  const blockLen = (size, bi) => Math.min(BLOCK, size - bi * BLOCK);
  /** a disc's name in cache keys: the last part of its URL's path */
  const discName = url => decodeURIComponent(new URL(url, location.href).pathname.replace(/\/+$/, '').split('/').pop());
  /** a manifest file {p, s, m} of the disc named `disc` -> its cache file's name (path and size, not mtime) */
  const key = (disc, f) => `${disc}/${f.p}`.replace(/[^A-Za-z0-9._-]/g, '_') + `.${f.s}`;
  const fileUrl = (disc, p) => disc.replace(/\/+$/, '') + '/' + p.split('/').map(encodeURIComponent).join('/');
  const LANES = 3, RUN = 16, FETCH_MS = 15000, TRIES = 6;   // predownload connections, blocks per request; a read that gets no data for FETCH_MS is tried again (fetchBlocks)
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
  /** an OPFS failure named by its call, for the log: message 'opfs: <op> <file>: <error>', the error's own name kept */
  const opfsErr = (op, f, e) => Object.assign(new Error(`opfs: ${op} ${f}: ${e?.name || 'Error'}: ${e?.message || e}`), { name: e?.name || 'Error', cause: e });
  /** a cache file's handles and map {k, h, haveH, have}; the map made empty if it does not fit the file */
  async function open(d, k, size) {
    let op = 'getFileHandle', f = k, h = null, haveH = null;
    try {
      const fh = await d.getFileHandle(k, { create: true });
      op = 'createSyncAccessHandle'; h = await syncHandle(fh);
      op = 'getFileHandle'; f = k + '.have';
      const hh = await d.getFileHandle(f, { create: true });
      op = 'createSyncAccessHandle'; haveH = await syncHandle(hh);
      const have = new Uint8Array(blockCount(size));
      op = 'read';
      if (haveH.getSize() === have.length) haveH.read(have, { at: 0 });
      else { op = 'write'; haveH.truncate(0); haveH.write(have, { at: 0 }); }
      return { k, h, haveH, have };
    } catch (e) {
      try { h?.close(); haveH?.close(); } catch { /* closed */ }
      throw opfsErr(op, f, e);
    }
  }
  function close(cf) { if (cf) try { cf.h.close(); cf.haveH.close(); } catch { /* closed */ } }
  /** block bi is in the cache file now (throws if not: no room, say) */
  function put(cf, bi, b) {
    let n;
    try { n = cf.h.write(b, { at: bi * BLOCK }); } catch (e) { throw opfsErr('write', `${cf.k} block ${bi}`, e); }
    if (n !== b.length) throw new Error(`opfs: write ${cf.k} block ${bi}: ${n} of ${b.length} bytes written`);
    cf.have[bi] = 1;
    try { cf.haveH.write(cf.have.subarray(bi, bi + 1), { at: bi }); } catch (e) { throw opfsErr('write', `${cf.k}.have`, e); }
  }

  // Direct reads (signer): a deploy's disc server (the Worker) may hand out presigned URLs straight into its bucket
  // (POST <disc>.sign, body: file paths one a line -> {exp, urls: {path: url}}), so a Range read goes to the bucket,
  // not through the server, which then sees a few requests a session instead of one a block. SIGN_BATCH paths a call.
  const SIGN_BATCH = 100, SIGN_EARLY = 10 * 60e3, SIGN_FRESH = 60e3;
  /** A disc's direct reads for the files `paths`: url(p) -> the getter fetchBlocks takes, async (prev) -> {u, direct}.
   * Everything is signed on the first read, in parallel batches; again near the end of the signatures or when the
   * bucket refuses one that is not fresh (403). Without signing (a local server, a server with no .sign, a failed
   * call) or once the bucket refused a fresh signature or gave no answer at all (CORS, say), the server's own URL. */
  function signer(disc, paths, warn = m => console.warn('[WEB] ' + m)) {
    const base = disc.replace(/\/+$/, '');
    let urls = new Map(), exp = 0, at = 0, job = null, off = LOCAL.test(new URL(base, location.href).hostname);
    const stop = why => { if (!off) { off = true; warn(`disc: direct reads off (${why}); reads go through the disc server`); } };
    const sign = () => job ??= (async () => {
      const chunks = [];
      for (let i = 0; i < paths.length; i += SIGN_BATCH) chunks.push(paths.slice(i, i + SIGN_BATCH));
      try {
        const rs = await Promise.all(chunks.map(async c => {
          const r = await fetch(base + '.sign', { method: 'POST', body: c.join('\n'), headers: { 'Content-Type': 'text/plain' }, credentials: 'include' });
          if (!r.ok) throw Object.assign(new Error(`HTTP ${r.status}`), { status: r.status });
          return r.json();
        }));
        urls = new Map(rs.flatMap(r => Object.entries(r.urls)));
        exp = Math.min(...rs.map(r => r.exp)); at = Date.now();
        warn(`disc: direct reads, ${urls.size} files signed until ${new Date(exp).toTimeString().slice(0, 5)}`);
      } catch (e) {
        if (e.status === 404 || e.status === 405 || e.status === 501) off = true;   // no signing on this server: quietly
        else stop(`signing failed: ${e.message || e}`);
      } finally { job = null; }
    })();
    return {
      url: p => async prev => {
        const own = fileUrl(disc, p);
        if (prev?.direct && !prev.stalled && !(prev.status >= 500)) {   // the bucket refused it, or no answer at all
          if (prev.status === 403 && Date.now() - at > SIGN_FRESH) await sign();
          else if (prev.status === 403 || !prev.status) stop(prev.status ? 'the bucket refused a fresh signature' : 'the bucket did not answer');
          else return { u: own, direct: false };    // 404, say: the server's answer decides
        }
        if (!off && Date.now() > exp - SIGN_EARLY) await sign();
        const u = !off && urls.get(p);
        return u ? { u, direct: true } : { u: own, direct: false };
      },
    };
  }

  /** blocks bi .. bi+n-1 of a file of `size` bytes at `url`, in one Range request; onBlock(j, bytes) as each block has
   * come (BLOCK bytes, the file's last shorter), all of them in an array at the end. A try that gets no data for
   * `stallMs` (no answer, or the body stops coming) is abandoned and tried again from the first block not yet in, so
   * a stalled connection never blocks a read for long, while a slow one that keeps sending is never cut; warn(msg)
   * logs each retry. `signal` (optional) abandons the whole fetch (AbortError); the blocks already in went to onBlock.
   * `url` is a URL, or a signer's getter (a URL a try: a refused direct one is tried again at once, as it says). */
  async function fetchBlocks(url, bi, n, size, { warn = () => {}, stallMs = FETCH_MS, signal, onBlock = () => {} } = {}) {
    const out = [], end = Math.min(size, (bi + n) * BLOCK);
    let failed = null;                            // onBlock threw (no room, say): not tried again
    let prev;                                     // the last try's failure, for the getter
    for (let attempt = 1; ; attempt++) {
      const at = (bi + out.length) * BLOCK, had = out.length;
      const ac = new AbortController(), stop = () => ac.abort();
      signal?.addEventListener('abort', stop);
      let timer, src = { u: url, direct: false };
      const arm = () => { clearTimeout(timer); timer = setTimeout(() => ac.abort(), stallMs); };
      try {
        if (signal?.aborted) throw new DOMException('stopped', 'AbortError');
        if (typeof url === 'function') src = await url(prev);
        arm();
        // direct: the bucket wants no cookie (its CORS allows none)
        const r = await fetch(src.u, { headers: { Range: `bytes=${at}-${end - 1}` }, credentials: src.direct ? 'omit' : 'include', signal: ac.signal });
        if (r.status !== 206 && r.status !== 200) throw Object.assign(new Error(`HTTP ${r.status}`), { status: r.status });
        arm();
        const rd = r.body.getReader();
        let pos = r.status === 200 ? 0 : at, cur = null, fill = 0;   // a server without ranges: the whole file
        try {
          while (pos < end) {                     // read as it comes: each piece restarts the stall timer
            const { done, value } = await rd.read();
            if (done) break;
            arm();
            let v = value;
            if (pos < at) { const s = Math.min(v.length, at - pos); v = v.subarray(s); pos += s; }
            while (v.length && pos < end) {
              const j = bi + out.length, len = blockLen(size, j);
              cur ??= new Uint8Array(len);
              const take = Math.min(v.length, len - fill);
              cur.set(v.subarray(0, take), fill); fill += take; pos += take; v = v.subarray(take);
              if (fill === len) {
                out.push(cur);
                try { onBlock(j, cur); } catch (e) { failed = e; throw e; }
                cur = null; fill = 0;
              }
            }
          }
        } finally { rd.cancel().catch(() => {}); }
        if (out.length < n) throw new Error(`${pos - at} of ${end - at} bytes`);
        return out;
      } catch (e) {
        if (failed) throw failed;
        if (signal?.aborted) throw new DOMException('stopped', 'AbortError');
        const why = ac.signal.aborted ? `no data for ${stallMs / 1000} s` : String(e.message || e);
        prev = { status: e.status, direct: src.direct, stalled: ac.signal.aborted };
        if (out.length > had) attempt = 1;        // it moved on: a fresh count of tries
        if (attempt >= TRIES || (e.status && e.status < 500 && !src.direct)) throw new Error(why);
        warn(`${why}${src.direct ? ' (direct)' : ''}: try ${attempt} of ${TRIES} again`);
        if (!src.direct || prev.stalled || e.status >= 500) await new Promise(res => setTimeout(res, 250 * 2 ** attempt));
      } finally { clearTimeout(timer); signal?.removeEventListener('abort', stop); }
    }
  }
  /** block bi alone (fetchBlocks) */
  const fetchBlock = (url, bi, size, warn, stallMs) => fetchBlocks(url, bi, 1, size, { warn, stallMs }).then(b => b[0]);

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
  // A job is a run of at most RUN blocks a file misses, one request (fetchBlocks); `signal` stops it at once (the
  // blocks already in are kept).
  async function predownload(disc, post, signal) {
    const s = await scan(disc), d = await dir(true), sig = signer(disc, s.files.map(x => x.f.p));
    let have = s.have, shown = 0;
    const progress = now => { const t = Date.now(); if (now || t - shown >= 250) { shown = t; post({ t: 'progress', have, total: s.total }); } };
    progress(true);
    const jobs = [];
    for (const x of s.files) {
      x.left = 0;
      for (let bi = 0; bi < blockCount(x.f.s); bi++) {
        if (x.have && x.have[bi]) continue;
        const job = jobs[jobs.length - 1];
        if (job && job[0] === x && job[1] + job[2] === bi && job[2] < RUN) job[2]++;
        else { jobs.push([x, bi, 1]); x.left++; }
      }
    }
    const ac = new AbortController(), halt = () => ac.abort();   // stopped, or one lane failed: every lane stops
    signal.addEventListener('abort', halt);
    let next = 0;
    const lane = async () => {
      while (!ac.signal.aborted && next < jobs.length) {
        const [x, bi, n] = jobs[next++];
        try {
          const cf = await (x.cf ??= open(d, x.k, x.f.s));
          for (let j = bi; j < bi + n && !ac.signal.aborted;) {   // the game may have read some meanwhile: what is still missing
            if (cf.have[j]) { j++; continue; }
            let m = 1;
            while (j + m < bi + n && !cf.have[j + m]) m++;
            await fetchBlocks(sig.url(x.f.p), j, m, x.f.s, {
              signal: ac.signal, warn: w => console.warn('[WEB] predownload: ' + w),
              onBlock: (k, b) => { if (!cf.have[k]) { put(cf, k, b); have += blockLen(x.f.s, k); progress(); } },
            });
            j += m;
          }
        } catch (e) { halt(); throw e; }
        finally { if (--x.left === 0 && x.cf) close(await x.cf.catch(() => null)); }
      }
    };
    let r;
    try { r = await Promise.allSettled(Array.from({ length: LANES }, lane)); }
    finally {
      signal.removeEventListener('abort', halt);
      for (const x of s.files) if (x.left > 0 && x.cf) close(await x.cf.catch(() => null));
    }
    const bad = r.find(x => x.status === 'rejected' && x.reason?.name !== 'AbortError');
    if (bad) throw bad.reason;
    progress(true);
    return { have, total: s.total };
  }
  if (typeof WorkerGlobalScope !== 'undefined' && self.name === 'mc3-predownload') {
    let stop = false, busy = false;
    const ac = new AbortController();
    const halt = () => { stop = true; ac.abort(); if (!busy) postMessage({ t: 'stopped' }); };
    // a game reading the disc (any tab) announces itself: the download stops, the game gets the connections
    new BroadcastChannel('mc3-disc').onmessage = e => { if (e.data && e.data.t === 'game') halt(); };
    self.onmessage = e => {
      if (e.data.t === 'stop') { halt(); return; }
      if (busy) return;
      busy = true;
      predownload(e.data.disc, m => postMessage(m), ac.signal).then(
        r => postMessage(stop ? { t: 'stopped' } : { t: 'done', ...r }),
        err => postMessage(stop ? { t: 'stopped' } : { t: 'error', msg: err.name === 'QuotaExceededError' ? 'the browser has no more room for it' : String(err.message || err) }),
      ).finally(() => { busy = false; });
    };
  }
  return { BLOCK, blockLen, discName, key, fileUrl, manifest, dir, open, close, put, signer, fetchBlock, fetchBlocks, scan, clear, enabled, setForce, use };
})();
