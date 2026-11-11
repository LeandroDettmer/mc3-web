/**
 * The WebGPU renderer's encoder (ADR 0011, docs/webgpu-plan.md), linked as a
 * --js-library (CMakeLists.txt) together with web/wgpu_fixed.js (its shaders,
 * library object $WGPU_FIXED, pulled in through __deps below).
 *
 * Runs on the render pthread, the worker that owns the transferred
 * OffscreenCanvas (xboxrecomp/src/nv2a_wgpu/wgpu_queue.c). C calls:
 *   wgpu_js_init(WgCtl *)    once: device and canvas set up asynchronously;
 *                            WgCtl.dev_state goes PENDING -> READY | FAILED
 *                            (Atomics.store + notify), LOST later if the
 *                            device goes;
 *   wgpu_js_frame(WgFrame *) per frame: write the arena, walk the command
 *                            list, encode, submit, return. Nothing is kept
 *                            from C's buffers after the call.
 * The contract (records, byte offsets, hand-off, pacing) is
 * xboxrecomp/src/nv2a_wgpu/wgpu_proto.h; the offset tables below mirror its
 * "JS mirror" section exactly. Fresh typed-array views on wasmMemory.buffer
 * are taken at every call (memory may grow).
 *
 * Ops (M2): BEGIN_PASS END_PASS PRESENT VIEWPORT SCISSOR BLEND_CONST
 * STENCIL_REF CLEAR_RECT TEX_CREATE TEX_FREE TEX_UPLOAD SAMPLER SHADER
 * PIPE_CREATE SET_PIPE SET_TEX DRAW COPY_TEX BLIT ZCONV. Unknown ops are
 * skipped by their size; a pass op outside a pass, or a copy/blit/zconv
 * inside one, is logged once and dropped. Resource ops (0x10-0x2F) are
 * device/queue calls and run wherever they sit (TEX_FREE's destroy waits
 * for the submit).
 *
 * The arena: one GPU buffer (STORAGE | INDEX | COPY_DST, a power of two,
 * grown when a frame's arena is larger), one writeBuffer per wgpu_js_frame.
 * Group 0 binds it whole three times (A u32, K vec4f, D WgDrawRec) and is
 * set, with the index buffer, at every BEGIN_PASS. DRAW is
 * drawIndexed(count, 1, first_index, 0, record): firstInstance = record.
 *
 * Caches: shader modules by C's id; pipelines by C's id and by
 * (vs, fs, WgPipeKey bytes), so a key C sends again under another id costs
 * nothing; group 1 layouts by tex_dim; group 1 bind groups by
 * (tex_dim, texture uids, sampler uids), dropped with their texture or
 * sampler, and set only when another than the pass's current one.
 *
 * Fixed shaders: $WGPU_FIXED (web/wgpu_fixed.js, agent F) holds the WGSL
 * and documents, beside each, its bindings, uniform bytes and pipeline
 * state; this file follows that (PRESENT vs_full/fs_present/fs_steady,
 * CLEAR vs_clear/fs_clear, BLIT vs_blit/fs_blit) with explicit layouts (a
 * module may declare fewer bindings than its layout has, never other
 * types). Every uniform block is one 256-byte slot of a per-call ring,
 * bound with a dynamic offset. ZCONV (M4, not written yet) is expected as
 * vs_full + fs_zconv: b0 texture_depth_2d (depth-only view, textureLoad),
 * b1 uniform { zmax u32 @0, flags u32 @4 (WG_ZC_*), width u32 @8, height
 * u32 @12 }, target the destination's format, viewport (0, 0, w, h).
 * A missing module or entry point is logged once and its op skipped.
 *
 * Logs: '[WGPU] adapter ... device ready, canvas <fmt>' once; every distinct
 * WebGPU validation error once; RECOMP_WGPU_STATS=1 prints a line a second
 * and, every 300 frames, '[WGPU] state calls per frame ...' (GPU state calls
 * issued and the records that repeated the pass's state);
 * '[WGPU] slow frame N ms, M pipelines made' for a frame over 100 ms that
 * made pipelines.
 *
 * Pipelines (M3): PIPE_CREATE only describes one. Its first SET_PIPE makes it
 * (createRenderPipeline); one no SET_PIPE of its list used -- the prewarm
 * from wgpu_pipe.c's OPFS key list -- is made at the list's end with
 * createRenderPipelineAsync, and a SET_PIPE before that resolves makes it
 * synchronously.
 *
 * Device set-up, the error/lost handling, the present's fullscreen triangle,
 * the arena and the firstInstance draw record follow nfsu2-web's
 * web/src/gpu/index.ts and encoder.ts by th0mk (MIT).
 */
