# WebGPU renderer: implementation plan

Decision and reasons: [ADR 0011](decisions/0011-webgpu-renderer.md). This file is the work plan: five milestones,
each one shippable and tested by the owner on :3000, with the files to add, a size estimate, the test, and how the
work splits among parallel implementer agents without two agents editing one file.

## Shape

```
executor thread (NV2A poll)          render pthread (owns the OffscreenCanvas)             same worker, JS
---------------------------          ------------------------------------------            -----------------
draw_raw/clear/blit/flip    ->ring->  C: surfaces, textures, WGSL, pipeline keys,  ->list-> web/wgpu.js: one call
(records, as q_draw)                  arena (verts, indices, consts, draw records)          per frame: writeBuffer,
                                      at flip: wgpu_encode(frame)                           passes, draws, submit
```

- **Group 0:** the frame arena as read-only storage (vertex pulling, constants, per-draw records).
- **Group 1:** 4 textures and 4 samplers, cached by key.
- **Per draw:** `setPipeline` and `setBindGroup(1)` when they change, `setScissorRect` when it changes, then
  `drawIndexed(count, 1, firstIndex, 0, recordIndex)`.
- **Switch:** `RECOMP_GPU=webgpu|webgl`. The page probes WebGPU and sets it; `?RECOMP_GPU=webgl` forces the old
  path.

## Files (all new unless marked)

| File | Role | Lines |
|---|---|---|
| `xboxrecomp/src/nv2a_wgpu/wgpu_proto.h` | The contract. Command opcodes and records shared with JS (offsets mirrored in `web/wgpu.js`), the frame header, `WgDrawRec` (per-draw record) and its WGSL struct text from the same macro | 250 |
| `xboxrecomp/src/nv2a_wgpu/nv2a_wgpu.h` | `nv2a_wgpu_install`, the forwarding entry points for the game-facing `nv2a_gl_*` calls | 40 |
| `xboxrecomp/src/nv2a_wgpu/wgpu_queue.c` | Back-end callbacks and record ring (from `q_*` in nv2a_gl.c:7232-7513), render pthread, flip back-pressure, `EM_JS` hand-off, device-ready wait in `prepare` | 450 |
| `xboxrecomp/src/nv2a_wgpu/wgpu_surf.c` | Surfaces and depth (`surf_get`/`depth_get` rules), passes, clears, NV062/NV09F blits, self-sampling copies, depth-as-colour, present command | 650 |
| `xboxrecomp/src/nv2a_wgpu/wgpu_tex.c` | Texture cache, hash, texfast, decode (toolkit decoders; BC kept), upload commands, one-write-per-frame renaming | 700 |
| `xboxrecomp/src/nv2a_wgpu/wgpu_draw.c` | Draw: primitive -> list, vertex-fetch descriptor, arena writes, constant dedupe, stage binding, pipeline key | 800 |
| `xboxrecomp/src/nv2a_wgpu/wgpu_pipe.c` | Shader and pipeline registries (ids), OPFS key list, boot prewarm | 350 |
| `xboxrecomp/src/nv2a_wgpu/wgsl_vsh.c/.h` | NV2A vertex programs, fixed-function and pre-transformed paths in WGSL; vertex-pulling prelude | 850 |
| `xboxrecomp/src/nv2a_wgpu/wgsl_psh.c/.h` | Register combiners in WGSL (`Nv2aPshKey`) | 550 |
| `xboxrecomp/src/nv2a_wgpu/wgsl_test.c`, `tools/wgsl_check.sh` | Emit WGSL for sample and recorded programs; validate with `naga` or `tint` when installed | 250 |
| `xboxrecomp/src/nv2a_wgpu/CMakeLists.txt` | `xbox_nv2a_wgpu`, Emscripten only | 20 |
| `web/wgpu.js` | Encoder (`--js-library`): device and canvas in the worker, resource tables, command walk, caches, in-flight count, device-lost reload. Ported from nfsu2-web `encoder.ts`/`index.ts` (credit th0mk) | 900 |
| `web/wgpu_fixed.js` | Fixed WGSL: present + 4:3 fill (+ FXAA/CAS in M5), clear quad, blit quad, depth -> RGBA8 | 350 |
| `CMakeLists.txt`, `xboxrecomp/CMakeLists.txt` (edit) | Link the library and the two JS files on Emscripten | 15 |
| `src/main.c` (edit) | `__EMSCRIPTEN__`: pick `nv2a_wgpu_install` or `nv2a_gl_install` from `RECOMP_GPU` (main.c:797-803) | 10 |
| `xboxrecomp/src/nv2a_gl/nv2a_gl.c` (edit) | `__EMSCRIPTEN__` forwarding hunk: fill hold/freeze, movie frame/end/shown, frame count, texture bytes, prof/queue stats, prewarm | 30 |
| `web/index.html` (edit), `NOTICE.md` (edit) | WebGPU probe -> `RECOMP_GPU`; Settings "Renderer: Auto / WebGPU / WebGL"; credit | 40 |

