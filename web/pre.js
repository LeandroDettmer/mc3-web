// Linked in front of the Emscripten runtime (--pre-js, CMakeLists.txt).
// Settings for getenv(): RECOMP_* / MC3_* from process.env under Node.js
// (make web-node), from Module.mc3Env in a browser (web/index.html: the start
// page's settings, its Advanced box, then the URL query; MC3_DISC_URL).
Module['preRun'] = [].concat(Module['preRun'] || [], function () {
  var env = Module['mc3Env'] || (typeof process == 'object' && process.env) || {};
  for (var k in env)
    if (/^(RECOMP_|MC3_)/.test(k)) ENV[k] = String(env[k]);
});

// A lost WebGL context answers getShaderInfoLog / getProgramInfoLog with null, and this Emscripten's
// glGetShaderInfoLog / glGetProgramInfoLog / glGetProgramiv(INFO_LOG_LENGTH) take its .length (stringToUTF8):
// "Cannot read properties of null (reading 'length')" killed the GL thread, 'A game thread crashed' (Android Mali,
// Chrome 156/157, 31 logs 2026-11-12/13, after 10-77 s GPU stalls). An empty log instead: the renderer then finds
// the context lost (nv2a_gl.c gl_lost) and logs '[GL] WebGL context lost', the page's 'Graphics context lost' box.
// Every thread (the GL thread is a pthread; the page thread when proxied).
[typeof WebGL2RenderingContext == 'function' && WebGL2RenderingContext,
 typeof WebGLRenderingContext == 'function' && WebGLRenderingContext].forEach(function (C) {
  if (C) ['getShaderInfoLog', 'getProgramInfoLog'].forEach(function (n) {
    var f = C.prototype[n];
    if (f) C.prototype[n] = function (o) { var r = f.call(this, o); return r == null ? '' : r; };
  });
  // Same family: getParameter(VIEWPORT) is null on a lost context and Emscripten's setCanvasElementSizeCallingThread
  // reads prevViewport[0] ("Cannot read properties of null (reading '0')", 10 logs 2026-11-13). Zeros instead.
  var gp = C && C.prototype.getParameter;
  if (gp) C.prototype.getParameter = function (p) {
    var r = gp.call(this, p);
    return r == null && p === 0xBA2 ? new Int32Array(4) : r;
  };
});

// The wasm memory, made here on the page thread (workers get it from the runtime). CMakeLists.txt links 256 MB
// at least, growth to 2 GB. A PC gets 2 GB up front and never grows (as the old fixed 2 GB); a browser that
// cannot set that aside (32-bit Chrome) gets the first of 1 GB, 512 MB, 256 MB that it gives and grows from
// there, then smaller maximums.
// Low-memory profile (web/index.html sets Module.mc3MemMB; TVs, phones, tablets) prefers a big enough INITIAL size
// over a big maximum: a small device cannot reserve 2 GB to grow into, and the game needs ~350-450 MB at once. So
// fixed sizes first (maximum = initial, no growth): 1024 down to 320 in small steps; then growing combos (initial +
// headroom), then 256-based ones as a last resort. A 32-bit Chrome (Android phones, TVs) that cannot set aside a
// memory's maximum quietly sets aside less, down to its initial size (nfsu2-web host/memory.ts, V8's
// backing-store.cc): a growing memory may never grow there, so the game must fit its initial size (a TCL Google TV
// refused every size from 448 MB up and gave 256/512, 2026-11-09).
// WebKit (iPhone, iPad, Safari) sets aside a shared memory's whole maximum (256 MB with a 2 GB maximum was
// refused on an iPhone): fixed sizes only (maximum = initial). Pool 40 (the 2 GB baseline), but the light profile's
// (8, index.html; 12 until 2026-11-14) since 2026-11-12: every iPhone ran 40 workers, each a JS heap of its own (Android's
// light profile makes the rest on demand); '[WEB] workers: N' below gives the real need. A Mac's
// Safari starts at 2 GB; iOS/iPadOS (the low profile, or the iPad's desktop user agent) at 1 GB: with a fixed 2 GB,
// 61 iPhones were killed silently in the menus (previous-crash logs, 2026-11-11; the game's peak is ~400 MB).
// Every refused size is logged; when nothing is given a page error says so (index.html failShow shows it).
if (!ENVIRONMENT_IS_PTHREAD && typeof WebAssembly == 'object' && !Module['wasmMemory']) (function () {
  var low = !!Module['mc3MemMB'], refused = [], i, ua = navigator.userAgent, log = Module['print'] || console.log;
  var ios = /iPhone|iPad|iPod/.test(ua) || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1);
  var webkit = ios || (/AppleWebKit/.test(ua) && !/Chrome|Chromium|Android/.test(ua));
  if (webkit && !low) delete Module['mc3Pool'];
  var tries = webkit && (low || ios) ? [[1024, 1024], [768, 768], [512, 512]]
    : webkit ? [[2048, 2048], [1536, 1536], [1024, 1024], [768, 768], [512, 512]]
    : low ? [[1024, 1024], [768, 768], [640, 640], [512, 512], [448, 448], [416, 416], [384, 384], [352, 352],
        [320, 320], [512, 768], [448, 768], [512, 1024], [448, 1024], [288, 288], [256, 512], [256, 1024]]
    : [[2048, 2048], [1024, 2048], [512, 2048], [256, 2048], [512, 1024], [256, 1024], [512, 512], [256, 512]];
  // Firefox refuses any typed array of 2 GB or more passed to a DOM call ("can't be an ArrayBuffer or an
  // ArrayBufferView larger than 2 GB", bugzil.la/1838218): WebGPU's writeBuffer (#1, a7042ba) and Emscripten's WebGL 2
  // calls, which pass the whole HEAPU8. One 64 KB page under 2048 MB keeps every heap view below the limit.
  var gecko = /Firefox\//.test(ua), pages = (mb) => mb * 16 - (gecko && mb >= 2048 ? 1 : 0);
  for (i = 0; i < tries.length; i++) {
    try {
      Module['wasmMemory'] = new WebAssembly.Memory({ initial: pages(tries[i][0]), maximum: pages(tries[i][1]), shared: true });
      log('[WEB] memory: ' + tries[i][0] + ' MB to start, ' + tries[i][1] + ' MB at most'
        + (webkit ? ' (WebKit: fixed, pool ' + (Module['mc3Pool'] || 40) + ')' : low ? ' (low profile)' : '') + (gecko ? ' (Firefox: 64 KB less than 2 GB)' : '')
        + (refused.length ? ' (refused: ' + refused.join('; ') + ')' : ''));
      return;
    } catch (e) {
      refused.push(tries[i][0] + '/' + tries[i][1] + ' ' + (e && e.message || String(e)));
      log('[WEB] memory: ' + tries[i][0] + ' MB to start, ' + tries[i][1] + ' MB at most refused: ' + (e && e.message || e));
    }
  }
  throw new Error('This TV/browser cannot give the game enough memory (needs about 256 MB; refused: ' + refused.join('; ')
    + '). Close other tabs and apps, or restart the browser.');
})();

// The pool workers (made when this script runs, before the .wasm): how many, and each one's loading (errors: below), into
// the log. A worker that never loads holds the start forever ('1 start-up step(s) left', web/index.html), silently
// before (Firefox on a TV, 2026-11-09).
if (!ENVIRONMENT_IS_PTHREAD) Module['preRun'] = [].concat(Module['preRun'] || [], function () {
  if (typeof PThread != 'object') return;
  var all = PThread.unusedWorkers.slice(), n = 0, log = Module['print'] || console.log;
  log('[WEB] module: ' + all.length + ' pool workers made, loading the module into them');
  all.forEach(function (w, i) {
    w.addEventListener('message', function (e) {
      // CMD_LOADED: 3 in Emscripten 6 (src/runtime_pthread.js), the string 'loaded' in older ones
      if (e.data && (e.data.cmd === 3 || e.data.cmd === 'loaded') && (++n == 1 || n == all.length || n % 8 == 0))
        log('[WEB] module: pool worker ' + n + ' of ' + all.length + ' loaded');
    });
  });
});

