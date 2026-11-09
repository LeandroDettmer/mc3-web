/**
 * The disc's WASMFS backend: replaces the JS half of Emscripten's fetch
 * backend (src/lib/libwasmfs_fetch.js), linked as a --js-library
 * (CMakeLists.txt). Its C++ half stays: one dedicated thread per backend runs
 * these async functions while the game thread that asked blocks, so that
 * thread is the disc's I/O worker (the design of nfsu2-web's io-worker.ts,
 * MIT, th0mk).
 *
 * A read goes, by 1 MiB block (web/disc_cache.js):
 *   memory  an LRU of the blocks last read, MEM_BYTES at most;
 *   OPFS    the disc cache, a sparse file + block map per disc file, kept
 *           across visits (the start page fills all of it ahead: "Download
 *           the whole game now"). Not on a local address, nor with
 *           MC3_DISC_CACHE=0 (web/disc_cache.js use());
 *   HTTP    a Range request, then written to OPFS. To the bucket itself when
 *           the disc server signs URLs for it (web/disc_cache.js signer),
 *           else to the server.
 * So memory stays bounded however much of a 4.5 GB disc a session reads
 * (the stock backend kept every chunk it ever fetched).
 *
 * Background fill (prefetch): what the game reads mid-play goes into the
 * OPFS cache while it plays, so those reads find OPFS, not the network (a
 * block fetched on demand stalls the game thread that asked, and a car
 * model read in free roam stalls the whole guest). In order (prefetchJobs):
 * assets_x.dat_2 (cars), the streamed sound banks (music mx.xwb first),
 * the Flash screens smallest first (pause, garage, rewards, menus, then the
 * showroom's per-car screens and the loading screens), the rest of
 * assets_x*; at most PREFETCH_BUDGET_MB still to fetch and what the browser
 * says it has room for. It starts PREFETCH_DELAY_MS after the first disc
 * file, fetches up to PREFETCH_RUN missing blocks in one request (a small
 * file whole) and only when the game has had no read of its own on the
 * network for IDLE_MS; blocks already kept (earlier visits too) are skipped.
 * Such a request is dropped the moment the game reads the network itself, or
 * wants one of its blocks more than a block past the one coming (that read
 * then fetches its block alone); the blocks already in are kept. Only when
 * the cache is on. Log lines '[WEB] prefetch: ...'.
 *
 * Readahead: a game read that goes on where its file's last read ended, into
 * a new block, also starts the next READAHEAD blocks not held yet (memory or
 * OPFS) in the background, one request; the game's later read of one joins
 * its fetch (inflight) or finds it in memory. '[WEB] readahead: <file> ...'
 * once a file. A game read of several blocks fetches each run of missing ones
 * in one request; a read of a file of at most SMALL bytes fetches all of it.
 *
 * The game's own network reads give up a try after GAME_STALL_MS without
 * data (and try again); the background ones keep the disc cache's FETCH_MS.
 *
 * Disc files come as <disc url>//<path> (WASMFS joins with '/'); sizes from
 * <disc url>.manifest (tools/web_serve.py), else a HEAD request.
 * Any other URL (the file list, src/web_fs.c) is fetched whole, uncached.
 */