Total about 7,300 lines over five milestones.

## Rules for every milestone

- **Contract first.** One lead agent extends `wgpu_proto.h` (and `WgDrawRec`) before the milestone's parallel agents
  start. After that, an agent that needs a contract change asks the lead and does not edit the file itself.
- **Build.** Each agent builds into its own folder: `BUILD_DIR=work-remix/build-web-wgpu-<name> tools/build_web.sh`,
  configured like `build-web`. It stops at a compile check plus `wgsl_test` where it applies. No agent opens a
  browser or the game.
- **Integration and test.** One integration step per milestone merges, builds once and promotes with
  `tools/web_promote.sh <build>`. The owner then tests `http://localhost:3000/?RECOMP_GPU=webgpu`, against
  `?RECOMP_GPU=webgl` on the same build.
- **Logs.** `[WGPU]` lines in the page console. `RECOMP_WGPU_STATS=1` prints a line every second: frames, draws,
  passes, pipelines made, translate ms (C), encode ms (JS), arena KB, textures uploaded. A WebGPU validation error
  is logged once per message (`uncapturederror`), never silently dropped.
- **Native builds are untouched.** Every edit to an existing file sits behind `__EMSCRIPTEN__`.

## M1: clear + present on WebGPU

**Goal:** the device comes up in the render worker. Clears and flips reach the canvas, the game runs to the title on
WebGPU (no draws yet, so the screen shows clear colours only), and WebGL still works.

**Files:**
- New: `wgpu_proto.h` (frame header, ops `TEX_CREATE`/`TEX_FREE`, `BEGIN_PASS`/`END_PASS` with clear values,
  `PRESENT`); `nv2a_wgpu.h`; `wgpu_queue.c`; `wgpu_surf.c` (surfaces, depth, whole-surface clears, present);
  `web/wgpu.js` (device, `getContext('webgpu')` on the OffscreenCanvas, M1 ops, `onSubmittedWorkDone` count);
  `web/wgpu_fixed.js` (present shader, plain scale).
- Edits: CMake, `main.c`, `index.html`, the `nv2a_gl.c` forwarding hunk.

**Size:** about 1,000 lines.

**Owner test on :3000:**
1. `?RECOMP_GPU=webgpu`. The console says `[WGPU] adapter ... device ready, canvas bgra8unorm`.
2. The boot runs: Rockstar logo sound plays, the screen is black or clear colours, no hang, no validation errors.
3. `RECOMP_WGPU_STATS=1` shows about 30 presented frames a second at the title.
4. `?RECOMP_GPU=webgl` still plays as before.
5. A browser without WebGPU picks WebGL by itself.

**What M1 verifies:** a worker OffscreenCanvas presents at the end of each turn; `writeBuffer`/`writeTexture` accept
views on the shared wasm memory; the 2-frame pacing does not stall the executor.

**Parallel split (3 agents):**
- **A:** `wgpu_queue.c` + `wgpu_surf.c`.
- **B:** `web/wgpu.js` + `web/wgpu_fixed.js`.
- **C:** `nv2a_wgpu.h`, CMake, `main.c`, `index.html`, `nv2a_gl.c` hunk, `NOTICE.md`.