// Errors with their cause (2026-11-12): WebKit hands the page's 'error' event only 'Script error.' (no file, no error
// object) for any error whose top frame has no script address -- a wasm trap, an engine RangeError -- not just for
// other origins' scripts: 8 iPhone logs said nothing more (VBARSZ during the compile, D69TNT/SFABFG in Free Roam).
// So the page thread's entries into the runtime are caught where they happen: every Worker made from here on (the
// pool's and those made on demand) logs its 'error' event at once with file:line, before Emscripten's onerror is set
// (a pool worker that dies while the .wasm compiles had none), and its onmessage (proxied calls, the mailbox: the
// page thread's wasm) logs an uncaught exception's stack, then rethrows as before. '[WEB] worker N ...' lines.
// Inside the pthreads Emscripten's own handler logs ('worker sent an error!'); unhandled rejections there throw too.
if (!ENVIRONMENT_IS_PTHREAD && !ENVIRONMENT_IS_NODE && typeof Worker == 'function' && !Worker.mc3Watched) (function () {
  var W = Worker, n = 0, onmsg = Object.getOwnPropertyDescriptor(W.prototype, 'onmessage');
  var log = function (m) { (Module['printErr'] || console.error)(m); };
  function why(x) {
    if (!x || typeof x != 'object') return String(x);
    var s = String(x.stack || '');
    return String(x) + (s && s.indexOf(String(x.message)) < 0 ? '; ' : s ? ' | ' : '') + s.replace(/\s*\n\s*/g, ' < ').slice(0, 1500);
  }
  function Watched(url, opts) {
    var w = new W(url, opts), id = ++n;
    w.addEventListener('error', function (e) {
      log('[WEB] worker ' + id + ' error: ' + (e.message || e.type) + (e.filename ? ' (' + String(e.filename).replace(/^.*\//, '')
        + ':' + e.lineno + ':' + e.colno + ')' : ' (no file: the browser hid it)') + (e.error ? '; ' + why(e.error) : ''));
    });
    if (onmsg && onmsg.set) Object.defineProperty(w, 'onmessage', { configurable: true, get: function () { return onmsg.get.call(w); },
      set: function (f) {
        onmsg.set.call(w, typeof f != 'function' ? f : function (e) {
          try { return f.apply(this, arguments); } catch (x) {
            if (x !== 'unwind' && !(x && x.mc3Seen)) {
              try { x.mc3Seen = 1; } catch (_) {}
              log('[WEB] worker ' + id + ' message' + (e && e.data && e.data.cmd ? ' (cmd ' + e.data.cmd + ')' : '') + ': page thread error: ' + why(x));
            }
            throw x;
          }
        });
      } });
    return w;
  }
  Watched.prototype = W.prototype;
  Watched.mc3Watched = true;
  globalThis.Worker = Watched;
})();

// How many workers the run really needed (pool + made on demand; idle ones are kept): logged at each new peak, so the
// pool size (Module.mc3Pool) can follow the logs. '[WEB] workers: N (R threads running)'. Module.mc3Workers() gives
// { all, run, ended } now (web/index.html's crash heartbeat), undefined before the runtime's PThread exists.
// Low-memory profile (Module.mc3MemMB, 2026-11-14): a worker whose thread ended is terminated instead of kept idle while
// more than KEEP exist (each is a JS heap; phones killed in menus ran 22-40). Not a cap on new ones: Emscripten's
// pthread_create gets EAGAIN (spawnThread returns 6) when no worker can be had, and a guest thread that fails to start
// breaks the game; an ended worker only costs the next thread a worker start. '[WEB] workers: idle ones beyond 16 ended ...'.
if (!ENVIRONMENT_IS_PTHREAD && !ENVIRONMENT_IS_NODE && typeof setInterval == 'function') (function () {
  var peak = 0, ended = 0, KEEP = 16, log = Module['print'] || console.log;
  if (Module['mc3MemMB']) Module['preRun'] = [].concat(Module['preRun'] || [], function () {
    if (typeof PThread != 'object' || typeof PThread.returnWorkerToPool != 'function') return;
    var back = PThread.returnWorkerToPool;
    PThread.returnWorkerToPool = function (w) {
      back(w);
      var i = PThread.unusedWorkers.indexOf(w);
      if (i < 0 || Object.keys(PThread.pthreads).length + PThread.unusedWorkers.length <= KEEP) return;
      PThread.unusedWorkers.splice(i, 1);
      w.onmessage = function () {};
      try { w.terminate(); } catch (e) {}
      if (!ended++) log('[WEB] workers: idle ones beyond ' + KEEP + ' ended (low-memory profile)');
    };
  });
  Module['mc3Workers'] = function () {
    if (typeof PThread != 'object' || !PThread.unusedWorkers) return;
    var run = Object.keys(PThread.pthreads).length;
    return { all: run + PThread.unusedWorkers.length, run: run, ended: ended };
  };
  setInterval(function () {
    if (typeof PThread != 'object' || !PThread.unusedWorkers) return;
    var run = Object.keys(PThread.pthreads).length, all = run + PThread.unusedWorkers.length;
    if (all > peak) { peak = all; log('[WEB] workers: ' + all + ' (' + run + ' threads running)'); }
  }, 5000);
})();

// A refused growth (sbrk past what the browser set aside; a 32-bit TV's maximum is often only its initial size):
// logged as '[WEB] out of memory: ...' (the page's 'Out of memory' box) before the runtime aborts the game
// (-sABORTING_MALLOC, CMakeLists.txt). Every thread: whichever one mallocs grows the memory.
if (typeof WebAssembly == 'object' && !WebAssembly.Memory.prototype.mc3Grow) (function () {
  var grow = WebAssembly.Memory.prototype.grow, shown = 0;
  WebAssembly.Memory.prototype.mc3Grow = grow;
  WebAssembly.Memory.prototype.grow = function (pages) {
    try { return grow.call(this, pages); } catch (e) {
      if (shown++ < 3) {
        var mb = this.buffer.byteLength / 1048576, msg = '[WEB] out of memory: growing the wasm memory from ' + mb
          + ' MB to ' + (mb + pages / 16) + ' MB refused (' + (e && e.message || e) + ')';
        (typeof err == 'function' ? err : console.error)(msg);
      }
      throw e;
    }
  };
})();

// No WebGL 2 in workers (web/index.html's probe sets RECOMP_GL_PROXY=1; a TCL Google TV, 2026-11-09): the canvas
// stays on the page thread, where the GL thread's context is made and every GL call runs (-sOFFSCREEN_FRAMEBUFFER,
// xboxrecomp/src/nv2a_gl/nv2a_gl.c gl_make_window). Emscripten's pthread_create hands a canvas over only when it has
// transferControlToOffscreen; hidden here, it logs "cannot transfer control" and keeps it.
if (!ENVIRONMENT_IS_PTHREAD && Module['canvas'] && (Module['mc3Env'] || {}).RECOMP_GL_PROXY == '1')
  Object.defineProperty(Module['canvas'], 'transferControlToOffscreen', { value: undefined, configurable: true });

// FPS overlay (web/index.html): presented-frame counter of the active renderer, read from shared wasm memory.
// Returns undefined until the runtime is up. WebGPU unless RECOMP_GPU=webgl (or its export is missing).
Module['mc3Frames'] = function () {
  if (typeof HEAPU8 == 'undefined' || !HEAPU8) return;
  var gl = (Module['mc3Env'] || {}).RECOMP_GPU == 'webgl';
  var box = (!gl && typeof _nv2a_wgpu_fps_box == 'function') ? _nv2a_wgpu_fps_box()
          : typeof _nv2a_gl_fps_box == 'function' ? _nv2a_gl_fps_box() : 0;
  return box ? Atomics.load(new Uint32Array(HEAPU8.buffer), box >>> 2) : undefined;
};

// Crash recovery (web/index.html resumeKeep): where Free Roam was last driven, from src/games/remix/roam_race_ai.c's
// roam_resume_box { seq, live, city, m[12] }. undefined until the runtime is up or on a game without it.
Module['mc3Resume'] = function () {
  if (typeof HEAPU8 == 'undefined' || !HEAPU8 || typeof _roam_resume_box != 'function') return;
  var b = _roam_resume_box() >>> 2, I32 = new Int32Array(HEAPU8.buffer), F32 = new Float32Array(HEAPU8.buffer);
  return { seq: Atomics.load(I32, b), live: I32[b + 1], city: I32[b + 2], m: Array.from(F32.subarray(b + 3, b + 15)) };
};

// What the player is doing (web/index.html lanWhere -> the switch's "where"), from src/games/remix/recomp_manual.c's
// mc3_activity_box: 0 loading (boot, intro movies), 1 menu, 2 career, 3 arcade. undefined until the runtime is up or
// on a game without it.
Module['mc3Activity'] = function () {
  if (typeof HEAPU8 == 'undefined' || !HEAPU8 || typeof _mc3_activity_box != 'function') return;
  return Atomics.load(new Int32Array(HEAPU8.buffer), _mc3_activity_box() >>> 2);
};

// Main menu "Online (N)": the players in Free Roam now (the switch's players push, Module.mc3Lan.players.freeRoam.total,
// web/lan.js) into src/games/remix/recomp_manual.c's mc3_online_count_box once a second; -1 unknown (offline, or
// web/server's switch without the push): the item reads plain "Online". The game reads it at each build of the menu.
// Page thread only, and the export checked first: Emscripten rewrites every HEAPU8 use into (growMemViews(), HEAPU8),
// which read wasmMemory.buffer before the memory was set (a pool worker, 2026-11-11: "The game could not start").
if (!ENVIRONMENT_IS_PTHREAD && typeof setInterval == 'function') setInterval(function () {
  if (typeof _mc3_online_count_box != 'function' || typeof wasmMemory == 'undefined' || !wasmMemory) return;
  var p = Module['mc3Lan'] && Module['mc3Lan'].players, n = p && p.freeRoam ? Number(p.freeRoam.total) : NaN;
  Atomics.store(new Int32Array(HEAPU8.buffer), _mc3_online_count_box() >>> 2, Number.isFinite(n) && n >= 0 ? Math.min(n, 9999) | 0 : -1);
}, 1000);

// Touch controls (web/touch.js): their virtual pad's 14 words into xboxrecomp/src/input/touch_pad_web.c's box, which
// the runtime merges into pad 1. False until the runtime is up (the caller sends again).
Module['mc3TouchPad'] = function (words) {
  if (typeof HEAPU8 == 'undefined' || !HEAPU8 || typeof _xbox_touch_pad_box != 'function') return false;
  var h = new Int32Array(HEAPU8.buffer), b = _xbox_touch_pad_box() >>> 2;
  for (var i = 0; i < 14; i++) Atomics.store(h, b + i, words[i] | 0);
  return true;
};

// Hor+ (WebGPU) on Auto (RECOMP_ASPECT unset or "auto"): the game canvas' shape as the page shows it, following
// resizes and full screen, for xboxrecomp/src/nv2a_wgpu/wgpu_surf.c (nv2a_wgpu_aspect_box: width / height x
// 10000; the renderer takes it at a frame's end). Written once a size has held for 250 ms. Main thread only.
// Word 1: the height in device pixels the picture is shown at (object-fit: contain), for the HUD drawn at the
// canvas' size (wgpu_surf.c "HUD at the canvas' size"; RECOMP_WGPU_HUD_NATIVE=0 off).
(function () {
  if (typeof document != 'object' || typeof ResizeObserver != 'function') return;
  var a = (Module['mc3Env'] || {}).RECOMP_ASPECT, canvas = Module['canvas'], val = 0, timer = 0, fixed = 0;
  if (!canvas) return;
  // fixed shape ("16:9", "21:9" or a ratio) instead of the canvas'; 0 = auto
  function parse(s) {
    var m = /^\s*([\d.]+)\s*:\s*([\d.]+)\s*$/.exec(s || ''), r = m ? m[1] / m[2] : parseFloat(s);
    return r > 0 ? Math.round(r * 10000) : 0;
  }
  function put() {
    timer = 0;
    // runtime not up yet (HEAPU8 exists before the exports do): try again, or a canvas that never resizes
    // after the first observation (a phone's full screen) would stay 16:9; on WebGL the box is ignored
    if (typeof HEAPU8 == 'undefined' || !HEAPU8 || typeof _nv2a_wgpu_aspect_box != 'function') { timer = setTimeout(put, 250); return; }
    var box = new Int32Array(HEAPU8.buffer), at = _nv2a_wgpu_aspect_box() >>> 2;
    Atomics.store(box, at, fixed || val);
    // shown height: a fixed shape wider than the box is letterboxed
    var w = canvas.clientWidth, h = canvas.clientHeight, r = (fixed || val) / 10000;
    var px = w && h && r > 0 ? Math.round(Math.min(h, w / r) * (window.devicePixelRatio || 1)) : 0;
    Atomics.store(box, at + 1, px);
  }
  function later() { clearTimeout(timer); timer = setTimeout(put, 250); }
  new ResizeObserver(function () {
    var w = canvas.clientWidth, h = canvas.clientHeight;
    if (!w || !h) return;
    val = Math.round(w / h * 10000);
    later();
  }).observe(canvas);
  // live switch from the page's bar: 'auto' (follow the canvas: Hor+ fills the screen) or a fixed shape ('16:9': no
  // Hor+, black bars from the canvas' object-fit)
  Module['mc3SetAspect'] = function (s) { fixed = s == 'auto' ? 0 : parse(s); later(); };
  Module['mc3SetAspect'](a || 'auto');
})();

// The in-game Settings window's live rows (web/index.html setSetting, with the value the game would get at start):
// each a 32-bit word in a box the C side reads at its next use, 0 = what the game started with. Renderer:
// xboxrecomp/src/nv2a_wgpu (nv2a_wgpu_live_box, WebGPU only); game: src/games/remix/recomp_manual.c (mc3_live_box);
// keyboard: xboxrecomp/src/input/sdl_keyboard.c (xbox_keyboard_box). Kept in `want` so a value set before the
// runtime is up lands once it is. Main thread only.
(function () {
  var onoff = function (v) { return v === '0' ? 1 : 2; };   // unset = on
  function gfx() { return typeof _nv2a_wgpu_live_box == 'function' ? _nv2a_wgpu_live_box() : 0; }
  function game() { return typeof _mc3_live_box == 'function' ? _mc3_live_box() : 0; }
  var BOX = {   // key: [box address (0: not up yet), word, value -> word]
    RECOMP_GL_SCALE: [gfx, 0, function (v) { return Math.round((parseFloat(v) || 1) * 1000); }],
    RECOMP_GL_ANISO: [gfx, 1, function (v) { return parseInt(v, 10) || 16; }],
    RECOMP_GL_MIPS: [gfx, 2, onoff],
    MC3_60FPS: [game, 0, function (v) { var n = parseInt(v, 10); return v === '1' || n === 60 ? 2 : n > 60 ? Math.min(n, 240) : 1; }],   // 1 = 30, 2 = 60, else N fps
    MC3_ROAM_CARS: [game, 1, function (v) { return parseInt(v, 10) || -1; }],   // -1 auto
    MC3_DRIFT: [game, 2, function (v) { return v === '4' ? 5 : v === '3' ? 4 : v === '2' ? 3 : v === '1' ? 2 : 1; }],
    MC3_DRIFT_CAM: [game, 5, function (v) { return v === '1' ? 2 : 1; }],
    MC3_RACE_INVITES: [game, 6, onoff],
    MC3_FX_BLUR: [game, 18, onoff],   // recomp_manual.c fx_step: the game's motion blur, 1 off / 2 on
    MC3_FX_GLOW: [game, 19, onoff],   // and its glow (bloom)
    MC3_ORBIT_CAM: [game, 20, onoff],   // orbit_cam.c: the mouse orbit camera (words 21-24: the Orbit camera block below)
    MC3_AI_DIFF: [game, 25, function (v) { return v === 'easy' ? 2 : v === 'hard' ? 3 : 1; }],   // recomp_manual.c ai_diff_call: 1 normal, 2 easy, 3 hard
    MC3_CAREER_ONLINE: [game, 3, function (v) { return v === '0' ? 1 : 2; }],
    RECOMP_KEYBOARD: [function () { return typeof _xbox_keyboard_box == 'function' ? _xbox_keyboard_box() : 0; }, 0, onoff],
  };
  // Drift setup panel (index.html): words 7.. (drift.c k_ds order), value x 1000 + 1, unset = -1 (the default)
  ['STEER', 'RESP', 'FGRIP', 'RGRIP', 'SPEED', 'REAR', 'COUNTER', 'STAB', 'HOLD', 'KEEP', 'CAM_K'].forEach(function (n, i) {
    BOX['MC3_DRIFT_' + n] = [game, 7 + i, function (v) { var x = parseFloat(v); return isFinite(x) && x >= 0 ? Math.round(x * 1000) + 1 : -1; }];
  });
  var want = {}, timer = 0;
  function put() {
    timer = 0;
    var k, b, a;
    for (k in want) {
      b = BOX[k];
      if (typeof HEAPU8 == 'undefined' || !HEAPU8 || !(a = b[0]())) { timer = setTimeout(put, 250); return; }
      Atomics.store(new Int32Array(HEAPU8.buffer), (a >>> 2) + b[1], b[2](want[k]));
      delete want[k];
    }
  }
  Module['mc3SetLive'] = function (key, value) {
    if (!BOX[key]) return false;
    want[key] = value === undefined ? '' : String(value);
    if (!timer) put();
    return true;
  };
  // The rate a race or cruise steps at now (mc3_live_box word 4, MC3_LIVE_FPS_NOW: 30, 60 or N), 0 outside them,
  // undefined until the runtime is up. HEAPU8 throws before then: checked inside try.
  Module['mc3FpsNow'] = function () {
    try {
      if (!runtimeInitialized || !game()) return;
      return Atomics.load(new Int32Array(HEAPU8.buffer), (game() >>> 2) + 4);
    } catch (e) {}
  };
})();

// Orbit camera (2026-11-16, untested in a browser; src/games/remix/orbit_cam.c): a left-button drag on the picture,
// or a finger that lands on the picture itself (not a touch control: those are other elements), turns the chase
// camera around our car; the wheel zooms. Running totals in mc3_live_box words 21 yaw, 22 pitch (radians x 1000),
// 23 zoom (x 1000), 24 = 1 while held; the game takes the change between reads, eases back behind the car itself and
// ignores them outside the chase camera (menus) or with Settings 'Orbit camera' Off. Drag right/down: the camera
// goes the other way round/up, as if the scene were grabbed (yaw's sign unproven: flip YAW if it feels inverted).
// No preventDefault on pointerdown: the canvas still takes focus (keys). Main thread only.
if (!ENVIRONMENT_IS_PTHREAD && typeof document == 'object') (function () {
  var canvas = Module['canvas'], YAW = 5, PITCH = 3, yaw = 0, pitch = 0, zoom = 0, held = 0, pid = null, x = 0, y = 0;
  if (!canvas) return;
  function put() {
    // HEAPU8 throws before the runtime is up: checked inside try
    try {
      if (!runtimeInitialized || typeof _mc3_live_box != 'function') return;
      var a = new Int32Array(HEAPU8.buffer), w = _mc3_live_box() >>> 2;
      Atomics.store(a, w + 21, yaw);
      Atomics.store(a, w + 22, pitch);
      Atomics.store(a, w + 23, zoom);
      Atomics.store(a, w + 24, held);
    } catch (e) {}
  }
  canvas.addEventListener('pointerdown', function (ev) {
    if (pid !== null || ev.target !== canvas || (ev.pointerType == 'mouse' && ev.button !== 0)) return;
    pid = ev.pointerId;
    x = ev.clientX;
    y = ev.clientY;
    held = 1;
    try { canvas.setPointerCapture(pid); } catch (e) {}
    put();
  });
  canvas.addEventListener('pointermove', function (ev) {
    if (ev.pointerId !== pid) return;
    yaw = (yaw + Math.round((ev.clientX - x) * YAW)) | 0;
    pitch = (pitch + Math.round((ev.clientY - y) * PITCH)) | 0;
    x = ev.clientX;
    y = ev.clientY;
    put();
  });
  function up(ev) { if (ev.pointerId === pid) { pid = null; held = 0; put(); } }
  ['pointerup', 'pointercancel', 'lostpointercapture'].forEach(function (t) { canvas.addEventListener(t, up); });
  canvas.addEventListener('wheel', function (ev) {
    if (!ev.deltaY) return;
    ev.preventDefault();
    zoom = (zoom + (ev.deltaY > 0 ? 80 : -80)) | 0;   // down: farther
    put();
  }, { passive: false });
})();

// A hidden tab (minimised, another tab, a locked phone) costs next to nothing: xbox_page_hidden_box
// (xboxrecomp/src/kernel/kernel_bridge.c) is 1 while document.hidden, and the vblank drops to 8 Hz (the game to 4 fps);
// web/wasmfs_fetch.js holds the disc's background fill, web/audio.js suspends the sound. Main thread only.
if (!ENVIRONMENT_IS_PTHREAD && typeof document == 'object') (function () {
  var timer = 0;
  function put() {
    timer = 0;
    // HEAPU8 is a getter that reads wasmMemory.buffer: touching it before the runtime is up throws (WebKit, log 5FAF8Q)
    try {
      if (!runtimeInitialized || typeof _xbox_page_hidden_box != 'function') { timer = setTimeout(put, 1000); return; }
      Atomics.store(new Int32Array(HEAPU8.buffer), _xbox_page_hidden_box() >>> 2, document.hidden ? 1 : 0);
    } catch (e) { timer = setTimeout(put, 1000); }
  }
  document.addEventListener('visibilitychange', function () { if (!timer) put(); });
  put();
})();

// Save progress (the in-game Settings button): src/games/remix/recomp_manual.c's mc3_save_box { request, answered,
// result }. mc3SaveNow bumps the request (the game takes it at its next state pass, on its own thread) and gives its
// number, false before the runtime is up; mc3SaveResult(n) gives 0 while that request is unanswered, else 1 pending,
// 2 saved, 3 refused (not now), 4 the write failed. Main thread only.
Module['mc3SaveNow'] = function () {
  if (typeof HEAPU8 == 'undefined' || !HEAPU8 || typeof _mc3_save_box != 'function') return false;
  var h = new Int32Array(HEAPU8.buffer), b = _mc3_save_box() >>> 2;
  return Atomics.add(h, b, 1) + 1;
};
Module['mc3SaveResult'] = function (n) {
  if (typeof HEAPU8 == 'undefined' || !HEAPU8 || typeof _mc3_save_box != 'function') return 0;
  var h = new Int32Array(HEAPU8.buffer), b = _mc3_save_box() >>> 2;
  return Atomics.load(h, b + 1) === n ? Atomics.load(h, b + 2) : 0;
};

// Which gamepad drives this tab (xboxrecomp/src/input/xinput_device.c, xbox_web_pad_box): -2 auto, -1 none, n = SDL
// joystick device index. Kept in `want` so a value set before the runtime is up lands once it is. Main thread only.
(function () {
  var want = -2, timer = 0;
  function put() {
    timer = 0;
    if (typeof HEAPU8 == 'undefined' || !HEAPU8 || typeof _xbox_web_pad_box != 'function') { timer = setTimeout(put, 250); return; }
    Atomics.store(new Int32Array(HEAPU8.buffer), _xbox_web_pad_box() >>> 2, want);
  }
  Module['mc3SetPad'] = function (n) { want = n | 0; if (!timer) put(); };
})();

// The player's button layout for that pad (xinput_device.c xbox_web_pad_map_box { seq, map[1020] }): an SDL mapping
// body (ASCII, "a:b2,b:b1,..."), '' = the standard layout. Written only when it changes; map first, then seq.
(function () {
  var want = '', sent = null, timer = 0;
  function put() {
    timer = 0;
    try {
      if (!runtimeInitialized || typeof _xbox_web_pad_map_box != 'function') { timer = setTimeout(put, 1000); return; }
      if (want === sent) return;
      var b = _xbox_web_pad_map_box(), u8 = HEAPU8, i, n = Math.min(want.length, 1019);
      for (i = 0; i < n; i++) u8[b + 4 + i] = want.charCodeAt(i) & 0x7F;
      u8[b + 4 + n] = 0;
      Atomics.add(new Int32Array(u8.buffer), b >>> 2, 1);
      sent = want;
    } catch (e) { timer = setTimeout(put, 1000); }
  }
  Module['mc3SetPadMap'] = function (m) { want = String(m || ''); if (!timer) put(); };
})();

// Real pads read as untouched while a page dialog uses them (xinput_device.c xbox_web_pad_mute_box, one int).
(function () {
  var want = 0, sent = 0, timer = 0;
  function put() {
    timer = 0;
    try {
      if (!runtimeInitialized || typeof _xbox_web_pad_mute_box != 'function') { timer = setTimeout(put, 1000); return; }
      if (want === sent) return;
      Atomics.store(new Int32Array(HEAPU8.buffer), _xbox_web_pad_mute_box() >>> 2, want);
      sent = want;
    } catch (e) { timer = setTimeout(put, 1000); }
  }
  Module['mc3PadMute'] = function (on) { want = on ? 1 : 0; if (!timer) put(); };
})();

// The player's keyboard keys (sdl_keyboard.c xbox_keymap_box { int seq, on; u16 map[512] }): pairs [scancode pressed,
// default scancode it stands for], null = the default keys. Map and on first, then seq (the game lets every key go).
(function () {
  var want = null, sent, timer = 0;
  function put() {
    timer = 0;
    try {
      if (!runtimeInitialized || typeof _xbox_keymap_box != 'function') { timer = setTimeout(put, 1000); return; }
      var key = JSON.stringify(want);
      if (key === sent) return;
      var b = _xbox_keymap_box(), i32 = new Int32Array(HEAPU8.buffer), u16 = new Uint16Array(HEAPU8.buffer, b + 8, 512), i;
      u16.fill(0);
      for (i = 0; want && i < want.length; i++) if (want[i][0] > 0 && want[i][0] < 512) u16[want[i][0]] = want[i][1];
      Atomics.store(i32, (b >>> 2) + 1, want ? 1 : 0);
      Atomics.add(i32, b >>> 2, 1);
      sent = key;
    } catch (e) { timer = setTimeout(put, 1000); }
  }
  Module['mc3SetKeyMap'] = function (pairs) { want = pairs || null; if (!timer) put(); };
})();

// A player's country flag as a small picture (flags.png from tools/flags_sheet.mjs: 48x36 flags, 16 a row, in the
// order of Module.mc3Flags, the page's FLAG_CODES); em-sized, so it follows the text's size. Unknown code: null.
Module['mc3FlagEl'] = function (code) {
  var i = (Module['mc3Flags'] || []).indexOf(code);
  if (i < 0 || typeof document != 'object') return null;
  var f = document.createElement('span');
  f.style.cssText = 'display:inline-block;width:1.3333em;height:1em;margin-right:.3em;vertical-align:-.12em;' +
    'border-radius:2px;box-shadow:0 0 0 1px rgba(0,0,0,.6);background:url(flags.png) no-repeat;background-size:21.3333em auto;' +
    'background-position:' + (-(i % 16) * 1.3333) + 'em ' + (-Math.floor(i / 16)) + 'em';
  return f;
};

// Cached rects of the canvas and its parent for the overlays below (2026-11-14: each read a layout every frame right
// after its own style writes: a forced layout per rAF). Refreshed at most every 250 ms and on resize; epoch counts
// the refreshes, so an overlay can skip placing itself while nothing it reads changed. One shared object.
(function () {
  if (typeof window != 'object') return;
  var pic = { r: null, L: null, epoch: 0 }, t = -1e9;
  addEventListener('resize', function () { t = -1e9; });
  Module['mc3Pic'] = function (now) {
    var cv = Module['canvas'];
    if (!(now - t < 250) && cv && cv.parentNode) { t = now; pic.r = cv.getBoundingClientRect(); pic.L = cv.parentNode.getBoundingClientRect(); pic.epoch++; }
    return pic;
  };
})();

// Name tags: other players' names over their cars in free roam, on the game's own opponent arrow. src/games/remix/
// roam_tags.c publishes, each frame of the main camera, where each driven remote car's arrow tip is on the picture
// (roam_tags_box: seq, n, picture width / height, the arrow's colour 0xRRGGBB, then n x 48 bytes: u, v (0..1), the
// arrow's alpha (it fades in from 25 to 100 m and out from 200 to 300 m), name (28), flag code (8, '' none: an older
// build or no flag; drawn from the page's flags.png before the name, Module.mc3FlagEl)); drawn here over the canvas at the
// page's frame rate, in the arrow's colour, no background, sized to the picture. Settings 'Name tags'
// (MC3_NAMETAGS=0 off), live through Module.mc3SetNameTags. Main thread only.
(function () {
  if (typeof document != 'object' || typeof requestAnimationFrame != 'function') return;
  var canvas = Module['canvas'], on = (Module['mc3Env'] || {}).MC3_NAMETAGS !== '0';
  // Settings 'Name tag size' (MC3_NAMETAG_SIZE, %, 50..150, page-only), live through Module.mc3SetNameTagSize
  var size = Math.min(150, Math.max(50, Number((Module['mc3Env'] || {}).MC3_NAMETAG_SIZE) || 50)) / 100;
  var layer = null, pool = [], shown = 0, seq = -1, seenAt = 0, I32 = null, F32 = null, U8 = null;
  if (!canvas) return;
  Module['mc3SetNameTags'] = function (v) { on = !!v; };
  Module['mc3SetNameTagSize'] = function (v) { size = Math.min(150, Math.max(50, Number(v) || 50)) / 100; };
  function hideFrom(n) { for (; shown > n; shown--) { pool[shown - 1].style.display = 'none'; pool[shown - 1]._d = 0; } }
  function frame(now) {
    requestAnimationFrame(frame);
    if (typeof HEAPU8 == 'undefined' || !HEAPU8 || typeof _roam_tags_box != 'function' || !canvas.parentNode) return;
    if (!I32) {   // the box is a static, below the initial memory: views made now still reach it after growth
      I32 = new Int32Array(HEAPU8.buffer); F32 = new Float32Array(HEAPU8.buffer); U8 = HEAPU8;
      layer = document.createElement('div');
      layer.style.cssText = 'position:absolute;inset:0;pointer-events:none;overflow:hidden;z-index:1';
      canvas.parentNode.append(layer);
    }
    var b = _roam_tags_box() >>> 2, s = I32[b], n = I32[b + 1], aspect = F32[b + 2],
        rgb = '#' + ('00000' + (I32[b + 3] & 0xFFFFFF).toString(16)).slice(-6);
    if (s !== seq) { seq = s; seenAt = now; }
    if (!on || now - seenAt > 400 || !(n > 0) || !(aspect > 0)) { hideFrom(0); return; }   // stale: game paused or gone
    n = Math.min(n, 32);
    // the picture inside the canvas: object-fit contain, or fill with the stage's Stretch
    var pc = Module['mc3Pic'](now), r = pc.r, L = pc.L, x = r.left - L.left, y = r.top - L.top,
        w = r.width, h = r.height;
    if (!canvas.parentNode.classList.contains('stretch')) {
      if (w / h > aspect) { x += (w - h * aspect) / 2; w = h * aspect; } else { y += (h - w / aspect) / 2; h = w / aspect; }
    }
    var fsz = (Math.max(10, h * 0.022) * size).toFixed(1) + 'px';
    for (var i = 0; i < n; i++) {
      var t = b + 4 + i * 12, el = pool[i], name = '', flag = '';
      for (var c = 0, p = (t + 3) * 4; c < 27 && U8[p + c]; c++) name += String.fromCharCode(U8[p + c]);
      for (c = 0, p = (t + 10) * 4; c < 7 && U8[p + c]; c++) flag += String.fromCharCode(U8[p + c]);
      var racing = name.charCodeAt(0) === 2;   // going to race or racing (roam_tags.c rch_marked): flag, gold
      if (racing) name = '🏁 ' + name.slice(1);
      if (!el) {
        el = pool[i] = document.createElement('div');
        el.style.cssText = 'position:absolute;left:0;top:0;white-space:nowrap;font-weight:700;' +
          'font-family:"Arial Narrow","Roboto Condensed",Arial,sans-serif;letter-spacing:.02em;' +
          'text-shadow:0 0 2px #000,0 1px 1px #000';
        layer.append(el);
      }
      if (el.dataset.k !== flag + '|' + name) {   // the flag's picture, then the name
        el.dataset.k = flag + '|' + name;
        el.textContent = '';
        var fe = Module['mc3FlagEl'](flag);
        if (fe) el.append(fe);
        el.append(name);
      }
      // styles are written only on change (an unchanged write still dirtied style)
      var col = racing ? '#ffd24a' : rgb;
      if (el._c !== col) el.style.color = el._c = col;
      if (el._f !== fsz) el.style.fontSize = el._f = fsz;   // about the HUD's small text, times the setting
      var tr = 'translate(' + (x + F32[t] * w).toFixed(1) + 'px,' + (y + F32[t + 1] * h).toFixed(1) + 'px) translate(-50%,-100%)';
      if (el._t !== tr) el.style.transform = el._t = tr;
      var op = Math.max(0, Math.min(1, F32[t + 2])).toFixed(2);   // the arrow's own fade
      if (el._o !== op) el.style.opacity = el._o = op;
      if (!el._d) { el.style.display = ''; el._d = 1; }
    }
    if (n > shown) shown = n;
    hideFrom(n);
  }
  requestAnimationFrame(frame);
})();

// Players panel: while the full map (I) is open in Free Roam, src/games/remix/roam_map.c counts its frames in
// roam_bigmap_box (seq, our city: the roam room), and this lists the players online in Free Roam beside it, from the
// switch's players push (Module.mc3Lan.players.list, web/lan.js: mode 'roam', city = room id): our city first, then the
// others with their city. On the right edge of the picture, at most 20% of it wide (32:9: beside the HUD's middle
// 16:9); hidden without a list (web/server's switch has none). Flags: ours from MC3_PLAYER_FLAG, the linked players'
// from roam_bigmap_box (name + flag rows; the push has none, players beyond our links get none). Main thread only.
// Folded (2026-11-15, owner: the map was cluttered): one small chip on the right edge (people icon + count, a flag
// while our race has standings); a click / tap opens the players, progress and race panels under it and closes them
// (localStorage mc3.mapPanels '1' open, default closed).
(function () {
  if (typeof document != 'object' || typeof requestAnimationFrame != 'function') return;
  var canvas = Module['canvas'], el = null, pel = null, pshown = null, rel = null, rshown = null, seq = -1, seenAt = 0, I32 = null, F32 = null, shown = null;
  var chip = null, chipKey = '', expanded = false;
  try { expanded = localStorage.getItem('mc3.mapPanels') === '1'; } catch (e) {}
  var PEOPLE = '<svg viewBox="0 0 16 16" width="1.15em" height="1.15em" fill="#7dff9a" aria-hidden="true" style="vertical-align:-.2em">' +
    '<circle cx="5.5" cy="5" r="2.5"/><path d="M1 14c0-3 2-4.5 4.5-4.5S10 11 10 14z"/><circle cx="11.5" cy="5.5" r="2"/>' +
    '<path d="M10.6 9.6c2.6-.4 4.4 1 4.4 4.4h-4c0-1.8-.2-3.2-.4-4.4z"/></svg>';
  var CITIES = ['San Diego', 'Atlanta', 'Detroit', 'Tokyo'];   // free-roam room ids 0..3 (web/index.html)
  var me = String((Module['mc3Env'] || {}).MC3_PLAYER_NAME || '').toLowerCase(), myFlag = (Module['mc3Env'] || {}).MC3_PLAYER_FLAG || '';
  if (!canvas) return;
  function city(c) { return CITIES[c] || 'City ' + c; }
  function row(text, css) { var d = document.createElement('div'); d.textContent = text; if (css) d.style.cssText = css; return d; }
  function frame(now) {
    requestAnimationFrame(frame);
    if (typeof HEAPU8 == 'undefined' || !HEAPU8 || typeof _roam_bigmap_box != 'function' || !canvas.parentNode) return;
    if (!I32) {   // a static box (as above): the view stays valid after growth
      I32 = new Int32Array(HEAPU8.buffer);
      F32 = new Float32Array(HEAPU8.buffer);
      el = document.createElement('div');
      el.style.cssText = 'position:absolute;left:0;top:0;pointer-events:none;z-index:1;box-sizing:border-box;' +
        'padding:.5em .8em;border-radius:8px;background:rgba(0,0,0,.65);border:1px solid rgba(255,255,255,.2);' +
        'color:#fff;text-shadow:0 1px 2px #000;overflow:hidden;white-space:nowrap;line-height:1.3;' +
        'font-weight:700;font-family:"Arial Narrow","Roboto Condensed",Arial,sans-serif;display:none';
      canvas.parentNode.append(el);
      // the players panel scrolls (wheel, drag on touch); the progress and race panels copy cssText, then undo this
      el.style.cssText += ';pointer-events:auto;overflow-x:hidden;overflow-y:auto;overscroll-behavior:contain;' +
        'scrollbar-width:thin;scrollbar-color:rgba(255,255,255,.35) transparent';
      chip = document.createElement('div');
      chip.style.cssText = 'position:absolute;left:0;top:0;z-index:1;display:none;pointer-events:auto;cursor:pointer;' +
        'touch-action:manipulation;box-sizing:border-box;white-space:nowrap;padding:.15em .55em;border-radius:1em;' +
        'background:rgba(0,0,0,.65);border:1px solid rgba(255,255,255,.25);color:#fff;text-shadow:0 1px 2px #000;' +
        'font-weight:700;font-family:"Arial Narrow","Roboto Condensed",Arial,sans-serif';
      chip.title = 'Players online';
      chip.addEventListener('pointerdown', function (ev) {   // not a click: the canvas keeps focus (keys)
        ev.preventDefault();
        ev.stopPropagation();
        expanded = !expanded;
        try { localStorage.setItem('mc3.mapPanels', expanded ? '1' : '0'); } catch (e) {}
      });
      canvas.parentNode.append(chip);
    }
    var b = _roam_bigmap_box() >>> 2, s = I32[b], here = I32[b + 1], flags = {}, fkey = '';
    for (var fi = 0, fn = Math.min(I32[b + 2] | 0, 64); fi < fn; fi++) {   // 32-byte rows: name (25), flag (7)
      var fp = (b + 3) * 4 + fi * 32, fname = '', fcode = '';
      for (var fc = 0; fc < 24 && HEAPU8[fp + fc]; fc++) fname += String.fromCharCode(HEAPU8[fp + fc]);
      for (fc = 0; fc < 6 && HEAPU8[fp + 25 + fc]; fc++) fcode += String.fromCharCode(HEAPU8[fp + 25 + fc]);
      flags[fname.toLowerCase()] = fcode;
      fkey += fname + fcode;
    }
    if (me) flags[me] = myFlag;
    function flagged(d, name) { var f = Module['mc3FlagEl'] && Module['mc3FlagEl'](flags[String(name).toLowerCase()]); if (f) d.prepend(f); return d; }
    if (s !== seq) { seq = s; seenAt = now; }
    var lan = Module['mc3Lan'], list = lan && lan.players && Array.isArray(lan.players.list) ? lan.players.list : null;
    var open = now - seenAt <= 400, show = open && !!list && expanded;
    Module['mc3BigMapOpen'] = open;   // the page (index.html) subscribes to the players push only while the full map is up   // map closed, paused or gone; no list: players hidden
    if (!show) el.style.display = 'none';
    if (!open) { chip.style.display = 'none'; if (pel) pel.style.display = 'none'; if (rel) rel.style.display = 'none'; return; }
    var roam = (list || []).filter(function (p) { return p && p.mode == 'roam'; }), mine = [], other = [];
    roam.forEach(function (p) { (p.city === here ? mine : other).push(p); });
    // who the game knows is racing (the Race panel's box below; the push has no race bit): our nick and the peers in it
    var racing = {}, iRace = false, qb = typeof _roam_race_box == 'function' ? _roam_race_box() >>> 2 : 0, qm = qb ? I32[qb] : 0;
    if (qm) {
      var qe = (qb + 1) << 2, qend = qe + 700;
      while (qe < qend && HEAPU8[qe]) qe++;
      new TextDecoder().decode(HEAPU8.slice((qb + 1) << 2, qe)).split('\n').slice(1).forEach(function (l) {
        var c = l.split('\t');
        if (qm == 2) { if (c[1] == 'me') iRace = true; else if (c[0]) racing[c[0].toLowerCase()] = 1; }
        else if (c[1] == 'You') iRace = true;
        else if (c[1] && !/out|DNF|invited/.test(c[3] || '')) racing[c[1].toLowerCase()] = 1;
      });
    }
    var key = here + '|' + fkey + '|' + (iRace ? 'R' : '') + roam.map(function (p) { return p.name + '\u0001' + p.city + (p.career ? 'c' : '') + (racing[String(p.name).toLowerCase()] ? 'r' : ''); }).join('\u0002');
    if (show && key !== shown) {
      shown = key;
      var keep = el.scrollTop;
      el.textContent = '';
      el.append(row('Online · ' + roam.length + ' playing', 'font-size:1.1em;letter-spacing:.03em;margin-bottom:.2em'));
      // our city split by mode: the switch's p.career = a career cruise with Online career on ('(racing)' needs the
      // roam bits, which the push lacks)
      [['Free Roam', false], ['Online career', true]].forEach(function (g) {
        var grp = mine.filter(function (p) { return !!p.career == g[1]; });
        if (!grp.length) return;
        el.append(row(city(here) + ' · ' + g[0] + ' (' + grp.length + ')', 'color:#ffd24a;font-size:.85em;margin-top:.2em'));
        function rc(p) { var k = String(p.name).toLowerCase(); return racing[k] || (k == me && iRace) ? 1 : 0; }
        grp.slice().sort(function (a, b) { return rc(b) - rc(a); }).forEach(function (p) {   // racers first
          var you = String(p.name).toLowerCase() == me, r = rc(p);
          el.append(flagged(row((r ? '🏁 ' : '') + p.name + (r ? ' · racing' : ''), you ? 'color:#7fd4ff' : r ? 'color:#ffd24a' : ''), p.name));
        });
      });
      if (other.length) el.append(row('Other cities', 'color:#ffd24a;font-size:.85em;margin-top:.35em'));
      other.forEach(function (p) {
        el.append(row(p.name + ' · ' + city(p.city) + (p.career ? ' (career)' : ''), 'opacity:.75'));   // other cities: the game does not see them, unmarked
      });
      var more = lan.players.more | 0;   // beyond the switch's own list
      if (more > 0) el.append(row('+' + more + ' not listed', 'opacity:.6;font-size:.85em'));
      el.scrollTop = keep;
    }
    // the picture inside the canvas (object-fit contain unless Stretch), its shape as the name tags got it
    var pc = Module['mc3Pic'](now), r = pc.r, L = pc.L, x = r.left - L.left,
        y = r.top - L.top, w = r.width, h = r.height, aspect = 16 / 9;
    if (typeof _roam_tags_box == 'function') {
      var a = F32[(_roam_tags_box() >>> 2) + 2];
      if (a > 0.5 && a < 5) aspect = a;
    }
    if (!canvas.parentNode.classList.contains('stretch')) {
      if (w / h > aspect) { x += (w - h * aspect) / 2; w = h * aspect; } else { y += (h - w / aspect) / 2; h = w / aspect; }
    }
    if (!(w > 0 && h > 0)) { chip.style.display = el.style.display = 'none'; if (pel) pel.style.display = 'none'; if (rel) rel.style.display = 'none'; return; }
    var st = el.style, pad = Math.max(8, h * 0.02), fs = Math.max(10, h * 0.022).toFixed(1) + 'px', right = x + w - pad;
    // the chip: people + count (none without the push), a flag while our race has standings, the fold's arrow
    var rb = typeof _roam_race_box == 'function' ? _roam_race_box() >>> 2 : 0, rmode = rb ? I32[rb] : 0;
    var ck = (list ? roam.length : '') + '|' + (rmode == 1) + '|' + expanded;
    if (ck !== chipKey) {
      chipKey = ck;
      chip.innerHTML = PEOPLE;
      chip.append((list ? ' ' + roam.length : '') + (rmode == 1 ? ' \ud83c\udfc1' : '') + (expanded ? ' ▾' : ' ▸'));
    }
    var cs = chip.style, ctop = y + h * 0.1;
    cs.fontSize = fs;
    cs.transform = 'translate(' + right.toFixed(1) + 'px,' + ctop.toFixed(1) + 'px) translateX(-100%)';
    cs.display = '';
    var gap = Math.max(6, h * 0.015), ptop = ctop + chip.offsetHeight + gap;
    st.fontSize = fs;
    st.maxWidth = (w * 0.2).toFixed(0) + 'px';
    st.maxHeight = (h * 0.7).toFixed(0) + 'px';
    st.transform = 'translate(' + right.toFixed(1) + 'px,' + ptop.toFixed(1) + 'px) translateX(-100%)';
    if (show) st.display = '';
    // Online progress panel (src/games/remix/online_progress.c roam_progress_box: online, wins, "Latest\nNext"),
    // to the left of the players panel (the right edge when that is hidden), same top.
    var edge = right - (show ? el.offsetWidth + gap : 0), top = ptop.toFixed(1);
    var pb = typeof _roam_progress_box == 'function' ? _roam_progress_box() >>> 2 : 0;
    if (!pb || !I32[pb] || !expanded) { if (pel) pel.style.display = 'none'; }   // not the Online session, or folded
    else {
      if (!pel) {
        pel = document.createElement('div');
        pel.style.cssText = el.style.cssText;
        pel.style.whiteSpace = 'normal';
        pel.style.pointerEvents = 'none';
        pel.style.overflow = 'hidden';
        canvas.parentNode.append(pel);
      }
      var pe = (pb + 2) << 2, pend = pe + 160;
      while (pe < pend && HEAPU8[pe]) pe++;
      var lines = new TextDecoder().decode(HEAPU8.slice((pb + 2) << 2, pe)).split('\n'), ptext = I32[pb + 1] + '|' + lines.join('|');
      if (ptext !== pshown) {
        pshown = ptext;
        pel.textContent = '';
        pel.append(row('Online progress', 'font-size:1.1em;letter-spacing:.03em;margin-bottom:.2em'));
        pel.append(row('Races won: ' + I32[pb + 1]));
        pel.append(row('Latest: ' + lines[0], 'opacity:.85'));
        pel.append(row(lines[1] ? 'Next: ' + lines[1] : 'Everything unlocked', 'color:#ffd24a;font-size:.85em;margin-top:.2em'));
      }
      var ps = pel.style;
      ps.fontSize = fs;
      ps.width = (w * 0.2).toFixed(0) + 'px';
      ps.maxHeight = (h * 0.7).toFixed(0) + 'px';
      ps.transform = 'translate(' + edge.toFixed(1) + 'px,' + top + 'px) translateX(-100%)';
      ps.display = '';
      edge -= pel.offsetWidth + gap;
    }
    // Race panel (src/games/remix/roam_challenge.c roam_race_box: mode 1 our race / 2 players racing near, '\n' lines,
    // '\t' columns), left of the progress panel; hidden with nothing to show.
    // Mode 2 ('Racing now', players racing near) is not drawn (2026-11-13: it covered the minimap); the players panel
    // marks those racers instead.
    if (rmode != 1 || !expanded) { if (rel) rel.style.display = 'none'; return; }
    if (!rel) {
      rel = document.createElement('div');
      rel.style.cssText = el.style.cssText;
      rel.style.whiteSpace = 'normal';
      rel.style.pointerEvents = 'none';
      rel.style.overflow = 'hidden';
      canvas.parentNode.append(rel);
    }
    var re = (rb + 1) << 2, rend = re + 700;
    while (re < rend && HEAPU8[re]) re++;
    var rtext = new TextDecoder().decode(HEAPU8.slice((rb + 1) << 2, re));
    if (rtext !== rshown) {
      rshown = rtext;
      rel.textContent = '';
      var rl = rtext.split('\n'), hd = rl[0].split('\t');
      {
        rel.append(row('Race · ' + hd[2], 'font-size:1.1em;letter-spacing:.03em'));
        rel.append(row(hd[0] + (hd[1] && hd[1] != 'start beacons' ? ' · ' + hd[1] : ''), 'color:#ffd24a;font-size:.85em;margin-bottom:.2em'));
        rl.slice(1).forEach(function (l) {
          var c = l.split('\t'), d = document.createElement('div');
          d.style.cssText = 'display:flex;gap:.5em;white-space:nowrap';
          [c[0] ? c[0] + '.' : '', c[1], c[2], c[3]].forEach(function (t, i) {
            var s = document.createElement('span');
            s.textContent = t || '';
            if (i == 0) s.style.minWidth = '1.2em';
            if (i == 1) { s.style.cssText = 'flex:1;overflow:hidden;text-overflow:ellipsis'; if (c[1] == 'You') s.style.color = '#7fd4ff'; }
            if (i == 3) s.style.cssText = 'font-size:.85em;color:' + (/DNF|out/.test(t || '') ? '#ff8a7a' : '#ffd24a');
            d.append(s);
          });
          rel.append(d);
        });
      }
    }
    var rs = rel.style;
    rs.fontSize = fs;
    rs.width = (w * 0.22).toFixed(0) + 'px';
    rs.maxHeight = (h * 0.7).toFixed(0) + 'px';
    rs.transform = 'translate(' + edge.toFixed(1) + 'px,' + top + 'px) translateX(-100%)';
    rs.display = '';
  }
  requestAnimationFrame(frame);
})();

// Race prompt: near a race start point or a player in free roam, src/games/remix/roam_race_ai.c / roam_challenge.c
// publish up to three lines joined by '\n' (roam_prompt_box: seq, then 192 bytes of UTF-8, empty = none: title,
// details, action; '[Q]' in the action is drawn as a keycap). Shown while fresh (the game updates it every frame) in a
// small box on the left of the picture, just above the HUD's minimap (bottom-left of the HUD's middle 16:9 under Hor+),
// at most 32% of the picture wide, and kept above the touch controls. A text starting with \x01 (the countdown, GO, the
// first 2 s of a result) is big and centred instead. Placed from the game picture's rect (letterboxing, Hor+), as the
// name tags. Main thread only.
// Its button (2026-11-12): roam_challenge.c's roam_race_button_box (act, click): act = what quitting does now (1 cancel
// the race we host, 2 leave a race we joined, 3 give up a running race, 4 decline an invite; 0 none), shown as a
// button under the text; a tap writes that act into click (Module.mc3RaceButton), the game takes it while still offered.
Module['mc3RaceButton'] = function (act) {
  if (typeof HEAPU8 == 'undefined' || !HEAPU8 || typeof _roam_race_button_box != 'function') return;
  Atomics.store(new Int32Array(HEAPU8.buffer), (_roam_race_button_box() >>> 2) + 1, act | 0);
};
// Minimal (2026-11-15, owner: race prompts cluttered the HUD): the idle offer ('\x02') is an icon chip (flag + Q
// keycap), its name only on hover or a tap of the chip; an invite received ('\x03' card '<host> · Race (N)\n[Q] join ·
// [E] decline · S s') is one line: flag, '<host> +N' (N other racers) + a green Q (act 5, join) and a red E (4, decline) button;
// other cards one line, the quit button inline (the native 'Hold Black [E] ...' line becomes its E keycap). Invites go
// into an inbox (envelope + badge of those still open, left of the box; kept 5 min, 8 at most): a tap lists host,
// players, seconds left (expired greyed); Accept only on the one the game offers now. Page only: the game's texts are
// unchanged.
(function () {
  if (typeof document != 'object' || typeof requestAnimationFrame != 'function') return;
  var canvas = Module['canvas'], el = null, ib = null, ibBadge = null, panel = null, seq = -1, seenAt = 0, I32 = null, F32 = null,
      shown = null, big = false, offer = false, card = false, placed = '', glow = false;
  var ACTS = { 1: 'Cancel race', 2: 'Leave race', 3: 'Give up race', 4: 'Decline' };
  // the minimap's top-left in the HUD's 16:9 (720x480 layout): left edge and top, fractions of the HUD's width/height
  var MAP_LEFT = 0.055, MAP_TOP = 0.64, GAP = 0.015;
  var FONT = 'font-weight:700;font-family:"Arial Narrow","Roboto Condensed",Arial,sans-serif';
  var FLAG = '<svg viewBox="0 0 16 16" width="1.1em" height="1.1em" fill="#7dff9a" aria-hidden="true" style="flex:none">' +
    '<path d="M3 1h1.5v14H3z"/><path d="M4.5 2h9l-2.5 3.2 2.5 3.2h-9z"/></svg>';
  var INVITE = /^(.+) · Race \((\d+)\)$/;
  var nameEl = null, hover = false, peek = false;   // the offer chip's name
  var invites = [], inboxOpen = false, panelKey = '';   // { host, n, exp (page ms) }, newest first
  if (!canvas) return;
  function keycap(t, small) {
    var k = document.createElement('span');
    k.textContent = t;
    k.style.cssText = 'display:inline-block;min-width:1.3em;padding:0 .3em;margin:0 .1em;border-radius:4px;border:1px solid #fff;' +
      'border-bottom-width:' + (small ? 2 : 3) + 'px;background:#333;color:#fff;text-shadow:none;text-align:center;line-height:1.2;' +
      'font-size:' + (small ? '.8em' : '1em');
    return k;
  }
  function withKeys(d, line) {   // '[Q]' = a keycap
    line.split(/(\[[^\]]+\])/).forEach(function (part) {
      if (/^\[.+\]$/.test(part)) d.append(keycap(part.slice(1, -1))); else if (part) d.append(part);
    });
    return d;
  }
  function onTap(node, fn) {   // pointerdown, not a click: the canvas keeps focus (keys)
    node.addEventListener('pointerdown', function (ev) { ev.preventDefault(); ev.stopPropagation(); fn(ev); });
  }
  function smallBtn(label, key, bg, act) {
    var b = document.createElement('button');
    b.type = 'button';
    b.tabIndex = -1;
    b.append(label);
    if (key) b.append(keycap(key, true));
    b.style.cssText = 'pointer-events:auto;flex:none;padding:.05em .5em;min-height:1.9em;border-radius:5px;' +
      'border:1px solid rgba(255,255,255,.6);background:' + bg + ';color:#fff;font:inherit;font-size:.85em;cursor:pointer;' +
      'touch-action:manipulation;white-space:nowrap';
    onTap(b, function () { Module['mc3RaceButton'](act); });
    return b;
  }
  function flexRow() { var d = document.createElement('div'); d.style.cssText = 'display:flex;align-items:center;gap:.35em'; return d; }
  function peekName() { if (nameEl) nameEl.style.display = hover || peek ? '' : 'none'; }
  function offerName(t) {   // 'Race N' the street race, '<race> · N' a group race, '<race>' the AI race
    var m = /^Race (\d+)$/.exec(t);
    return m ? 'Street race · ' + m[1] + ' nearby' : t.replace(/ · (\d+)$/, ' · $1 nearby');
  }
  function frame(now) {
    requestAnimationFrame(frame);
    if (typeof HEAPU8 == 'undefined' || !HEAPU8 || typeof _roam_prompt_box != 'function' || !canvas.parentNode) return;
    if (!I32) {   // a static box (as above): the view stays valid after growth
      I32 = new Int32Array(HEAPU8.buffer);
      F32 = new Float32Array(HEAPU8.buffer);
      el = document.createElement('div');
      el.style.cssText = 'position:absolute;left:0;top:0;pointer-events:none;z-index:1;box-sizing:border-box;' +
        'padding:.45em .8em;border-radius:8px;background:rgba(0,0,0,.72);border:1px solid rgba(255,255,255,.25);' +
        'box-shadow:0 2px 10px rgba(0,0,0,.6);color:#fff;text-shadow:0 1px 2px #000;overflow-wrap:anywhere;' +
        'touch-action:manipulation;' + FONT + ';display:none';
      // only the offer chip takes the pointer (pointer-events below): hover (mouse) or a tap shows its name
      el.addEventListener('pointerenter', function (ev) { if (ev.pointerType == 'mouse') { hover = true; peekName(); } });
      el.addEventListener('pointerleave', function () { hover = false; peekName(); });
      onTap(el, function () { peek = !peek; peekName(); });
      ib = document.createElement('div');   // the invites inbox: envelope + badge
      ib.style.cssText = 'position:absolute;left:0;top:0;z-index:1;display:none;pointer-events:auto;cursor:pointer;' +
        'touch-action:manipulation;box-sizing:border-box;white-space:nowrap;padding:.15em .4em;border-radius:8px;' +
        'background:rgba(0,0,0,.65);border:1px solid rgba(255,255,255,.25);color:#fff;' + FONT;
      ib.innerHTML = '<svg viewBox="0 0 16 16" width="1.2em" height="1.2em" fill="none" stroke="#fff" stroke-width="1.4" ' +
        'aria-hidden="true" style="vertical-align:middle"><rect x="1.5" y="3.5" width="13" height="9" rx="1"/>' +
        '<path d="M2 4.5l6 4.5 6-4.5"/></svg>';
      ibBadge = document.createElement('span');
      ibBadge.style.cssText = 'display:none;margin-left:.2em;padding:0 .35em;border-radius:1em;background:#2a8a3a;' +
        'color:#fff;font-size:.8em;vertical-align:middle';
      ib.append(ibBadge);
      ib.title = 'Race invites';
      onTap(ib, function () { inboxOpen = !inboxOpen; panelKey = ''; });
      panel = document.createElement('div');
      panel.style.cssText = 'position:absolute;left:0;top:0;z-index:1;display:none;pointer-events:none;box-sizing:border-box;' +
        'white-space:nowrap;padding:.3em .6em;border-radius:8px;background:rgba(0,0,0,.72);border:1px solid rgba(255,255,255,.25);' +
        'color:#fff;text-shadow:0 1px 2px #000;line-height:1.5;' + FONT;
      canvas.parentNode.append(el, ib, panel);
    }
    var b = _roam_prompt_box(), s = I32[b >>> 2], text = '';
    if (s !== seq) { seq = s; seenAt = now; }
    var fresh = now - seenAt < 400;   // stale: paused, loading or gone
    if (fresh) {
      var e = b + 4, end = e + 192;   // RRA_TEXT bytes, NUL ended, UTF-8
      while (e < end && HEAPU8[e]) e++;
      text = new TextDecoder().decode(HEAPU8.slice(b + 4, e));
    }
    var act = text && typeof _roam_race_button_box == 'function' ? I32[_roam_race_button_box() >>> 2] | 0 : 0;
    if (!ACTS[act]) act = 0;
    // an invite received (roam_challenge.c): into the inbox, one entry per host and invite
    var lines = text.charCodeAt(0) === 3 ? text.slice(1).split('\n') : null, m = lines && lines.length > 1 && INVITE.exec(lines[0]), inv = null;
    if (m) {
      var secs = /(\d+) s$/.exec(lines[1]);
      inv = { host: m[1], n: m[2] | 0, left: secs ? secs[1] | 0 : 0, slow: /^Slow down/.test(lines[1]) };
      var ent = null;
      invites.forEach(function (x) { if (!ent && x.host.toLowerCase() == inv.host.toLowerCase() && x.exp > now - 1000) ent = x; });
      if (!ent) { invites.unshift(ent = { host: inv.host }); invites.length = Math.min(invites.length, 8); }
      ent.n = inv.n;
      ent.exp = now + inv.left * 1000;
    }
    var current = inv && act == 4 ? inv.host.toLowerCase() : '';   // the invite the game takes Accept for now
    invites = invites.filter(function (x) { return now - x.exp < 300000; });
    if (text + '|' + act !== shown) {
      shown = text + '|' + act;
      big = text.charCodeAt(0) === 1;
      offer = text.charCodeAt(0) === 2;   // the idle offer: an icon chip, its name on hover / tap
      card = text.charCodeAt(0) === 3;   // compact card: one line
      var t = big || offer || card ? text.slice(1) : text;
      el.textContent = '';
      nameEl = null;
      if (!offer) hover = peek = false;
      var os = el.style;
      os.background = offer ? 'rgba(20,110,50,.6)' : 'rgba(0,0,0,.72)';
      os.borderColor = offer ? 'rgba(120,255,150,.7)' : 'rgba(255,255,255,.25)';
      os.padding = offer ? '.2em .4em' : card ? '.2em .5em' : '.45em .8em';
      os.whiteSpace = offer || card ? 'nowrap' : '';
      os.boxShadow = offer || card ? 'none' : '0 2px 10px rgba(0,0,0,.6)';
      os.pointerEvents = offer ? 'auto' : 'none';
      os.cursor = offer ? 'pointer' : '';
      if (offer) {
        var pd = flexRow();
        pd.innerHTML = FLAG;
        pd.append(keycap('Q'));
        nameEl = document.createElement('span');
        nameEl.textContent = offerName(t);
        pd.append(nameEl);
        peekName();
        el.append(pd);
      } else if (card) {
        var cd = flexRow();
        if (inv) {
          cd.insertAdjacentHTML('beforeend', FLAG);
          // owner 2026-11-15: as short as it gets: flag, '<host> +N' and two keycap buttons (green Q join, red E no)
          cd.append(inv.host + (inv.n > 2 ? ' +' + (inv.n - 2) : '') + (inv.slow ? ' · slow' : ''));
          if (act == 4) cd.append(smallBtn('', 'Q', '#2a8a3a', 5), smallBtn('', 'E', '#a3262a', 4));
        } else {
          withKeys(cd, lines[0]);   // a second line is the native 'Hold Black [E] ...': the button's keycap instead
          if (act) cd.append(smallBtn(ACTS[act].replace(' race', ''), 'E', '#a3262a', act));
        }
        el.append(cd);
      } else {
        t.split('\n').forEach(function (line, i) {
          var d = document.createElement('div');
          if (i === 0) d.style.cssText = big ? 'font-size:2.6em;line-height:1.05;color:#ffd24a' : 'font-size:1.15em;letter-spacing:.03em';
          else if (i === 1) d.style.cssText = 'opacity:.85;font-size:.9em';
          else if (i >= 3) d.style.cssText = 'margin-top:.2em;font-size:.8em;opacity:.8';   // the quit hint
          else d.style.cssText = 'margin-top:.25em;color:#ffd24a';
          if (t) el.append(withKeys(d, line));
        });
        if (act) {   // the box ignores the pointer; its buttons take it (mouse and touch)
          var bar = document.createElement('div');
          bar.style.cssText = 'display:flex;gap:.5em;margin-top:.45em' + (big ? ';justify-content:center' : '');
          (act == 4 ? [[5, 'Accept', '#2a8a3a'], [4, ACTS[4], '#a3262a']] : [[act, ACTS[act], '#a3262a']]).forEach(function (o) {
            var btn = smallBtn(o[1], '', o[2], o[0]);
            btn.style.padding = '.3em 1em';
            btn.style.minHeight = '2.2em';
            btn.style.fontSize = '.9em';
            bar.append(btn);
          });
          el.append(bar);
        }
      }
    }
    var wantGlow = !!text && (offer || (card && text.indexOf('[Q]') >= 0));   // touch Flash green while White can act (offer, invite, start)
    if (wantGlow !== glow) {
      glow = wantGlow;
      var fb = document.querySelector('.tc-white');
      if (fb) fb.classList.toggle('offer', glow);
    }
    // the inbox: badge = invites still open; the list rebuilt when a second ticks or Accept moves
    var live = 0, showIb = fresh && invites.length > 0;
    invites.forEach(function (x) { if (x.exp > now) live++; });
    if (!showIb) inboxOpen = false;
    var bt = live ? String(live) : '';
    if (ibBadge.textContent !== bt) { ibBadge.textContent = bt; ibBadge.style.display = live ? '' : 'none'; ib.style.opacity = live ? '1' : '.6'; }
    if (inboxOpen) {
      var pkey = current + '|' + invites.map(function (x) { return x.host + x.n + (x.exp > now ? Math.ceil((x.exp - now) / 1000) : 'x'); }).join(',');
      if (pkey !== panelKey) {
        panelKey = pkey;
        panel.textContent = '';
        var hd = document.createElement('div');
        hd.textContent = 'Race invites';
        hd.style.cssText = 'font-size:.85em;color:#7dff9a';
        panel.append(hd);
        invites.forEach(function (x) {
          var d = flexRow(), open = x.exp > now;
          d.append(x.host + ' · ' + x.n + ' players · ' + (open ? Math.ceil((x.exp - now) / 1000) + ' s' : 'expired'));
          if (!open) d.style.opacity = '.45';
          if (open && x.host.toLowerCase() == current) d.append(smallBtn('Accept', 'Q', '#2a8a3a', 5));
          panel.append(d);
        });
      }
    }
    if (!text && !showIb) {
      if (placed) { el.style.display = ib.style.display = panel.style.display = 'none'; placed = ''; }
      return;
    }
    // the picture inside the canvas (object-fit contain unless Stretch), its shape as the name tags got it
    var pc = Module['mc3Pic'](now), r = pc.r, L = pc.L, x = r.left - L.left,
        y = r.top - L.top, w = r.width, h = r.height, aspect = 16 / 9;
    if (typeof _roam_tags_box == 'function') {
      var a = F32[(_roam_tags_box() >>> 2) + 2];
      if (a > 0.5 && a < 5) aspect = a;
    }
    if (!canvas.parentNode.classList.contains('stretch')) {
      if (w / h > aspect) { x += (w - h * aspect) / 2; w = h * aspect; } else { y += (h - w / aspect) / 2; h = w / aspect; }
    }
    if (!(w > 0 && h > 0)) { el.style.display = ib.style.display = panel.style.display = 'none'; placed = ''; return; }
    // placement only when an input changed: it reads layout (offsetHeight, touch buttons' rects) after last frame's writes
    var pk = shown + '|' + pc.epoch + '|' + x + '|' + y + '|' + w + '|' + h + '|' + showIb + bt + '|' + (inboxOpen && panelKey);
    if (pk === placed) return;
    placed = pk;
    var st = el.style, small = Math.max(10, h * 0.018).toFixed(1) + 'px';
    st.display = text ? '' : 'none';
    ib.style.display = showIb ? '' : 'none';
    panel.style.display = showIb && inboxOpen ? '' : 'none';
    ib.style.fontSize = panel.style.fontSize = small;
    if (big) {
      st.fontSize = Math.max(14, h * 0.03).toFixed(1) + 'px';
      st.maxWidth = (w * 0.6).toFixed(0) + 'px';
      st.textAlign = 'center';
      st.transform = 'translate(' + (x + w / 2).toFixed(1) + 'px,' + (y + h * 0.3).toFixed(1) + 'px) translate(-50%,-50%)';
    }
    var hw = Math.min(w, h * 16 / 9), hx = x + (w - hw) / 2,   // the HUD keeps its 16:9 in the middle (Hor+)
        left = hx + hw * MAP_LEFT, bottom = y + h * (MAP_TOP - GAP), gp = Math.max(4, h * 0.008);
    if (!big) {
      st.fontSize = offer || card ? small : Math.max(10, h * 0.021).toFixed(1) + 'px';
      st.maxWidth = (w * 0.32).toFixed(0) + 'px';
      st.textAlign = 'left';
    }
    // touch buttons the row would cover: only those overlapping its own rect (a phone's top row of buttons sits above
    // the picture and pushed the box off it, 2026-11-10); it never leaves the picture's top
    var bh = Math.max(text && !big ? el.offsetHeight : 0, showIb ? ib.offsetHeight : 0) || h * 0.12;
    document.querySelectorAll('.tc:not([hidden]) .tc-b').forEach(function (c) {
      var q = c.getBoundingClientRect(), qt = q.top - L.top, qb = q.bottom - L.top;
      if (q.width && q.left - L.left < left + w * 0.32 && q.right - L.left > left && qb > bottom - bh && qt < bottom)
        bottom = Math.min(bottom, qt - 6);
    });
    bottom = Math.max(y, Math.max(bottom, y + bh));
    var ibw = showIb ? ib.offsetWidth + gp : 0;
    if (showIb) {
      ib.style.transform = 'translate(' + left.toFixed(1) + 'px,' + bottom.toFixed(1) + 'px) translateY(-100%)';
      panel.style.transform = 'translate(' + left.toFixed(1) + 'px,' + (bottom - ib.offsetHeight - gp).toFixed(1) + 'px) translateY(-100%)';
    }
    if (!big) st.transform = 'translate(' + (left + ibw).toFixed(1) + 'px,' + bottom.toFixed(1) + 'px) translateY(-100%)';
  }
  requestAnimationFrame(frame);
})();