addToLibrary({
  $WGPU__deps: ['$WGPU_FIXED', '$WGPU_GT', '$GL', '$wasmMemory', 'getenv', '$UTF8ToString',
                '$stringToUTF8OnStack', '$withStackSave'],
  $WGPU: {
    MAGIC: 0x55504757,   // WG_MAGIC "WGPU"
    VERSION: 2,          // WG_VERSION
    // WgCtl byte offsets (84 bytes)
    C: { magic: 0, version: 4, size: 8, dev_state: 12, canvas_format: 16, features: 20,
         max_tex_2d: 24, max_storage_binding: 28, max_in_flight: 32, frames_submitted: 36,
         frames_done: 40, frames_presented: 44, canvas_w: 48, canvas_h: 52, st_cmds: 56,
         st_passes: 60, st_draws: 64, st_enc_us: 68, st_errors: 72, st_pipelines: 76,
         st_tex_live: 80, SIZE: 84 },
    // WgFrame byte offsets (48 bytes)
    F: { magic: 0, version: 4, size: 8, seq: 12, flags: 16, cmd_ptr: 20, cmd_bytes: 24,
         cmd_count: 28, arena_ptr: 32, arena_bytes: 36, translate_us: 40, draws: 44, SIZE: 48 },
    // records: byte offsets inside each, and their sizes
    BP: { color_tex: 4, depth_tex: 8, color_load: 12, color_store: 16, depth_load: 20,
          depth_store: 24, stencil_load: 28, stencil_store: 32, clear_r: 36, clear_g: 40,
          clear_b: 44, clear_a: 48, clear_depth: 52, clear_stencil: 56, SIZE: 60 },
    PR: { src_tex: 4, src_x: 8, src_y: 12, src_w: 16, src_h: 20, out_w: 24, out_h: 28,
          filter: 32, flags: 36, crop_x: 40, crop_y: 44, crop_w: 48, crop_h: 52, band_l: 56,
          band_r: 60, sharp: 64, SIZE_M1: 40, SIZE: 68 },
    VP: { x: 4, y: 8, w: 12, h: 16, min: 20, max: 24, SIZE: 28 },
    SC: { x: 4, y: 8, w: 12, h: 16, SIZE: 20 },
    BC: { r: 4, g: 8, b: 12, a: 16, SIZE: 20 },
    SR: { ref: 4, SIZE: 8 },
    CR: { x: 4, y: 8, w: 12, h: 16, flags: 20, r: 24, g: 28, b: 32, a: 36, depth: 40,
          stencil: 44, SIZE: 48 },
    TC: { id: 4, format: 8, width: 12, height: 16, layers: 20, mips: 24, usage: 28, dim: 32,
          SIZE: 36 },
    TF: { id: 4, SIZE: 8 },
    TU: { id: 4, mip: 8, layer: 12, x: 16, y: 20, w: 24, h: 28, src: 32, bpr: 36, rows: 40,
          bytes: 44, SIZE: 48 },
    SM: { id: 4, addr_u: 8, addr_v: 12, addr_w: 16, mag: 20, min: 24, mip: 28, aniso: 32,
          lod_min: 36, lod_max: 40, SIZE: 44 },
    SH: { id: 4, src: 8, bytes: 12, stages: 16, SIZE: 20 },
    PC: { id: 4, vs: 8, fs: 12, key: 16, SIZE: 44 },
    // WgPipeKey byte offsets (28 bytes, HEAPU8)
    K: { topology: 0, cull: 1, front_face: 2, unclipped_depth: 3, color_fmt: 4, depth_fmt: 5,
         color_mask: 6, blend_enable: 7, blend_src: 8, blend_dst: 9, blend_op: 10,
         depth_write: 11, depth_cmp: 12, stencil_enable: 13, stencil_cmp: 14, stencil_fail: 15,
         stencil_zfail: 16, stencil_pass: 17, stencil_read_mask: 18, stencil_write_mask: 19,
         tex_dim: 20, SIZE: 28 },
    SP: { id: 4, SIZE: 8 },
    ST: { tex: 4, smp: 20, SIZE: 36 },
    DR: { first_index: 4, index_count: 8, record: 12, SIZE: 16 },
    CT: { src: 4, dst: 8, src_x: 12, src_y: 16, dst_x: 20, dst_y: 24, w: 28, h: 32,
          src_mip: 36, dst_mip: 40, src_layer: 44, dst_layer: 48, SIZE: 52 },
    BL: { src: 4, dst: 8, sx: 12, sy: 16, sw: 20, sh: 24, dx: 28, dy: 32, dw: 36, dh: 40,
          filter: 44, SIZE: 48 },
    ZC: { src: 4, dst: 8, w: 12, h: 16, zmax: 20, flags: 24, SIZE: 28 },
    OP_NOP: 0x00, OP_BEGIN_PASS: 0x01, OP_END_PASS: 0x02, OP_PRESENT: 0x03,
    OP_VIEWPORT: 0x04, OP_SCISSOR: 0x05, OP_BLEND_CONST: 0x06, OP_STENCIL_REF: 0x07,
    OP_CLEAR_RECT: 0x08,
    OP_TEX_CREATE: 0x10, OP_TEX_FREE: 0x11, OP_TEX_UPLOAD: 0x12, OP_SAMPLER: 0x13,
    OP_SHADER: 0x20, OP_PIPE_CREATE: 0x21,
    OP_SET_PIPE: 0x30, OP_SET_TEX: 0x31, OP_DRAW: 0x32,
    OP_COPY_TEX: 0x40, OP_BLIT: 0x41, OP_ZCONV: 0x42,
    // shortest record each op needs, by op number (PRESENT: M1's 40, the fill fields are checked by size)
    SIZES: { 1: 60, 3: 40, 4: 28, 5: 20, 6: 20, 7: 8, 8: 48, 16: 36, 17: 8, 18: 48, 19: 44, 32: 20,
             33: 44, 48: 8, 49: 36, 50: 16, 64: 52, 65: 48, 66: 28 },
    // WG_FMT_* -> GPUTextureFormat, in the header's order
    FMT: [null, 'bgra8unorm', 'rgba8unorm', 'depth24plus-stencil8', 'depth32float', 'r8unorm',
          'rg8unorm', 'bc1-rgba-unorm', 'bc2-rgba-unorm', 'bc3-rgba-unorm', 'rgba16float'],
    // the header's enums -> WebGPU strings, in its order
    TOPO: ['point-list', 'line-list', 'triangle-list'],
    CULL: ['none', 'front', 'back'],
    FACE: ['ccw', 'cw'],
    BF: ['zero', 'one', 'src', 'one-minus-src', 'src-alpha', 'one-minus-src-alpha', 'dst',
         'one-minus-dst', 'dst-alpha', 'one-minus-dst-alpha', 'src-alpha-saturated', 'constant',
         'one-minus-constant'],
    BO: ['add', 'subtract', 'reverse-subtract', 'min', 'max'],
    CMP: ['never', 'less', 'equal', 'less-equal', 'greater', 'not-equal', 'greater-equal', 'always'],
    SO: ['keep', 'zero', 'replace', 'invert', 'increment-clamp', 'decrement-clamp',
         'increment-wrap', 'decrement-wrap'],
    ADDR: ['repeat', 'mirror-repeat', 'clamp-to-edge'],
    FILT: ['nearest', 'linear'],
    DEV_PENDING: 1, DEV_READY: 2, DEV_FAILED: 3, DEV_LOST: 4,
    FEAT_BC: 1, FEAT_DEPTH_CLIP_CONTROL: 2, FEAT_TIMESTAMP: 4,
    FF_PARTIAL: 1,
    PF_FILL: 1, PF_STEADY: 2, PF_FREEZE: 4,
    CR_COLOR: 1, CR_DEPTH: 2, CR_STENCIL: 4,
    TEXDIM_CUBE: 2,
    ARENA_MIN: 1 << 20,
    USLOT: 256, USLOTS: 512,   // fixed-shader uniform ring: 256-byte slots per wgpu_js_frame
    BG1_MAX: 8192,             // group 1 bind groups kept before the cache is emptied

    // state (set in init/start: the library object is serialised as source,
    // so no Map/Set here)
    ctl: 0, dev: null, ctx: null, canvas: null, canvasInfo: null, canvasFmt: '',
    tex: null, dead: null, said: null, stats: false, win: null, ct: null, uid: 0,
    samp: null, white2d: null, whiteCube: null, defSmp: null,
    arena: null, arenaSize: 0, arenaOk: false, bgl0: null, bg0: null,
    ubuf: null, ustage: null, uf: null, uu: null, uused: 0,
    shaders: null, pipes: null, pipeByKey: null, bgl1: null, pl: null, bg1: null,
    smps: null, fbgl: null, fmods: null, fpipes: null, fillState: null, ps: null,
    pend: [], lastT: 0, fPipes: 0, fAsync: 0,

    log(s) { err('[WGPU] ' + s); },
    once(key, s) { if (!WGPU.said.has(key)) { WGPU.said.add(key); WGPU.log(s); } },
    u32() { return new Uint32Array(wasmMemory.buffer); },
    env(name) {
      return withStackSave(() => {
        var p = _getenv(stringToUTF8OnStack(name));
        return p ? UTF8ToString(p) : '';
      });
    },
    // ctl field, as a u32 index into a fresh view
    ci(field) { return (WGPU.ctl + WGPU.C[field]) >>> 2; },
    setState(s) {
      var I = new Int32Array(wasmMemory.buffer), i = WGPU.ci('dev_state');
      Atomics.store(I, i, s);
      Atomics.notify(I, i);
    },
    // a validation error scope around f(); on an error, bad(message) later
    scoped(f, bad) {
      var dev = WGPU.dev;
      dev.pushErrorScope('validation');
      var r = f();
      dev.popErrorScope().then((e) => { if (e) bad(e.message); }, () => {});
      return r;
    },

    init(ctl) {
      var W = WGPU, C = W.C;
      W.ctl = ctl >>> 0;
      W.tex = new Map(); W.dead = []; W.said = new Set();
      var U = W.u32(), c = W.ctl >>> 2;
      if (U[c + (C.magic >> 2)] !== W.MAGIC || U[c + (C.version >> 2)] !== W.VERSION
          || U[c + (C.size >> 2)] !== C.SIZE) {
        W.log('control block magic/version/size mismatch (C ' + U[c + 1] + '/' + U[c + 2]
              + ', JS ' + W.VERSION + '/' + C.SIZE + '): rebuild; no WebGPU');
        W.setState(W.DEV_FAILED);
        return;
      }
      W.stats = /^[1-9]/.test(W.env('RECOMP_WGPU_STATS'));
      W.ct = W.stats ? W.newCt() : null;
      W.setState(W.DEV_PENDING);
      W.start().catch((e) => {
        W.log('no WebGPU device: ' + (e && e.message || e));
        W.dev = null;
        W.setState(W.DEV_FAILED);
      });
    },

    async start() {
      var W = WGPU;
      if (typeof navigator == 'undefined' || !navigator.gpu)
        throw new Error('navigator.gpu is missing in the render worker');
      // the canvas this pthread owns (emscripten_pthread_attr_settransferredcanvases "#canvas")
      var oc = GL.offscreenCanvases || {};
      var info = oc['canvas'] || Object.values(oc).find(Boolean) || null;
      var canvas = info ? info.offscreenCanvas : Module['canvas'];
      if (!canvas || !canvas.getContext) throw new Error('no OffscreenCanvas in the render worker');
      var adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
      if (!adapter) throw new Error('no adapter');
      var want = ['texture-compression-bc', 'depth-clip-control', 'timestamp-query']
        .filter((f) => adapter.features.has(f));
      var L = adapter.limits;
      var dev = await adapter.requestDevice({ requiredFeatures: want, requiredLimits: {
        maxBufferSize: L.maxBufferSize, maxStorageBufferBindingSize: L.maxStorageBufferBindingSize,
        maxTextureDimension2D: L.maxTextureDimension2D } });
      dev.addEventListener('uncapturederror', (e) => {
        var U = W.u32(), i = W.ci('st_errors');
        U[i] = U[i] + 1;
        var m = e.error && e.error.message || String(e.error);
        W.once('e:' + m, 'WebGPU error: ' + m);
      });
      dev.lost.then((i) => W.lost(dev, i.reason + ' ' + i.message));
      var ctx = canvas.getContext('webgpu');
      if (!ctx) { dev.destroy(); throw new Error("getContext('webgpu') failed on the OffscreenCanvas"); }
      var fmt = navigator.gpu.getPreferredCanvasFormat();
      ctx.configure({ device: dev, format: fmt, alphaMode: 'opaque',
                      usage: GPUTextureUsage.RENDER_ATTACHMENT });

      W.dev = dev; W.ctx = ctx; W.canvas = canvas; W.canvasInfo = info; W.canvasFmt = fmt;
      W.setup();
      W.win = W.newWin(performance.now());
      var U = W.u32();
      U[W.ci('canvas_format')] = Math.max(0, W.FMT.indexOf(fmt));
      U[W.ci('features')] = (dev.features.has('texture-compression-bc') ? W.FEAT_BC : 0)
        | (dev.features.has('depth-clip-control') ? W.FEAT_DEPTH_CLIP_CONTROL : 0)
        | (dev.features.has('timestamp-query') ? W.FEAT_TIMESTAMP : 0);
      U[W.ci('max_tex_2d')] = dev.limits.maxTextureDimension2D;
      U[W.ci('max_storage_binding')] = Math.min(dev.limits.maxStorageBufferBindingSize, 0xFFFFFFFF);
      U[W.ci('canvas_w')] = canvas.width;
      U[W.ci('canvas_h')] = canvas.height;
      var ai = adapter.info || {};
      W.log('adapter ' + ([ai.vendor, ai.architecture, ai.description].filter(Boolean).join(' ') || '?')
            + ', features ' + (want.join(' ') || 'none') + ', device ready, canvas ' + fmt);
      W.setState(W.DEV_READY);
    },

    // No more drawing on this device: frames are dropped and the render thread consumes the ring
    // without drawing (wq_turn), so the game runs on; the page's '[WGPU] device lost' box offers a
    // restart, and its next start on this device uses WebGL (index.html gpuChoice).
    lost(dev, why) {
      var W = WGPU;
      if (W.dev !== dev) return;
      W.dev = null;
      W.log('device lost: ' + why);
      W.setState(W.DEV_LOST);
    },

    newWin(t) {
      return { t: t, frames: 0, presented: 0, passes: 0, cmds: 0, encUs: 0, trUs: 0, draws: 0,
               pipes: 0, arena: 0 };
    },
    // RECOMP_WGPU_STATS=1: state-call counters (W.ct, null when off), logged every CT_FRAMES frames
    CT_FRAMES: 300,
    newCt() {
      return { f: 0, draws: 0, dpass: 0, passes: 0, sp: 0, spSame: 0, st: 0, stSame: 0, stWhite: 0,
               bgNew: 0, vp: 0, vpSame: 0, sc: 0, scSame: 0, bc: 0, sr: 0, clr: 0,
               wbArena: 0, wbUni: 0, wtex: 0 };
    },
    // a state record: counted, and whether it repeats what the pass has
    ctSame(k, a, b) {
      var c = WGPU.ct, i;
      c[k]++;
      for (i = 0; i < a.length && a[i] === b[i]; i++);
      if (i === a.length) c[k + 'Same']++;
    },

    // tables, the arena and group 0, the uniform ring, defaults, fixed layouts
    setup() {
      var W = WGPU, dev = W.dev, VF = GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT;
      W.shaders = new Map(); W.pipes = new Map(); W.pipeByKey = new Map(); W.bgl1 = new Map();
      W.pl = new Map(); W.bg1 = new Map(); W.smps = new Map(); W.fmods = new Map();
      W.fpipes = new Map();
      W.samp = [dev.createSampler({ magFilter: 'nearest', minFilter: 'nearest' }),
                dev.createSampler({ magFilter: 'linear', minFilter: 'linear' })];
      W.defSmp = { gpu: W.samp[0], uid: 0, bgKeys: [] };
      var white = (cube) => {
        var t = dev.createTexture({ size: [1, 1, cube ? 6 : 1], format: 'rgba8unorm',
          usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
        dev.queue.writeTexture({ texture: t }, new Uint8Array(4 * (cube ? 6 : 1)).fill(255),
                               { bytesPerRow: 4, rowsPerImage: 1 }, [1, 1, cube ? 6 : 1]);
        return t.createView({ dimension: cube ? 'cube' : '2d' });
      };
      W.white2d = white(false); W.whiteCube = white(true);

      W.bgl0 = dev.createBindGroupLayout({ entries: [0, 1, 2].map((b) => (
        { binding: b, visibility: VF, buffer: { type: 'read-only-storage' } })) });
      W.arenaSize = 0;
      W.arenaGrow(W.ARENA_MIN);

      W.ubuf = dev.createBuffer({ size: W.USLOT * W.USLOTS,
                                  usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
      W.ustage = new ArrayBuffer(W.USLOT * W.USLOTS);
      W.uf = new Float32Array(W.ustage); W.uu = new Uint32Array(W.ustage); W.uused = 0;
      var ub = { type: 'uniform', hasDynamicOffset: true };
      var tf = { sampleType: 'float' };
      W.fbgl = {
        tex: dev.createBindGroupLayout({ entries: [
          { binding: 0, visibility: VF, texture: tf },
          { binding: 1, visibility: VF, sampler: { type: 'filtering' } },
          { binding: 2, visibility: VF, buffer: ub },
          { binding: 3, visibility: VF, texture: { sampleType: 'float' } }] }),
        z: dev.createBindGroupLayout({ entries: [
          { binding: 0, visibility: VF, texture: { sampleType: 'depth' } },
          { binding: 1, visibility: VF, buffer: ub }] }),
      };

      // the steady fill's counter: two 1x1 textures, alternating; starts filled (as nv2a_gl)
      var st = [0, 1].map(() => {
        var t = dev.createTexture({ size: [1, 1, 1], format: 'rgba8unorm', usage:
          GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_DST });
        dev.queue.writeTexture({ texture: t }, new Uint8Array([255, 255, 255, 255]),
                               { bytesPerRow: 4, rowsPerImage: 1 }, [1, 1, 1]);
        return t.createView();
      });
      W.fillState = { view: st, cur: 0 };
    },

    // the arena buffer at least `need` bytes (a power of two); group 0 rebuilt
    arenaGrow(need) {
      var W = WGPU, dev = W.dev;
      var lim = Math.min(dev.limits.maxStorageBufferBindingSize, dev.limits.maxBufferSize);
      if (need <= W.arenaSize) return true;
      var size = Math.max(W.ARENA_MIN, W.arenaSize);
      while (size < need) size *= 2;
      if (size > lim) size = lim & ~255;
      if (size < need) return false;
      if (W.arena) W.dead.push(W.arena);   // destroyed after the submit
      W.arena = dev.createBuffer({ size: size,
        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST });
      W.arenaSize = size;
      W.bg0 = dev.createBindGroup({ layout: W.bgl0, entries: [0, 1, 2].map((b) => (
        { binding: b, resource: { buffer: W.arena } })) });
      return true;
    },

    // a 256-byte slot of the uniform ring: its byte offset, or -1 when full
    uslot() {
      var W = WGPU;
      if (W.uused >= W.USLOTS) { W.once('uring', 'fixed-shader uniform ring full (' + W.USLOTS + ' a frame): op dropped'); return -1; }
      var o = W.uused++ * W.USLOT;
      W.uf.fill(0, o >> 2, (o + W.USLOT) >> 2);
      return o;
    },

    // ---- fixed pipelines (web/wgpu_fixed.js) ----

    fmod(name) {
      var W = WGPU;
      if (W.fmods.has(name)) return W.fmods.get(name);
      var code = WGPU_FIXED[name], m = null;
      if (typeof code != 'string') W.once('fx' + name, '$WGPU_FIXED.' + name + ' is missing: its ops are skipped');
      else {
        m = W.dev.createShaderModule({ label: 'fixed ' + name, code: code });
        m.getCompilationInfo().then((ci) => {
          for (var e of ci.messages) if (e.type === 'error')
            W.once('fxc' + name + e.message, 'WGSL $WGPU_FIXED.' + name + ':' + e.lineNum + ': ' + e.message);
        }, () => {});                  // a refusal unhandled in this pthread would end the game ('worker sent an error!')
      }
      W.fmods.set(name, m);
      return m;
    },
    // a fixed pipeline, made once per key; null when its module is missing or it failed
    fpipe(key, name, vsEntry, fsEntry, bgls, targets, depthStencil) {
      var W = WGPU;
      if (W.fpipes.has(key)) return W.fpipes.get(key);
      var m = W.fmod(name), p = null;
      // an entry point the module lacks would make an invalid pipeline and lose a whole frame
      var has = (fn) => new RegExp('fn\\s+' + fn + '\\s*\\(').test(WGPU_FIXED[name]);
      if (m && !(has(vsEntry) && has(fsEntry))) {
        W.once('fxe' + key, '$WGPU_FIXED.' + name + ' has no ' + vsEntry + '/' + fsEntry + ': ' + key + ' skipped');
        m = null;
      }
      if (m) {
        var d = { label: 'fixed ' + key, layout: W.dev.createPipelineLayout({ bindGroupLayouts: bgls }),
                  vertex: { module: m, entryPoint: vsEntry },
                  fragment: { module: m, entryPoint: fsEntry, targets: targets },
                  primitive: { topology: 'triangle-list' } };
        if (depthStencil) d.depthStencil = depthStencil;
        p = W.scoped(() => W.dev.createRenderPipeline(d), (msg) => {
          W.once('fpb' + key, 'fixed pipeline ' + key + ' (' + name + ' ' + vsEntry + '/' + fsEntry + '): ' + msg);
          W.fpipes.set(key, null);
        });
        W.statPipe();
      }
      W.fpipes.set(key, p);
      return p;
    },
    statPipe() {
      var W = WGPU, U = W.u32(), i = W.ci('st_pipelines');
      U[i] = U[i] + 1;
      W.fPipes++;
      if (W.win) W.win.pipes++;
    },

    // ---- textures, samplers ----

    // a view for a render attachment: one mip, one layer
    att(t) {
      if (!t.att) t.att = t.layers === 1 && t.mips === 1 ? t.view
        : t.gpu.createView({ dimension: '2d', baseMipLevel: 0, mipLevelCount: 1, baseArrayLayer: 0, arrayLayerCount: 1 });
      return t.att;
    },
    // a texture or sampler goes: its group 1 bind groups go with it
    unbind(o) {
      for (var i = 0; i < o.bgKeys.length; i++) WGPU.bg1.delete(o.bgKeys[i]);
      o.bgKeys.length = 0;
    },
    texDrop(t) { WGPU.unbind(t); WGPU.dead.push(t.gpu); },

    texCreate(p, U) {
      var W = WGPU, T = W.TC;
      var id = U[(p + T.id) >>> 2], fi = U[(p + T.format) >>> 2];
      var format = W.FMT[fi];
      if (!id || !format) { W.once('tc' + fi, 'TEX_CREATE: id ' + id + ', unknown format ' + fi + ': skipped'); return; }
      var w = U[(p + T.width) >>> 2] || 1, h = U[(p + T.height) >>> 2] || 1;
      var layers = U[(p + T.layers) >>> 2] || 1, mips = U[(p + T.mips) >>> 2] || 1;
      var cube = U[(p + T.dim) >>> 2] === 1;
      var old = W.tex.get(id);
      if (old) { W.once('tcdup', 'TEX_CREATE: id ' + id + ' is still live: replaced'); W.texDrop(old); }
      var gpu = W.dev.createTexture({ size: [w, h, layers], format: format, mipLevelCount: mips,
                                      usage: U[(p + T.usage) >>> 2], dimension: '2d' });
      W.tex.set(id, { gpu: gpu, w: w, h: h, layers: layers, mips: mips, format: format, cube: cube,
                      stencil: fi === 3, depth: fi === 3 || fi === 4, uid: ++W.uid, bgKeys: [],
                      view: gpu.createView(cube ? { dimension: 'cube' } : undefined),
                      att: null, zview: null, bg: {} });
    },

    texFree(p, U) {
      var W = WGPU, id = U[(p + W.TF.id) >>> 2], t = W.tex.get(id);
      if (!t) { W.once('tf', 'TEX_FREE: unknown id ' + id); return; }
      W.tex.delete(id);
      W.texDrop(t);   // destroyed after the submit
    },

    texUpload(p, U) {
      var W = WGPU, T = W.TU, id = U[(p + T.id) >>> 2], t = W.tex.get(id);
      if (!t) { W.once('tu', 'TEX_UPLOAD: unknown texture ' + id); return; }
      var src = U[(p + T.src) >>> 2], n = U[(p + T.bytes) >>> 2];
      if (W.ct) W.ct.wtex += n;
      W.dev.queue.writeTexture(
        { texture: t.gpu, mipLevel: U[(p + T.mip) >>> 2],
          origin: { x: U[(p + T.x) >>> 2], y: U[(p + T.y) >>> 2], z: U[(p + T.layer) >>> 2] } },
        new Uint8Array(U.buffer, src, n),
        { bytesPerRow: U[(p + T.bpr) >>> 2], rowsPerImage: U[(p + T.rows) >>> 2] },
        { width: U[(p + T.w) >>> 2], height: U[(p + T.h) >>> 2], depthOrArrayLayers: 1 });
    },

    sampler(p, U, F) {
      var W = WGPU, S = W.SM, id = U[(p + S.id) >>> 2];
      if (!id) return;
      var mag = W.FILT[U[(p + S.mag) >>> 2]] || 'nearest', min = W.FILT[U[(p + S.min) >>> 2]] || 'nearest';
      var mip = W.FILT[U[(p + S.mip) >>> 2]] || 'nearest';
      var an = Math.min(Math.max(U[(p + S.aniso) >>> 2], 1), 16);
      if (mag !== 'linear' || min !== 'linear' || mip !== 'linear') an = 1;   // WebGPU's rule
      var lo = Math.max(F[(p + S.lod_min) >>> 2], 0), hi = Math.max(F[(p + S.lod_max) >>> 2], lo);
      var gpu = W.dev.createSampler({
        addressModeU: W.ADDR[U[(p + S.addr_u) >>> 2]] || 'clamp-to-edge',
        addressModeV: W.ADDR[U[(p + S.addr_v) >>> 2]] || 'clamp-to-edge',
        addressModeW: W.ADDR[U[(p + S.addr_w) >>> 2]] || 'clamp-to-edge',
        magFilter: mag, minFilter: min, mipmapFilter: mip, lodMinClamp: lo, lodMaxClamp: hi,
        maxAnisotropy: an });
      var old = W.smps.get(id);
      if (old) W.unbind(old);
      W.smps.set(id, { gpu: gpu, uid: ++W.uid, bgKeys: [] });
    },

    // ---- shaders, pipelines ----

    shader(p, U) {
      var W = WGPU, S = W.SH, id = U[(p + S.id) >>> 2];
      var code = UTF8ToString(U[(p + S.src) >>> 2], U[(p + S.bytes) >>> 2]);
      var e = { mod: null, bad: false };
      e.mod = W.scoped(() => W.dev.createShaderModule({ label: 'shader ' + id, code: code }),
                       (m) => { e.bad = true; W.once('shv' + id, 'SHADER ' + id + ': ' + m); });
      e.mod.getCompilationInfo().then((ci) => {
        for (var x of ci.messages) if (x.type === 'error') {
          e.bad = true;
          W.once('shc' + id, 'SHADER ' + id + ' line ' + x.lineNum + ': ' + x.message);
        }
      }, () => {});
      if (W.shaders.has(id)) W.once('shdup', 'SHADER: id ' + id + ' sent again: replaced');
      W.shaders.set(id, e);
    },

    // group 1's layout for a tex_dim list: bindings 0-3 textures, 4-7 samplers
    bgl1For(dims) {
      var W = WGPU, k = dims.join('');
      var l = W.bgl1.get(k);
      if (!l) {
        var e = [];
        for (var i = 0; i < 4; i++)
          e.push({ binding: i, visibility: GPUShaderStage.FRAGMENT,
                   texture: { sampleType: 'float', viewDimension: dims[i] === W.TEXDIM_CUBE ? 'cube' : '2d' } });
        for (i = 0; i < 4; i++)
          e.push({ binding: 4 + i, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } });
        l = W.dev.createBindGroupLayout({ label: 'group1 ' + k, entries: e });
        W.bgl1.set(k, l);
        W.pl.set(k, W.dev.createPipelineLayout({ bindGroupLayouts: [W.bgl0, l] }));
      }
      return l;
    },

    pipeCreate(p, U) {
      var W = WGPU, P = W.PC, K = W.K;
      var id = U[(p + P.id) >>> 2], vs = U[(p + P.vs) >>> 2], fs = U[(p + P.fs) >>> 2];
      var B = new Uint8Array(U.buffer, p + P.key, K.SIZE), b = Array.from(B);
      var dims = b.slice(K.tex_dim, K.tex_dim + 4), dk = dims.join('');
      var key = vs + ',' + fs + ',' + b.join('.');
      var e = W.pipeByKey.get(key);
      if (!e) {
        e = { gpu: null, bad: false, dk: dk };
        var vm = W.shaders.get(vs), fm = W.shaders.get(fs);
        if (!vm || !fm || vm.bad || fm.bad) {
          W.once('pcm' + id, 'PIPE_CREATE ' + id + ': shader ' + vs + '/' + fs + ' missing or failed: its draws dropped');
          e.bad = true;
        } else {
          W.bgl1For(dims);
          var d = { label: 'pipe ' + id, layout: W.pl.get(dk),
                    vertex: { module: vm.mod, entryPoint: 'vs_main' },
                    primitive: { topology: W.TOPO[b[K.topology]] || 'triangle-list',
                                 cullMode: W.CULL[b[K.cull]] || 'none',
                                 frontFace: W.FACE[b[K.front_face]] || 'ccw' } };
          if (b[K.unclipped_depth] && W.dev.features.has('depth-clip-control')) d.primitive.unclippedDepth = true;
          var targets = [], cf = W.FMT[b[K.color_fmt]];
          if (cf) {
            var t = { format: cf, writeMask: b[K.color_mask] & 15 };
            if (b[K.blend_enable]) {
              var op = W.BO[b[K.blend_op]] || 'add', mm = op === 'min' || op === 'max';
              var c = { operation: op, srcFactor: mm ? 'one' : (W.BF[b[K.blend_src]] || 'one'),
                        dstFactor: mm ? 'one' : (W.BF[b[K.blend_dst]] || 'zero') };
              t.blend = { color: c, alpha: c };
            }
            targets.push(t);
          }
          d.fragment = { module: fm.mod, entryPoint: 'fs_main', targets: targets };
          var df = W.FMT[b[K.depth_fmt]];
          if (df) {
            var ds = { format: df, depthWriteEnabled: !!b[K.depth_write],
                       depthCompare: W.CMP[b[K.depth_cmp]] || 'always' };
            if (b[K.stencil_enable] && df === 'depth24plus-stencil8') {
              var f = { compare: W.CMP[b[K.stencil_cmp]] || 'always', failOp: W.SO[b[K.stencil_fail]] || 'keep',
                        depthFailOp: W.SO[b[K.stencil_zfail]] || 'keep', passOp: W.SO[b[K.stencil_pass]] || 'keep' };
              ds.stencilFront = f; ds.stencilBack = f;
              ds.stencilReadMask = b[K.stencil_read_mask]; ds.stencilWriteMask = b[K.stencil_write_mask];
            }
            d.depthStencil = ds;
          }
          // made at its first SET_PIPE (sync), or at the frame's end without
          // one (async: the C prewarm, wgpu_pipe.c) -- pipeMake / pipeAsync
          e.desc = d;
          e.what = 'PIPE_CREATE ' + id + ' (vs ' + vs + ', fs ' + fs + ', key ' + b.join('.') + '): ';
          W.pend.push(e);
        }
        W.pipeByKey.set(key, e);
      }
      if (W.pipes.has(id)) W.once('pcdup', 'PIPE_CREATE: id ' + id + ' sent again: replaced');
      W.pipes.set(id, e);
    },

    pipeMake(e) {
      var W = WGPU;
      e.gpu = W.scoped(() => W.dev.createRenderPipeline(e.desc), (m) => {
        e.bad = true;
        W.once('pcb' + e.what, e.what + m);
      });
      e.desc = null;
      W.statPipe();
    },
    pipeAsync(e) {
      var W = WGPU, d = e.desc;
      e.desc = null; e.async = d;      // kept for a SET_PIPE before it resolves (made sync then)
      W.dev.createRenderPipelineAsync(d).then((p) => {
        if (!e.gpu && !e.bad) e.gpu = p;
        e.async = null;
      }, (x) => {
        if (!e.gpu) { e.bad = true; W.once('pcb' + e.what, e.what + (x && x.message || x)); }
        e.async = null;
      });
      W.statPipe();
      W.fAsync++;
    },

    // ---- pass state ----

    beginPass(enc, p, U, F) {
      var W = WGPU, B = W.BP, d = { colorAttachments: [] };
      var ct = U[(p + B.color_tex) >>> 2], dt = U[(p + B.depth_tex) >>> 2];
      var tw = 0, th = 0, cf = null, df = null, ds = false;
      if (ct) {
        var t = W.tex.get(ct);
        if (!t) W.once('bpc', 'BEGIN_PASS: unknown colour texture ' + ct);
        else {
          d.colorAttachments.push({ view: W.att(t),
            loadOp: U[(p + B.color_load) >>> 2] === 1 ? 'clear' : 'load',
            storeOp: U[(p + B.color_store) >>> 2] === 1 ? 'discard' : 'store',
            clearValue: { r: F[(p + B.clear_r) >>> 2], g: F[(p + B.clear_g) >>> 2],
                          b: F[(p + B.clear_b) >>> 2], a: F[(p + B.clear_a) >>> 2] } });
          tw = t.w; th = t.h; cf = t.format;
        }
      }
      if (dt) {
        var z = W.tex.get(dt);
        if (!z) W.once('bpd', 'BEGIN_PASS: unknown depth texture ' + dt);
        else {
          var a = { view: W.att(z),
            depthLoadOp: U[(p + B.depth_load) >>> 2] === 1 ? 'clear' : 'load',
            depthStoreOp: U[(p + B.depth_store) >>> 2] === 1 ? 'discard' : 'store',
            depthClearValue: F[(p + B.clear_depth) >>> 2] };
          if (z.stencil) {   // WebGPU wants no stencil ops on a format without stencil
            a.stencilLoadOp = U[(p + B.stencil_load) >>> 2] === 1 ? 'clear' : 'load';
            a.stencilStoreOp = U[(p + B.stencil_store) >>> 2] === 1 ? 'discard' : 'store';
            a.stencilClearValue = U[(p + B.clear_stencil) >>> 2] & 255;
          }
          d.depthStencilAttachment = a;
          if (!tw) { tw = z.w; th = z.h; }
          df = z.format; ds = z.stencil;
        }
      }
      if (!d.colorAttachments.length && !d.depthStencilAttachment) {
        W.once('bpn', 'BEGIN_PASS without attachments: skipped');
        return null;
      }
      var pass = enc.beginRenderPass(WGPU_GT.tw(d, 0));   // M4 GPU timing (wgpu_stats.js)
      pass.setBindGroup(0, W.bg0);
      pass.setIndexBuffer(W.arena, 'uint32');
      if (W.ct) W.ct.dpass++;
      // WebGPU's state at a pass's start, as the contract says C assumes
      W.ps = { pass: pass, tw: tw, th: th, cf: cf, df: df, ds: ds, pipe: null, bg1: null,
               vp: [0, 0, tw, th, 0, 1], sc: [0, 0, tw, th], ref: 0, bc: { r: 0, g: 0, b: 0, a: 0 } };
      return pass;
    },

    viewport(p, F) {
      var W = WGPU, V = W.VP, s = W.ps;
      var w = F[(p + V.w) >>> 2], h = F[(p + V.h) >>> 2];
      if (!(w > 0 && h > 0)) { W.once('vp0', 'SET_VIEWPORT: empty (' + w + 'x' + h + '): dropped'); return; }
      var lo = Math.min(Math.max(F[(p + V.min) >>> 2], 0), 1), hi = Math.min(Math.max(F[(p + V.max) >>> 2], lo), 1);
      var o = s.vp;
      s.vp = [F[(p + V.x) >>> 2], F[(p + V.y) >>> 2], w, h, lo, hi];
      if (W.ct) W.ctSame('vp', o, s.vp);
      s.pass.setViewport(s.vp[0], s.vp[1], w, h, lo, hi);
    },
    // a rectangle clamped to the attachment
    clampRect(x, y, w, h) {
      var s = WGPU.ps;
      x = Math.min(x, s.tw); y = Math.min(y, s.th);
      return [x, y, Math.min(w, s.tw - x), Math.min(h, s.th - y)];
    },
    scissor(p, U) {
      var W = WGPU, S = W.SC, s = W.ps, o = s.sc;
      s.sc = W.clampRect(U[(p + S.x) >>> 2], U[(p + S.y) >>> 2], U[(p + S.w) >>> 2], U[(p + S.h) >>> 2]);
      if (W.ct) W.ctSame('sc', o, s.sc);
      s.pass.setScissorRect(s.sc[0], s.sc[1], s.sc[2], s.sc[3]);
    },

    // CLEAR_RECT: a quad with its own pipeline, then the pass state as it was
    clearRect(p, U, F) {
      var W = WGPU, R = W.CR, s = W.ps, pass = s.pass;
      var fl = U[(p + R.flags) >>> 2];
      var r = W.clampRect(U[(p + R.x) >>> 2], U[(p + R.y) >>> 2], U[(p + R.w) >>> 2], U[(p + R.h) >>> 2]);
      if (!r[2] || !r[3]) return;
      var col = s.cf && (fl & W.CR_COLOR), dep = s.df && (fl & W.CR_DEPTH), stn = s.ds && (fl & W.CR_STENCIL);
      if (!col && !dep && !stn) return;
      if (W.ct) W.ct.clr++;
      var mask = col ? (fl >>> 8) & 15 : 0;
      var key = 'clear ' + s.cf + '/' + s.df + '/' + mask + '/' + (dep ? 1 : 0) + (stn ? 1 : 0);
      // the colour is the blend constant (1 x constant + 0), the depth the viewport's min = max
      var bc = { operation: 'add', srcFactor: 'constant', dstFactor: 'zero' };
      var targets = s.cf ? [{ format: s.cf, writeMask: mask, blend: { color: bc, alpha: bc } }] : [], dsd = null;
      if (s.df) {
        dsd = { format: s.df, depthWriteEnabled: !!dep, depthCompare: 'always' };
        if (s.ds) {
          var f = { compare: 'always', failOp: 'keep', depthFailOp: 'keep', passOp: 'replace' };
          dsd.stencilFront = f; dsd.stencilBack = f;
          dsd.stencilReadMask = 0xFF; dsd.stencilWriteMask = stn ? 0xFF : 0;
        }
      }
      var pipe = W.fpipe(key, 'CLEAR', 'vs_clear', 'fs_clear', [], targets, dsd);
      if (!pipe) return;
      var z = Math.min(Math.max(F[(p + R.depth) >>> 2], 0), 1);
      pass.setPipeline(pipe);
      pass.setViewport(r[0], r[1], r[2], r[3], z, z);
      pass.setScissorRect(r[0], r[1], r[2], r[3]);
      if (col) pass.setBlendConstant({ r: F[(p + R.r) >>> 2], g: F[(p + R.g) >>> 2],
                                       b: F[(p + R.b) >>> 2], a: F[(p + R.a) >>> 2] });
      if (stn) pass.setStencilReference(U[(p + R.stencil) >>> 2] & 255);
      pass.draw(3);
      // back to the pass's own state (groups 0 and 1 stay bound across pipelines)
      if (s.pipe) pass.setPipeline(s.pipe.gpu);
      pass.setViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3], s.vp[4], s.vp[5]);
      pass.setScissorRect(s.sc[0], s.sc[1], s.sc[2], s.sc[3]);
      if (col) pass.setBlendConstant(s.bc);
      if (stn) pass.setStencilReference(s.ref);
    },

    setPipe(p, U) {
      var W = WGPU, s = W.ps, id = U[(p + W.SP.id) >>> 2], e = W.pipes.get(id);
      if (!e) { W.once('spu' + id, 'SET_PIPE: unknown pipeline ' + id + ': its draws dropped'); s.pipe = null; return; }
      if (!e.gpu && !e.bad && (e.desc || e.async)) {
        if (!e.desc) e.desc = e.async;   // still compiling asynchronously: made now, as without prewarm
        W.pipeMake(e);
      }
      if (e.bad || !e.gpu) { s.pipe = null; return; }
      if (W.ct) { W.ct.sp++; if (s.pipe === e) W.ct.spSame++; }
      if (s.pipe !== e) {
        if (s.bg1 && (!s.pipe || s.pipe.dk !== e.dk)) s.bg1 = null;   // another group 1 layout: rebind before a draw
        s.pass.setPipeline(e.gpu); s.pipe = e;
      }
    },

    setTex(p, U) {
      var W = WGPU, s = W.ps, e = s.pipe;
      if (!e) return;
      var dims = e.dk, key = dims, i, ts = [], ss = [];
      for (i = 0; i < 4; i++) {
        var cube = dims.charCodeAt(i) - 48 === W.TEXDIM_CUBE;
        var t = W.tex.get(U[(p + W.ST.tex + 4 * i) >>> 2]) || null;
        if (t && (t.cube !== cube || t.depth)) {
          W.once('stx' + i + cube + t.depth, 'SET_TEX: stage ' + i + ' texture ' + (t.depth ? 'is depth' : 'dimension mismatch') + ': white');
          t = null;
        }
        var m = W.smps.get(U[(p + W.ST.smp + 4 * i) >>> 2]) || W.defSmp;
        ts.push(t); ss.push(m);
        key += '|' + (t ? t.uid : cube ? 'c' : 'w') + ':' + m.uid;
      }
      var bg = W.bg1.get(key);
      if (!bg) {
        if (W.bg1.size >= W.BG1_MAX) {   // shortcut: empties the lot, then refills; an LRU if it ever churns
          W.bg1.clear();
          W.tex.forEach((x) => { x.bgKeys.length = 0; });
          W.smps.forEach((x) => { x.bgKeys.length = 0; });
        }
        var en = [];
        for (i = 0; i < 4; i++) {
          var cb = dims.charCodeAt(i) - 48 === W.TEXDIM_CUBE;
          en.push({ binding: i, resource: ts[i] ? ts[i].view : cb ? W.whiteCube : W.white2d });
          en.push({ binding: 4 + i, resource: ss[i].gpu });
          if (ts[i]) ts[i].bgKeys.push(key);
          if (ss[i] !== W.defSmp) ss[i].bgKeys.push(key);
        }
        bg = W.dev.createBindGroup({ layout: W.bgl1.get(dims), entries: en });
        W.bg1.set(key, bg);
        if (W.ct) W.ct.bgNew++;
      }
      if (W.ct) { W.ct.st++; if (s.bg1 === bg) W.ct.stSame++; }
      if (s.bg1 !== bg) { s.pass.setBindGroup(1, bg); s.bg1 = bg; }
    },

    // group 1 all white for a pipeline drawn without SET_TEX (a validation error otherwise)
    bg1White(e) {
      var W = WGPU, key = e.dk + '|white', bg = W.bg1.get(key);
      if (!bg) {
        var en = [];
        for (var i = 0; i < 4; i++) {
          en.push({ binding: i, resource: e.dk.charCodeAt(i) - 48 === W.TEXDIM_CUBE ? W.whiteCube : W.white2d });
          en.push({ binding: 4 + i, resource: W.defSmp.gpu });
        }
        bg = W.dev.createBindGroup({ layout: W.bgl1.get(e.dk), entries: en });
        W.bg1.set(key, bg);
        if (W.ct) W.ct.bgNew++;
      }
      return bg;
    },

    // ---- encoder ops (outside passes) ----

    copyTex(enc, p, U) {
      var W = WGPU, T = W.CT;
      var s = W.tex.get(U[(p + T.src) >>> 2]), d = W.tex.get(U[(p + T.dst) >>> 2]);
      if (!s || !d || s === d) { W.once('ctx', 'COPY_TEX: unknown or same textures ' + U[(p + T.src) >>> 2] + ' -> ' + U[(p + T.dst) >>> 2]); return; }
      enc.copyTextureToTexture(
        { texture: s.gpu, mipLevel: U[(p + T.src_mip) >>> 2],
          origin: { x: U[(p + T.src_x) >>> 2], y: U[(p + T.src_y) >>> 2], z: U[(p + T.src_layer) >>> 2] } },
        { texture: d.gpu, mipLevel: U[(p + T.dst_mip) >>> 2],
          origin: { x: U[(p + T.dst_x) >>> 2], y: U[(p + T.dst_y) >>> 2], z: U[(p + T.dst_layer) >>> 2] } },
        { width: U[(p + T.w) >>> 2], height: U[(p + T.h) >>> 2], depthOrArrayLayers: 1 });
    },

    // a bind group of the fixed 'tex' layout, cached on the source texture
    fbgTex(t, k, view, smp, state) {
      var W = WGPU;
      return t.bg[k] || (t.bg[k] = W.dev.createBindGroup({ layout: W.fbgl.tex, entries: [
        { binding: 0, resource: view }, { binding: 1, resource: smp },
        { binding: 2, resource: { buffer: W.ubuf, size: W.USLOT } },
        { binding: 3, resource: state }] }));
    },

    blit(enc, p, U) {
      var W = WGPU, B = W.BL;
      var s = W.tex.get(U[(p + B.src) >>> 2]), d = W.tex.get(U[(p + B.dst) >>> 2]);
      if (!s || !d || s === d || s.depth || d.depth) {
        W.once('blx', 'BLIT: unknown, same or depth textures ' + U[(p + B.src) >>> 2] + ' -> ' + U[(p + B.dst) >>> 2]);
        return false;
      }
      var dx = U[(p + B.dx) >>> 2], dy = U[(p + B.dy) >>> 2], dw = U[(p + B.dw) >>> 2], dh = U[(p + B.dh) >>> 2];
      var rx = Math.min(dx, d.w), ry = Math.min(dy, d.h), rw = Math.min(dw, d.w - rx), rh = Math.min(dh, d.h - ry);
      if (!dw || !dh || !rw || !rh) return false;
      var pipe = W.fpipe('blit ' + d.format, 'BLIT', 'vs_blit', 'fs_blit', [W.fbgl.tex], [{ format: d.format }]);
      if (!pipe) return false;
      var o = W.uslot();
      if (o < 0) return false;
      // uniform vec4u = the record's bytes 12..27 (sx, sy, sw, sh) as they are
      W.uu.set(U.subarray((p + B.sx) >>> 2, (p + B.sx + 16) >>> 2), o >> 2);
      var fl = U[(p + B.filter) >>> 2] === 1 ? 1 : 0;
      var pass = enc.beginRenderPass(WGPU_GT.tw({ colorAttachments: [{ view: W.att(d), loadOp: 'load', storeOp: 'store' }] }, 1));
      pass.setPipeline(pipe);
      pass.setBindGroup(0, W.fbgTex(s, 'b' + fl, s.view, W.samp[fl], W.white2d), [o]);
      pass.setViewport(dx, dy, dw, dh, 0, 1);
      pass.setScissorRect(rx, ry, rw, rh);
      pass.draw(3);
      pass.end();
      return true;
    },

    zconv(enc, p, U) {
      var W = WGPU, Z = W.ZC;
      var s = W.tex.get(U[(p + Z.src) >>> 2]), d = W.tex.get(U[(p + Z.dst) >>> 2]);
      if (!s || !d || !s.depth || d.depth) {
        W.once('zcx', 'ZCONV: unknown textures or wrong kinds ' + U[(p + Z.src) >>> 2] + ' -> ' + U[(p + Z.dst) >>> 2]);
        return false;
      }
      var w = Math.min(U[(p + Z.w) >>> 2], d.w, s.w), h = Math.min(U[(p + Z.h) >>> 2], d.h, s.h);
      if (!w || !h) return false;
      var pipe = W.fpipe('zconv ' + d.format, 'ZCONV', 'vs_full', 'fs_zconv', [W.fbgl.z], [{ format: d.format }]);
      if (!pipe) return false;
      var o = W.uslot();
      if (o < 0) return false;
      var b = o >> 2;
      W.uu[b] = U[(p + Z.zmax) >>> 2]; W.uu[b + 1] = U[(p + Z.flags) >>> 2]; W.uu[b + 2] = w; W.uu[b + 3] = h;
      if (!s.zview) s.zview = s.gpu.createView({ dimension: '2d', aspect: 'depth-only', baseMipLevel: 0,
                                                  mipLevelCount: 1, baseArrayLayer: 0, arrayLayerCount: 1 });
      var bg = s.bg.z || (s.bg.z = W.dev.createBindGroup({ layout: W.fbgl.z, entries: [
        { binding: 0, resource: s.zview }, { binding: 1, resource: { buffer: W.ubuf, size: W.USLOT } }] }));
      var pass = enc.beginRenderPass(WGPU_GT.tw({ colorAttachments: [{ view: W.att(d), loadOp: 'load', storeOp: 'store' }] }, 1));
      pass.setPipeline(pipe);
      pass.setBindGroup(0, bg, [o]);
      pass.setViewport(0, 0, w, h, 0, 1);
      pass.setScissorRect(0, 0, w, h);
      pass.draw(3);
      pass.end();
      return true;
    },

    // PRESENT: the steady fill's step (if asked), then src into the canvas
    present(enc, p, U, F, size) {
      var W = WGPU, R = W.PR;
      var t = W.tex.get(U[(p + R.src_tex) >>> 2]);
      if (!t) { W.once('prt', 'PRESENT: unknown source texture ' + U[(p + R.src_tex) >>> 2]); return 0; }
      var ow = U[(p + R.out_w) >>> 2], oh = U[(p + R.out_h) >>> 2], cv = W.canvas;
      if (ow && oh && (cv.width !== ow || cv.height !== oh)) {
        cv.width = ow; cv.height = oh;
        var sp = W.canvasInfo && W.canvasInfo.canvasSharedPtr;   // what emscripten_get_canvas_element_size reads
        if (sp) { U[sp >>> 2] = ow; U[(sp + 4) >>> 2] = oh; }
        U[W.ci('canvas_w')] = ow; U[W.ci('canvas_h')] = oh;
      }
      var pipe = W.fpipe('present ' + W.canvasFmt, 'PRESENT', 'vs_full', 'fs_present', [W.fbgl.tex],
                         [{ format: W.canvasFmt }]);
      var o = pipe ? W.uslot() : -1;
      if (o < 0) return 0;
      // uniform P = the record's bytes 8..67 as they are (8..39 for M1's 40-byte record; the rest 0)
      var words = (Math.min(size, R.SIZE) - R.src_x) >>> 2;
      W.uu.set(U.subarray((p + R.src_x) >>> 2, ((p + R.src_x) >>> 2) + words), o >> 2);
      var flags = U[(p + R.flags) >>> 2], fl = U[(p + R.filter) >>> 2] === 1 ? 1 : 0;
      var passes = 0, fs = W.fillState;
      if ((flags & W.PF_STEADY) && !(flags & W.PF_FREEZE)) {   // frozen: the counter stands
        var sp2 = W.fpipe('steady', 'PRESENT', 'vs_full', 'fs_steady', [W.fbgl.tex], [{ format: 'rgba8unorm' }]);
        if (sp2) {
          var nxt = fs.cur ^ 1;
          var sp3 = enc.beginRenderPass(WGPU_GT.tw({ colorAttachments: [{ view: fs.view[nxt], loadOp: 'clear',
            storeOp: 'store', clearValue: { r: 0, g: 0, b: 0, a: 0 } }] }, 2));
          sp3.setPipeline(sp2);
          sp3.setBindGroup(0, W.fbgTex(t, 'p' + fl + fs.cur, t.view, W.samp[fl], fs.view[fs.cur]), [o]);
          sp3.draw(3);
          sp3.end();
          fs.cur = nxt;
          passes++;
        }
      }
      var bg = W.fbgTex(t, 'p' + fl + fs.cur, t.view, W.samp[fl], fs.view[fs.cur]);
      var pass = enc.beginRenderPass(WGPU_GT.tw({ colorAttachments: [{ view: W.ctx.getCurrentTexture().createView(),
        loadOp: 'clear', storeOp: 'store', clearValue: { r: 0, g: 0, b: 0, a: 1 } }] }, 2));
      pass.setPipeline(pipe);
      pass.setBindGroup(0, bg, [o]);
      pass.draw(3);
      pass.end();
      return passes + 1;
    },

    frame(fp) {
      var W = WGPU, Fo = W.F, t0 = performance.now(), dev = W.dev;
      if (!dev) { if (W.said) W.once('nodev', 'wgpu_js_frame without a device: frame dropped'); return; }
      fp >>>= 0;
      var U = W.u32(), F = new Float32Array(U.buffer);
      if (U[(fp + Fo.magic) >>> 2] !== W.MAGIC || U[(fp + Fo.version) >>> 2] !== W.VERSION
          || U[(fp + Fo.size) >>> 2] !== Fo.SIZE) {
        W.once('fmagic', 'WgFrame magic/version/size mismatch: frames dropped');
        return;
      }
      var flags = U[(fp + Fo.flags) >>> 2];
      var p = U[(fp + Fo.cmd_ptr) >>> 2], end = p + U[(fp + Fo.cmd_bytes) >>> 2];
      var count = U[(fp + Fo.cmd_count) >>> 2];
      var n = 0, passes = 0, presented = 0, draws = 0, pass = null, s = null;

      // the arena first: one writeBuffer, before any command of this submit
      var ap = U[(fp + Fo.arena_ptr) >>> 2], ab = U[(fp + Fo.arena_bytes) >>> 2];
      W.arenaOk = false;
      if (ap && ab) {
        if (!W.arenaGrow(ab)) W.once('abig', 'arena of ' + ab + ' bytes is over the device limit: draws dropped');
        // a view of just the arena: Firefox refuses a source view over 2 GB (the whole heap)
        else { dev.queue.writeBuffer(W.arena, 0, U.subarray(ap >>> 2, (ap >>> 2) + ((ab + 3) >>> 2))); W.arenaOk = true; }
        if (W.ct && W.arenaOk) W.ct.wbArena += ab;
      }
      W.uused = 0;

      // Safari throws InvalidStateError here once its device is gone, before (or without) dev.lost
      // resolving: uncaught, it killed the render thread (6 Macs, 2026-11-11)
      var enc;
      try { enc = dev.createCommandEncoder(); }
      catch (e) { W.lost(dev, 'createCommandEncoder failed: ' + (e && e.message || e)); return; }
      WGPU_GT.begin(dev);   // M4 GPU timing
      try {
        while (p + 4 <= end) {
          var w = U[p >>> 2], op = w & 0xFFFF, size = w >>> 16;
          if (size < 4 || (size & 3) || p + size > end) {
            W.once('csize', 'bad record size ' + size + ' (op ' + op + ') at +' + (p - U[(fp + Fo.cmd_ptr) >>> 2]) + ': rest of the list dropped');
            break;
          }
          n++;
          // where an op may sit (wgpu_proto.h): pass ops 0x04-0x0F and 0x30-0x3F
          var passOp = (op >= 0x04 && op <= 0x0F) || (op >= 0x30 && op <= 0x3F);
          if (passOp && !pass) { W.once('po' + op, 'op 0x' + op.toString(16) + ' outside a pass: dropped'); p += size; continue; }
          if (op >= 0x40 && op <= 0x4F && pass) { W.once('eo' + op, 'op 0x' + op.toString(16) + ' inside a pass: dropped'); p += size; continue; }
          var need = W.SIZES[op];
          if (need && size < need) { W.once('sz' + op, 'op 0x' + op.toString(16) + ' record too short (' + size + ' < ' + need + '): dropped'); p += size; continue; }
          switch (op) {
            case W.OP_BEGIN_PASS:
              if (pass) { W.once('nest', 'BEGIN_PASS inside a pass: the open pass ended'); pass.end(); }
              pass = W.beginPass(enc, p, U, F);
              s = pass ? W.ps : null;
              if (pass) passes++;
              break;
            case W.OP_END_PASS:
              if (pass) { pass.end(); pass = null; s = null; W.ps = null; }
              break;
            case W.OP_PRESENT:
              if (pass) { W.once('prp', 'PRESENT inside a pass: the pass ended'); pass.end(); pass = null; s = null; W.ps = null; }
              var np = W.present(enc, p, U, F, size);
              if (np) { passes += np; presented++; }
              break;
            case W.OP_VIEWPORT: W.viewport(p, F); break;
            case W.OP_SCISSOR: W.scissor(p, U); break;
            case W.OP_BLEND_CONST:
              s.bc = { r: F[(p + W.BC.r) >>> 2], g: F[(p + W.BC.g) >>> 2],
                       b: F[(p + W.BC.b) >>> 2], a: F[(p + W.BC.a) >>> 2] };
              pass.setBlendConstant(s.bc);
              if (W.ct) W.ct.bc++;
              break;
            case W.OP_STENCIL_REF:
              if (W.ct) W.ct.sr++;
              s.ref = U[(p + W.SR.ref) >>> 2] & 255;
              pass.setStencilReference(s.ref);
              break;
            case W.OP_CLEAR_RECT: W.clearRect(p, U, F); break;
            case W.OP_TEX_CREATE: W.texCreate(p, U); break;
            case W.OP_TEX_FREE: W.texFree(p, U); break;
            case W.OP_TEX_UPLOAD: W.texUpload(p, U); break;
            case W.OP_SAMPLER: W.sampler(p, U, F); break;
            case W.OP_SHADER: W.shader(p, U); break;
            case W.OP_PIPE_CREATE: W.pipeCreate(p, U); break;
            case W.OP_SET_PIPE: W.setPipe(p, U); break;
            case W.OP_SET_TEX: W.setTex(p, U); break;
            case W.OP_DRAW:
              var e = s.pipe;
              if (!e || e.bad || !W.arenaOk) break;
              if (!s.bg1) { s.bg1 = W.bg1White(e); pass.setBindGroup(1, s.bg1); if (W.ct) W.ct.stWhite++; }
              pass.drawIndexed(U[(p + W.DR.index_count) >>> 2], 1, U[(p + W.DR.first_index) >>> 2], 0,
                               U[(p + W.DR.record) >>> 2]);
              draws++;
              break;
            case W.OP_COPY_TEX: W.copyTex(enc, p, U); break;
            case W.OP_BLIT: if (W.blit(enc, p, U)) passes++; break;
            case W.OP_ZCONV: if (W.zconv(enc, p, U)) passes++; break;
            default:   // NOP and ops this encoder does not know: skipped by size
              break;
          }
          p += size;
        }
      } catch (e) {
        W.once('x:' + (e && e.message), 'frame encode failed: ' + (e && e.message || e));
      }
      if (pass) { try { pass.end(); } catch (e) { /* the encoder is invalid already */ } }
      W.ps = null;
      // pipelines no draw of this list used (the prewarm): compiled off the frame
      for (var j = 0; j < W.pend.length; j++) if (!W.pend[j].gpu && !W.pend[j].bad && W.pend[j].desc) W.pipeAsync(W.pend[j]);
      W.pend.length = 0;
      if (n !== count) W.once('ccount', 'command count: list has ' + n + ', WgFrame says ' + count);
      if (W.uused) dev.queue.writeBuffer(W.ubuf, 0, W.ustage, 0, W.uused * W.USLOT);
      if (W.ct) W.ct.wbUni += W.uused * W.USLOT;
      WGPU_GT.end(enc);
      try { dev.queue.submit([enc.finish()]); }
      catch (e) { W.once('sub:' + (e && e.message), 'submit failed: ' + (e && e.message || e)); }
      WGPU_GT.after();
      for (var i = 0; i < W.dead.length; i++) W.dead[i].destroy();
      W.dead.length = 0;

      // pacing: frame ends only (not a partial flush); done bumps whatever happened to the submit
      U = W.u32();
      var I = new Int32Array(U.buffer);
      if (!(flags & W.FF_PARTIAL)) {
        Atomics.add(I, W.ci('frames_submitted'), 1);
        var done = () => {
          var I2 = new Int32Array(wasmMemory.buffer), j = W.ci('frames_done');
          Atomics.add(I2, j, 1);
          Atomics.notify(I2, j);
        };
        dev.queue.onSubmittedWorkDone().then(done, done);
      }
      var tEnd = performance.now(), us = Math.round((tEnd - t0) * 1000);
      // '[WGPU] slow frame': over 100 ms since the last frame end with pipelines made in it
      if (!(flags & W.FF_PARTIAL)) {
        if (W.lastT && tEnd - W.lastT > 100 && W.fPipes)
          W.log('slow frame ' + Math.round(tEnd - W.lastT) + ' ms, ' + W.fPipes + ' pipelines made'
                + (W.fAsync ? ' (' + W.fAsync + ' asynchronously)' : ''));
        W.lastT = tEnd; W.fPipes = 0; W.fAsync = 0;
      }
      U[W.ci('frames_presented')] += presented;
      U[W.ci('st_cmds')] += n;
      U[W.ci('st_passes')] += passes;
      U[W.ci('st_draws')] += draws;
      U[W.ci('st_enc_us')] += us;
      U[W.ci('st_tex_live')] = W.tex.size;

      if (W.stats) {
        var st = W.win;
        if (!(flags & W.FF_PARTIAL)) st.frames++;
        st.presented += presented; st.passes += passes; st.cmds += n; st.encUs += us;
        st.trUs += U[(fp + Fo.translate_us) >>> 2]; st.draws += draws; st.arena += ab;
        var now = performance.now();
        if (now - st.t >= 1000) {
          var fr = Math.max(st.frames, 1);
          W.log(((now - st.t) / 1000).toFixed(1) + ' s: ' + st.frames + ' frames, ' + st.presented
                + ' presented, ' + st.passes + ' passes, ' + st.draws + ' draws, ' + st.cmds + ' cmds, '
                + (st.pipes * 1000 / (now - st.t)).toFixed(1) + ' pipelines made/s ('
                + W.pipeByKey.size + ' cached), arena '
                + (st.arena / fr / 1024).toFixed(0) + ' KB/frame (buffer ' + (W.arenaSize >> 10) + ' KB), '
                + W.tex.size + ' textures live, ' + W.bg1.size + ' bind groups, translate '
                + (st.trUs / fr / 1000).toFixed(2) + ' ms, encode ' + (st.encUs / fr / 1000).toFixed(2)
                + ' ms/frame, in flight ' + (I[W.ci('frames_submitted')] - Atomics.load(I, W.ci('frames_done')))
                + ', errors ' + U[W.ci('st_errors')]);
          W.win = W.newWin(now);
        }
        var c = W.ct;
        c.draws += draws; c.passes += passes;
        if (!(flags & W.FF_PARTIAL) && ++c.f >= W.CT_FRAMES) {
          var k = 1 / c.f, q = (x) => (x * k).toFixed(1), kb = (x) => (x * k / 1024).toFixed(0);
          // issued = records that reached the GPU call; same = records that repeated the pass's state
          W.log('state calls per frame (' + c.f + ' frames): draws ' + q(c.draws) + ', passes ' + q(c.passes)
                + ' (draw passes ' + q(c.dpass) + ': setBindGroup(0) + setIndexBuffer each), setPipeline '
                + q(c.sp - c.spSame) + ' (SET_PIPE ' + q(c.sp) + ', same ' + q(c.spSame) + '), setBindGroup(1) '
                + q(c.st - c.stSame + c.stWhite) + ' (SET_TEX ' + q(c.st) + ', same ' + q(c.stSame) + ', white '
                + q(c.stWhite) + '), bind groups made ' + q(c.bgNew) + ', setViewport ' + q(c.vp) + ' (same '
                + q(c.vpSame) + '), setScissorRect ' + q(c.sc) + ' (same ' + q(c.scSame) + '), blend const '
                + q(c.bc) + ', stencil ref ' + q(c.sr) + ', clear quads ' + q(c.clr)
                + ' (each setPipeline/Viewport/ScissorRect x2), setVertexBuffer 0 (vertex pulling), writeBuffer '
                + kb(c.wbArena + c.wbUni) + ' KB (arena ' + kb(c.wbArena) + ', uniforms ' + kb(c.wbUni)
                + '), writeTexture ' + kb(c.wtex) + ' KB');
          W.ct = W.newCt();
        }
      }
    },
  },

  wgpu_js_init__deps: ['$WGPU'],
  wgpu_js_init: (ctl) => WGPU.init(ctl),
  wgpu_js_frame__deps: ['$WGPU'],
  wgpu_js_frame: (frame) => WGPU.frame(frame),
});
