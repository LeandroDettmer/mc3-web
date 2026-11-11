// Linked in front of the Emscripten runtime (--pre-js, CMakeLists.txt).
// Settings for getenv(): RECOMP_* / MC3_* from process.env under Node.js
// (make web-node), from Module.mc3Env in a browser (web/index.html: the start
// page's settings, its Advanced box, then the URL query; MC3_DISC_URL).
Module['preRun'] = [].concat(Module['preRun'] || [], function () {
  var env = Module['mc3Env'] || (typeof process == 'object' && process.env) || {};
  for (var k in env)
    if (/^(RECOMP_|MC3_)/.test(k)) ENV[k] = String(env[k]);
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
// refused on an iPhone) and ran fine with a fixed 2 GB and 40 pool workers before 981d032: that baseline first,
// fixed sizes only (maximum = initial), pool kept at 40 even in the light profile.
// Every refused size is logged; when nothing is given a page error says so (index.html failShow shows it).
if (!ENVIRONMENT_IS_PTHREAD && typeof WebAssembly == 'object' && !Module['wasmMemory']) (function () {
  var low = !!Module['mc3MemMB'], refused = [], i, ua = navigator.userAgent, log = Module['print'] || console.log;
  var webkit = /iPhone|iPad|iPod/.test(ua) || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1)
    || (/AppleWebKit/.test(ua) && !/Chrome|Chromium|Android/.test(ua));
  if (webkit) delete Module['mc3Pool'];
  var tries = webkit ? [[2048, 2048], [1536, 1536], [1024, 1024], [768, 768], [512, 512]]
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
        + (webkit ? ' (WebKit: fixed, pool 40)' : low ? ' (low profile)' : '') + (gecko ? ' (Firefox: 64 KB less than 2 GB)' : '')
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

// The pool workers (made when this script runs, before the .wasm): how many, and each one's loading or error, into
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
    w.addEventListener('error', function (e) { log('[WEB] module: pool worker ' + (i + 1) + ' error: ' + (e.message || e.type)); });
  });
});

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
    Atomics.store(new Int32Array(HEAPU8.buffer), _nv2a_wgpu_aspect_box() >>> 2, fixed || val);
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

