# 11. A WebGPU renderer for the browser, WebGL 2 kept as the fallback

Date: 2026-11-08. Status: proposed (design only; plan in [docs/webgpu-plan.md](../webgpu-plan.md)).
Changes ADR 0010 item 1, which chose WebGL 2 and ruled WebGPU out as "a second renderer to keep in step".

## Context

- Measured on the owner's PC (Chrome 157, Windows, RTX 2060, ANGLE on D3D11), Remix, Cruise: 15 fps. The GL thread
  takes 50-65 ms a frame for 700-900 draws, 500-900 WebGL draw calls.
- CPU-side cuts in `nv2a_gl` (vertex pool, VAO cache, texture memo, state cache in `gl_es.c`) moved time between
  buckets but not the total. Chrome's GPU process (ANGLE D3D11, passthrough decoder) is saturated by the call count.
  The web path has no sync points (audited). WebGL 2 cannot do better: ANGLE validates every state change and draw
  and translates it to D3D11.
- The owner's `chrome://gpu`: WebGPU available (Dawn on D3D12), Vulkan not supported, WebGL only through D3D11.
- nfsu2-web (MIT, th0mk) runs NFSU2 (D3D9 HLE) at 60 fps on WebGPU. The game thread records draws into shared memory
  and a TypeScript encoder submits them, with one `writeBuffer` per arena per frame, a static vertex/index pool, and
  pipeline and bind-group caches.
- mc3-web's back end sees the NV2A push buffer, not D3D: a method shadow (`regs`), vertex programs and register
  combiners (`Nv2aRawBatch`, `xboxrecomp/src/kernel/nv2a_backend.h`). The game's logic runs at 30 fps (`MC3_60FPS=1`
  is opt-in).
- Two renderers already use that interface: `nv2a_gl` (the GL thread runs `q_draw` records through `q_gl_run` into
  `gl_draw_raw`) and `nv2a_vk` (Vulkan: images, pipelines keyed by state, a frame ring, per-instance constant sets;
  older, with no blits, depth-as-colour or 4:3 fill).

  The shader emitters `gl_vsh.c`/`gl_psh.c` already speak two dialects (GLSL 330 and Vulkan GLSL).

## Decision

1. **Where WebGPU runs: a JS encoder on the render worker, fed by a command list that C writes.**
   - A new back end, `xboxrecomp/src/nv2a_wgpu/`, keeps the GL renderer's thread split: the executor's callbacks
     queue records as `q_draw` does.
   - A render pthread owns the OffscreenCanvas (transferred as in `q_start`). It does the NV2A-level work in C:
     surface, texture and pipeline caches, decoding, WGSL generation, arena writes. It writes a flat command list
     (`wgpu_proto.h`).
   - At each flip it makes **one** call into `web/wgpu.js`. That JS, in the same worker, issues the WebGPU calls and
     submits. The worker then returns to its event loop (`emscripten_set_main_loop` turns, as now), which presents
     the canvas and resolves WebGPU promises.
   - Not emdawnwebgpu / `-sUSE_WEBGPU`: a wasm->JS crossing per WebGPU call (5-8 per draw), an asynchronous device
     that needs ASYNCIFY or JSPI, and a bigger binary.
   - Not the page's main thread: the worker already returns to its event loop, and the page also serves input,
     audio and the UI. The list is in shared memory and carries resource creation too, so moving the encoder there
     later only changes who calls it.
2. **Shaders: WGSL emitters in C, new files that follow `gl_vsh.c`/`gl_psh.c` function by function** (`wgsl_vsh.c`,
   `wgsl_psh.c`).
   - They keep the same field decode, `Nv2aPshKey`, parallel MAC/ILU issue, `a0`-relative constants and parity with
     the CPU interpreter.
   - Not a third dialect flag in the GLSL emitters: WGSL has no swizzled assignment (masked writes become `select`),
     no implicit int/float conversion, no `?:`, and other declarations.
   - Rejected: GLSL -> WGSL at run time through naga/Tint in wasm (several MB, latency per shader, GLSL front-end
     gaps); SPIR-V (Chrome takes WGSL only); an uber-shader that interprets the microcode (a branchy loop per
     vertex; kept in reserve against compile hitches).
   - A host test emits WGSL for a recorded program list and validates it offline (naga or tint CLI).