The lead writes `wgpu_proto.h` first.

## M2: movies and 2D

**Goal:**
- Boot movies, legal screens and the loading screen draw correctly: pre-transformed and fixed-function batches,
  register combiners, all texture formats, blend and alpha test, NV09F blits.
- The 4:3 fill and `MC3_FILL_SKIP` behave as on WebGL (fill decided in the present shader).

**Files:**
- New: `wgsl_psh.c/.h`; `wgsl_vsh.c/.h` (prelude, vertex pulling for every NV2A attribute type, fixed-function +
  fog, pre-transformed main); `wgsl_test.c` + `tools/wgsl_check.sh`; `wgpu_tex.c`; `wgpu_draw.c`; `wgpu_pipe.c`
  (in-memory only).
- Additions: `wgpu_surf.c` gets blits, partial clears (quad) and the fill parameters. `web/wgpu.js` gets the
  arena `writeBuffer`, `SET_PIPE`/`SET_TEX`/`DRAW`/`COPY_TEX`, the pipeline/bind-group/sampler caches and the
  shader modules. `web/wgpu_fixed.js` gets the fill, clear-quad and blit-quad shaders.
- Contract (lead, first): `WgDrawRec`, vertex-fetch descriptor, texture and pipeline ops.

**Size:** about 3,200 lines.

**Owner test:**
- The Rockstar and Rockstar San Diego logos and mc3intro play with correct colours and aspect, in sync with the
  sound.
- The loading screen's circles close (blits).
- The title screen's 2D layers show.
- Compare with `?RECOMP_GPU=webgl` at the same moments.
- `MC3_SKIP_MOVIES=hard` still works.

**Parallel split (6 agents):**
- **A:** `wgsl_psh.c` + its cases in `wgsl_test.c`.
- **B:** `wgsl_vsh.c` (fixed and pre-transformed) + `tools/wgsl_check.sh`.
- **C:** `wgpu_tex.c`.
- **D:** `wgpu_draw.c` + `wgpu_pipe.c`.
- **E:** `web/wgpu.js`.
- **F:** `wgpu_surf.c` + `web/wgpu_fixed.js`.

`wgsl_test.c` belongs to A. B adds its cases in `wgsl_vsh_test.inc`, which `wgsl_test.c` includes.

## M3: menus

**Goal:**
- The title flythrough (3D city), PRESS START, the main menu (Flash UI), profiles and the garage (car models)
  draw as on WebGL.
- Needed: vertex-program bodies, render-to-texture, depth/stencil/cull, scissor, pipeline persistence.

**Files:**
- `wgsl_vsh.c` program body: MAC/ILU, `a0`-relative constants, local constant copy when a program writes c[],
  `c[-38]`/`c[-37]` viewport mirrors as in gl_vsh.
- `wgpu_draw.c`: full stencil, blend and cull mapping into the key; strips, fans, polygons and line loops -> lists;
  points.
- `wgpu_surf.c`: copy before a draw that samples its own target; scissor and viewport per surface (`to_stored`).
- `wgpu_pipe.c` + `web/wgpu.js`: OPFS key list (`/opfs/cache/wgpu-pipes.bin`); prewarm with
  `createRenderPipelineAsync` behind the legal screen.

**Size:** about 1,200 lines.

**Owner test:**
- Title -> menu -> profile -> garage, side by side with WebGL.
- No hitches over about 100 ms after the second visit: `[hitch]`-style line `[WGPU] slow frame ... N pipelines
  made`.
- A second page load shows fewer pipelines made.

**Parallel split (4 agents):**
- **A:** `wgsl_vsh.c`.
- **B:** `wgpu_draw.c`.
- **C:** `wgpu_surf.c`.
- **D:** `wgpu_pipe.c` + `web/wgpu.js` (pipeline cache part only; no other agent touches `wgpu.js` in M3).

## M4: 3D cruise parity

