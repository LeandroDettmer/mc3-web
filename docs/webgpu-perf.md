# WebGPU renderer: measured costs and the next levers

Measured 2026-11-10 with `RECOMP_WGPU_STATS=1` (web/wgpu.js, one log line every 300 frames: `[WGPU] state calls
per frame (300 frames): ...`). Desktop Chrome, WebGPU, build `aa0bd57`, 30 fps. Title-screen city flythrough
only (Arcade/race not reached in the 5-minute test slot). iPhone and TV not measured yet.

## Numbers (per frame)

| Scene | Draws | Passes (draw passes) | setPipeline (same as previous) | setBindGroup(1) (repeats) | Arena upload | GPU |
|---|---|---|---|---|---|---|
| Legal/menu screen | 846 | 2 | 4.7 | - | 522 KB | 0.95 ms |
| Title flythrough | 983-1037 | 16 (14) | 369-396 (0 %) | 650-677 (47-52, ~7 %) | 4.9-5.4 MB | 3.2-6.4 ms |
| After Enter (next screens) | 1001-1200 | 16-21 | - | 579-805 | ~5 MB | 3.7 ms at 1206 draws |

Other: translate 1.5-1.65 ms, encode 1.4 ms, bind groups created 0-0.3, setScissorRect 9, clear quads 1,
writeTexture 0-1 KB, 0 errors.

## What is already done

Redundant state is already filtered, the way the D3D12 recomp (znxee/mc3-remix-xbe-recomp: 28 -> 53 fps at 100
cars) does it:
- C, `xboxrecomp/src/nv2a_wgpu/wgpu_draw.c:602-648`: last state per pass (reset when the pass changes);
  VIEWPORT / SCISSOR / BLEND / STENCIL / SET_PIPE / SET_TEX recorded only when changed.
- JS, `web/wgpu.js`: a repeated pipeline (:724) and bind group 1 (:765) skip the GPU call; bind groups cached by key
  (:745); group 0 and the index buffer once per pass; no setVertexBuffer (vertices are pulled from the arena).
- Leftover: SET_TEX is re-recorded after each new pipeline (wgpu_draw.c:641); JS drops it (key string + Map lookup,
  no GPU call). Expected gain from more dedupe: about 0.

## The levers left (not started)

Both matter for tile-based GPUs (iPhone, Android, TV chips), where a desktop GPU hides them.

### 1. Arena upload: ~5 MB per frame (~150 MB/s at 30 fps)

Every frame re-uploads all vertex and index data plus the draw records (one `writeBuffer` of the arena,
web/wgpu.js:896-901). The city geometry is mostly the same from frame to frame.

Plan (already M5 in docs/webgpu-plan.md, never built): `wgpu_pool.c`, a content-hashed vertex/index pool in a
persistent GPU buffer (the GL backend's vpool idea), so only new or changed buffers are copied; draws reference the
pool. Measure: arena KB/frame before and after in a cruise, translate ms (the hash costs CPU).

### 2. Passes: 16 per frame (14 with draws)

Each target change (reflection map, shadow, motion-blur depth read, minimap, HUD, present) opens a pass; on a tiled
GPU every pass loads and stores its attachments. Sources to check:
- passes split because a draw samples its own target: a shadow copy and a new pass segment
  (`xboxrecomp/src/nv2a_wgpu/wgpu_surf.c:630`, "draw samples its own target ... copied first, once per pass");
- `loadOp: load` where the pass is fully overwritten or starts with a clear (use `clear`), `storeOp: store` for
  targets never read again (depth: `discard`);
- consecutive passes on the same target that could merge.

Measure: passes per frame and GPU ms in a cruise and a race, on a phone.

## How to measure

Settings (or the page's settings store) `RECOMP_WGPU_STATS=1`; read `[WGPU] state calls per frame` and the per-second
`[WGPU]` line in the log. For a scout run: local only on http://localhost:3000, one browser and one game, at most
5 minutes. The page's `#log` is hidden by default (set `hidden = false` to read it), and the nick dialog must be
filled before Play.