// Race start names on the full map (2026-11-15, owner: names only on demand): src/games/remix/roam_map.c's
// roam_mapicons_box (seq bumped each frame the full map draws the start points, n, then 88-byte rows: u, v (0..1 of
// the HUD's 720x480, the icon's centre through the game's own transform), two lines of UTF-8 text) -> a small label
// by the icon the mouse is over, or the last tap landed on (4 s). Mapped into the HUD's middle 16:9 (Hor+) of the
// picture rect, as the panels. Listens only (no preventDefault): the game keeps its input. Main thread only.
(function () {
  if (typeof document != 'object' || typeof requestAnimationFrame != 'function') return;
  var canvas = Module['canvas'], tip = null, seq = -1, seenAt = 0, I32 = null, F32 = null, ptr = null, tapAt = -1e9, shownKey = '';
  if (!canvas) return;
  function track(ev, tap) {   // client coordinates: the frame maps them with the cached rects (no layout read here)
    ptr = { cx: ev.clientX, cy: ev.clientY, mouse: ev.pointerType == 'mouse' };
    if (tap) tapAt = performance.now();
  }
  addEventListener('pointermove', function (ev) { if (ev.pointerType == 'mouse') track(ev, false); }, { passive: true });
  addEventListener('pointerdown', function (ev) { track(ev, true); }, { passive: true, capture: true });
  function frame(now) {
    requestAnimationFrame(frame);
    if (typeof HEAPU8 == 'undefined' || !HEAPU8 || typeof _roam_mapicons_box != 'function' || !canvas.parentNode) return;
    if (!I32) {
      I32 = new Int32Array(HEAPU8.buffer);
      F32 = new Float32Array(HEAPU8.buffer);
      tip = document.createElement('div');
      tip.style.cssText = 'position:absolute;left:0;top:0;z-index:1;display:none;pointer-events:none;white-space:nowrap;' +
        'padding:.15em .5em;border-radius:6px;background:rgba(0,0,0,.78);border:1px solid rgba(125,255,154,.6);color:#fff;' +
        'text-shadow:0 1px 2px #000;line-height:1.3;font-weight:700;font-family:"Arial Narrow","Roboto Condensed",Arial,sans-serif';
      canvas.parentNode.append(tip);
    }
    var b = _roam_mapicons_box() >>> 2, s = I32[b], n = Math.min(I32[b + 1] | 0, 64);
    if (s !== seq) { seq = s; seenAt = now; }
    var live = ptr && (ptr.mouse || now - tapAt < 4000);
    if (now - seenAt > 400 || !(n > 0) || !live) { if (shownKey) { tip.style.display = 'none'; shownKey = ''; } return; }
    var pc = Module['mc3Pic'](now), r = pc.r, L = pc.L, x = r.left - L.left, y = r.top - L.top, w = r.width, h = r.height, aspect = 16 / 9;
    if (typeof _roam_tags_box == 'function') {
      var a = F32[(_roam_tags_box() >>> 2) + 2];
      if (a > 0.5 && a < 5) aspect = a;
    }
    if (!canvas.parentNode.classList.contains('stretch')) {
      if (w / h > aspect) { x += (w - h * aspect) / 2; w = h * aspect; } else { y += (h - w / aspect) / 2; h = w / aspect; }
    }
    var hw = Math.min(w, h * 16 / 9), hx = x + (w - hw) / 2, reach = Math.max(10, h * (ptr.mouse ? 0.022 : 0.04)), best = -1, bd = reach * reach;
    for (var i = 0; i < n; i++) {
      var t = b + 2 + i * 22, dx = hx + F32[t] * hw - (ptr.cx - L.left), dy = y + F32[t + 1] * h - (ptr.cy - L.top), d = dx * dx + dy * dy;
      if (d < bd) { bd = d; best = i; }
    }
    if (best < 0) { if (shownKey) { tip.style.display = 'none'; shownKey = ''; } return; }
    var rt = b + 2 + best * 22, p = (rt + 2) * 4, q = p;
    while (q < p + 80 && HEAPU8[q]) q++;
    var text = new TextDecoder().decode(HEAPU8.slice(p, q)), tx = hx + F32[rt] * hw, ty = y + F32[rt + 1] * h;
    var key = text + '|' + tx.toFixed(0) + '|' + ty.toFixed(0) + '|' + h.toFixed(0);
    if (key === shownKey) return;
    shownKey = key;
    tip.textContent = '';
    text.split('\n').forEach(function (line, k) {
      var d = document.createElement('div');
      d.textContent = line;
      if (k) d.style.cssText = 'font-size:.8em;opacity:.8';
      tip.append(d);
    });
    tip.style.fontSize = Math.max(10, h * 0.02).toFixed(1) + 'px';
    tip.style.transform = 'translate(' + tx.toFixed(1) + 'px,' + (ty - reach * 0.6).toFixed(1) + 'px) translate(-50%,-100%)';
    tip.style.display = '';
  }
  requestAnimationFrame(frame);
})();

