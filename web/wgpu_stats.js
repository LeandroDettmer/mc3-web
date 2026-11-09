/*
 * wgpu_stats.js -- GPU timing for the WebGPU renderer (M4; docs/webgpu-plan.md).
 * A js-library beside wgpu.js. Runs on the render pthread only.
 *
 * With the 'timestamp-query' feature, every render pass web/wgpu.js begins
 * gets timestampWrites (begin + end) into one query set. At the end of a
 * submit the pairs are resolved and copied into a pooled MAP_READ buffer;
 * mapAsync reads them back a few frames later (never blocking a frame) and
 * adds the durations, in microseconds, to the counters wgpu_stats.c binds
 * (g_wgpu_gt[], see wgpu_stats.c for the layout). Passes past the slot limit,
 * or a submit with no free readback buffer, are skipped and counted.
 *
 * Chrome rounds timestamps to 100 us unless "WebGPU Developer Features" is on.
 * Off with RECOMP_WGPU_GPUTIME=0 (wgpu_stats.c passes on=0).
 */
addToLibrary({
  $WGPU_GT__deps: ['$wasmMemory'],
  $WGPU_GT: {
    N: 128,            // timed passes per submit
    POOL: 4,           // readback buffers (submits in flight)
    G: 0, on: false, dead: false, qs: null, rs: null, pool: null, cur: null,
    // counter slots: wgpu_stats.c's g_wgpu_gt
    S_US: 0, S_SAMPLES: 4, S_SKIPPED: 5, S_ACTIVE: 6,

    bind(ptr, on) { var T = WGPU_GT; T.G = ptr >>> 2; T.on = !!on; },

    add(slot, v) { Atomics.add(new Int32Array(wasmMemory.buffer), WGPU_GT.G + slot, v | 0); },

    // frame start: pick a readback buffer (or none: this submit is not timed)
    begin(dev) {
      var T = WGPU_GT;
      T.cur = null;
      if (!T.on || T.dead) return;
      if (!T.qs) {
        try {
          if (!dev.features.has('timestamp-query')) { T.dead = true; return; }
          var sz = 16 * T.N;
          T.qs = dev.createQuerySet({ type: 'timestamp', count: 2 * T.N });
          T.rs = dev.createBuffer({ size: sz, usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC });
          T.pool = [];
          for (var i = 0; i < T.POOL; i++)
            T.pool.push(dev.createBuffer({ size: sz, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST }));
          T.add(T.S_ACTIVE, 1);
        } catch (e) { T.dead = true; T.qs = null; return; }
      }
      var rb = T.pool.pop();
      if (!rb) { T.add(T.S_SKIPPED, 1); return; }
      T.cur = { rb: rb, kinds: [] };
    },

    // a pass descriptor of kind k (0 scene, 1 aux blit/zconv, 2 present); returns d
    tw(d, k) {
      var T = WGPU_GT, c = T.cur;
      if (!c) return d;
      var i = c.kinds.length;
      if (i >= T.N) { T.add(T.S_SKIPPED, 1); return d; }
      c.kinds.push(k);
      d.timestampWrites = { querySet: T.qs, beginningOfPassWriteIndex: 2 * i, endOfPassWriteIndex: 2 * i + 1 };
      return d;
    },

    // before enc.finish()
    end(enc) {
      var T = WGPU_GT, c = T.cur, n;
      if (!c) return;
      n = 2 * c.kinds.length;
      if (!n) { T.pool.push(c.rb); T.cur = null; return; }
      try {
        enc.resolveQuerySet(T.qs, 0, n, T.rs, 0);
        enc.copyBufferToBuffer(T.rs, 0, c.rb, 0, 8 * n);
      } catch (e) { T.pool.push(c.rb); T.cur = null; }
    },

    // after queue.submit(): read the durations back asynchronously
    after() {
      var T = WGPU_GT, c = T.cur;
      T.cur = null;
      if (!c) return;
      var n = 2 * c.kinds.length, rb = c.rb;
      rb.mapAsync(GPUMapMode.READ, 0, 8 * n).then(() => {
        try {
          var t = new BigInt64Array(rb.getMappedRange(0, 8 * n).slice(0));
          for (var i = 0; i < c.kinds.length; i++) {
            var b = t[2 * i], e = t[2 * i + 1];
            if (b > 0n && e >= b) {
              T.add(T.S_US + c.kinds[i], Math.round(Number(e - b) / 1000));
              T.add(T.S_SAMPLES, 1);
            }
          }
          rb.unmap();
        } catch (e) { /* buffer lost with the device */ }
        T.pool.push(rb);
      }, () => { T.pool.push(rb); });
    },
  },

  // C side: wgpu_stats_init() binds g_wgpu_gt
  wgpu_gt_js_bind__deps: ['$WGPU_GT'],
  wgpu_gt_js_bind: (ptr, on) => WGPU_GT.bind(ptr, on),
});