// Name tags: other players' names over their cars in free roam, on the game's own opponent arrow. src/games/remix/
// roam_tags.c publishes, each frame of the main camera, where each driven remote car's arrow tip is on the picture
// (roam_tags_box: seq, n, picture width / height, the arrow's colour 0xRRGGBB, then n x 40 bytes: u, v (0..1), the
// arrow's alpha (it fades in from 25 to 100 m and out from 200 to 300 m), name); drawn here over the canvas at the
// page's frame rate, in the arrow's colour, no background, sized to the picture. Settings 'Name tags'
// (MC3_NAMETAGS=0 off), live through Module.mc3SetNameTags. Main thread only.
(function () {
  if (typeof document != 'object' || typeof requestAnimationFrame != 'function') return;
  var canvas = Module['canvas'], on = (Module['mc3Env'] || {}).MC3_NAMETAGS !== '0';
  var layer = null, pool = [], shown = 0, seq = -1, seenAt = 0, I32 = null, F32 = null, U8 = null;
  if (!canvas) return;
  Module['mc3SetNameTags'] = function (v) { on = !!v; };
  function hideFrom(n) { for (; shown > n; shown--) pool[shown - 1].style.display = 'none'; }
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
    var r = canvas.getBoundingClientRect(), L = layer.getBoundingClientRect(), x = r.left - L.left, y = r.top - L.top,
        w = r.width, h = r.height;
    if (!canvas.parentNode.classList.contains('stretch')) {
      if (w / h > aspect) { x += (w - h * aspect) / 2; w = h * aspect; } else { y += (h - w / aspect) / 2; h = w / aspect; }
    }
    for (var i = 0; i < n; i++) {
      var t = b + 4 + i * 10, el = pool[i], name = '';
      for (var c = 0, p = (t + 3) * 4; c < 27 && U8[p + c]; c++) name += String.fromCharCode(U8[p + c]);
      if (!el) {
        el = pool[i] = document.createElement('div');
        el.style.cssText = 'position:absolute;left:0;top:0;white-space:nowrap;font-weight:700;' +
          'font-family:"Arial Narrow","Roboto Condensed",Arial,sans-serif;letter-spacing:.02em;' +
          'text-shadow:0 0 2px #000,0 1px 1px #000';
        layer.append(el);
      }
      if (el.textContent !== name) el.textContent = name;
      el.style.color = rgb;
      el.style.fontSize = Math.max(10, h * 0.022).toFixed(1) + 'px';   // about the HUD's small text
      el.style.transform = 'translate(' + (x + F32[t] * w).toFixed(1) + 'px,' + (y + F32[t + 1] * h).toFixed(1) +
        'px) translate(-50%,-100%)';
      el.style.opacity = Math.max(0, Math.min(1, F32[t + 2])).toFixed(2);   // the arrow's own fade
      el.style.display = '';
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
// 16:9); hidden without a list (web/server's switch has none). Main thread only.
(function () {
  if (typeof document != 'object' || typeof requestAnimationFrame != 'function') return;
  var canvas = Module['canvas'], el = null, seq = -1, seenAt = 0, I32 = null, F32 = null, shown = null;
  var CITIES = ['San Diego', 'Atlanta', 'Detroit', 'Tokyo'], ROWS = 24;   // free-roam room ids 0..3 (web/index.html)
  var me = String((Module['mc3Env'] || {}).MC3_PLAYER_NAME || '').toLowerCase();
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
    }
    var b = _roam_bigmap_box() >>> 2, s = I32[b], here = I32[b + 1];
    if (s !== seq) { seq = s; seenAt = now; }
    var lan = Module['mc3Lan'], list = lan && lan.players && Array.isArray(lan.players.list) ? lan.players.list : null;
    if (now - seenAt > 400 || !list) { el.style.display = 'none'; return; }   // map closed, paused or gone; no list
    var roam = list.filter(function (p) { return p && p.mode == 'roam'; }), mine = [], other = [];
    roam.forEach(function (p) { (p.city === here ? mine : other).push(p); });
    var key = here + '|' + roam.map(function (p) { return p.name + '\u0001' + p.city; }).join('\u0002');
    if (key !== shown) {
      shown = key;
      el.textContent = '';
      el.append(row('Free Roam · ' + roam.length + ' online', 'font-size:1.1em;letter-spacing:.03em;margin-bottom:.2em'));
      var n = 0;
      if (mine.length) el.append(row(city(here), 'color:#ffd24a;font-size:.85em;margin-top:.2em'));
      mine.forEach(function (p) {
        if (n++ < ROWS) el.append(row(p.name, String(p.name).toLowerCase() == me ? 'color:#7fd4ff' : ''));
      });
      if (other.length) el.append(row('Other cities', 'color:#ffd24a;font-size:.85em;margin-top:.35em'));
      other.forEach(function (p) {
        if (n++ < ROWS) el.append(row(p.name + ' · ' + city(p.city), 'opacity:.75'));
      });
      if (n > ROWS) el.append(row('+' + (n - ROWS) + ' more', 'opacity:.6;font-size:.85em'));
      var more = lan.players.more | 0;   // beyond the switch's own list
      if (more > 0) el.append(row('+' + more + ' not listed', 'opacity:.6;font-size:.85em'));
    }
    // the picture inside the canvas (object-fit contain unless Stretch), its shape as the name tags got it
    var r = canvas.getBoundingClientRect(), L = canvas.parentNode.getBoundingClientRect(), x = r.left - L.left,
        y = r.top - L.top, w = r.width, h = r.height, aspect = 16 / 9;
    if (typeof _roam_tags_box == 'function') {
      var a = F32[(_roam_tags_box() >>> 2) + 2];
      if (a > 0.5 && a < 5) aspect = a;
    }
    if (!canvas.parentNode.classList.contains('stretch')) {
      if (w / h > aspect) { x += (w - h * aspect) / 2; w = h * aspect; } else { y += (h - w / aspect) / 2; h = w / aspect; }
    }
    if (!(w > 0 && h > 0)) { el.style.display = 'none'; return; }
    var st = el.style, pad = Math.max(8, h * 0.02);
    st.fontSize = Math.max(10, h * 0.022).toFixed(1) + 'px';
    st.maxWidth = (w * 0.2).toFixed(0) + 'px';
    st.maxHeight = (h * 0.8).toFixed(0) + 'px';
    st.transform = 'translate(' + (x + w - pad).toFixed(1) + 'px,' + (y + h * 0.1).toFixed(1) + 'px) translateX(-100%)';
    st.display = '';
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
(function () {
  if (typeof document != 'object' || typeof requestAnimationFrame != 'function') return;
  var canvas = Module['canvas'], el = null, seq = -1, seenAt = 0, I32 = null, F32 = null, shown = null, big = false;
  // the minimap's top-left in the HUD's 16:9 (720x480 layout): left edge and top, fractions of the HUD's width/height
  var MAP_LEFT = 0.055, MAP_TOP = 0.64, GAP = 0.015;
  if (!canvas) return;
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
        'font-weight:700;font-family:"Arial Narrow","Roboto Condensed",Arial,sans-serif;display:none';
      canvas.parentNode.append(el);
    }
    var b = _roam_prompt_box(), s = I32[b >>> 2], text = '';
    if (s !== seq) { seq = s; seenAt = now; }
    if (now - seenAt < 400) {   // stale: paused, loading or gone
      var e = b + 4, end = e + 192;   // RRA_TEXT bytes, NUL ended, UTF-8
      while (e < end && HEAPU8[e]) e++;
      text = new TextDecoder().decode(HEAPU8.slice(b + 4, e));
    }
    if (text !== shown) {
      shown = text;
      big = text.charCodeAt(0) === 1;
      if (big) text = text.slice(1);
      el.textContent = '';
      text.split('\n').forEach(function (line, i) {
        var d = document.createElement('div');
        if (i === 0) d.style.cssText = big ? 'font-size:2.6em;line-height:1.05;color:#ffd24a' : 'font-size:1.15em;letter-spacing:.03em';
        else if (i === 1) d.style.cssText = 'opacity:.85;font-size:.9em';
        else d.style.cssText = 'margin-top:.25em;color:#ffd24a';
        line.split(/(\[[^\]]+\])/).forEach(function (part) {   // [Q] = a keycap
          if (/^\[.+\]$/.test(part)) {
            var k = document.createElement('span');
            k.textContent = part.slice(1, -1);
            k.style.cssText = 'display:inline-block;min-width:1.4em;padding:0 .35em;margin:0 .1em;border-radius:4px;' +
              'border:1px solid #fff;border-bottom-width:3px;background:#333;color:#fff;text-shadow:none;text-align:center';
            d.append(k);
          } else d.append(part);
        });
        if (text) el.append(d);
      });
    }
    if (!text) { el.style.display = 'none'; return; }
    // the picture inside the canvas (object-fit contain unless Stretch), its shape as the name tags got it
    var r = canvas.getBoundingClientRect(), L = canvas.parentNode.getBoundingClientRect(), x = r.left - L.left,
        y = r.top - L.top, w = r.width, h = r.height, aspect = 16 / 9;
    if (typeof _roam_tags_box == 'function') {
      var a = F32[(_roam_tags_box() >>> 2) + 2];
      if (a > 0.5 && a < 5) aspect = a;
    }
    if (!canvas.parentNode.classList.contains('stretch')) {
      if (w / h > aspect) { x += (w - h * aspect) / 2; w = h * aspect; } else { y += (h - w / aspect) / 2; h = w / aspect; }
    }
    if (!(w > 0 && h > 0)) { el.style.display = 'none'; return; }
    var st = el.style;
    if (big) {
      st.fontSize = Math.max(14, h * 0.03).toFixed(1) + 'px';
      st.maxWidth = (w * 0.6).toFixed(0) + 'px';
      st.textAlign = 'center';
      st.transform = 'translate(' + (x + w / 2).toFixed(1) + 'px,' + (y + h * 0.3).toFixed(1) + 'px) translate(-50%,-50%)';
    } else {
      var hw = Math.min(w, h * 16 / 9), hx = x + (w - hw) / 2,   // the HUD keeps its 16:9 in the middle (Hor+)
          left = hx + hw * MAP_LEFT, bottom = y + h * (MAP_TOP - GAP);
      st.fontSize = Math.max(10, h * 0.021).toFixed(1) + 'px';
      st.maxWidth = (w * 0.32).toFixed(0) + 'px';
      // touch buttons the box would cover: only those overlapping its own rect (a phone's top row of buttons sits
      // above the picture and pushed the box off it, 2026-11-10); it never leaves the picture's top
      var bh = el.offsetHeight || h * 0.12;
      document.querySelectorAll('.tc:not([hidden]) .tc-b').forEach(function (c) {
        var q = c.getBoundingClientRect(), qt = q.top - L.top, qb = q.bottom - L.top;
        if (q.width && q.left - L.left < left + w * 0.32 && q.right - L.left > left && qb > bottom - bh && qt < bottom)
          bottom = Math.min(bottom, qt - 6);
      });
      bottom = Math.max(bottom, y + bh);
      st.textAlign = 'left';
      st.transform = 'translate(' + left.toFixed(1) + 'px,' + Math.max(y, bottom).toFixed(1) + 'px) translateY(-100%)';
    }
    st.display = '';
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
// the page's game box; clicks pass through except the button. Main thread only.
(function () {
  if (typeof document != 'object') return;
  var canvas = Module['canvas'], el = null, txt = null, btn = null, mode = 0;
  if (!canvas) return;
  setInterval(function () {
    var lan = Module['mc3Lan'], d = lan && lan.roamDown || 0;
    if (!d || !canvas.parentNode) { if (el) el.style.display = 'none'; mode = 0; return; }
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
    var m = Date.now() - d >= 30000 ? 2 : 1;
    if (m !== mode) {
      mode = m;
      txt.textContent = m == 2 ? 'Free Roam: no connection to the other players — reload the page' : 'Free Roam: connection lost — reconnecting…';
      btn.style.display = m == 2 ? '' : 'none';
    }
    el.style.display = '';
  }, 500);
})();