3. **Pipeline key: only what WebGPU bakes, about 100 bits hashed to an id.**
   - The key holds: vertex-shader id (program hash, or the fixed/pre-transformed kind); pixel-shader id
     (`Nv2aPshKey` hash); topology class (point, line or triangle list); cull mode and front face; depth function,
     depth write and whether a zeta exists; stencil enable, function, masks and three ops; blend enable, source,
     destination and equation; colour mask and target format; the four stages' texture dimension (2D or cube).
   - Outside the key: blend colour and stencil reference (dynamic), viewport and scissor (dynamic), and alpha test,
     fog and combiner factors (per-draw data, as `nv2a_gl`'s uniforms).
   - Vertex layout is **not** in the key. Shaders pull attributes from a storage buffer (nfsu2-web's method). That
     covers every NV2A type (D3DCOLOR, S1, S32K, float, three components) without WebGPU's vertex-format gaps or its
     8-buffer limit.
   - Fans, polygons, quads, strips and line loops become lists on the CPU.
   - `unclippedDepth` (`depth-clip-control`) brings back the NV2A depth clamp that WebGL lost.
   - Keys seen go to an OPFS list and are prebuilt with `createRenderPipelineAsync` at boot, as `nv2a_gl`'s program
     list is.
4. **Data.**
   - **Frame arena.** A wasm-side arena holds the frame's vertices, 32-bit indices, transform constants and
     per-draw records:
     - constants: 192 vec4, written only when `vp_const_gen` changes and the values differ;
     - each record: the `GlUni` fields, the vertex-fetch descriptor and the attribute constants.
   - **Upload.** One `queue.writeBuffer` per frame, or per mid-frame flush when the arena fills. The arena is bound
     once per pass as read-only storage. `firstInstance` -> `instance_index` picks the draw's record (flat to the
     fragment stage).
   - **Per draw:** `setPipeline` and `setBindGroup(1)` when they change, then one `drawIndexed`.
   - **Vertex pool (M5).** `nv2a_gl`'s vpool hash keeps repeated meshes in a persistent buffer, filled by
     `copyBufferToBuffer`.
   - **Textures.** The toolkit decoders turn linear, swizzled and palette textures into BGRA8, uploaded as
     `bgra8unorm` with no CPU swizzle.
     - DXT stays BC1-3 when `texture-compression-bc` is present.
     - 16-bit formats have no WebGPU equivalent: expanded to BGRA8.
     - Cache, hash and texfast rules as `nv2a_gl`.
     - A resource written twice in one frame gets a new id: queue writes land before the frame's commands.
   - **Surfaces.** One `bgra8unorm` texture per colour address (`surf_get` rules: grow, never shrink, scale, square
     pixels). One `depth24plus-stencil8` per (zeta, size), as `depth_get`.
     - A pass starts on a surface or zeta change.
     - A whole-surface clear is `loadOp: clear`; a partial clear is a quad.
     - A draw that samples its own target copies the target first (WebGPU forbids it in-pass).
     - NV062/NV09F blits are `copyTextureToTexture` at equal scale, a quad otherwise.
   - **Depth as colour.** `RECOMP_GL_DEPTH_TEX` is a pass that reads the depth aspect (`textureLoad`) and packs
     `z * 0xFFFFFF` into RGBA8 as `zconv` does.
   - **Movies.** On the web the lifted Bink writes a linear A8R8G8B8 texture: the ordinary texture path.
5. **Pacing and present.**
   - At a flip, the present pass draws the last surface into `getCurrentTexture()`, with the 4:3 fill decided on the
     GPU as `present_fxaa` does. Then submit.
   - At most 2 frames in flight, counted down by `onSubmittedWorkDone`. The executor stays at most
     `RECOMP_GL_QUEUE_FRAMES` flips ahead, as with `q_flip`.
   - The game paces itself (flip interval 2, `nv2a_pb_flip_retired`). The worker uses timer turns, not rAF.
   - `MC3_60FPS=1` presents every flip.
6. **WebGL 2 stays as the fallback: `RECOMP_GPU=webgpu|webgl`.**
   - Both renderers are linked into the wasm; `src/main.c` picks one at boot (web only).
   - The page probes `navigator.gpu.requestAdapter()` before the game starts and sets `RECOMP_GPU`. Settings or
     `?RECOMP_GPU=` can force either.
   - It must be decided first: an OffscreenCanvas that has had a `webgpu` context cannot get `webgl2`.
   - A device lost later reloads the page with `RECOMP_GPU=webgl`.
   - Game-facing `nv2a_gl_*` calls (fill hold and freeze, movie hooks, frame count, stats) forward through one small
     `__EMSCRIPTEN__` hunk in `nv2a_gl.c`.
   - Native builds do not change.

## Consequences

- One more renderer to keep in step. It follows `nv2a_gl`'s rules and `nv2a_vk`'s structure by function name.
  Where they disagree, `nv2a_gl` is the reference.
- Expected gains: the GPU process records a draw through Dawn on D3D12 instead of ANGLE's D3D11 translation, and JS
  crossings fall from about 5 per draw to 1 per frame. Measured from M1 on, not promised.
- Losses for now: no mip generation by the driver (the title's own levels only); no LOD bias in samplers (it moves
  to `textureSampleBias`); frame dumps and `RECOMP_GL_WATCH` read-backs come later (M5).
- Toolkit changes stay in `xboxrecomp/` (prefix `xboxrecomp:`, ADR 0002). Everything is in new files except the
  forwarding hunk.
- Code reused from nfsu2-web (encoder.ts, index.ts, vsemu_wgsl.inc) credits th0mk in its header and in NOTICE.md.
