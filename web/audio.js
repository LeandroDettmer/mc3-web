// The game's sound in the browser: linked in front of the Emscripten runtime
// (--pre-js, CMakeLists.txt), like pre.js. The APU writes float32 stereo
// frames into a ring in the shared wasm memory (xboxrecomp/src/apu/apu_web.c);
// an AudioWorklet here plays them. Design after nfsu2-web (MIT, th0mk:
// web/src/audio/index.ts and worklet.ts).
//   - Main thread only (the runtime's workers load this file too).
//   - The context is made at once: the page loads the game inside its Play
//     click, so it has had a user gesture and Chrome lets it run. Any later
//     click or key press resumes a context the browser suspended.
//   - 48 kHz asked for (the APU's rate); a context that runs at another rate
//     resamples linearly in the worklet.
//   - RECOMP_AUDIO=0 in Module.mc3Env: no context at all.
//   - Module.mc3Audio() returns the counters (fill, underruns, quanta, ...).
(function () {
  if (typeof document != 'object' || typeof AudioContext != 'function') return;
  var env = Module['mc3Env'] || {};
  if (String(env.RECOMP_AUDIO) === '0') return;

  // header words: apu_web.c's enum
  var MAGIC = 0x58415544, AR_MAGIC = 0, AR_FRAMES = 2, AR_RATE = 3, AR_W = 4, AR_R = 5,
      AR_UNDERRUNS = 6, AR_CTX = 7, AR_QUANTA = 8, AR_LOUD = 9, AR_PAGE_RATE = 10;

  // The processor, loaded from a Blob of its own source (no extra file to serve).
  function processor() {
    var AR_W = 4, AR_R = 5, AR_UNDERRUNS = 6, AR_QUANTA = 8, AR_LOUD = 9;
    class Mc3Out extends AudioWorkletProcessor {
      constructor(o) {
        super();
        var p = o.processorOptions;
        this.h = new Int32Array(p.buf, p.ptr, 32);
        this.frames = p.frames;
        this.pcm = new Float32Array(p.buf, p.ptr + 128, p.frames * 2);
        this.step = p.rate / sampleRate;
        this.frac = 0;
        this.primed = false;      // no underruns counted before the first data
      }
      process(inputs, outputs) {
        var out = outputs[0];
        if (!out || !out.length) return true;
        var h = this.h, pcm = this.pcm, L = out[0], R = out[1] || out[0], n = L.length, MASK = this.frames - 1;
        Atomics.add(h, AR_QUANTA, 1);
        var w = Atomics.load(h, AR_W), r = Atomics.load(h, AR_R);
        var avail = (w - r) >>> 0, k = 0, loud = false, a, b;
        if (avail > this.frames) avail = 0;
        if (this.step === 1) {
          k = Math.min(n, avail);
          for (var i = 0; i < k; i++) {
            var j = ((r + i) & MASK) << 1;
            a = L[i] = pcm[j]; b = R[i] = pcm[j + 1];
            if (a > 3e-5 || a < -3e-5 || b > 3e-5 || b < -3e-5) loud = true;
          }
          r = (r + k) | 0;
        } else {
          // linear interpolation between ring frames
          var f = this.frac;
          for (; k < n; k++) {
            var i0 = Math.floor(f);
            if (i0 + 1 >= avail) break;
            var t = f - i0, j0 = ((r + i0) & MASK) << 1, j1 = ((r + i0 + 1) & MASK) << 1;
            a = L[k] = pcm[j0] + (pcm[j1] - pcm[j0]) * t;
            b = R[k] = pcm[j0 + 1] + (pcm[j1 + 1] - pcm[j0 + 1]) * t;
            if (a > 3e-5 || a < -3e-5 || b > 3e-5 || b < -3e-5) loud = true;
            f += this.step;
          }
          var used = Math.floor(f);
          this.frac = f - used;
          r = (r + used) | 0;
        }
        if (k > 0) this.primed = true;
        if (k < n) {
          for (var z = k; z < n; z++) { L[z] = 0; R[z] = 0; }
          if (this.primed) Atomics.add(h, AR_UNDERRUNS, 1);
        }
        if (loud) Atomics.add(h, AR_LOUD, 1);
        Atomics.store(h, AR_R, r);
        return true;
      }
    }
    registerProcessor('mc3-out', Mc3Out);
  }

  var ctx, node = null, h = null;
  try { ctx = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' }); }
  catch (e) { ctx = new AudioContext({ latencyHint: 'interactive' }); }
  var loaded = ctx.audioWorklet.addModule(URL.createObjectURL(
    new Blob(['(' + processor.toString() + ')()'], { type: 'text/javascript' })));

  function ctxWord() {
    if (h) Atomics.store(h, AR_CTX, node && ctx.state == 'running' ? 1 : 0);
  }
  ctx.onstatechange = ctxWord;
  function resume() { if (ctx.state != 'running' && ctx.state != 'closed') ctx.resume().then(ctxWord, function () {}); }
  ['pointerdown', 'keydown', 'touchend'].forEach(function (t) { window.addEventListener(t, resume, true); });
  resume();
  // A hidden tab is silent: the context suspended (the APU then waits, apu_web.c), resumed when it shows (iOS may
  // want a tap: the page's 'Tap for sound'). web/pre.js slows the game meanwhile.
  document.addEventListener('visibilitychange', function () {
    if (!document.hidden) resume();
    else if (ctx.state == 'running') ctx.suspend().then(ctxWord, function () {});
  });

  // The ring is a static in the wasm image; its magic is set once the APU opens it.
  var poll = setInterval(function () {
    if (typeof _apu_web_ring != 'function' || typeof HEAPU8 == 'undefined' || !HEAPU8) return;
    var ptr = _apu_web_ring() >>> 0, buf = HEAPU8.buffer;
    var v = new Int32Array(buf, ptr, 32);
    if ((v[AR_MAGIC] >>> 0) != MAGIC) return;
    clearInterval(poll);
    h = v;
    loaded.then(function () {
      node = new AudioWorkletNode(ctx, 'mc3-out', {
        numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
        processorOptions: { buf: buf, ptr: ptr, frames: v[AR_FRAMES], rate: v[AR_RATE] },
      });
      node.connect(ctx.destination);
      Atomics.store(h, AR_PAGE_RATE, ctx.sampleRate | 0);
      ctxWord();
      err('[audio] worklet attached: ' + ctx.sampleRate + ' Hz, base latency ' +
          (ctx.baseLatency * 1000).toFixed(1) + ' ms, state ' + ctx.state);
    }, function (e) { err('[audio] the worklet did not load: ' + e + '; no sound'); });
  }, 100);

  Module['mc3Audio'] = function () {
    var z = function (i) { return h ? Atomics.load(h, i) >>> 0 : 0; };
    return { state: ctx.state, attached: !!node, sampleRate: ctx.sampleRate,
             fill: (z(AR_W) - z(AR_R)) >>> 0, written: z(AR_W), read: z(AR_R),
             underruns: z(AR_UNDERRUNS), quanta: z(AR_QUANTA), loud: z(AR_LOUD) };
  };
})();
