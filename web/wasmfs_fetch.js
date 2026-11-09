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
 *   HTTP    a Range request, then written to OPFS.
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
 * file, fetches one block at a time and only when the game has had no read
 * of its own on the network for IDLE_MS; blocks already kept (earlier visits
 * too) are skipped. Only when the cache is on. Log lines '[WEB] prefetch: ...'.
 *
 * Readahead: a game read that goes on where its file's last read ended, into
 * a new block, also starts the next READAHEAD blocks not held yet (memory or
 * OPFS) in the background; the game's later read of one joins its fetch
 * (inflight) or finds it in memory. '[WEB] readahead: <file> ...' once a file.
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
  ],
  _wasmfs_create_fetch_backend_js: async function(backend) {
    const C = MC3DiscCache, BLOCK = C.BLOCK;
    const MEM_BYTES = 48 << 20;     // the memory LRU
    const OPEN_FILES = 128;         // cache files kept open (two sync handles each)
    const mem = new Map();          // "<key>#<block>" -> Uint8Array, least recently used first
    let memBytes = 0;
    const inflight = new Map();     // the same, while being read
    const files = {};               // WASMFS file -> Promise<{url, size, key} | {url, size, data}>
    const manifests = {};           // disc url -> Promise<Map path -> {p, s, m}> | null
    const open = new Map();         // cache key -> Promise<cache file | null>, least recently used first
    let cacheDir = null, cacheOn = null, warned = false;
    const BANKS = ['mx', 'RaceStarters', 'Engine_Revs', 'dx', 'City_Walla'].map(n => `audio/xstreams/${n.toLowerCase()}.xwb`);
    const PREFETCH_DELAY_MS = 15000, IDLE_MS = 300, PREFETCH_BUDGET_MB = 2560;
    const READAHEAD = 2, GAME_STALL_MS = 4000;
    let netReads = 0, lastNet = 0, prefetching = false;   // the game's own reads on the network, when the last ended
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
      if (m && !prefetching) { prefetching = true; prefetch(disc, m).catch(e => err(`[WEB] prefetch stopped: ${e}`)); }
      let f = m && m.get(rel);
      if (!f) {
        const r = await fetch(url, { method: 'HEAD', credentials: 'include' });
        if (!r.ok) throw r.status;
        f = { p: rel, s: +r.headers.get('Content-Length') };
      }
      return { url, size: f.s, key: C.key(C.discName(disc), f) };
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
      netReads++;
      try { b = await C.fetchBlock(fi.url, bi, fi.size, m => err(`[WEB] disc read stalled: ${fi.url} block ${bi}: ${m}`), GAME_STALL_MS); }
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
      if (!p) {
        p = load(fi, bi).then(b => { remember(k, b); return b; }).finally(() => inflight.delete(k));
        inflight.set(k, p);
      }
      return p;
    }
    // Blocks bi+1 .. bi+READAHEAD of fi into memory in the background, those not held yet (see the header).
    async function readahead(fi, bi) {
      const cf = await cacheFile(fi);
      for (let j = bi + 1; j <= bi + READAHEAD && j * BLOCK < fi.size; j++) {
        const k = fi.key + '#' + j;
        if (mem.has(k) || inflight.has(k) || (cf && cf.have[j])) continue;
        if (!fi.ra) { fi.ra = true; err(`[WEB] readahead: ${fi.url} from block ${j}`); }
        block(fi, j).catch(() => {});             // a failed one is fetched again by the game's own read
      }
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
        const fi = { p: f.p, url: C.fileUrl(disc, f.p), size: f.s, key: C.key(name, f) };
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
        for (let bi = 0; bi * BLOCK < fi.size; bi++) {
          const k = fi.key + '#' + bi;
          let cf = await cacheFile(fi);
          if (!cf) return;
          if (cf.have[bi]) continue;
          await idle();
          if (!(cf = await cacheFile(fi))) return;
          if (cf.have[bi] || inflight.has(k)) continue;   // the game read it meanwhile, or is reading it
          let kept = null;
          // in inflight, so a game read of this block waits for this fetch instead of fetching it again
          const p = C.fetchBlock(fi.url, bi, fi.size, w => err(`[WEB] prefetch stalled: ${fi.p} block ${bi}: ${w}`))
            .then(async b => {
              const c = await cacheFile(fi);
              try { if (c && !c.have[bi]) C.put(c, bi, b); } catch (e) { kept = e; }   // the game's read still gets the block
              return b;
            });
          inflight.set(k, p);
          try { await p; } finally { if (inflight.get(k) === p) inflight.delete(k); }
          if (kept) throw kept;
          done += C.blockLen(fi.size, bi);
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