// Drift score: src/games/remix/drift_score.c's drift_hud_box (seq, 96 bytes UTF-8 "DRIFT 1,240 x2\n6,300 / 10,000 ...",
// 32 bytes the payout "+$10,000"), bumped every frame the car moves: shown while fresh on the HUD's left edge (the
// race prompt box's left, 16:9 under Hor+) at 36% of the picture's height, the payout big just above it: clear of
// the prompt box and the minimap below (was bottom-centre under the car, 2026-11-12); right of any touch button
// under it (a phone's d-pad). Main thread only.
(function () {
  if (typeof document != 'object' || typeof requestAnimationFrame != 'function') return;
  var canvas = Module['canvas'], el = null, bigEl = null, seq = -1, seenAt = 0, I32 = null, shown = null, bigShown = null, placed = '';
  if (!canvas) return;
  function str(p, n) { var e = p; while (e < p + n && HEAPU8[e]) e++; return new TextDecoder().decode(HEAPU8.slice(p, e)); }
  function frame(now) {
    requestAnimationFrame(frame);
    if (typeof HEAPU8 == 'undefined' || !HEAPU8 || typeof _drift_hud_box != 'function' || !canvas.parentNode) return;
    if (!I32) {
      I32 = new Int32Array(HEAPU8.buffer);
      var css = 'position:absolute;left:0;top:0;pointer-events:none;z-index:1;color:#fff;text-align:center;white-space:nowrap;' +
        'text-shadow:0 1px 3px #000,0 0 8px rgba(0,0,0,.7);font-weight:700;font-family:"Arial Narrow","Roboto Condensed",Arial,sans-serif;display:none';
      el = document.createElement('div');
      el.style.cssText = css;
      bigEl = document.createElement('div');
      bigEl.style.cssText = css + ';color:#5cff7a';
      canvas.parentNode.append(el, bigEl);
    }
    var b = _drift_hud_box(), s = I32[b >>> 2], text = '', big = '';
    if (s !== seq) { seq = s; seenAt = now; }
    if (now - seenAt < 400) { text = str(b + 4, 96); big = str(b + 100, 32); }   // stale: paused, loading or gone
    if (text !== shown) {
      shown = text;
      el.textContent = '';
      text.split('\n').forEach(function (line, i) {
        var d = document.createElement('div');
        d.textContent = line;
        d.style.cssText = i ? 'font-size:.7em;opacity:.85;margin-top:.15em' :
          'font-size:1.4em;letter-spacing:.04em;color:' + (line === 'Drift lost' ? '#ff5a4a' : '#ffd24a');
        if (line) el.append(d);
      });
    }
    if (big !== bigShown) { bigShown = big; bigEl.textContent = big; }
    if (!text && !big) { el.style.display = bigEl.style.display = 'none'; return; }
    var pc = Module['mc3Pic'](now), r = pc.r, L = pc.L, x = r.left - L.left,
        y = r.top - L.top, w = r.width, h = r.height, aspect = 16 / 9;
    if (typeof _roam_tags_box == 'function') {
      var a = new Float32Array(HEAPU8.buffer)[(_roam_tags_box() >>> 2) + 2];
      if (a > 0.5 && a < 5) aspect = a;
    }
    if (!canvas.parentNode.classList.contains('stretch')) {
      if (w / h > aspect) { x += (w - h * aspect) / 2; w = h * aspect; } else { y += (h - w / aspect) / 2; h = w / aspect; }
    }
    if (!(w > 0 && h > 0)) { el.style.display = bigEl.style.display = 'none'; return; }
    // placement only when an input changed (it read offsetWidth/Height and touch rects after last frame's writes)
    var pk = shown + '|' + bigShown + '|' + pc.epoch + '|' + x + '|' + y + '|' + w + '|' + h + '|' + el.style.display + bigEl.style.display;
    if (pk === placed) return;
    placed = pk;
    var st = el.style, bs = bigEl.style, hw = Math.min(w, h * 16 / 9),
        left = x + (w - hw) / 2 + hw * 0.055, top = y + h * 0.36, bt = y + h * 0.27,
        bw = Math.max(el.offsetWidth, bigEl.offsetWidth) || w * 0.2, bb = top + (el.offsetHeight || h * 0.08);
    document.querySelectorAll('.tc:not([hidden]) .tc-b').forEach(function (c) {
      var q = c.getBoundingClientRect(), ql = q.left - L.left, qr = q.right - L.left;
      if (q.width && ql < left + bw && qr > left && q.bottom - L.top > bt && q.top - L.top < bb) left = qr + 6;
    });
    st.fontSize = Math.max(11, h * 0.026).toFixed(1) + 'px';
    st.textAlign = bs.textAlign = 'left';
    st.transform = 'translate(' + left.toFixed(1) + 'px,' + top.toFixed(1) + 'px)';
    st.display = text ? '' : 'none';
    bs.fontSize = Math.max(18, h * 0.06).toFixed(1) + 'px';
    bs.transform = 'translate(' + left.toFixed(1) + 'px,' + bt.toFixed(1) + 'px)';
    bs.display = big ? '' : 'none';
  }
  requestAnimationFrame(frame);
})();

