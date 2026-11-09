// Linked in front of the Emscripten runtime (--pre-js, CMakeLists.txt).
// Settings for getenv(): RECOMP_* / MC3_* from process.env under Node.js
// (make web-node), from Module.mc3Env in a browser (web/index.html: the start
// page's settings, its Advanced box, then the URL query; MC3_DISC_URL).
Module['preRun'] = [].concat(Module['preRun'] || [], function () {
  var env = Module['mc3Env'] || (typeof process == 'object' && process.env) || {};
  for (var k in env)
    if (/^(RECOMP_|MC3_)/.test(k)) ENV[k] = String(env[k]);
});

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
