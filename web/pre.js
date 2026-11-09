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
// cannot set that aside (a TV, 32-bit Chrome) gets the first of 1 GB, 512 MB, 256 MB that it gives and grows
// from there. The page's low-memory profile (Module.mc3MemMB) starts lower. A 32-bit browser may also refuse to
// reserve a 2 GB maximum: then smaller maximums.
if (!ENVIRONMENT_IS_PTHREAD && typeof WebAssembly == 'object' && !Module['wasmMemory']) (function () {
  var start = Module['mc3MemMB'] || 2048, tries = [], last = null, i;
  [2048, 1024, 512, 256].forEach(function (mb) { if (mb <= start) tries.push([mb, 2048]); });
  tries.push([256, 1024], [256, 512]);
  for (i = 0; i < tries.length; i++) {
    try {
      Module['wasmMemory'] = new WebAssembly.Memory({ initial: tries[i][0] * 16, maximum: tries[i][1] * 16, shared: true });
      (Module['print'] || console.log)('[WEB] memory: ' + tries[i][0] + ' MB to start, ' + tries[i][1] + ' MB at most'
        + (i ? ' (larger sizes refused: ' + last + ')' : ''));
      return;
    } catch (e) { last = e && e.message || String(e); }
  }
  throw new Error('This browser cannot set aside memory for the game (256 MB, shared): ' + last);
})();

// FPS overlay (web/index.html): presented-frame counter of the active renderer, read from shared wasm memory.
// Returns undefined until the runtime is up. WebGPU unless RECOMP_GPU=webgl (or its export is missing).
Module['mc3Frames'] = function () {
  if (typeof HEAPU8 == 'undefined' || !HEAPU8) return;
  var gl = (Module['mc3Env'] || {}).RECOMP_GPU == 'webgl';
  var box = (!gl && typeof _nv2a_wgpu_fps_box == 'function') ? _nv2a_wgpu_fps_box()
          : typeof _nv2a_gl_fps_box == 'function' ? _nv2a_gl_fps_box() : 0;
  return box ? Atomics.load(new Uint32Array(HEAPU8.buffer), box >>> 2) : undefined;
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

// Race prompt: near a race start point in free roam, src/games/remix/roam_race_ai.c publishes one line
// (roam_prompt_box: seq, then 192 bytes of UTF-8, empty = none): up to three lines joined by '\n' (title, details,
// action; '[Q]' in the action is drawn as a keycap); shown at the bottom middle of the canvas while fresh (the game updates it every frame). Main thread only.
(function () {
  if (typeof document != 'object' || typeof requestAnimationFrame != 'function') return;
  var canvas = Module['canvas'], el = null, seq = -1, seenAt = 0, I32 = null, shown = null;
  if (!canvas) return;
  function frame(now) {
    requestAnimationFrame(frame);
    if (typeof HEAPU8 == 'undefined' || !HEAPU8 || typeof _roam_prompt_box != 'function' || !canvas.parentNode) return;
    if (!I32) {   // a static box (as above): the view stays valid after growth
      I32 = new Int32Array(HEAPU8.buffer);
      el = document.createElement('div');
      el.style.cssText = 'position:absolute;left:50%;bottom:14%;transform:translateX(-50%);pointer-events:none;' +
        'z-index:1;box-sizing:border-box;max-width:calc(100% - 32px);padding:.5em 1em;border-radius:8px;' +
        'background:rgba(0,0,0,.72);border:1px solid rgba(255,255,255,.25);box-shadow:0 2px 10px rgba(0,0,0,.6);' +
        'color:#fff;text-align:center;text-shadow:0 1px 2px #000;overflow-wrap:anywhere;' +
        'font:700 clamp(12px,2.2vh,22px) "Arial Narrow","Roboto Condensed",Arial,sans-serif;display:none';
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
      el.textContent = '';
      text.split('\n').forEach(function (line, i) {
        var d = document.createElement('div');
        if (i === 0) d.style.cssText = 'font-size:1.2em;letter-spacing:.03em';
        else if (i === 1) d.style.cssText = 'opacity:.85;font-size:.9em';
        else d.style.cssText = 'margin-top:.3em;color:#ffd24a';
        line.split(/(\[[^\]]+\])/).forEach(function (part) {   // [Q] = a keycap
          if (/^\[.+\]$/.test(part)) {
            var k = document.createElement('span');
            k.textContent = part.slice(1, -1);
            k.style.cssText = 'display:inline-block;min-width:1.4em;padding:0 .35em;margin:0 .1em;border-radius:4px;' +
              'border:1px solid #fff;border-bottom-width:3px;background:#333;color:#fff;text-shadow:none';
            d.append(k);
          } else d.append(part);
        });
        if (text) el.append(d);
      });
    }
    el.style.display = text ? '' : 'none';
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
