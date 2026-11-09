/**
 * Fixed WGSL for the WebGPU renderer (ADR 0011, docs/webgpu-plan.md), used by
 * web/wgpu.js. Linked as a --js-library next to it (CMakeLists.txt): it only
 * defines the library object $WGPU_FIXED, which web/wgpu.js lists in its
 * __deps, so the two files may come in either order on the command line.
 *
 * M2: PRESENT (with the 4:3 fill, per frame or steady), CLEAR (CLEAR_RECT's
 * quad), BLIT (BLIT's scaled quad). M4: ZCONV (depth -> RGBA8). M5 adds
 * FXAA/CAS in PRESENT.
 * Beside each: its bindings, its uniform block's byte layout and the
 * pipeline state wgpu.js gives it. Rows are top-first everywhere (texture
 * row 0 is the top of the picture; viewport y grows downwards).
 *
 * The fullscreen triangle and the copy shader follow nfsu2-web's
 * web/src/gpu/wgsl.ts (COPY) by th0mk (MIT). The fill tests are nv2a_gl.c's
 * present_fxaa bars() and fill_steady_pass.
 */
addToLibrary({
  $WGPU_FIXED: {
    // PRESENT: entry points vs_full + fs_present (the canvas pass) and
    // vs_full + fs_steady (the steady fill's counter pass). Both use the
    // same group 0, so one explicit bind group layout serves both:
    //   binding 0  texture_2d<f32>  the source surface (WgPresent.src_tex)
    //   binding 1  sampler          filtering; nearest or linear by WgPresent.filter
    //   binding 2  uniform P (64 bytes) = the PRESENT record's bytes 8..67
    //              copied as they are (writeBuffer(buf, 0, HEAPU8.buffer,
    //              p + 8, 60); the last 4 bytes stay 0). A 40-byte record
    //              (no fill fields): bytes 8..39, the rest 0 = no fill.
    //              offset 0 src_x, 4 src_y, 8 src_w, 12 src_h, 16 out_w,
    //              20 out_h, 24 filter, 28 flags (WG_PF_*), 32 crop_x,
    //              36 crop_y, 40 crop_w, 44 crop_h, 48 band_l, 52 band_r,
    //              56 sharp (f32, unused until M5), 60 zero. All u32 but
    //              sharp; rectangles in source texels.
    //   binding 3  texture_2d<f32>  the steady state: 1x1 'rgba8unorm',
    //              always bound (state[cur]).
    // The steady state: two 1x1 rgba8unorm textures (RENDER_ATTACHMENT |
    // TEXTURE_BINDING), both written (255,255,255,255) at creation (start
    // filled), and an index cur. Per PRESENT, before the canvas pass:
    //   if (flags & STEADY 2) && !(flags & FREEZE 4): a render pass on
    //   state[cur ^ 1] (loadOp 'clear', storeOp 'store'), pipeline fs_steady
    //   (target 'rgba8unorm', no blend), group 0 with binding 3 = state[cur],
    //   draw(3); then cur ^= 1.
    // Then the canvas pass (fs_present, target the canvas format, no blend,
    // loadOp 'clear' black), group 0 with binding 3 = state[cur], draw(3).
    // Canvas size: WgPresent.out_w x out_h (C picks it; resize when it
    // differs).
    PRESENT: `
struct P {
  src: vec4u,
  out_size: vec2u,
  filt: u32,
  flags: u32,
  crop: vec4u,
  band: vec2u,
  sharp: f32,
  pad: u32,
};
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var smp: sampler;
@group(0) @binding(2) var<uniform> u: P;
@group(0) @binding(3) var state: texture_2d<f32>;
struct VOut { @builtin(position) pos: vec4f, @location(0) t: vec2f }
@vertex fn vs_full(@builtin(vertex_index) i: u32) -> VOut {
  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
  var o: VOut;
  o.pos = vec4f(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
  o.t = p;
  return o;
}
fn dims() -> vec2f { return vec2f(textureDimensions(src)); }
// a texel rectangle as uv: x, y, w, h
fn ruv(r: vec4u) -> vec4f { let d = dims(); return vec4f(vec2f(r.xy) / d, vec2f(r.zw) / d); }
fn lit(uv: vec2f) -> bool {
  let c = textureSampleLevel(src, smp, uv, 0.0).rgb;
  return max(c.r, max(c.g, c.b)) > 0.008;
}
// Both side bands black from top to bottom (8 rows): a 4:3 screen the title
// pillarboxed itself. Same texels for every pixel: cache hits.
fn bars() -> bool {
  let d = dims();
  let f = ruv(u.src);
  let bl = (f32(u.band.x) + 0.5) / d.x;
  let br = (f32(u.band.y) + 0.5) / d.x;
  var dark = true;
  for (var k = 0; k < 8; k++) {
    let v = f.y + f.w * (f32(k) + 0.5) / 8.0;
    if (lit(vec2f(bl, v)) || lit(vec2f(br, v))) { dark = false; }
  }
  return dark;
}
// Steady test: at most a third of a 3 x 12 grid in each band lit. A loading
// screen's circle crossing into a band lights a few points; a picture that
// really spans the width, most of them.
fn bars_grid() -> bool {
  let d = dims();
  let f = ruv(u.src);
  let bl = (f32(u.band.x) + 0.5) / d.x;
  let br = (f32(u.band.y) + 0.5) / d.x;
  let bw = f32(u.crop.x - u.src.x) / d.x;
  var n = 0;
  for (var k = 0; k < 12; k++) {
    let v = f.y + f.w * (f32(k) + 0.5) / 12.0;
    for (var c = -1; c <= 1; c++) {
      let du = f32(c) * 0.3 * bw;
      if (lit(vec2f(bl + du, v))) { n++; }
      if (lit(vec2f(br + du, v))) { n++; }
    }
  }
  return n <= 24;
}
// The counter: a step towards 1 for black bands, towards 0 otherwise; the
// present fills while it is >= 0.5, so a change holds 4-5 frames first.
@fragment fn fs_steady(i: VOut) -> @location(0) vec4f {
  let s = textureLoad(state, vec2i(0, 0), 0).r + select(-0.125, 0.125, bars_grid());
  return vec4f(clamp(s, 0.0, 1.0));
}
@fragment fn fs_present(i: VOut) -> @location(0) vec4f {
  let d = dims();
  var r = ruv(u.src);
  if ((u.flags & 1u) != 0u) {
    var filled: bool;
    if ((u.flags & 2u) != 0u) { filled = textureLoad(state, vec2i(0, 0), 0).r >= 0.5; }
    else { filled = bars(); }
    if (filled) { r = ruv(u.crop); }
  }
  // stay inside the drawn texels (linear filtering at the edges)
  let lo = vec2f(u.src.xy) / d + 0.5 / d;
  let hi = vec2f(u.src.xy + u.src.zw) / d - 0.5 / d;
  let uv = clamp(r.xy + i.t * r.zw, lo, hi);
  return vec4f(textureSampleLevel(src, smp, uv, 0.0).rgb, 1.0);
}`,

    // CLEAR: CLEAR_RECT inside the open pass, entry points vs_clear +
    // fs_clear. No bindings, no uniforms: everything is dynamic pass state.
    //   pipeline (cache by flags & 7, colour mask, pass colour/depth format):
    //     targets [{ format: pass colour format, writeMask: (flags & COLOR 1)
    //       ? (flags >> 8) & 15 : 0, blend: { color: { operation 'add',
    //       srcFactor 'constant', dstFactor 'zero' }, alpha: same } }];
    //     if the pass has a depth attachment: depthStencil { format,
    //       depthWriteEnabled: !!(flags & DEPTH 2), depthCompare 'always',
    //       stencilFront = stencilBack = { compare 'always', failOp 'keep',
    //       depthFailOp 'keep', passOp (flags & STENCIL 4) ? 'replace' : 'keep' },
    //       stencilReadMask 0xFF, stencilWriteMask 0xFF };
    //     primitive triangle-list, no cull.
    //   then: setViewport(x, y, width, height, depth, depth),
    //     setScissorRect(x, y, width, height), setBlendConstant(color r,g,b,a),
    //     setStencilReference(stencil), draw(3); then put back the pass state
    //     (pipeline, group 1, viewport, scissor, blend constant, stencil ref).
    //   The colour is the blend constant (fragment 1 x constant), the depth
    //   the viewport's min = max depth, the stencil the reference.
    CLEAR: `
@vertex fn vs_clear(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
  return vec4f(p * 2.0 - 1.0, 0.5, 1.0);
}
@fragment fn fs_clear() -> @location(0) vec4f {
  return vec4f(1.0);
}`,

    // BLIT: its own render pass on dst (colour only, loadOp 'load', storeOp
    // 'store'), entry points vs_blit + fs_blit, target the dst format
    // ('bgra8unorm'), no blend.
    //   binding 0  texture_2d<f32>  src
    //   binding 1  sampler          nearest or linear by WgBlit.filter
    //   binding 2  uniform vec4u (16 bytes) = the BLIT record's bytes 12..27
    //              as they are: sx, sy, sw, sh (source texels). Each BLIT in
    //              a submit needs its own 16 bytes (queue writes all land
    //              before the first pass): a buffer per record or a ring with
    //              dynamic offsets. BLITs are rare (only when scales differ).
    //   setViewport(dx, dy, dw, dh, 0, 1), draw(3).
    BLIT: `
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var smp: sampler;
@group(0) @binding(2) var<uniform> r: vec4u;
struct VOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f }
@vertex fn vs_blit(@builtin(vertex_index) i: u32) -> VOut {
  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
  let d = vec2f(textureDimensions(src));
  var o: VOut;
  o.pos = vec4f(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
  o.uv = (vec2f(r.xy) + p * vec2f(r.zw)) / d;
  return o;
}
@fragment fn fs_blit(i: VOut) -> @location(0) vec4f {
  return textureSampleLevel(src, smp, i.uv, 0.0);
}`,

    // ZCONV: a depth buffer packed into RGBA8 as the title reads it back
    // (nv2a_gl.c zconv_init: the D24S8 dword seen as A8R8G8B8 or A8B8G8R8).
    // Its own render pass on dst (colour only, loadOp 'load', storeOp
    // 'store'), entry points vs_full + fs_zconv, target the dst format
    // ('bgra8unorm'), no blend.
    //   binding 0  texture_depth_2d  src, a depth-only view (textureLoad)
    //   binding 1  uniform vec4u (16 bytes): zmax, flags (WG_ZC_*), width,
    //              height (the record's zmax, flags; width/height clamped)
    //   setViewport(0, 0, w, h, 0, 1), scissor the same, draw(3).
    // v = depth * zmax: hi = v >> 16, mid = (v >> 8) & 255, lo = v & 255,
    // stencil 0. A8R8G8B8 texel (r, g, b, a) = (mid, lo, 0, hi); with
    // WG_ZC_SWAP_RB (A8B8G8R8) = (0, lo, mid, hi).
    ZCONV: `
@group(0) @binding(0) var dep: texture_depth_2d;
@group(0) @binding(1) var<uniform> u: vec4u;
@vertex fn vs_full(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
  let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
  return vec4f(p * 2.0 - 1.0, 0.0, 1.0);
}
@fragment fn fs_zconv(@builtin(position) pos: vec4f) -> @location(0) vec4f {
  let c = min(vec2u(pos.xy), max(u.zw, vec2u(1u)) - 1u);
  let z = textureLoad(dep, vec2i(c), 0);
  let v = u32(clamp(z, 0.0, 1.0) * f32(u.x) + 0.5);
  let hi = f32((v >> 16u) & 255u) / 255.0;
  let mid = f32((v >> 8u) & 255u) / 255.0;
  let lo = f32(v & 255u) / 255.0;
  if ((u.y & 1u) != 0u) { return vec4f(0.0, lo, mid, hi); }
  return vec4f(mid, lo, 0.0, hi);
}`,
  },
});
