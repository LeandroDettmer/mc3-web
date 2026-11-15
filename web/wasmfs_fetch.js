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
 * assets_x.dat_2 (cars), the streamed sound banks (music mx.xwb first), assets_x.dat, _3, _1 (city),
 * resources/ smallest first (car builds .xbpf, hudmap), banks.dat. Since
 * 2026-11-14 (R2 reads cost) no more: the Flash screens, the rest of
 * assets_x* and the movies come only when the game reads them (the fill was
 * the whole disc, ~4.4 GiB a player); at most PREFETCH_BUDGET_MB still to fetch and what the browser
 * says it has room for. It starts PREFETCH_DELAY_MS after the first disc
 * file (MC3_PREFETCH=cars, the page's low-memory profile: also LOW_DELAY_MS
 * after the game's first frame, the learned fill too; 2026-11-14, phones
 * were killed 15-165 s after Play), fetches up to PREFETCH_RUN missing blocks in one request (a small
 * file whole) and only when the game has had no read of its own on the
 * network for IDLE_MS; blocks already kept (earlier visits too) are skipped.
 * Such a request is dropped the moment the game reads the network itself
 * (unless within BG_FINISH blocks of its end: 2026-11-14), or
 * wants one of its blocks more than a block past the one coming (that read
 * then fetches its block alone); the blocks already in are kept. Only when
 * the cache is on. Log lines '[WEB] prefetch: ...'. The music's track starts
 * (below) do not wait PREFETCH_DELAY_MS (2026-11-15), only idle().
 *
 * Music track starts first (2026-11-11): every disc read waits under the
 * guest lock, and a track change reads a random track of mx.xwb (135 entries,
 * shuffled) from its start, 352 KB packets: on a cold cache each change froze
 * the game 0.5-2.5 s (players' logs). Before anything else the fill fetches
 * the blocks holding the first HEAD_BYTES of every entry (its XACT table, in
 * block 0; ~206 MiB). Where the cache was meant to be on but OPFS refused it
 * or ran out of room (a Chrome incognito tab: estimate() reports ~10 GiB, the
 * real quota is far smaller), those blocks are kept in memory for the session
 * instead (`pinned`, never evicted; those the cache file already held are
 * read from it as the cache turns off, keepHeads; only where the browser says 8 GB of
 * device memory or more: Safari says nothing, and its Macs pinned 206 MiB,
 * then were killed, 2026-11-12 logs TYM47H/VJK9ZT).
 *
 * Readahead: a game read that goes on where its file's last read ended, into
 * a new block, also wants the next READAHEAD blocks held (memory or OPFS). Its
 * own missing block and those go in ONE request (fetchRun): the body is read as
 * a stream and each block is handed over as its bytes come, so the read's own
 * block (first in the range) is not held back by the rest; the rest go on in
 * the background into memory and OPFS, and the game's later read of one joins
 * its fetch (inflight) or finds it in memory. A run that reaches past the
 * window is made RA_RUN blocks long, so a file read straight through costs one
 * request per RA_RUN blocks, not one per block (R2 bills requests, 2026-11-14).
 * A streamed sound bank (.xwb) does so on every read, a jump to a new track
 * included, so the packet that crosses into the next block finds it; so does
 * the car pack assets_x.dat_2 (2026-11-14, untested in a browser: a car is a
 * jump then ~2-4 MB read on, 2-4 serial 1 MiB trips cold before). '[WEB]
 * readahead: <file> ...' once a file; '[WEB] disc: N requests for M blocks'
 * once a session. A game read of several blocks fetches each run of missing
 * ones in one request; a read of a file of at most SMALL bytes fetches all of
 * it. Other reads (a jump elsewhere) fetch their block alone, as before.
 *
 * Hidden tab (web/pre.js -> xbox_page_hidden_box, 2026-11-14): the memory
 * LRU is emptied (checked each second; the pinned track starts stay, kept
 * only where deviceMemory >= 8), no readahead, every background run is
 * dropped and the fill waits (idle()). '[WEB] disc: tab hidden: ...'.
 *
 * The game's own network reads give up a try after GAME_STALL_MS without
 * data (and try again); the background ones keep the disc cache's FETCH_MS.
 *
 * Disc files come as <disc url>//<path> (WASMFS joins with '/'); sizes from
 * <disc url>.manifest (tools/web_serve.py), else a HEAD request.
 * Any other URL (the file list, src/web_fs.c) is fetched whole, uncached.
 */
addToLibrary({
  // Emscripten's OPFS backend (src/lib/libwasmfs_opfs.js) opens its root with navigator.storage.getDirectory()
  // and no catch: a refusal (WebKit's UnknownError under memory pressure, private browsing) was an unhandled
  // rejection that killed its worker ('worker sent an error!') and left the mount waiting. Replaced: three tries,
  // then '[WEB] opfs: getDirectory ...' and src/web_fs.c told (mc3_web_opfs_failed): saves go to memory, the game
  // goes on. Every other OPFS call there already turns a failure into an errno.
  _wasmfs_opfs_init_root_directory__deps: ['$wasmfsOPFSDirectoryHandles', '$wasmfsOPFSProxyFinish', 'mc3_web_opfs_failed'],
  _wasmfs_opfs_init_root_directory__async: false,   // proxied to its own worker (PTHREADS), as the original
  _wasmfs_opfs_init_root_directory: async (ctx) => {
    for (let i = 1; wasmfsOPFSDirectoryHandles.allocated.length == 1; i++) {   // handle 0 is reserved: 1 is the root
      try { wasmfsOPFSDirectoryHandles.allocated.push(await navigator.storage.getDirectory()); }
      catch (e) {
        if (i < 3) { await new Promise(r => setTimeout(r, 250 * i)); continue; }
        err(`[WEB] opfs: getDirectory (saves root): ${e?.name || 'Error'}: ${e?.message || e}`);
        _mc3_web_opfs_failed();
        break;
      }
    }
    wasmfsOPFSProxyFinish(ctx);
  },

  _wasmfs_create_fetch_backend_js__deps: [
    '$wasmFS$backends',
    '_wasmfs_fetch_get_file_url',
    'getenv', '$withStackSave', '$stringToUTF8OnStack', '$UTF8ToString',
  ],
  _wasmfs_create_fetch_backend_js: async function(backend) {
    const C = MC3DiscCache, BLOCK = C.BLOCK;
    // Settings from the game's environment (this runs on a worker, where Module.mc3Env is not):
    // MC3_DISC_MEM_MB (the LRU's size), MC3_PREFETCH=0 (no background fill), =cars (car models only).
    const env = k => withStackSave(() => { const p = _getenv(stringToUTF8OnStack(k)); return p ? UTF8ToString(p) : ''; });
    const E = { MC3_DISC_MEM_MB: env('MC3_DISC_MEM_MB'), MC3_PREFETCH: env('MC3_PREFETCH') };
    const MEM_BYTES = (Number(E.MC3_DISC_MEM_MB) || 48) << 20;   // the memory LRU (8 in the page's low-memory profile)
    // Cache files kept open (two sync handles each): 16 on WebKit (iOS browsers, Safari) and in the low-memory
    // profile, whose handle limits and memory are tighter; 128 elsewhere.
    const ua = navigator.userAgent, webkit = /AppleWebKit/.test(ua) && !/Chrome\/|Chromium|Android/.test(ua);
    const OPEN_FILES = webkit || MEM_BYTES < (48 << 20) ? 16 : 128;
    const mem = new Map();          // "<key>#<block>" -> Uint8Array, least recently used first
    let memBytes = 0;
    const pinned = new Map();       // the same, kept for the session: music track starts without OPFS (pinHeads)
    const inflight = new Map();     // the same, while being read
    const files = {};               // WASMFS file -> Promise<{url, size, key} | {url, size, data}>
    const manifests = {};           // disc url -> Promise<Map path -> {p, s, m}> | null
    const signers = {};             // disc url -> its direct reads (C.signer), when it has a manifest
    const open = new Map();         // cache key -> Promise<cache file | null>, least recently used first
    let cacheDir = null, cacheOn = null, cacheOff = false, failures = 0;
    const BANKS = ['mx', 'RaceStarters', 'Engine_Revs', 'dx', 'City_Walla'].map(n => `audio/xstreams/${n.toLowerCase()}.xwb`);
    const PREFETCH_DELAY_MS = 15000, LOW_DELAY_MS = 60000, IDLE_MS = 300, PREFETCH_BUDGET_MB = 4608;
    const READAHEAD = 2, RA_RUN = READAHEAD + 1, GAME_STALL_MS = 4000, PREFETCH_RUN = 16, SMALL = 2 * BLOCK;
    // the game's own requests (misses, readahead) and the blocks they asked for: '[WEB] disc: ...' once
    let odReq = 0, odBlk = 0;
    function counted(n) {
      odReq++; odBlk += n;
      if (odBlk >= 128 && odBlk - n < 128)
        err(`[WEB] disc: ${odReq} requests for ${odBlk} blocks read on demand (misses + readahead, runs up to ${RA_RUN})`);
    }
    // the music bank, and how much of each track's start is fetched first: two of the Remix's 352 KB stream packets
    const MUSIC = BANKS[0], HEAD_BYTES = 0xB0000, STREAM = /\.xwb$/i;
    // the car models' pack: a car build jumps to a car's first entry (a 4 KB read) and reads ~2-4 MB on from it
    // (cold logs 2026-11-14), all under the guest lock: readahead on every read, the jump included
    const CARS = /\/assets_x\.dat_2$/i;
    let netReads = 0, lastNet = 0, prefetching = false;   // the game's own reads on the network, when the last ended
    // the page's tab is hidden (web/pre.js -> xbox_page_hidden_box): the background fill waits
    const hidden = () => typeof _xbox_page_hidden_box == 'function'
      && Atomics.load(new Int32Array(HEAPU8.buffer), _xbox_page_hidden_box() >>> 2) !== 0;
    const idle = async () => {      // until the game has had no read of its own on the network for IDLE_MS
      while (netReads > 0 || performance.now() - lastNet < IDLE_MS || hidden()) await new Promise(r => setTimeout(r, IDLE_MS));
    };
    const bgRuns = new Set();       // the background fill's requests in progress (fetchRun)
    // Hidden: the LRU emptied, background runs dropped (see the header); the pinned blocks stay
    let wasHidden = false;
    setInterval(() => {
      if (!hidden()) { wasHidden = false; return; }
      if (!wasHidden) err(`[WEB] disc: tab hidden: ${(memBytes / BLOCK).toFixed(0)} MiB of disc memory dropped, background reads stopped`);
      wasHidden = true;
      mem.clear(); memBytes = 0;
      for (const run of bgRuns) run.ac.abort();
    }, 1000);
    // The game's presented frames (either renderer's counter), 0 before the first
    const frames = () => {
      const u = new Uint32Array(HEAPU8.buffer), w = typeof _nv2a_wgpu_fps_box == 'function' ? _nv2a_wgpu_fps_box() : 0,
        g = typeof _nv2a_gl_fps_box == 'function' ? _nv2a_gl_fps_box() : 0;
      return (w ? Atomics.load(u, w >>> 2) : 0) + (g ? Atomics.load(u, g >>> 2) : 0);
    };
    const sleep = ms => new Promise(r => setTimeout(r, ms));
    // When background fetching may begin (see the header): once, shared by the fill and the learned fill
    let gate = null;
    const fillGate = () => gate ??= (async () => {
      await sleep(PREFETCH_DELAY_MS);
      if (E.MC3_PREFETCH !== 'cars') return;
      while (!frames()) await sleep(1000);
      err(`[WEB] prefetch: low-memory profile: background reads start ${LOW_DELAY_MS / 1000} s after the first frame`);
      await sleep(LOW_DELAY_MS);
    })();
    // A game read that waited SLOW_MS or more (the guest thread that asked stood still meanwhile; a free-roam car
    // build waits under the guest lock, so every player's frame does): '[WEB] disc wait: ...', at most a line a
    // second, with the slow reads since the last line summed (web/index.html's '[WEB] hitch' names the last one).
    const SLOW_MS = 100;
    let slowN = 0, slowMs = 0, slowAt = 0;
    function slowRead(fi, offset, length, ms) {
      learn(fi, ms);
      slowN++; slowMs += ms;
      const now = performance.now();
      if (now - slowAt < 1000) return;
      err(`[WEB] disc wait: ${fi.url} @${offset} ${(length / 1024).toFixed(0)} KB waited ${Math.round(ms)} ms (${slowN} slow`
        + ` read${slowN == 1 ? '' : 's'}, ${Math.round(slowMs)} ms in all, since the last such line)`);
      slowAt = now; slowN = 0; slowMs = 0;
    }
    // Learned fill (2026-11-14): a file whose own reads kept the game waiting LEARN_MS in all is fetched whole in the
    // background (learnChain: one such file at a time, beside the fill: both wait for the game to be idle), so what this player's game actually reads
    // (Flash screens, the other city packs, the rest of a sound bank) stops freezing it. Not the movies (played once),
    // not files over LEARN_MAX, at most LEARN_BUDGET a session, never where the fill is off (MC3_PREFETCH=0).
    const LEARN_MS = 2000, LEARN_MAX = 400 * BLOCK, LEARN_BUDGET = 1024 * BLOCK, learnMs = new Map();
    let learnChain = Promise.resolve(), learnBytes = 0;
    function learn(fi, ms) {
      if (E.MC3_PREFETCH === '0' || !fi.key || fi.size > LEARN_MAX || /\/video\//i.test(fi.url)) return;
      const t = (learnMs.get(fi.key) ?? 0) + ms;
      learnMs.set(fi.key, t);
      if (t < LEARN_MS || t - ms >= LEARN_MS || learnBytes + fi.size > LEARN_BUDGET) return;   // once, on crossing
      learnBytes += fi.size;
      learnChain = learnChain.then(async () => {
        if (E.MC3_PREFETCH === 'cars') await fillGate();
        if (!(await cacheFolder())) return;
        const t0 = performance.now(), nb = Math.ceil(fi.size / BLOCK);
        err(`[WEB] prefetch: ${fi.url} kept the game waiting ${Math.round(t)} ms: the whole file (${nb} MiB) in the background`);
        if (await fill(fi, Array.from({ length: nb }, (_, j) => j), false))
          err(`[WEB] prefetch: ${fi.url} kept, after ${((performance.now() - t0) / 1000).toFixed(0)} s`);
      }).catch(e => err(`[WEB] prefetch stopped: ${e}`));
    }
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
      p = C.open(d, fi.key, fi.size).catch(e => { opfsFail(e); return null; });
      open.set(fi.key, p);
      if (open.size > OPEN_FILES) {
        const [k, q] = open.entries().next().value;
        open.delete(k); q.then(C.close);
      }
      return p;
    }
    // An OPFS failure (a cache file's open, a block's write): logged with its call, '[WEB] opfs: <op> <file>:
    // <error>' (the first few). WebKit's UnknownError (handle limits, memory pressure), a full disk, or 8 failures
    // turn the cache off for the session: every open file is closed and reads go to the server only.
    function opfsFail(e) {
      if (cacheOff) return;
      if (failures++ < 3) err(`[WEB] ${e?.message || e}`);   // C.open/C.put messages start 'opfs: <op> <file>'
      if (e?.name !== 'UnknownError' && e?.name !== 'QuotaExceededError' && failures < 8) return;
      cacheOff = true; cacheDir = Promise.resolve(null);
      for (const q of open.values()) q.then(cf => { keepHeads(cf); C.close(cf); });
      open.clear();
      err(`[WEB] disc cache off for this session (${e?.name || e}): the disc is read from the server`);
    }

    // OPFS gone mid-session (an incognito tab's real quota is far below its estimate): the music's track starts its
    // file already holds go into `pinned` before it is closed, so pinHeads does not fetch them all again (2026-11-15).
    let heads = null;   // {key, size, list}: the music bank's head blocks, once the fill has read its table
    function keepHeads(cf) {
      if (!cf || !heads || cf.k !== heads.key || !(navigator.deviceMemory >= 8)) return;
      let n = 0;
      for (const j of heads.list) {
        if (!cf.have[j]) continue;
        const b = new Uint8Array(C.blockLen(heads.size, j));
        try { if (cf.h.read(b, { at: j * BLOCK }) !== b.length) continue; } catch { break; }
        pinned.set(cf.k + '#' + j, b); n++;
      }
      if (n) err(`[WEB] prefetch: disc cache off: ${n} of ${heads.list.length} MiB of the music's track starts kept in memory from it`);
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
      netReads++; stopBackground(); counted(1);
      try { b = await C.fetchBlock(fi.src, bi, fi.size, m => err(`[WEB] disc read stalled: ${fi.url} block ${bi}: ${m}`), GAME_STALL_MS); }
      finally { netReads--; lastNet = performance.now(); }
      if ((cf = await cacheFile(fi))) {
        try { C.put(cf, bi, b); }
        catch (e) { opfsFail(e); }
      }
      return b;
    }
    function block(fi, bi) {
      const k = fi.key + '#' + bi;
      const b = mem.get(k);
      if (b) { mem.delete(k); mem.set(k, b); return b; }
      const pb = pinned.get(k);
      if (pb) return pb;
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

    // A game read on the network drops the background runs, except those within BG_FINISH blocks of their end (they
    // share the link a moment more): each dropped run is a billed request whose rest is asked for again, and in a
    // busy city every miss cut the fill (2026-11-14, untested in a browser).
    const BG_FINISH = 4;
    function stopBackground() { for (const run of bgRuns) if (run.end - run.next > BG_FINISH) run.ac.abort(); }
    // Blocks bi .. bi+n-1 of fi in one request (C.fetchBlocks). Each is in `inflight` until it has come, so a read
    // of it waits for that, and goes into OPFS as it comes (into memory too for the game's runs, game true). A
    // background run (game false) is dropped by stopBackground and by block (see the header). Resolves to the
    // number of blocks that came; rejects with the fetch's error, or a background block OPFS did not keep.
    // pin: a background run kept in `pinned` instead of OPFS.
    async function fetchRun(fi, bi, n, game, pin = false) {
      const run = { ac: new AbortController(), next: bi, end: bi + n, game }, waits = [], puts = [];
      for (let j = bi; j < bi + n; j++) {         // before any await: a read right after this call joins
        const w = { k: fi.key + '#' + j };
        w.p = new Promise((res, rej) => { w.res = res; w.rej = rej; });
        w.p.catch(() => {});                      // nobody may wait for it
        w.p.run = run;
        inflight.set(w.k, w.p); waits.push(w);
      }
      const drop = w => { if (inflight.get(w.k) === w.p) inflight.delete(w.k); };
      let kept = null;
      if (game) { netReads++; stopBackground(); counted(n); } else bgRuns.add(run);
      try {
        await C.fetchBlocks(fi.src, bi, n, fi.size, {
          signal: run.ac.signal, stallMs: game ? GAME_STALL_MS : undefined,
          warn: m => err(`[WEB] ${game ? 'disc read' : 'prefetch'} stalled: ${fi.url} blocks ${bi}-${bi + n - 1}: ${m}`),
          onBlock: (j, b) => {
            const w = waits[j - bi];
            run.next = j + 1;
            if (game) remember(w.k, b);
            if (pin) pinned.set(w.k, b);
            else puts.push(cacheFile(fi).then(c => { if (c && !c.have[j]) C.put(c, j, b); }).catch(e => { kept ??= e; opfsFail(e); }));
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
      if (kept && !game) throw kept;            // logged by opfsFail; the game's blocks came all the same
      return run.next - bi;
    }
    // The blocks lo..hi of fi nobody holds or fetches, one game request (fetchRun) per run of them at least `min`
    // long; a run is carried past hi, over blocks still free, until it is `ext` long (readahead). Returns the first
    // block asked for, or -1.
    async function fetchRuns(fi, lo, hi, min, ext = 0) {
      const nb = Math.ceil(fi.size / BLOCK), cf = await cacheFile(fi), free = j => {
        const k = fi.key + '#' + j;
        return !mem.has(k) && !pinned.has(k) && !inflight.has(k) && !(cf && cf.have[j]);
      };
      let first = -1;
      for (let j = lo; j <= hi;) {
        if (!free(j)) { j++; continue; }
        let n = 1;
        while ((j + n <= hi || n < ext) && j + n < nb && free(j + n)) n++;
        if (n >= min) { if (first < 0) first = j; fetchRun(fi, j, n, true).catch(() => {}); }   // a failed block is fetched again by its read
        j += n;
      }
      return first;
    }
    // What to fill, in order (see the header): manifest files.
    function prefetchJobs(m) {
      const all = [...m.values()], lc = f => f.p.toLowerCase();
      const byPath = new Map(all.map(f => [lc(f), f]));
      const dir = d => all.filter(f => lc(f).startsWith(d)).sort((a, b) => a.s - b.s);
      // MC3_PREFETCH=cars (the page's low-memory profile, 2026-11-14): the car models only, 233 MiB once. A phone read
      // them on demand, ~1.4 s a 1 MiB block from R2 under the guest lock: 3.6-6.9 s frozen at each new car's build.
      // + the music's track starts (prefetch: heads only, not the rest of the bank): without them each new track froze
      // a phone the same way (owner, 2026-11-14).
      if (E.MC3_PREFETCH === 'cars') return [byPath.get(MUSIC), byPath.get('assets_x.dat_2')].filter(Boolean);
      return [
        byPath.get('assets_x.dat_2'),
        ...BANKS.map(p => byPath.get(p)),
        // the city packs the game reads while driving (on demand: ~60% more disc waits a log after they left the fill,
        // 2026-11-14); a fill run is one request per 16 MiB, cheaper in R2 reads than the game's 1-3 MiB misses
        ...['assets_x.dat', 'assets_x.dat_3', 'assets_x.dat_1'].map(p => byPath.get(p)),
        ...dir('resources/'),
        byPath.get('banks.dat'),
        // 2026-11-14: no Flash screens, assets_x.dat_4/_5 or movies: the game fetches them on use
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

    // A disc file of the manifest, as describe() gives it.
    const fileInfo = (disc, f) => ({ p: f.p, url: C.fileUrl(disc, f.p), src: signers[disc].url(f.p), size: f.s, key: C.key(C.discName(disc), f) });
    // The blocks (ascending) holding the first HEAD_BYTES of every entry of a wave bank, from its XACT table (version
    // 3, the Xbox's: segments at +8, bank data, entry metadata, seek tables, wave data; an entry's play region at +8),
    // which sits in block 0. None for anything else.
    async function headBlocks(fi) {
      const h = await block(fi, 0), v = new DataView(h.buffer, h.byteOffset, h.byteLength);
      const u = o => o + 4 <= h.length ? v.getUint32(o, true) : 0, out = new Set();
      if (u(0) !== 0x444E4257 || u(4) !== 3) return [];   // 'WBND'
      const bank = u(8), meta = u(16), wave = u(32), n = u(bank + 4), esz = u(bank + 0x18);
      if (esz < 16) return [];
      for (let i = 0; i < n; i++) {
        const e = meta + i * esz, s = wave + u(e + 8), len = Math.min(HEAD_BYTES, u(e + 12));
        if (e + 16 > h.length || !len || s + len > fi.size) continue;
        for (let j = Math.floor(s / BLOCK); j <= Math.floor((s + len - 1) / BLOCK); j++) out.add(j);
      }
      return [...out].sort((a, b) => a - b);
    }
    // The music's track starts in HEAD_LANES requests at a time (list cut in that many slices, each filled in order):
    // one at a time, ~94 requests took 1-2 min, past a new player's boot and first track changes (2026-11-15).
    // True when every slice was.
    const HEAD_LANES = 3;
    async function lanes(list, f) {
      const k = Math.ceil(list.length / HEAD_LANES), parts = [];
      for (let i = 0; i < list.length; i += k) parts.push(list.slice(i, i + k));
      return (await Promise.all(parts.map(f))).every(Boolean);
    }
    // Blocks `list` (ascending) of fi in the background while the game is idle, runs of up to PREFETCH_RUN: into the
    // OPFS cache, or into `pinned` (pin). onRun(bi, got) after each request. False when the cache went away.
    async function fill(fi, list, pin, onRun = () => {}) {
      let cf = null;
      const busy = j => (pin ? pinned.has(fi.key + '#' + j) : cf.have[j]) || inflight.has(fi.key + '#' + j);
      for (let i = 0; i < list.length;) {
        if (!pin && !(cf = await cacheFile(fi))) return false;
        if (busy(list[i])) { i++; continue; }
        await idle();
        if (!pin && !(cf = await cacheFile(fi))) return false;
        const bi = list[i];
        let n = 0;                                // from bi, the blocks still missing that the game is not reading
        while (n < PREFETCH_RUN && i + n < list.length && list[i + n] === bi + n && !busy(bi + n)) n++;
        if (!n) continue;
        let got;
        try { got = await fetchRun(fi, bi, n, false, pin); }   // fewer when dropped for the game: the rest after idle()
        catch (e) { if (cacheOff) return false; throw e; }       // OPFS full (logged by opfsFail)
        onRun(bi, got);
        i += got;
      }
      return true;
    }
    // No OPFS where the cache was meant to be on (refused, or out of room: an incognito tab): the music's track
    // starts kept in memory for the session (see the header).
    async function pinHeads(disc, m) {
      if (!(await cacheOn) || !(navigator.deviceMemory >= 8)) return;
      const f = [...m.values()].find(f => f.p.toLowerCase() === MUSIC);
      if (!f) return;
      const fi = fileInfo(disc, f), list = await headBlocks(fi);
      err(`[WEB] prefetch: no disc cache: the music's track starts (${list.length} MiB) kept in memory instead`);
      const t0 = performance.now();
      await lanes(list, l => fill(fi, l, true));
      err(`[WEB] prefetch: music track starts in memory, ${pinned.size} MiB in ${((performance.now() - t0) / 1000).toFixed(0)} s`);
    }

    // The background fill (see the header).
    // The music's track starts go at once (2026-11-15: 15 s after the first disc file plus ~94 requests one at a time
    // left a new player's first track changes cold; idle() keeps them off the game's reads), the rest after
    // fillGate. The low-memory profile waits for the gate first (phones were killed).
    async function prefetch(disc, m) {
      const gated = fillGate();
      if (E.MC3_PREFETCH === 'cars') await gated;
      const d = await cacheFolder();
      if (!d) return pinHeads(disc, m);           // no cache in this run: nothing to fill (track starts in memory if OPFS refused)
      const mib = n => (n / BLOCK).toFixed(0);
      let budget = PREFETCH_BUDGET_MB * BLOCK, room = '';
      try {
        const e = await navigator.storage.estimate();
        const free = e.quota - e.usage - 512 * BLOCK;   // leave the browser some room
        if (free < budget) { budget = Math.max(0, free); room = `, the browser's room ${mib(budget)} MiB`; }
      } catch { /* no estimate: the budget alone */ }
      const jobs = [];
      let left = 0, total = 0, skipped = 0, done = 0, shown = 0;
      for (const f of prefetchJobs(m)) {
        const fi = fileInfo(disc, f);
        const n = await missing(d, fi);
        total += f.s;
        if (!n) continue;
        if (left + n > budget) { skipped += n; continue; }
        left += n; jobs.push(fi);
      }
      const t0 = performance.now();
      err(`[WEB] prefetch: ${mib(left)} MiB in ${jobs.length} files to fetch in the background, of ${mib(total)} MiB`
        + ` (cars, sound banks, resources, banks.dat); budget ${PREFETCH_BUDGET_MB} MiB${room}`
        + (skipped ? `, ${mib(skipped)} MiB left out` : ''));
      if (!left) return;
      const run = (fi, list) => fill(fi, list, false, (bi, got) => {
        for (let j = bi; j < bi + got; j++) done += C.blockLen(fi.size, j);
        const pct = Math.floor(done * 10 / left);
        if (pct > shown && done < left) { shown = pct; err(`[WEB] prefetch: ${pct * 10}% (${mib(done)} of ${mib(left)} MiB), ${fi.p}`); }
      });
      const music = jobs.find(fi => fi.p.toLowerCase() === MUSIC);
      if (music) {                                // the music's track starts before anything else
        const list = await headBlocks(music);
        heads = { key: music.key, size: music.size, list };
        if (!(await lanes(list, l => run(music, l)))) return pinHeads(disc, m);
        err(`[WEB] prefetch: music track starts kept (${list.length} MiB) after ${((performance.now() - t0) / 1000).toFixed(0)} s`);
      }
      await gated;
      for (const fi of jobs)
        if (!(E.MC3_PREFETCH === 'cars' && fi === music) && !(await run(fi, Array.from({ length: Math.ceil(fi.size / BLOCK) }, (_, j) => j)))) return pinHeads(disc, m);
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
        const first = (offset / BLOCK) | 0, last = ((offset + length - 1) / BLOCK) | 0, want = [], t0 = performance.now();
        // several blocks, a small file, or readahead: the missing ones in one request per run (see the header).
        // Before block(): its read of a block in such a run joins the run and gets it as soon as it has come.
        const small = fi.size <= SMALL, nb = Math.ceil(fi.size / BLOCK), prev = fi.last;   // prev: this file's last read's last block
        fi.last = last;
        const ahead = !small && !hidden() && (STREAM.test(fi.url) || CARS.test(fi.url) || (prev !== undefined && first >= prev && first <= prev + 1 && last > prev));
        const lo = small ? 0 : first, hi = small ? nb - 1 : ahead ? Math.min(last + READAHEAD, nb - 1) : last;
        if (hi > lo) {
          let j = lo;
          while (j <= hi && mem.has(fi.key + '#' + j)) j++;
          if (j <= hi) {
            j = await fetchRuns(fi, lo, hi, ahead ? 1 : 2, ahead ? RA_RUN : 0);
            if (ahead && j >= 0 && !fi.ra) { fi.ra = true; err(`[WEB] readahead: ${fi.url} from block ${j}`); }
          }
        }
        for (let bi = first; bi <= last; bi++) want.push(block(fi, bi));
        let bs;
        try { bs = await Promise.all(want); }
        catch (e) { err(`[WEB] disc read failed: ${fi.url} @${offset}: ${e}`); return -{{{ cDefs.EIO }}}; }
        const waited = performance.now() - t0;
        if (waited >= SLOW_MS) slowRead(fi, offset, length, waited);
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