addToLibrary({
  _wasmfs_create_fetch_backend_js__deps: [
    '$wasmFS$backends',
    '_wasmfs_fetch_get_file_url',
    'getenv', '$withStackSave', '$stringToUTF8OnStack', '$UTF8ToString',
  ],
  _wasmfs_create_fetch_backend_js: async function(backend) {
    const C = MC3DiscCache, BLOCK = C.BLOCK;
    // Settings from the game's environment (this runs on a worker, where Module.mc3Env is not):
    // MC3_DISC_MEM_MB (the LRU's size), MC3_PREFETCH=0 (no background fill).
    const env = k => withStackSave(() => { const p = _getenv(stringToUTF8OnStack(k)); return p ? UTF8ToString(p) : ''; });
    const E = { MC3_DISC_MEM_MB: env('MC3_DISC_MEM_MB'), MC3_PREFETCH: env('MC3_PREFETCH') };
    const MEM_BYTES = (Number(E.MC3_DISC_MEM_MB) || 48) << 20;   // the memory LRU (16 in the page's low-memory profile)
    const OPEN_FILES = 128;         // cache files kept open (two sync handles each)
    const mem = new Map();          // "<key>#<block>" -> Uint8Array, least recently used first
    let memBytes = 0;
    const inflight = new Map();     // the same, while being read
    const files = {};               // WASMFS file -> Promise<{url, size, key} | {url, size, data}>
    const manifests = {};           // disc url -> Promise<Map path -> {p, s, m}> | null
    const signers = {};             // disc url -> its direct reads (C.signer), when it has a manifest
    const open = new Map();         // cache key -> Promise<cache file | null>, least recently used first
    let cacheDir = null, cacheOn = null, warned = false;
    const BANKS = ['mx', 'RaceStarters', 'Engine_Revs', 'dx', 'City_Walla'].map(n => `audio/xstreams/${n.toLowerCase()}.xwb`);
    const PREFETCH_DELAY_MS = 15000, IDLE_MS = 300, PREFETCH_BUDGET_MB = 2560;
    const READAHEAD = 2, GAME_STALL_MS = 4000, PREFETCH_RUN = 16, SMALL = 2 * BLOCK;
    let netReads = 0, lastNet = 0, prefetching = false;   // the game's own reads on the network, when the last ended
    const bgRuns = new Set();       // the background fill's requests in progress (fetchRun)
    const announce = new BroadcastChannel('mc3-disc');   // the predownload (web/disc_cache.js) stops for a game
    announce.postMessage({ t: 'game' });
    self.mc3DiscIO = { mem, inflight, files, open };   // for a debugger attached to this thread

    async function describe(url) {
      const i = url.indexOf('//', url.indexOf('://') + 3);
      if (i < 0) {
        const r = await fetch(url, { credentials: 'include' });   // a deploy's disc server may want its sign-in cookie
        if (!r.ok) throw r.status;
        const data = new Uint8Array(await r.arrayBuffer());
        return { url, size: data.length, data };
      }
      const disc = url.slice(0, i), rel = url.slice(i + 2);
      const m = await (manifests[disc] ??= C.manifest(disc).then(fs => new Map(fs.map(f => [f.p, f])), () => null));
      if (m && !prefetching && E.MC3_PREFETCH !== '0') { prefetching = true; prefetch(disc, m).catch(e => err(`[WEB] prefetch stopped: ${e}`)); }
      const sig = m && (signers[disc] ??= C.signer(disc, [...m.keys()], s => err('[WEB] ' + s)));
      let f = m && m.get(rel);
      if (!f) {
        const r = await fetch(url, { method: 'HEAD', credentials: 'include' });
        if (!r.ok) throw r.status;
        f = { p: rel, s: +r.headers.get('Content-Length') };
      }
      return { url, src: sig && m.has(rel) ? sig.url(rel) : url, size: f.s, key: C.key(C.discName(disc), f) };
    }
    function info(file) {
      return files[file] ??= describe(UTF8ToString(__wasmfs_fetch_get_file_url(file)))
        .catch(e => { delete files[file]; throw e; });
    }

    // The cache folder; null without the cache (a local address, MC3_DISC_CACHE=0, no OPFS).
    async function cacheFolder() {
      cacheOn ??= C.use();
      if (!(await cacheOn)) return null;
      cacheDir ??= C.dir(true).catch(e => { err(`[WEB] disc cache unavailable (${e}): the disc is read from the server`); return null; });
      return cacheDir;
    }
    // The file's cache file; null without the cache, or OPFS failed (no room, an open that failed).
    async function cacheFile(fi) {
      const d = await cacheFolder();
      if (!d) return null;
      let p = open.get(fi.key);
      if (p) { open.delete(fi.key); open.set(fi.key, p); return p; }
      p = C.open(d, fi.key, fi.size).catch(e => { err(`[WEB] disc cache: ${fi.key}: ${e}`); return null; });
      open.set(fi.key, p);
      if (open.size > OPEN_FILES) {
        const [k, q] = open.entries().next().value;
        open.delete(k); q.then(C.close);
      }
      return p;
    }

    function remember(k, b) {
      mem.set(k, b); memBytes += b.length;
      for (const [ok, ob] of mem) {
        if (memBytes <= MEM_BYTES) break;
        mem.delete(ok); memBytes -= ob.length;
      }
    }
    async function load(fi, bi) {
      const len = C.blockLen(fi.size, bi);
      let cf = await cacheFile(fi);
      if (cf && cf.have[bi]) {
        try {
          const b = new Uint8Array(len);
          if (cf.h.read(b, { at: bi * BLOCK }) === len) return b;
        } catch { /* closed meanwhile: from the server */ }
      }
      let b;
      netReads++; stopBackground();
      try { b = await C.fetchBlock(fi.src, bi, fi.size, m => err(`[WEB] disc read stalled: ${fi.url} block ${bi}: ${m}`), GAME_STALL_MS); }
      finally { netReads--; lastNet = performance.now(); }
      if ((cf = await cacheFile(fi))) {
        try { C.put(cf, bi, b); }
        catch (e) { if (!warned) { warned = true; err(`[WEB] disc cache: a block was not kept (${e})`); } }
      }
      return b;
    }
    function block(fi, bi) {
      const k = fi.key + '#' + bi;
      const b = mem.get(k);
      if (b) { mem.delete(k); mem.set(k, b); return b; }
      let p = inflight.get(k);
      if (p && p.run) {                           // a block of a run (fetchRun): if the run drops it, this read fetches it
        if (!p.run.game && bi > p.run.next + 1) p.run.ac.abort();   // far down a background run: not worth the wait
        return p.catch(() => block(fi, bi));
      }
      if (!p) {
        p = load(fi, bi).then(b => { remember(k, b); return b; }).finally(() => inflight.delete(k));
        inflight.set(k, p);
      }
      return p;
    }

    function stopBackground() { for (const run of bgRuns) run.ac.abort(); }
    // Blocks bi .. bi+n-1 of fi in one request (C.fetchBlocks). Each is in `inflight` until it has come, so a read
    // of it waits for that, and goes into OPFS as it comes (into memory too for the game's runs, game true). A
    // background run (game false) is dropped by stopBackground and by block (see the header). Resolves to the
    // number of blocks that came; rejects with the fetch's error, or a background block OPFS did not keep.
    async function fetchRun(fi, bi, n, game) {
      const run = { ac: new AbortController(), next: bi, game }, waits = [], puts = [];
      for (let j = bi; j < bi + n; j++) {         // before any await: a read right after this call joins
        const w = { k: fi.key + '#' + j };
        w.p = new Promise((res, rej) => { w.res = res; w.rej = rej; });
        w.p.catch(() => {});                      // nobody may wait for it
        w.p.run = run;
        inflight.set(w.k, w.p); waits.push(w);
      }
      const drop = w => { if (inflight.get(w.k) === w.p) inflight.delete(w.k); };
      let kept = null;
      if (game) { netReads++; stopBackground(); } else bgRuns.add(run);
      try {
        await C.fetchBlocks(fi.src, bi, n, fi.size, {
          signal: run.ac.signal, stallMs: game ? GAME_STALL_MS : undefined,
          warn: m => err(`[WEB] ${game ? 'disc read' : 'prefetch'} stalled: ${fi.url} blocks ${bi}-${bi + n - 1}: ${m}`),
          onBlock: (j, b) => {
            const w = waits[j - bi];
            run.next = j + 1;
            if (game) remember(w.k, b);
            puts.push(cacheFile(fi).then(c => { if (c && !c.have[j]) C.put(c, j, b); }).catch(e => { kept ??= e; }));
            drop(w); w.res(b);
          },
        });
      } catch (e) {
        if (!run.ac.signal.aborted) throw e;      // dropped: the blocks that came are kept
      } finally {
        if (game) { netReads--; lastNet = performance.now(); } else bgRuns.delete(run);
        for (const w of waits.slice(run.next - bi)) { drop(w); w.rej(new Error('dropped')); }
      }
      await Promise.all(puts);
      if (kept) {
        if (!game) throw kept;
        if (!warned) { warned = true; err(`[WEB] disc cache: a block was not kept (${kept})`); }
      }
      return run.next - bi;
    }
    // The blocks lo..hi of fi nobody holds or fetches, one game request (fetchRun) per run of them at least `min`
    // long. Returns the first block asked for, or -1.
    async function fetchRuns(fi, lo, hi, min) {
      const cf = await cacheFile(fi), free = j => {
        const k = fi.key + '#' + j;
        return !mem.has(k) && !inflight.has(k) && !(cf && cf.have[j]);
      };
      let first = -1;
      for (let j = lo; j <= hi;) {
        if (!free(j)) { j++; continue; }
        let n = 1;
        while (j + n <= hi && free(j + n)) n++;
        if (n >= min) { if (first < 0) first = j; fetchRun(fi, j, n, true).catch(() => {}); }   // a failed block is fetched again by its read
        j += n;
      }
      return first;
    }
    // Blocks bi+1 .. bi+READAHEAD of fi into memory in the background, those not held yet (see the header).
    async function readahead(fi, bi) {
      const j = await fetchRuns(fi, bi + 1, Math.min(bi + READAHEAD, Math.ceil(fi.size / BLOCK) - 1), 1);
      if (j >= 0 && !fi.ra) { fi.ra = true; err(`[WEB] readahead: ${fi.url} from block ${j}`); }
    }

    // What to fill, in order (see the header): manifest files.
    function prefetchJobs(m) {
      const all = [...m.values()], lc = f => f.p.toLowerCase();
      const byPath = new Map(all.map(f => [lc(f), f]));
      return [
        byPath.get('assets_x.dat_2'),
        ...BANKS.map(p => byPath.get(p)),
        ...all.filter(f => lc(f).startsWith('flash/')).sort((a, b) => a.s - b.s),
        ...all.filter(f => /^assets_x\.dat(_\d+)?$/.test(lc(f)) && lc(f) !== 'assets_x.dat_2'),
      ].filter(Boolean);
    }
    // Bytes of fi not in the cache, from its block map read without opening the file (an open one's map if it is).
    async function missing(d, fi) {
      let have = null;
      const p = open.get(fi.key);
      if (p) have = (await p)?.have ?? null;
      else try { have = new Uint8Array(await (await (await d.getFileHandle(fi.key + '.have')).getFile()).arrayBuffer()); } catch { /* none yet */ }
      let n = 0;
      for (let bi = 0; bi * BLOCK < fi.size; bi++) if (!have || !have[bi]) n += C.blockLen(fi.size, bi);
      return n;
    }

    // The background fill (see the header).
    async function prefetch(disc, m) {
      await new Promise(r => setTimeout(r, PREFETCH_DELAY_MS));
      const d = await cacheFolder();
      if (!d) return;                             // no cache in this run: nothing to fill
      const name = C.discName(disc), mib = n => (n / BLOCK).toFixed(0);
      let budget = PREFETCH_BUDGET_MB * BLOCK, room = '';
      try {
        const e = await navigator.storage.estimate();
        const free = e.quota - e.usage - 512 * BLOCK;   // leave the browser some room
        if (free < budget) { budget = Math.max(0, free); room = `, the browser's room ${mib(budget)} MiB`; }
      } catch { /* no estimate: the budget alone */ }
      const jobs = [];
      let left = 0, total = 0, skipped = 0, done = 0, shown = 0;
      for (const f of prefetchJobs(m)) {
        const fi = { p: f.p, url: C.fileUrl(disc, f.p), src: signers[disc].url(f.p), size: f.s, key: C.key(name, f) };
        const n = await missing(d, fi);
        total += f.s;
        if (!n) continue;
        if (left + n > budget) { skipped += n; continue; }
        left += n; jobs.push(fi);
      }
      const t0 = performance.now();
      err(`[WEB] prefetch: ${mib(left)} MiB in ${jobs.length} files to fetch in the background, of ${mib(total)} MiB`
        + ` (cars, sound banks, Flash screens, city); budget ${PREFETCH_BUDGET_MB} MiB${room}`
        + (skipped ? `, ${mib(skipped)} MiB left out` : ''));
      if (!left) return;
      const idle = async () => {
        while (netReads > 0 || performance.now() - lastNet < IDLE_MS) await new Promise(r => setTimeout(r, IDLE_MS));
      };
      for (const fi of jobs) {
        for (let bi = 0; bi * BLOCK < fi.size;) {
          let cf = await cacheFile(fi);
          if (!cf) return;
          if (cf.have[bi] || inflight.has(fi.key + '#' + bi)) { bi++; continue; }
          await idle();
          if (!(cf = await cacheFile(fi))) return;
          let n = 0;                              // from bi, the blocks still missing that the game is not reading
          while (n < PREFETCH_RUN && (bi + n) * BLOCK < fi.size && !cf.have[bi + n] && !inflight.has(fi.key + '#' + (bi + n))) n++;
          if (!n) continue;
          const got = await fetchRun(fi, bi, n, false);   // fewer when dropped for the game: the rest after idle()
          for (let j = bi; j < bi + got; j++) done += C.blockLen(fi.size, j);
          bi += got;
          const pct = Math.floor(done * 10 / left);
          if (pct > shown && done < left) { shown = pct; err(`[WEB] prefetch: ${pct * 10}% (${mib(done)} of ${mib(left)} MiB), ${fi.p}`); }
        }
      }
      err(`[WEB] prefetch: done, ${mib(done)} MiB in ${((performance.now() - t0) / 1000).toFixed(0)} s`);
    }

    wasmFS$backends[backend] = {
      allocFile: async (file) => {},
      freeFile: async (file) => { delete files[file]; },
      write: async (file, buffer, length, offset) => -{{{ cDefs.EROFS }}},
      read: async (file, buffer, length, offset) => {
        let fi;
        try { fi = await info(file); } catch (e) { return e === 404 ? -{{{ cDefs.ENOENT }}} : -{{{ cDefs.EIO }}}; }
        if (offset < 0 || length <= 0 || offset >= fi.size) return 0;
        length = Math.min(length, fi.size - offset);
        if (fi.data) { HEAPU8.set(fi.data.subarray(offset, offset + length), buffer); return length; }
        const first = (offset / BLOCK) | 0, last = ((offset + length - 1) / BLOCK) | 0, want = [];
        // several blocks, or a small file: the missing ones in one request per run (see the header)
        const small = fi.size <= SMALL, lo = small ? 0 : first, hi = small ? Math.ceil(fi.size / BLOCK) - 1 : last;
        if (hi > lo) {
          let j = lo;
          while (j <= hi && mem.has(fi.key + '#' + j)) j++;
          if (j <= hi) await fetchRuns(fi, lo, hi, 2);
        }
        for (let bi = first; bi <= last; bi++) want.push(block(fi, bi));
        const prev = fi.last;                     // the last block of this file's previous read
        fi.last = last;
        if (prev !== undefined && first >= prev && first <= prev + 1 && last > prev) readahead(fi, last).catch(() => {});
        let bs;
        try { bs = await Promise.all(want); }
        catch (e) { err(`[WEB] disc read failed: ${fi.url} @${offset}: ${e}`); return -{{{ cDefs.EIO }}}; }
        for (let bi = first; bi <= last; bi++) {
          const s = Math.max(offset, bi * BLOCK), e = Math.min(offset + length, (bi + 1) * BLOCK);
          HEAPU8.set(bs[bi - first].subarray(s - bi * BLOCK, e - bi * BLOCK), buffer + s - offset);
        }
        return length;
      },
      getSize: async (file) => {
        try { return (await info(file)).size; } catch { return 0; }
      },
    };
  },
});