// Street challenge marker: while racing a player in free roam, src/games/remix/roam_challenge.c publishes the next
// checkpoint (roam_cp_box: seq, on, u, v (0..1 of the picture), angle (radians, 0 = right, clockwise), edge (1: off
// the picture, an arrow at its edge), distance m, checkpoint, count, finish). A pin over it, or an edge arrow
// pointing the way, with "2/5 · 340 m". Shown only to the two racers (the box is theirs). Main thread only.
(function () {
  if (typeof document != 'object' || typeof requestAnimationFrame != 'function') return;
  var canvas = Module['canvas'], el = null, pin = null, label = null, seq = -1, seenAt = 0, I32 = null, F32 = null;
  if (!canvas) return;
  function frame(now) {
    requestAnimationFrame(frame);
    if (typeof HEAPU8 == 'undefined' || !HEAPU8 || typeof _roam_cp_box != 'function' || !canvas.parentNode) return;
    if (!I32) {
      I32 = new Int32Array(HEAPU8.buffer);
      F32 = new Float32Array(HEAPU8.buffer);
      el = document.createElement('div');
      el.style.cssText = 'position:absolute;left:0;top:0;pointer-events:none;z-index:1;display:none;text-align:center;' +
        'color:#ffd24a;font:700 clamp(11px,2vh,20px) "Arial Narrow","Roboto Condensed",Arial,sans-serif;' +
        'text-shadow:0 0 3px #000,0 1px 2px #000;white-space:nowrap';
      pin = document.createElement('div');
      pin.style.cssText = 'font-size:1.8em;line-height:1';
      label = document.createElement('div');
      el.append(label, pin);
      canvas.parentNode.append(el);
    }
    var b = _roam_cp_box() >>> 2, s = I32[b];
    if (s !== seq) { seq = s; seenAt = now; }
    if (!I32[b + 1] || now - seenAt > 400) { el.style.display = 'none'; return; }
    var u = F32[b + 2], v = F32[b + 3], ang = F32[b + 4], edge = I32[b + 5], dist = F32[b + 6], idx = I32[b + 7],
        n = I32[b + 8], fin = I32[b + 9];
    // the picture inside the canvas, as the name tags place it (object-fit contain unless Stretch)
    var r = canvas.getBoundingClientRect(), L = canvas.parentNode.getBoundingClientRect(), x = r.left - L.left,
        y = r.top - L.top, w = r.width, h = r.height, aspect = 16 / 9;
    if (typeof _roam_tags_box == 'function') {   // the picture's shape the name tags got (Hor+, 4:3)
      var a = F32[(_roam_tags_box() >>> 2) + 2];
      if (a > 0.5 && a < 5) aspect = a;
    }
    if (!canvas.parentNode.classList.contains('stretch')) {
      if (w / h > aspect) { x += (w - h * aspect) / 2; w = h * aspect; } else { y += (h - w / aspect) / 2; h = w / aspect; }
    }
    var txt = (fin ? 'FINISH' : idx + '/' + n) + ' · ' + Math.round(dist) + ' m';
    if (label.textContent !== txt) label.textContent = txt;
    pin.textContent = edge ? '➤' : '▼';
    pin.style.transform = edge ? 'rotate(' + ang.toFixed(3) + 'rad)' : '';
    el.style.transform = 'translate(' + (x + u * w).toFixed(1) + 'px,' + (y + v * h).toFixed(1) + 'px) translate(-50%,' +
      (edge ? '-50%' : '-100%') + ')';
    el.style.display = '';
  }
  requestAnimationFrame(frame);
})();