**Goal:** `MC3_AUTOSTART=cruise` (page "Start in Cruise") drives the city as on WebGL:
- motion blur (depth as colour);
- fog;
- reflections and cube maps, the texture modes `gl_psh` supports;
- the 384x240 effect buffer;
- the minimap's narrow clip (grow, never shrink);
- HUD;
- `RECOMP_GL_SCALE`, square pixels, `RECOMP_GL_SMALL_SCALE`;
- LOD bias via `textureSampleBias`, anisotropy, the title's mip levels.

**Files:**
- `wgpu_surf.c`: depth -> RGBA8 pass (`zconv` rules, `RECOMP_GL_DEPTH_TEX`), scale and square pixels, shared zeta
  across colour surfaces.
- `web/wgpu_fixed.js`: the zconv shader.
- `wgsl_psh.c`: cube and dependent stages at `gl_psh` parity, bias.
- `wgpu_tex.c`: cube textures, mip levels, samplers (aniso, filters).
- New: `wgpu_stats.c` + `web/wgpu_stats.js` (GPU time per pass with `timestamp-query` when the adapter has it,
  like `gq_*`).

**Size:** about 1,000 lines.

**Owner test:**
- Cruise for 5 minutes at the same spots as on WebGL: rain, night, speed (blur near/far), tunnels, minimap.
- Owner's screenshots side by side.
- `RECOMP_WGPU_STATS=1` fps and GPU ms recorded for M5's baseline.

**Parallel split (4 agents):**
- **A:** `wgpu_surf.c` + `web/wgpu_fixed.js`.
- **B:** `wgsl_psh.c`.
- **C:** `wgpu_tex.c`.
- **D:** `wgpu_stats.c` + `web/wgpu_stats.js`.

## M5: performance

**Goal:** Cruise at a steady 30 fps on the owner's PC; 60 with `MC3_60FPS=1` where the GPU allows.

**Files:**
- New `wgpu_pool.c`: `nv2a_gl`'s vpool content hash for vertices and indices into a persistent GPU buffer
  (`COPY_BUF` from the arena at frame start); a free list swept after frames retire.
- `wgpu_draw.c`: use the pool.
- `web/wgpu.js` encoder hot path: no per-draw allocations, typed-array decoding, cached `setScissorRect`, bind group
  0 once per pass.
- `web/wgpu_fixed.js`: FXAA and CAS in the present pass (`RECOMP_GL_FXAA`, `RECOMP_GL_SHARPEN`).
- New `wgpu_dump.c` + `web/wgpu_dump.js`: `RECOMP_GL_DUMP`/WATCH shots through `copyTextureToBuffer` + `mapAsync`.
- Measure first, then decide whether translation moves to the executor thread (one copy of vertex data less).

**Size:** about 900 lines.

**Owner test:**
- Cruise and a rain race with `RECOMP_WGPU_STATS=1`: fps, translate/encode/GPU ms against the M4 baseline.
- `MC3_60FPS=1` cruise.
- `RECOMP_GL_FXAA=1` picture.

**Parallel split (4 agents):**
- **A:** `wgpu_pool.c` + `wgpu_draw.c`.
- **B:** `web/wgpu.js`.
- **C:** `web/wgpu_fixed.js`.
- **D:** `wgpu_dump.c` + `web/wgpu_dump.js`.

## Risks to check early

- **Worker present timing (M1).** The OffscreenCanvas `webgpu` context must present each turn without rAF. If it
  does not, use rAF turns (`RECOMP_GL_RAF` exists) or move the encoder to the page; the list is shared memory.
- **Compile hitches.** D3D12 compiles each pipeline on first use. Prewarm from the OPFS list (M3). The uber-shader
  is the fallback (ADR 0011).
- **Format gaps.** 16-bit textures need CPU expansion, and points have no sizes (point sprites draw 1 px). Log
  both the first time they are met.
- **Texture timing.** The render thread reads guest textures behind the executor, as the GL thread does today.
  Same behaviour, same races.
- **Mid-frame arena overflow.** Flush: encode and submit the part so far, then continue. Test with a small arena
  (`RECOMP_WGPU_ARENA_MB=1`) in M2.