// Free Roam link notice: web/lan.js sets Module.mc3Lan.roamDown while Free Roam has lost its link to the other players
// (switch socket down, or the city socket not open): 'connecting...' at once, after 30 s a reload button. Top centre of
// the page's game box; clicks pass through except the button. Main thread only. The same box says so while lan.js has
// kicked us for inactivity (lan.afk; no button: any key or button reconnects).
(function () {
  if (typeof document != 'object') return;
  var canvas = Module['canvas'], el = null, txt = null, btn = null, mode = 0;
  if (!canvas) return;
  setInterval(function () {
    var lan = Module['mc3Lan'], d = lan && lan.roamDown || 0, afk = !!(lan && lan.afk);
    if ((!d && !afk) || !canvas.parentNode) { if (el) el.style.display = 'none'; mode = 0; return; }
    if (!el) {
      el = document.createElement('div');
      el.style.cssText = 'position:absolute;left:50%;top:8px;transform:translateX(-50%);z-index:6;pointer-events:none;' +
        'box-sizing:border-box;max-width:92%;padding:.5em .9em;border-radius:8px;background:rgba(0,0,0,.78);' +
        'border:1px solid #ffd24a;color:#fff;text-shadow:0 1px 2px #000;font:700 15px Arial,sans-serif;text-align:center;display:none';
      txt = document.createElement('span');
      btn = document.createElement('button');
      btn.textContent = 'Reload';
      btn.style.cssText = 'pointer-events:auto;margin-left:.8em;padding:.2em .8em;font:inherit;cursor:pointer';
      btn.onclick = function () { location.reload(); };
      el.append(txt, btn);
      canvas.parentNode.append(el);
    }
    var m = afk ? 3 : Date.now() - d >= 30000 ? 2 : 1;
    if (m !== mode) {
      mode = m;
      var s = Math.round(lan.afkMs / 1000);
      txt.textContent = m == 3 ? 'Disconnected for inactivity (' + (s % 60 ? s + ' s' : s / 60 + ' min') + '). Press any key or button to reconnect.' :
        m == 2 ? 'Free Roam: no connection to the other players — reload the page' : 'Free Roam: connection lost — reconnecting…';
      btn.style.display = m == 2 ? '' : 'none';
    }
    el.style.display = '';
  }, 500);
})();

// Tuning screen docked panel (2026-11-16): roam_tune_box word 0 = 1 while the game's pause-menu Tuning screen is open
// (online_tune.c); the page (index.html driftPanel.dock) shows its Drift setup beside the menu, pointer only.
(function () {
  var last = 0;
  setInterval(function () {
    try {
      if (typeof HEAPU8 == 'undefined' || !HEAPU8 || typeof _roam_tune_box != 'function') return;
      var o = new Int32Array(HEAPU8.buffer, _roam_tune_box(), 1)[0] ? 1 : 0;
      if (o !== last) { last = o; if (window.mc3TuneDock) window.mc3TuneDock(!!o); }
    } catch (e) {}
  }, 200);
})();
