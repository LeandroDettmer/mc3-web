# mc3-web: Midnight Club 3 (Xbox) in the browser: engineering notes

**mc3-web** (github.com/LeandroDettmer/mc3-web) is our fork of mcdub3-nx
(Laure Ladastra, MIT; remote `upstream`) that runs the static recompilation
in the browser: Emscripten (wasm32, pthreads) + WebGL2 + AudioWorklet, with
online play through our own server. Blueprint: nfsu2-web (th0mk, MIT; a
clone sits at `E:\Development\Dev\nfsu2-web`). The repo is about the
browser port only (2026-11-09: the Switch, Windows, macOS and VM tooling
was removed); the native Linux build stays as the debug host (gdb,
difftest, perf) and must keep its behaviour. The other hosts' platform code
inside `xboxrecomp/` and `src/` stays but is not built or tested. Facts
below that mention the Switch, Mac or Windows are history from mcdub3-nx.
Keep this file current: findings, rules, status. It is the
project's memory. The browser port's own notes are in "Browser port" below;
the rest is mcdub3-nx's (Xbox *Midnight Club 3: DUB Edition*, lifted to C
with xboxrecomp; modelled on nfsu2-sw).

## Rules

- Never commit game data (ISO, `default.xbe`, disc files) or generated C.
  Every path is relative to the repo root: the discs in `game*/`, everything
  generated in `work-<game>/` (both gitignored; tools/env.sh). No default
  paths outside the repo.
- Two games share the repo (ADR 0006): `GAME=dub` (default) and
  `GAME=remix` on every target. A game's own things live in
  `config/<game>/` and `src/games/<game>/` and nowhere else; shared code
  takes no game's addresses (`config/<game>/game.h` gives them). A fix for
  one game must not change the other's behaviour.
- Never commit, upload or store on GitHub console keys (`keys/`, gitignored).
- Toolkit changes go in `xboxrecomp/` as their own commits, subject prefixed
  `xboxrecomp:`. Prefer new files over edits to files upstream also
  changes (ADR 0002). Note upstreamable fixes in
  docs/upstream.md (patch or issue, and which upstream: xboxrecomp or nfsu2-sw).
- nfsu2-sw's `src/` is unlicensed: read it for ideas, don't copy it.
- The toolkit is our fork without upstream history (ADR 0007): take an
  upstream fix as a patch (`git format-patch` upstream, `git am
  --directory=xboxrecomp` here), committed in our name with the original
  author credited in the message. Deliberately, never routinely.
- After toolkit header changes (`templates/runtime/recomp_types.h`), regen:
  it rewrites `gen/recomp_types.h`. Then rebuild all generated code.
- Every seed added to `config/<game>/seed_functions.json` gets a note, and is
  reviewed: a seed inside a function splits it.
- Record decisions in `docs/decisions/` (short ADRs). Open work, bugs and
  decisions still to evaluate live in `TODO.md` (gitignored, personal);
  this file keeps facts and rules.

## Workflow

Run `make` in the repo root for every target (`GAME=remix` for the Remix).
Logs land in `work-<game>/logs/`.
Technical docs (stages, what sits where, tests, the loop): CONTRIBUTING.md, the source of truth.

| Change | Then |
|---|---|
| `src/*.c` (not overrides), runtime (`xboxrecomp/src`), `web/*.js` | `make web` (own `BUILD_DIR`) |
| body of an existing override in `src/games/<game>/recomp_manual.c` | `make web` |
| added/removed an override, translator | `make relift web` |
| `config/<game>/seed_functions.json`, disasm/func_id | `make regen web` |
| native debugging (gdb, difftest, perf) | `make build run` (native Linux) |

Runtime switches: any `RECOMP_*` / `MC3_*` variable (the page's Settings,
or the URL). The full `RECOMP_*` list with meanings is in nfsu2-sw's README
(Configuration section); the toolkit is the same.

## Browser port

Decision record: docs/decisions/0010-browser-port.md. Player's guide:
docs/web/PLAYING.md.

### Rules (browser)

- Every browser-only change sits behind `__EMSCRIPTEN__` (or a web-only
  file); native builds keep byte-for-byte the same behaviour.
- Agents develop; the owner tests in his own Chrome. Agents do not run
  headless-browser or fps test sessions unless asked. A compile check
  (`make web`) is fine.
- Parallel scouts never launch the game (no browser, headless Chrome,
  Node run or web_serve.py instance of their own): each stops at a compile
  check into its own `BUILD_DIR` and reports what to test. One final
  integration step merges every scout's change into a single build and
  tests it once, at the end (2026-11-06: several scouts at once each
  opened their own game instances).
- Nothing large on C: (it filled up once, 2026-11-05, with a Chrome
  profile's OPFS disc cache). Chrome profiles, OPFS tests and big scratch go
  in `.scratch/<name>/` (git-excluded via .git/info/exclude). Kill a
  headless Chrome with `taskkill /T /F /PID`: killing the launcher leaves
  it running.
- Dev server port 3000 (not 8080-style ports). It serves
  `work-remix/build-web`, the owner's test build: agents never build into it
  while he may be testing (a link rewrites the .wasm in place and the page
  loads half a module). An agent builds into its own folder
  (`cmake --build work-remix/build-web-<name>`, configured like build-web;
  tools/build_web.sh) and never serves it on another port: a finished
  improvement goes to :3000 with `tools/web_promote.sh <build folder>`
  (atomic copy; also copy the build's index.html/lan.js/disc_cache.js if
  they changed). The owner tests every improvement on :3000 (2026-11-07).
- `work-remix/gen` is the lift with the g_guest memory alias and
  RECOMP_INLINE=16 (was gen-opt, 2026-11-07); the old one, without the
  alias, is `work-remix/gen-old-noalias`. Regen with RECOMP_INLINE=16 or
  the inlined leaves are lost.
- Never leave a game running: an agent kills every browser (headless or
  not), CDP driver and mc3-server it started before it reports
  (`taskkill /PID <pid> /T /F`), and says so. Only the owner's server on
  :3000 stays. Left-over runs (2026-11-06: six headless Chromes and two
  servers) take the CPU and GPU from the owner's game.
- nfsu2-web code reused (MIT) carries a credit to th0mk in the file header
  and in NOTICE.md.
- CHANGELOG.md is player-facing: one short line per player-visible change; no
  logs, addresses, env names or internals.
- Several agents share one tree and one build folder: small commits of
  your own hunks only (`git add -p` is unavailable to agents: stage whole
  files you alone touched, or ask); never commit another agent's hunks.

### Build and run (Windows host, WSL 2)

The toolchain lives in WSL Ubuntu-24.04: emsdk at /opt/emsdk, clang,
cmake, ninja; the repo is reached as /mnt/e/Development/Dev/mc3-web. WSL
has 14 GB RAM + 16 GB swap (%USERPROFILE%\.wslconfig). Checkout with
`core.autocrlf=input` (scripts must be LF in WSL).

```sh
wsl -d Ubuntu-24.04 -u root -- bash -lc '. /opt/emsdk/emsdk_env.sh && cd /mnt/e/Development/Dev/mc3-web && JOBS=8 make web GAME=remix'
```

| Step | Command | Notes |
|---|---|---|
| unpack a disc | `make extract GAME=remix EDITION=usa ISO=<file>` | into game-remix-usa/ |
| lift | `make lift GAME=remix` | ~15 min; writes work-remix/gen (2.9 M lines) |
| browser build | `make web GAME=remix` | work-remix/build-web: mcdub3remix.js + .wasm (~41 MB); lifted chunks ~4-6 min from clean |
| node build | `make web-node GAME=remix` | NODERAWFS, headless boot test |
| serve (dev) | `make web-serve GAME=remix` | tools/web_serve.py on :3000 |
| serve (server) | `cd web/server && cargo build --release && target/release/mc3-server --web .. --build ../../work-remix/build-web --disc ../../game-remix-usa --port 3000` | Rust; the /lan switch |

The Remix USA disc lifts to the same C as the European one (the lifter
takes the base's condition at an edition-variant jcc; test_edition_variants.py).
A clean checkout's mtimes trip the "lift is stale" check: `touch` the
generated files when the lift inputs did not change.

### Layout

- `web/`: start page (index.html: Play, Settings -> the game's env,
  Controls, Saves, Online, About), pre-js files (pre.js, audio.js,
  disc_cache.js, lan.js, net.js), the WASMFS JS backend wasmfs_fetch.js.
- `web/server/`: Rust server (page, build, discs with Range + COOP/COEP,
  the /lan WebSocket switch). Crates adapted from nfsu2-web's server-rs.
- `src/web_fs.c`: mounts the disc at /game (fetch backend) and saves at
  /opfs/save (MC3_SAVE_DIR).
- `xboxrecomp/src/platform/wasm_memory.c`, `kernel/xbox_guest_ptr.h`: guest
  memory; `nv2a_gl/gl_es.c`: WebGL2; `apu/apu_web.c`: audio ring;
  `net/xnet_hle.c` + `net/xnet_web.c`: System Link.

### How it works

- **Memory:** wasm has no mmap aliasing. On `__EMSCRIPTEN__`, `XBOX_PTR`
  translates through a 1 MB-granule table (`g_xbox_region_off[4096]`): RAM
  backed, its 28 mirrors alias it, contiguous window 0x80000000 backed, the
  tiled aperture 0xF0000000 aliases it, NV2A/MCPX/flash backed, extended
  VMA backed on commit. Raw `g_xbox_mem_offset` uses go through
  `xbox_ptr()`; native pointer back to guest VA through `xbox_native_to_va`.
  RECOMP_RAM_MB=128 (every host, default 64; 2026-11-09, untested in game)
  makes the contiguous window 128 MB like a devkit (g_xbox_contig_size,
  kernel.h; bus RAM tag = window size, bit 27); low RAM stays 64 MB. The
  Remix's main heap (47 MB, sub_002698C0 override) is MC3_HEAP_MB, default
  63 at 128 MB (95 with MC3_ROAM_HEAP_MB=0); '[HEAP] main heap N KB' at
  boot. Free Roam's remote cars get their own game heap at 128 MB (below).
- **Threads:** PROXY_TO_PTHREAD; guest threads are pthreads (workers) under
  the guest lock. The page's main thread never blocks.
- **Graphics:** the WebGL2 context is made with
  `emscripten_webgl_create_context("#canvas")` on the GL queue thread, which
  owns the OffscreenCanvas (SDL's EGL path is proxied to the main thread and
  cannot reach a transferred canvas). gl_es.c rewrites shaders to GLSL ES
  300, swizzles BGRA on the CPU, maps buffers to BufferSubData, drops depth
  clamp / border clamp / program binaries. LOD bias is lost; the vertex
  shader needs ~330 uniform vectors (RECOMP_GL_PERDRAW=0 if a GPU gives 256).
- **Audio:** SPSC float ring (48 kHz stereo) in wasm memory read by an
  AudioWorklet (web/audio.js, apu_web.c); the AudioContext starts on the
  Play click. SDL audio is not used on the web (it crashed from a worker).
- **Disc:** RAM LRU (48 MB) -> OPFS block cache (sparse file + .have map,
  1 MiB blocks; keys by path + size, not mtime; scheme 2, older folders
  emptied once) -> HTTP Range. Direct reads (2026-11-09, untested in a
  browser): a deploy's disc Worker hit the free plan's 100k requests/day
  proxying every block, so the page and game ask POST <disc>.sign once
  (100 paths a call) for presigned R2 URLs (12 h) and read ranges from the
  bucket, no cookie (disc_cache.js signer; re-signed on a stale 403; a
  fresh 403, no answer or no .sign -> the server's URL, '[WEB] disc: direct
  reads ...'). On a local or LAN address (localhost, 127.x,
  ::1, *.local, 10/8, 172.16/12, 192.168/16) there is no cache: reads go
  straight to HTTP. `?MC3_DISC_CACHE=1` / `=0` forces it. A predownload
  worker fetches the whole disc on the page's click only, 3 connections,
  and stops when a game starts (BroadcastChannel 'mc3-disc'); a read that
  gets no data for 15 s (the game's own reads: 4 s; no progress, so a slow
  link that keeps sending is never cut) is tried again and logged. In game,
  the disc thread fills the cache in the background (2026-11-09,
  untested): assets_x.dat_2 (free-roam car builds read it synchronously
  under the guest lock: 0.2-1.5 s frozen cold, an estimate; measured
  warm on localhost, a swap's build is 105 ms (two-browser run
  2026-11-09); one car is ~0.35 MB model + ~1.7 MB _g file + its mesh
  parts, so a cold build is several 1 MiB block trips. That is the brief
  freeze every player in Free Roam sees when one joins (2026-11-10):
  '[WEB] disc wait: ...' (a game read waiting >= 100 ms), '[ROAM] cars:
  car N built ... in N ms', '[WEB] hitch: N ms without a frame; last car
  build ...; last disc wait ...' (>= 250 ms; one of >= 1 s in Free Roam
  sends the log, tag hitch) split it next time), the streamed sound banks
  (audio/XStreams/*.xwb, 486 MiB, mx.xwb first: a track change fetched on
  demand stalled the game), resources/ smallest first (642 MiB: car builds
  .xbpf, hudmap), banks.dat, flash/ smallest first (643 MiB: pause, garage,
  rewards, menus, the showroom's per-car vs/ps/ms/ds_vp screens), the rest
  of assets_x*, then video/Story, Tutorial, Reward, TokyoTut (not the boot
  movies): the whole disc but those, ~4.4 GiB (budget 4608 MiB and the
  browser's estimate; the page's predownload already takes every file); 15 s after boot, one block at a time, only after 300 ms
  without a game read on the network; '[WEB] prefetch: ...' lines. A game
  read that continues its file into a new block starts the next 2 blocks
  (readahead, '[WEB] readahead: ...' once per file). Music track changes
  (2026-11-11, untested in a browser): every disc read waits under the guest
  lock; a change reads a random entry of mx.xwb (135, shuffled) from its
  start in 352 KB packets, and players' logs showed 0.5-2.5 s freezes per
  change on a cold cache (one at the start, one when a packet crossed into
  the next 1 MiB block). Now the fill fetches, before anything else, the
  blocks holding each entry's first 704 KB (parsed from the bank's XACT v3
  table in block 0: 206 MiB), and every .xwb read starts the readahead (a
  jump to a new track too). Chrome incognito: estimate() says ~10 GiB
  (Chrome 133+), the real quota is far smaller, so OPFS writes fail and
  the cache turns off for the session; then those track-start blocks are
  kept in memory (pinned, ~206 MiB JS, not below deviceMemory 8; '[WEB]
  prefetch: no disc cache: the music's track starts ... kept in memory').
  Emscripten's own WASMFS fetch
  backend left stale data across chunks, hence our backend.
- **Input:** SDL keyboard/Gamepad on the GL thread; a key tap is held at
  least 120 ms (a down+up in one rAF pump was lost by the menus).
  Touch controls (2026-11-09, untested on a phone; web/touch.js, after
  nfsu2-web's): a virtual pad the page writes into xbox_touch_pad_box
  (input/touch_pad_web.c, 14 int32 words via Module.mc3TouchPad), merged
  into port 0 like the keyboard; Settings 'Touch controls' Auto/On/Off
  (page-only MC3_TOUCH*). Phones (MC3Touch.isPhone): full screen + landscape
  on Play, RECOMP_GL_SCALE 1 and MC3_60FPS off unless picked; Wake Lock on
  every device. Page defaults (2026-11-11, untested in game): every device
  4x aniso, FXAA and sharpen off, MC3_MOVIE_HD 0; PC 1.5x (was 3x), phones
  and lowMem 1x (phones were 1.5x).
- **Wasm memory and TVs (2026-11-09, untested on a TV):** the fixed 2 GB
  (INITIAL_MEMORY, no growth) never started on a TV browser (black
  screen). Now linked 256 MB min, growth to 2 GB (Emscripten's
  check-and-refresh views; not GROWABLE_ARRAYBUFFERS: its growable SAB is
  refused by WebKit's WebGPU/WebGL calls, an iPhone went black after the
  legal screen, 2026-11-09); web/pre.js makes the Memory itself
  (Module.wasmMemory, hence the INCOMING_MODULE_JS_API list in
  CMakeLists.txt): 2 GB up front on a PC (never grows, as before), else
  1 GB / 512 MB / 256 MB and grows, then smaller maximums; WebKit (iPhone,
  iPad, Safari) sets aside the whole maximum (256 MB with a 2 GB maximum
  was refused on an iPhone): fixed 1024 / 768 / 512 MB, maximum = initial
  (iOS/iPadOS always; a Mac's Safari 2048 first; 2048 on iPhones was 61
  silent kills in menus, 2026-11-11);
  '[WEB] memory: ...' at start. Estimated need ~350-450 MB (guest RAM 64
  at wasm 0, contiguous 64/128, NV2A 16, MCPX 8, ~25-30 thread stacks x 4
  MB, WebGPU arena 32, renderer caches). Pool PTHREAD_POOL_SIZE =
  Module.mc3Pool || 40. Low-memory profile (index.html lowMem: a TV user
  agent, a phone or tablet (iOS/iPadOS, Android phones, MC3Touch.isPhone)
  or deviceMemory <= 2; MC3_LOWMEM=1/0 forces): memory from 512 MB,
  24 pool workers, RECOMP_GL_SCALE 1, RECOMP_RAM_MB 64, MC3_ROAM_CARS 7,
  MC3_DISC_MEM_MB 16 (disc LRU, default 48), MC3_PREFETCH=0, no 60 fps, light
  filtering. Failures show a box over the picture with the reason and the
  log's last 20 lines (index.html failShow: abort, page errors before the
  runtime is up, '[WGPU] device lost', '[GL] WebGL context lost', out of
  memory lines, no frame 90 s after the runtime is up; runtime not up 5 min
  after Play). Crash tracing (2026-11-10, untested in a browser): every
  failure box, page error after boot (no box, 3 a session), freeze (15 s
  without a new frame, tab visible) and killed tab (log tail + heartbeat
  in localStorage every 5 s, cleared on pagehide, sent on the next visit)
  goes to config.json logUrl as POST ?tag=fatal|error|freeze|
  previous-crash, headed by build, UA, GPU, memory, online state, times
  (previous-crash also lists the last memory/failure/watchdog/roam lines
  and '[WEB] tab hidden|visible' first; a hitch within 5 s of a Free Roam
  status change is not sent; Firefox's captureStream transfer refusal sets
  mc3.glProxy);
  the log Worker (.deploy/worker.js, local) keys it logs/<day>-<tag>-
  <code>.txt and takes 10 an hour per address.
  New version (2026-11-12, untested in a browser): .deploy/stage.sh stamps config.json "version" (short hash of ../mc3-web-main HEAD); the page
  (index.html versionCheck) re-reads it every 60 s and on tab visible, and on a change reloads online players (lan.roaming or a room) after a 60 s box,
  offline ones get a dismissable notice only; no version (web_serve.py) = off. Pages loaded before this shipped have no poll.
  TCL Google TV (Chrome for Android TV, 32-bit, 2026-11-09, fix untested
  on the TV): every memory from 448 MB up was refused (256/512 given; a
  32-bit V8 that cannot set aside the maximum may set aside only the
  initial size, so the low profile now tries fixed sizes down to 320
  MB first), and '[GL] WebGL2 context on #canvas failed (0)': WebGL 2
  on the page but none on an OffscreenCanvas in a worker. Now the page
  probes a worker at Play ('[WEB] graphics: ...'). Where the worker has
  no WebGL 2, or a start failed there before (localStorage
  mc3.glProxy), it sets RECOMP_GL_PROXY=1: pre.js hides the canvas's
  transferControlToOffscreen, so the canvas stays on the page thread.
  The GL thread then makes the context there (-sOFFSCREEN_FRAMEBUFFER,
  PROXY_ALWAYS + renderViaOffscreenBackBuffer). Void GL calls are queued
  to the page, getters and uploads over 256 KB are round trips, and
  emscripten_webgl_commit_frame runs at each flip. Slower; =0 forces the
  worker. A refused context logs '[GL] try N refused: <browser's
  reason>; attributes ...; this worker: webgl2 ..., webgl ...; UA', then
  one retry with plainer attributes.
  Fit in 256 MB (2026-11-09, untested on the TV): the TV gave 256 MB
  and never grew; at 252 MB a malloc returned NULL, and since wasm
  address 0 is guest RAM a NULL host struct reads guest bytes silently
  (ICALL to 0x94 / 0x00700094, then a guest-lock hang). Now
  -sABORTING_MALLOC=1 (a failed malloc aborts: 'Out of memory on this
  device' box) and pre.js logs '[WEB] out of memory: growing the wasm
  memory from N MB to M MB refused'; failShow writes '[WEB] failure:
  <title>: <why>' into the log. Budget (MB, early boot, before -> after):
  guest RAM 64 -> 64; static data 28 -> 16 (pb_exec software-vertex
  caches on the heap at first use, the GL/WebGPU paths never get there);
  STACK_SIZE 8, paid twice (page thread + main under PROXY_TO_PTHREAD)
  16 -> 4 (lifted code uses no shadow stack: 980 of 23,979 functions have
  a frame, max 9.6 KB); contiguous window 64 -> 64; NV2A 16 -> 9 and MCPX
  8 -> 4 (only the register megabytes backed, xbox_memory_layout.c
  MCPX_AT); GL queue 16 -> 4 (RECOMP_GL_QUEUE_MB, web floor 4; a draw
  over a quarter is dropped and logged); GL ring copies 20 -> 5
  (RECOMP_GL_RING_MB=4, they grow on demand); GL program table 4 -> 1
  (RECOMP_GL_MAX_PROG=256). The vertex pool (RECOMP_GL_VPOOL, 8 in the
  low profile) and the disc LRU are GPU / JS memory, not wasm.
  MC3_THREAD_STACK_KB=1024 stays (ample by the frame sizes above).
  '[WEB] memory in use: N MB of M MB' logs each 16 MB of sbrk (GL
  path), to learn the real peak.
- **Online (System Link):** the XNet API thunks (21 Remix overrides, MC3_NETPLAY=1)
  go to xnet_hle.c; on the web its transport is a WebSocket to the server's
  /lan switch (virtual IPs 10.77.0.0/18, broadcast to System Link peers).
  The game uses UDP port 1000 (+broadcast discovery) and VDP 1001; no XNet
  security (both ends are ours). DUB overrides wait for a DUB XBE.
- **Online Races (race rooms, 2026-11-09):** the page's Races list makes and
  joins rooms (invite link ?race=<id>, optional password); inside a room
  the game's System Link (Online Races -> Host / Join Game) sees only the
  room's players. Contract docs/races-protocol.md: step 1 rooms on the /lan
  switch (crates/lan), step 2 WebRTC data channels between members
  (web/rtc.js, unordered/unreliable, the switch as fallback relay), step 3
  the same list on a Cloudflare Worker with a Durable Object
  (web/races-worker, config.json racesUrl; not needed while lanUrl is
  set). The game's own Xbox Live race list cannot come back; this replaces
  it. Open: Worker + relay together give a client two vips.
- **Free roam (2026-11-08, O2-O4 built, two-browser run works):** nfsu2-web's
  roam (MIT, th0mk) in xboxrecomp/src/net/roam (API roam_api.h: roam_start,
  roam_set_local, roam_peers, roam_set_cfg/roam_peer_cfg; wire magic MC3R,
  52-byte state, 20 Hz, interpolation) over the page's /lan link (own SAB
  mailbox), and the hub in web/server/crates/roam (mc3-server --roam, on by
  default; vip 10.77.0.253:47210; sends each player its K nearest, K = 7,
  MC3_ROAM_K). It is a mesh shaped by the hub: snapshots go player to
  player through the switch; the hub only picks the links. Hybrid P2P
  (2026-11-10, untested in a browser; web/rtc.js free roam, docs/races-protocol.md
  step 2): the page opens WebRTC channels to the up to 12 (low-memory 6) busiest
  roam destinations (= nearest), signals "roam":1 relayed by the Worker switch
  within a city; frames to a peer with an open channel go over it, the hub's and
  the rest via the switch; ICE from the switch's /lan/ice (TURN), page setting
  MC3_ROAM_P2P (shares IPs), '[rtc] free roam ...' lines. One Durable Object
  per city (2026-11-10, untested in a browser; web/lan-worker/src/city.js): with
  the ROAM binding + ROAM_SECRET the page opens a second socket per city
  (wss .../lan/roam?city=N + a ticket from the switch, web/lan.js roamSend, picked
  by the game's HELLO room, closed by its BYE) carrying that city's hub, relay
  frames and roam signals; the Switch keeps lobby, races, players list (city
  reports) and its own hub as fallback; '[net] free roam: own socket to city N
  open'; a lost link (switch or city socket not open for 3 s after a HELLO) shows a notice over the picture (lan.js roamDown, pre.js; reload button after 30 s; '[net] free roam: link lost'). Load per city object ~3 msgs/s per player (HELLO + 2 SNAP) + relayed
  pairs without a P2P channel (the larger share while K > roamMax).
  Over 10 players (2026-11-11, untested in a browser): the core's group cap
  stays the hub era's (RC_LIST_MAX + 1) while a configured hub is silent
  (was max_players 8: peers refused 'full'; LAN discovery alone keeps 8);
  rtc.js thins a relayed SNAP only beyond max(8, roamMax) busiest peers and
  never one the core already thinned by distance (flags every-n 4/10: the
  two stacked gave ~0.5 Hz vs the 3 s lost timer); '[rtc] free roam load: N
  peers, P2P x, relayed y, thinned z/s' every 30 s on change; the city
  object builds its report only after CITY.reportMs; Worker CTL 40/s burst
  400 (a 32-player join's ICE signalling overran 20/200).
  Link churn and listed-but-no-car (2026-11-12, untested in a browser):
  with K 32 above every city's size (<= 12 seen) the hub drops a live pair
  only when one side's socket to it reconnects; the core then dropped it
  'out of range' after 2.5 s and re-linked a second later while P2P
  snapshots kept coming (log HN5PQT). Now a linked peer still sending is
  kept 15 s unlisted (T_HUB_GRACE_LIVE), a silent one 2.5 s as before.
  rtc.js uses a free-roam channel only after a frame came over it within
  5 s (1-byte probe each tick until then; an open-but-dead channel ate
  the frames until ICE failed: 'joined' then 'no packets for 3 s' to the
  timeout), '[rtc] free roam X: channel open, nothing came over it ...'.
  lan.js drops the switch hub's frames while the city socket is open (the
  switch hub kept sending PEERS/FAR ~10 s, its gone_ms). '[roam] <name>
  <id>: listed by the hub N s, no snapshot (state, hears us, datagrams in)'
  / ': linked, first snapshot N s after ...' split dimmed blips. Game side Game side
  src/games/remix/roam_game.c: the Online cruise's race setup [0x53F2E0]
  gets 1 + N entries (copies of entry 0, +0x10/+0x14/+0x18 = -1, net flag
  0), so the game builds N extra player cars at load (N = peers at load
  + 2, at most 7, MC3_ROAM_SLOTS=N fixed: all 7 after a garage visit
  overran the game's heap, '[GUEST] Heap overrun', allocator sub_001B4740,
  current heap [0x52B488]; '[ROAM] heap ...' lines log the margin; no slot
  can be added mid-cruise) (the 8 player slots
  are hard-wired: GetPlayer sub_002BF570 has 209 callers, sub_002BDC00 is
  unrolled 8x -- raising them is not worth it); each frame SetMatrix
  0x2A35A0 + SetVel 0x3432B0 only park a slot or snap a new peer; remote
  cars move through System Link's own remote path (msg 0x42 pose block at
  player+0x90, inputs at [veh+4]+0x6C sent as rc_state.ext, then
  sub_002C0540 + sub_002C9570 with a stand-in session at [0x557EFC]; the
  car builder's controller step sub_002CB320 runs for remote slots as in a
  network race, else a pad -1 car has no controller [veh+4] and the inputs
  are dropped: wheels still, car dragged; MC3_ROAM_TRACE=1 per-slot line);
  unused slots are parked 30 km off and 500 m below, hidden and out of
  the physics level (2026-11-10: in it, the 2 spare slots were invisible
  cars our car hit at the spawn; back in while a state request is queued,
  for the game's delete; roam_slot_phys) ([[veh+0xC]+0x12D] = 0, the car's drawn flag: the minimap pins far blips
  to its rim, sub_000B95B0, and skips undrawn cars); minimap blips come
  free (sub_000BF850 walks the player manager, position [veh+0x1C]+0x34). Each slot's race-setup entry has its own CarCfg (O4, copied
  with operator= sub_00272010). A car in the sea: the vehicle update
  (vtable 0x454F40 +8, sub_000FFD90) opens the no-Resume pause menu
  (screen 2) for any player car whose reset flag (sub_000A5890) is set
  outside a network race, so a peer falling in the sea gave everyone that
  menu (2026-11-10); in the Free Roam cruise the sub_000A5890 override
  answers "no reset" there for every car but ours ('[ROAM] car ... in the
  water'). Re-entering Free Roam resets the 1 + N
  count we left in the race setup. Nick from MC3_PLAYER_NAME. Cars are built at load; mid-cruise a slot
  whose peer has another CarCfg is rebuilt (2026-11-09, untested; the
  pool's queue since 2026-11-11, below: out of the world sub_000FFB40 +
  delete as the player dtor, build as the player ctor, after the physics
  step; one at a time; MC3_ROAM_SWAP=0 off; no motorbike mid-cruise, 2026-11-11: its rider, sub_002CBF70 -> sub_00117880, loops on Sleep(1) until every streamed resource is idle, never in a running cruise -- main frozen for good on an iPhone; roam_car_bike, the old car stands in; the pool's heap check). Garage round trip
  (2026-11-09, untested): the session stays (roam_stop only in
  online_leave); state 5 (sub_00021A60) sets status RC_ST_SHOP, a cruise
  load RC_ST_LOADING, the first cruise frame RC_ST_ROAM + teleport + entry
  0's CarCfg again; peers park and hide a SHOP/LOADING peer's car, slot
  kept (swap to the new car meanwhile); while not roaming the core re-sends
  our last state to the hub every 2 s (hub stale_ms 60 s: links hold).
  Logs '[roam] status ...', '[roam] cfg sent: <car>', '[roam] web: the
  switch link is down'. Slots stay their
  peer's 3 s after it drops out; pose blend tau 100 ms, snap 8 m. MC3_ROAM_FAKE=N: N copies of your car trail
  you, for a one-browser test. The world is not shared (System Link
  doesn't share it either: its RACE_CONFIG msg 0x43 carries only city,
  time, weather, race type, count; traffic, peds and props are local).
  Map icons (2026-11-09, untested; src/games/remix/roam_map.c): the HUD
  map's built-in garage (type 6, icon_garage) and mover (type 5) points of
  interest go into the map server's per-frame icon list only through
  sub_000BF060, which in some modes adds them one frame (dirty byte
  [map+0x468]); the Online session sets that byte every call. That map
  (vtable 0x4509D0) exists only in the career's HUD page, built in game mode
  2 ([0x53F2D4]) only: Free Roam (Arcade cruise) had none, so no garage icon
  (2026-11-09). Now a hidden one is built after the HUD's init
  (sub_000B6A90 override) and pushes after every map-server clear
  (sub_000D0140 override); deleted with the HUD (sub_000B7720). Log
  '[ONLINE] map: garage map ...'. Players beyond
  the K nearest: the hubs send a FAR list (msg 9, every 1 s, the whole
  city up to 256 (FAR_MAX): id, status, place, orientation, name; old
  cores drop it as an unknown type; 2026-11-10, untested in game: parts
  of 20 (FAR_PART) a datagram, one seq, header flags part | parts << 8,
  nearest part 0 sent last so a core before 18 keeps the nearest 20; the
  core takes the list at part 0, RC_FAR_PARTS kept; the minimap draws the
  20 nearest + any within 400 m, the full map all), roam_far() gives it, and an sub_000BF850 override draws each as a
  dimmed, smaller blip with the car blip's primitive sub_000BB310
  (MC3_ROAM_MAP=0 off); every linked peer gets one too, on the minimap
  and the full map (0x450900) alike. Since 2026-11-10 (untested in game)
  the FAR list also carries the linked players, after the unlinked ones
  (a link whose snapshots do not arrive still gets a blip; roam_far drops
  those drawn as cars), and '[roam] map: ...' logs on each change of the
  counts (it used to log the first frame only). Online progress panel
  (2026-11-12, untested in a browser): the same pre.js code puts it left of
  the players panel, from roam_progress_box (online_progress.c: online,
  wins, "Latest\nNext" UTF-8; refilled on each call). Players panel: while the
  full map draws, roam_bigmap_box (seq, city) tells web/pre.js, which
  lists the Free Roam players from the switch's players push on the
  picture's right edge (same city first; none on web/server, which has no
  push). Open: names on the full map.
  Race start points (2026-11-09, untested in game; roam_races.{c,h},
  roam_map.c): the cities' event lists are loaded at boot (sub_0026F910,
  tune/race/<city>.loc + .locinf) into the city table [0x557EE4]: +0x14
  count, +0x18 names, +0x20 events of 0x30 B (+0 RaceType 0..22, names at
  0x4B37D0; +0xF opponents; +0x10 laps; +0x14 StartBeaconPosition, (50,0,0)
  = none; no heading: the grid is in the race's .rac). Arcade picks a race
  by that name (settings +0x68, sub_0026B010). The same sub_000BF850 hook
  draws them with the garages' icon primitive sub_000BDBE0 (map textures:
  diamond +0xA8 by colour for sprint/circuit/unordered/autocross, trophy
  +0x9C track, star +0xA4 frenzy): the full map all of the city, the
  minimap (0x450850) the 4 nearest within 300 m; Free Roam cruise only
  (race type 0, not our AI race: in a race they sat over the game's own
  checkpoints). Each city's list is cut at load (rr_curate, ~70 in San
  Diego): variants of one course (same kind, starts within 50 m) become
  the medium/normal one, every track and frenzy stays, then at most 28 a
  city at least 150 m apart; only those get the prompt. roam_races_nearest() is
  the API for starting races from the map. '[ROAM] races: ...' at first
  use; MC3_ROAM_RACES=0 off, =2 logs each race.
  AI race from a start point (feature 3, 2026-11-09, untested in game;
  src/games/remix/roam_race_ai.c, docs/roam-races.md): within 30 m of a
  start point, slower than 8 m/s, the page shows "Race: <name> ... press
  White (Q)" (roam_prompt_box, web/pre.js); White fills [0x53F2E4] (copy
  sub_00268E50, SetEvent sub_0026B8E0(city, event), 1 player) and sends
  Request(8) + Request(6), roam status RC_ST_EVENT (peers hide our car).
  roam_build adds no slots to a load with race type != 0, so the race ends
  normally (the phase-7 hold needs slots). Way back: while that race runs,
  a front-end request (0x14/0x16: the race-over or pause menu's quit)
  becomes Request(6) with [0x53F2E4] = the city's cruise (SetEvent(city,
  -1), time and weather kept); the first cruise frame puts the car back
  and holds it 1.5 s, re-placed if it falls. '[RACE]' lines;
  MC3_ROAM_RACE=0 off, =2 Request(6) alone. No AI cars until 2026-11-12
  (fix untested in game): the race takes min(.rac racers, setup +0x40)
  AI and none when setup +0x10D is 0 (.rac reader sub_001D4F20 ->
  sub_001D3AE0, 0x1D42DB-0x1D42FB; +0x10D is "AI racers on", not
  hookmen: Arcade's start sub_00184770 copies 1 from 0x4B36B3; Arcade's Go
  sets +0x40 = sub_00165F50 = min(OpponentCount, 5)); the copied Free
  Roam cruise had +0x40 = 0 (fr_direct_pass) and rra_start wrote +0x10D =
  0. Now +0x40 = min(OpponentCount, 8 - humans), +0x10D = 1; the way back
  sets +0x40 = 0. Racer records: players first (race+0x98, 0xB8 B), AI
  after (race+0x658, 0x60 B), pointers at race+0x65C. Local players are
  placed side by side at PlayerPosDir ([race+0x7E8]: x y z heading;
  sub_001CBCA0, event race, 2+ players: player i at ((i & 1) - 0.5) x
  2.5 m across, (i >> 1) x 6 m back). Group race (2026-11-12, untested in
  game): at a start point with players near, roam_challenge.c's prompt
  offers the event as a group race (its INVITE with 0 checkpoints + the
  event; v5 builds before it answer NO busy); at GO each racer loads it
  with its own AI, roam_build gives the race a slot per other racer
  (s_roam_group, no standalone cars, no phase-7 hold), our car moves to
  its join-order grid spot for 3 s, a racer not driving 10 s (after 30 s)
  or still racing 30 s after our finish gets its record's +7 (out) so the
  race ends; result (places among humans + AI) and prize back in the
  cruise. Nobody joins: alone vs the AI. '[RACE] group ...'.
  Street challenge (feature 2, 2026-11-09, untested in game;
  src/games/remix/roam_challenge.c, docs/roam-races.md F2): up to 8 racers
  (2026-11-12, untested in game; was a 1v1 headlight flash, flash back to
  accept): driving under 12 m/s within 40 m (height 10 m) of 1+ driving
  players, the prompt box offers 'Street race? N nearby players will be
  invited'; White (Q, touch 'Flash'; still also the game's headlight toggle)
  sends INVITE to the up to 7 nearest every 0.3 s for 10 s; each invitee
  sees '<host> invites you to a race (N players)' and joins with White (under
  12 m/s) within 10 s; the window closes at 10 s, when every invitee still in
  joined, or on the host's White; nobody joined = a note; else GO to each
  racer (its slot, every racer's id) and NO late to the rest; places from
  finish times (each on its own clock from GO), then checkpoints; decided when
  all still in finished, or one left 1 s past the latest finish (last), or at
  600 s; a racer gone/away/silent 5 s or saying NO is out (DNF); the result
  lists every racer. While that prompt shows, White never starts the AI race; the course is the nearest
  sprint/circuit's own checkpoints, read from its .rac at challenge time
  (tune/race/<city>/<event>.rac through the game's file system and text
  tokenizer: position + 4 sizes), the whole course from checkpoint 0 to the
  last (2026-11-11, untested in game; was ~2 km / at most 6, the ASK's 96-byte
  cap: ASK v4 now sends city + event and each side reads the course from its
  own .rac, declining on another count or start line; only the start-beacon
  fallback still sends <= 6 points; time limit 600 s). Rework (2026-11-10,
  untested in game): every car goes to the race's own start grid (2026-11-11,
  untested: the .rac's PlayerPosDir for the host, slot k its k-th Opponent
  PosDir within 40 m (2026-11-12), else rows of two behind PlayerPosDir
  (odd slots 3.5 m aside, 8 m a row), facing the .rac's dir; the course runs from
  checkpoint 0; the car matrix's row +0x28 is the car's back, not its front
  (the racer placement sub_001CBCA0 -> sub_002C8860 turns local +z to
  (sin h, cos h), h = atan2(-dx, -dz), i.e. -dir), so that row gets -dir:
  until 2026-11-12 (untested in game) both cars stood turned around; a slot
  whose dir points away from checkpoint 0 is turned; each invitee reads the .rac itself; start beacons
  only: 2.5 m to each side of the host, rows 8 m apart) and are held there for the 3 s
  countdown;
  the checkpoints are the game's own: the Free Roam cruise's race object
  [[0x533E24]+4] (vt 0x46B608) is an mcRaceBase with none, and its per-player
  draw (vt+0x110 sub_001CF3B0, from the player render sub_002BE8E0) runs the
  flare of each checkpoint in race+0x24/+0x28. The cruise's .rac has
  numcheckpoints 0 and its reader sub_001CDC80 new[]s 4 + 0x120 n bytes
  anyway, so +0x28 is a non-null empty array (count 0 at -4): until
  2026-11-11 that made every challenge refuse ('the cruise has its own (0),
  page marker instead', 12 player sessions); now it is kept aside, ours
  (sized to the course) goes in, and at the end ours is freed (sub_001B3CD0)
  and the empty one put back. We fill those (game new[]
  sub_001B3C60 + ctor sub_001CA0A0, as the .rac reader sub_001CDC80), racer
  record 0 (race+0x98) +0x34 target / +0x18 laps 1; pass = the game's gate
  test sub_001D1510 + audio event 0x15; the flares' tick is race vt+0x11C
  (sub_001CCA20 -> sub_001C8D50 -> sub_001C7740: passed state 1 fades to 3
  hidden, 2 fades in to 0; scale sub_001C77C0), called by the checkpoint
  races' update sub_001D8F20 only, never in the cruise: a passed flare stayed
  up at full size until 2026-11-12 (untested in game), now we call it each
  frame while ours are in; the HUD arrow (race vt+0xE8, a stub
  in the cruise) through the cruise vtable slot pointed at sub_001DF290
  meanwhile; countdown = HUD message queue [[player+0x58]+0x50]
  sub_000B6330 + audio event 0xC. The game's map draws no race checkpoints
  in the cruise (sub_000C2CE0 is race-type gated), so ours draw next + after
  in the map tune's colours. Page marker roam_cp_box only as fallback.
  Prompt box (web/pre.js): left, above the minimap; '\x01' first = big and
  centred (countdown, GO, 2 s of result). Messages: roam core GAME record
  (type 10, core 17, peer to peer, the hubs untouched; challenge v5 since
  2026-11-12: INVITE/JOIN/NO/GO/PROG, v4 and v5 drop each other's).
  '[CHALLENGE]' lines; MC3_ROAM_CHALLENGE=0 off, =2 no HUD/audio calls,
  =3 no native checkpoints.
  Many cars (2026-11-09, untested in game; src/games/remix/roam_cars.c,
  docs/roam-many-cars.md): peers beyond the 7 slots get standalone cars
  (the player builder, no player; pose + inputs written after the physics
  step, sub_002BDEC0 hook; in the physics level within 20 m only; parked
  and hidden when unused). MC3_ROAM_CARS=N remote cars at most, slots
  included (page 'Players shown'; auto 32 at RECOMP_RAM_MB=128, 12 at
  64 MB; 7 = slots only); also the K asked of the hub, 32 at least
  (roam_set_k; hub default_k 32 for a HELLO asking none, mc3-server
  --roam-k, worker ROAM_K). The hub links a pair only while both are
  under their K: phones asked 7 and, with 10 in a city, left the
  players beyond their 7 nearest unlinked, each the other's dimmed FAR
  blip, no car (2026-11-11, untested in game). Remote cars pool
  (2026-11-11, untested in game; src/games/remix/roam_pool.c): one table
  s_rc[] owns every remote car, slot-backed (entries 1..slots) or
  standalone, and its peer; one assignment (a peer keeps its car while
  linked + 3 s; a newcomer, nearest first, takes a free car with its look,
  its car, any, then the longest-unseen held one, slots first on a tie;
  none: a new standalone car), one queue after the physics step (one
  build/rebuild per 0.25 s, held while a state request is queued or a
  page-in runs), one look check (car list, no bike mid-cruise, the car
  cache's 16 models: slot swaps included), one heap check (free +
  cost(old car) >= keep + cost(new): keep 3 MB main heap, 2 MB cars heap;
  cost measured per model on each build, an unmeasured model the largest
  measured, at least 1000 KB; the load's slot count uses it too), one park
  (hidden, out of the physics level, 30 km off). Nearest gets a car: while
  no car can be built (count or heap), the nearest driving peer without one
  takes the farthest holder's (away = farthest) once 50 m nearer for 3 s;
  '[ROAM] nearest: ...'. Failures (2026-11-12, untested in game): a
  rebuild refused for heap room keeps the old car, waits 10 s and sets
  the pool full (it was retried every 2 s ahead of new cars, so a full
  phone never built one nor ran the nearest rule); a failed new car sets
  it full too (was a whole build every 0.25 s); a slot whose new car
  fails after its old one was deleted builds the old one again. Our pose and status go out in every Free Roam
  cruise, with 0 remote slots too (s_roam_cruise; a low-heap phone used to
  send nothing, the others never saw it). '[ROAM] pool: N peers, C cars (S
  slots + T standalone), D dimmed, heap F KB' every 30 s at most, when it
  changes. A phone (64 MB, main heap 47 MB,
  8.5 MB free at load) fits 4 remote slots ('[ROAM] heap: 4 remote cars
  fit', ~800-950 KB each, ~3.7 MB left; 1.26 MB was fatal), and only
  peers at load + 2 of them (alone at load: 2). Logs '[ROAM] cars: ...'. Cars heap
  (2026-11-10, untested in game; roam_cheap, docs/roam-many-cars.md): a
  second heap of the game's own type ('RoamCars', 48 MB of the contiguous
  window at 128 MB, MC3_ROAM_HEAP_MB, 0 off; none at 64 MB) is made
  current ([0x52B488]) around every remote car's build (the player
  manager at load, ours too; swaps; standalone cars): the main heap no
  longer bounds the players shown, the cars heap keeps 2 MB. Frees find
  their heap by address (sub_001B3D80 walks the heap list [0x52B478]).
  Not the extended VMA: car textures/vertices are GPU-read, window only.
  '[ROAM] cars heap: ...'. Teardown: the
  state pass (sub_00020EC0) removes the standalone cars whenever a state
  request is queued in the cruise (the player manager's delete has no
  override), but not for requests 1-4 (pause and resume: pause menu, full
  map; their handlers sub_00020AF0/B70/BE0 only set pause flags and call
  player methods; until 2026-11-12, untested in game, every pause dimmed
  the peers beyond the slots); a cruise that goes on rebuilds them one per 0.25 s from 1 s
  later, each a short freeze (kept: safe over cheap).
  Open: hub-distributed world settings, names over cars, ghost mode for
  traffic, the hub's O(n^2) link pick
  (nfsu2-web uses a grid), KIND_BUNDLE.

### Performance (2026-11-05)

Below 5 fps on the owner's Chrome at first; headless: title 16-19 fps,
cruise 4-9. Costs found, in order: (1) guest-lock hand-offs between workers
(main thread waits 54-89%, ~700-1900 hand-offs/s at 0.3-1.5 ms); fix in
progress: nfsu2-web's FIFO ticket lock + a short spin; (2) every guest
access is volatile + a table load: fix in progress: RAM at wasm address 0,
no volatile, nfsu2-web's `g_guest` alias trick (needs a regen); (3) log
spam proxied from workers to the page: RECOMP_QUIET=1 on the web, page log
batched per frame. Not yet tried: -O3/-msimd128/no memory growth,
nfsu2-web's lift options (FLAGABI, X87OPT, hot-callee inlining).

### WebGPU renderer and frame rate (2026-11-08)

- **The 16 fps cap was the vblank, not the renderer.** The timer thread
  takes the guest lock before each tick; in the browser it was often more
  than half a period late and kernel_vblank_tick restarted its schedule,
  dropping the tick: D3D saw 32-38 vblanks/s, and MC3 flips at interval 2.
  Late ticks are now caught up (a second one in the same lock hold if
  owed; restart only beyond 4 periods): 60.0 Hz, cruise at 30 fps.
  `[perf] D3D vblank handler: N per second (vblanks raised N Hz)`.
- **WebGL2 is capped by Chrome's GPU process** (ANGLE D3D11, ~700-900 draws
  a frame): CPU-side cuts (RECOMP_GL_VPOOL vertex/index pool,
  RECOMP_GL_VAOCACHE, RECOMP_GL_TEXFAST; RECOMP_GL_CUBO_SUB opt-in, slower
  on D3D11) moved time between buckets, the total stayed ~50-65 ms. No sync
  points on the web path (audited).
- **WebGPU back end** xboxrecomp/src/nv2a_wgpu (ADR 0011, docs/webgpu-plan.md,
  milestones M1-M4 done): a render pthread translates NV2A batches in C
  (WGSL emitters wgsl_vsh.c/wgsl_psh.c ported from gl_vsh/gl_psh, texture,
  surface and pipeline caches, one arena per frame, contract wgpu_proto.h
  WG_VERSION 2) and hands each frame to web/wgpu.js in one call. Cruise:
  30 fps, ~3.5 ms of render work a frame, 0 validation errors. The page
  picks WebGPU when an adapter answers (?RECOMP_GPU=webgl forces WebGL).
  RECOMP_WGPU_STATS=1 (per-second line, GPU timestamps), MC3_PERF's
  '[perf] GL WebGPU' line; pipeline list cached in OPFS and prewarmed.
  tools/wgsl_check.sh validates the WGSL with naga.
- **Widescreen (Hor+)**: RECOMP_ASPECT=auto|16:9|21:9|32:9 (page 'Screen
  shape'; Auto follows the canvas through a ResizeObserver ->
  nv2a_wgpu_aspect_box). Surfaces drawn 720 wide (the back buffer's pitch
  is 768: size from the drawn clip) are stored k = aspect/(16:9) wider,
  nv2a_clip squeezes x by 1/k, full-screen 2D snaps to the edges, the HUD
  keeps its shape in the middle 16:9; the game culls to its frustum, so the
  projection override (recomp_manual.c ~:1709) passes aspect*k for culling
  and restores m00. WebGPU only. Two more culls/snaps found 2026-11-09
  (untested in game): the toolkit's own CPU batch cull (cull_offscreen,
  nv2a_pb_exec.c, RECOMP_CULL) tested against the narrow 720 clip and
  dropped every batch wholly in the side areas (buildings, props missing at
  the sides; widened by k now); and the edge rule left every 2D vertex
  beyond the 16:9 edge unsqueezed, so sprites the game projects into the
  sides (glows, flares) were thrown out or smeared -- now only a
  pre-transformed draw spanning both edges keeps it (wgpu_draw.c
  wide_full_2d), any other is squeezed whole.
- **Live settings** (2026-11-12, untested in a browser): the in-game
  Settings window's live rows (web/index.html LIVE, setSetting) go through
  web/pre.js Module.mc3SetLive(key, the value buildEnv would give) into
  32-bit boxes, 0 = the env's value at start: nv2a_wgpu_live_box
  (wgpu_int.h WG_LIVE_*; WebGPU only, WebGL rows say restart):
  RECOMP_GL_SCALE x1000 (frame end: every surface made again at its next
  use, s_scale_gen, canvas height re-set), RECOMP_GL_ANISO (in the sampler
  key: new samplers), RECOMP_GL_MIPS 1/2 (next frame: every cached texture
  freed and uploaded again); mc3_live_box (recomp_manual.c): MC3_60FPS
  1/2 (applied at once, mid-drive too; was next drive only, so Free Roam stayed at 30),
  MC3_ROAM_CARS (-1 auto; roam_pool_net parks the farthest holders over it
  and gives newcomers no car while it is reached; raising builds new cars
  only up to the vehicle registry sized at the cruise's load; the hub's K
  follows); MC3_DRIFT 1/2 (drift_step, below); xbox_keyboard_box
  (sdl_keyboard.c) RECOMP_KEYBOARD 1/2; touch
  rows page-only (touch.js setMode/setOpacity). Restart stays for
  RECOMP_GPU, RECOMP_RAM_MB, MC3_LOWMEM (renderer and memory layout are
  set at boot) and RECOMP_GL_LOD_BIAS (in every fragment shader's text and
  the OPFS pipeline list's keys: live would recompile every pipeline).
- **Builds**: tools/build_web.sh uses ccache when installed (WSL:
  ~/.cache/ccache-mc3, CCACHE_BASEDIR = repo, so build folders share it;
  CCACHE=0 off). Builds on /mnt/e are slow; one relift per round.

## Lessons inherited from nfsu2-sw (verify for MC3)

- **Guest threads race** (streaming systems handing half-filled blocks to
  workers): the guest lock (GIL, `RECOMP_GIL`) runs one guest thread at a time.
  MC3 streams a whole city: expect this class of bug.
- **AArch64 Linux address space** (found 2026-10-14, MC3): mmap is top-down,
  so the guest base landed 3.8 GB under the 48-bit limit and every mapping at
  base + guest VA above that (contiguous window, NV2A/MCPX/flash apertures)
  failed with error 8. Fixed in xbox_memory_layout.c by probing for a free
  4 GB hole first. Watch for `failed (error 8)` lines at boot.
- **AC'97 reset in "plain" mode** (found 2026-10-14, MC3): DSOUND writes the
  channel reset bit (NABM +0x0B, RR) and reads it back once (MSVC hoisted the
  load), so the toolkit's after-the-fact clearing thread loses and DirectSound
  spins forever. src/ac97_nabm.c models those registers through the MMIO hooks
  (DSOUND is lifted with them): RR never lands. Needs DSOUND in
  MC3_MMIO_SECTIONS.
- **Lifter: movsx from bp/sp** (found 2026-10-15, MC3, also upstream): was
  lifted as a zero extension. Bink IDCT corruption + 5 game-code sites.
  Fixed in tools/recomp/lifter.py with a regression test. Found by
  `make difftest` (src/difftest.c + tools/difftest.py: replay one captured
  call on real x86 in Unicorn and name the first wrongly written bytes):
  the tool to reach for whenever output is subtly wrong.
- **Movies:** replacing the lifted decoder with a native one (FFmpeg) was a big
  win. Find MC3's movie format early.
- **Performance levers:** register/x87 locals in the translator, spin-loop
  hints, native replacements for hot functions, shader/pipeline cache, LTO,
  Vulkan (NVK). For MC3 (2026-10-27): LTO gave nothing in rain races
  (22.0 vs 22.9 fps; the GL thread is the limit), removed; Vulkan (NVK)
  ran 14 fps with visual bugs, reverted.

## MC3 facts

(Fill in from tools/first_lift.sh and bring-up.)

- Disc: NTSC-U retail, `default.xbe` "Midnight Club 3: DUB edition" (TT-121),
  4.9 MB XBE, 3.4 GB extracted. The European (PAL) disc (Redump "Europe
  (En,Fr,De,Es,It)") runs on the same lift (2026-10-24): its XBE differs in
  531 header bytes only (signature, certificate region 0x4 and version 2,
  section digests; code identical), its data in three files
  (flash/pause_x.xbck, video/Story/Suschamp.bik, video/Tutorial/Trlogos.bik).
  config/dub/xbe_editions.h lists every accepted XBE (FNV-1a 64) for the boot
  check, which logs `disc: edition <name>`. Discs side by side: game/ and
  game-<edition>/ (gitignored). Which plays (tools/env.sh, windows_host.c):
  EDITION= first; else the only disc; of several game/, then the lifted
  edition's, then the first by name (noted once). Languages come
  from XC_LANGUAGE: RECOMP_LANGUAGE=fr|de|es|it (kernel_xbox.c; the title
  screen's "Press Start" stays English). Data: `assets_x.dat` (+ `_1`..`_5`),
  `banks.dat`, `audio/` (XACT `.xwb`/`.xsb`), `flash/`, `resources/`.
  Other XBEs on the disc (`dashupdate.xbe`, `downloader.xbe`, `update.xbe`)
  are Xbox system/online tools: not lifted.
- XBE: entry 0x000129DF, title ID 0x54540079 (config/dub/xbe_facts.h), retail,
  built 2005-03-25 (`C:\soft\mc3\src\mcproduct\xbox_final\mc.exe`), XDK
  5849 (same as NFSU2's era), image 5.23 MB, base 0x10000, 154 kernel imports.
- Code: `.text` 3.35 MB (game). Statically linked XDK/middleware, each its own
  section: D3D, XGRPH, D3DX (graphics); DSOUND, XACTENG (XACT audio), WMADEC
  (WMA decoder), DOLBY (APU DSP code); XPP (pads); BINK + 14 BINK* blitter
  sections + BINKDATA (Bink video player); XNET, XONLINE, XON_RD (Xbox Live:
  stub, never needed offline).
- MMIO sections (`MC3_MMIO_SECTIONS`, default `DSOUND,XPP`): the libraries
  that drive hardware registers directly, as in NFSU2.
- Kernel imports worth watching: `XeLoadSection`/`XeUnloadSection` (sections
  loaded on demand), `Phy*` + XNET (network: stub), `Io*` device creation
  (XAPI internals), `Xc*` crypto (saves/signatures), `MmClaimGpuInstanceMemory`.
- Movies: **Bink** (`video/*.bik`, 64 files), played by the statically linked
  Bink library (BINK* sections). FFmpeg decodes Bink, so the nfsu2-sw route
  (native decoder in place of the lifted one) applies.
  src/bink_native.c replaces only the video decode (0x0037CCD0); Bink keeps
  I/O, timing, audio and the YUV→RGB copy. Bink's frame buffer is Y, **V**, U,
  packed (pitch = width); FFmpeg's binkvideo is bit-exact with it.
  `MC3_NATIVE_BINK=0` brings back the lifted decoder. Native Linux only:
  the browser build has no FFmpeg and plays the lifted decoder.
  Result (2026-10-15, windowed, virgl): movies at a steady 30 fps; per frame
  decode <1.5 ms, Bink's lifted YUV→RGB copy 4–7 ms, draw+Present ~11 ms,
  ~15 ms spare. `MC3_BINK_TRACE=1` prints this split every second.
- Movie sound: Bink decodes it correctly (matches FFmpeg, r = 0.99999), but
  the way out (Bink's DirectSound ring → emulated APU voice → output)
  repeats 256–768-sample pieces, skips, and runs at ~84% speed. So FFmpeg
  plays the soundtrack on a runtime software-mixer voice (`apu_mixer_*`),
  synced to Bink's frame number, and Bink's decoded PCM is zeroed
  (`MC3_NATIVE_BINK_AUDIO=0` to compare). `tools/audio_diff.py capture.wav
  movie.bik` measures holes, skips, repeats and drift against the original
  track. The renderer is set up at boot (Nv2aBackend.prepare): set up at the
  first flip, the Rockstar logo stayed black ~1 s while its soundtrack (tyre
  screeches) already played.
- In-game audio (XACT, music WMA from `audio/XStreams/mx.xwb`) works since
  2026-10-17, after four fixes, each found by measuring:
  1. lifter: a jcc after a join of different-width cmps lifted as
     `if (_flags)` (never taken) -- XACT refused every sound start;
  2. bus addresses: heap PCM (0x0120FF80) was read from the contiguous window
     at the same offset (noise); MmGetPhysicalAddress now tags RAM (bit 26);
  3. APU GP/EP memory read as 0, so DirectSound found the EP "dead" (P-memory
     word 6 != 0xCCCCCC) and reset both DSPs with a 10 ms stall from every
     XACT call: half-speed APU, menu lag, garage hang. Now storage, and the
     stub EP reads alive;
  4. (movies) see above. Tools: `MC3_XACT_TRACE`, `MC3_WMA_TRACE`,
     `RECOMP_APU_TRACE` (adds `[APU-ST]`: samples/s, stream consumption,
     pacing), `RECOMP_AUDIO_DUMP=path,start,secs` (written at exit/Ctrl+C).
- Rendering found and fixed 2026-10-17: vertex programs read the viewport
  scale/offset as constants c[-38]/c[-37] (NV2A mirrors the registers there:
  cars were invisible); a narrower clip into the back buffer (race minimap,
  175x451) rebuilt the surface (black race screen). Find such bugs with
  `make run WATCH=1 RECOMP_GL_WATCH_N=1500 RECOMP_GL_WATCH_SHOTS=<prefix>`:
  F12 dumps every draw and clear of the frame, images every 50 draws.
- Motion blur (fixed 2026-10-18): MC3 reads the depth buffer (0x80A24000)
  as a colour texture (LIN_A8R8G8B8 / LIN_A8B8G8R8) to weight its blur.
  Depth only existed in GL, so the mask was garbage and races smeared at
  speed; nv2a_gl now packs GL depth into those formats
  (RECOMP_GL_DEPTH_TEX=0 to compare). A race frame is ~1800 draws: use
  RECOMP_GL_WATCH_N=4000 for a whole one.
- Picture quality (GL, 2026-10-18; ideas from nfsu2-sw's Vulkan renderer):
  square pixels in 16:9 (RECOMP_GL_SQUARE; 1280x720 at RECOMP_GL_SCALE=1.5,
  the sweet spot for a 720p screen), the title's mip levels + trilinear +
  16x anisotropic + -0.25 LOD bias (RECOMP_GL_MIPS/_ANISO/_LOD_BIAS), FXAA
  and CAS sharpening in the present pass (RECOMP_GL_FXAA, RECOMP_GL_SHARPEN),
  and the 4:3-to-16:9 fill: MC3 lays menus and videos out in the middle 640
  of 720 columns and clears the rest; black side bands -> the middle is
  shown full width (RECOMP_GL_FILL), except the boot movies
  (MC3_FILL_SKIP=rockstar,sdlogo,mc3intro). MC3 has no 720p mode (no
  1280x720 in its code). Movies upscaled 2x on the GPU (MC3_MOVIE_HD,
  RECOMP_GL_MOVIE_SCALE; first test pending). HD video packs made from the
  game's videos can't be shipped.
- Input must reach the game as native controls do (the emulated pad), so
  the game decides what can be skipped: the Rockstar and Rockstar San Diego
  logos cannot, mc3intro can (Enter). The keyboard is pad 1 by default
  (src/main.c; RECOMP_KEYBOARD=0 off). `make run WATCH=1`: F12 writes every
  draw of the frame on screen to the log (a diagnostic; nothing visible).
  Keyboard (Linux since 2026-10-16, input/sdl_keyboard.c; relaid
  2026-10-21 so buttons sit where their names say): A/B/X/Y = buttons A/B/
  X/Y, Enter = Start, Backspace = Back, arrows left/right = steer (left
  stick), up = gas (right trigger) + stick up, down = brake (left trigger)
  + stick down (menus navigate with the stick; while driving, where the
  title says so through xbox_keyboard_driving -- the Remix does -- the
  arrows are the triggers alone, RECOMP_KEYBOARD_STICK_Y=0/1 forces), I/J/K/L = d-pad (songs and
  map in a race), Q/E (or Shift/Ctrl) = White/Black (the Switch's L/R),
  Z/C = stick clicks, T/F/G/H or numpad 8/4/2/6 = right stick, Space = A
  (MC3's handbrake, the Switch's A), Tab = Back too. Unattended runs only:
  `MC3_SKIP_MOVIES=1` pulses A+Start on the pad while intro movies play.
  `MC3_SKIP_MOVIES=hard` (the page's "Skip intro", 2026-11-06) plays none
  of the boot movies, logos included: BinkOpen marks the HBINK, and its
  BinkDoFrame/CopyToBuffer/NextFrame/BinkWait return at once with
  FrameNum = Frames, so the movie loop (Remix sub_003638A0) returns 3
  (ended) and the movie state goes on as after a whole movie. Opening
  failed (NULL HBINK) would not do: the movie state then requests state
  0x11 instead. DUB uses the same hooks, not yet run there. Browser only
  (2026-11-11, untested in a browser): hard also hides the legal screen
  (flash/legal_x.xbck, red, scrolling copyright text; the loading loop
  sub_0001B9D0's first run): no timer, it lasts the boot load (main sets
  +0x4004 via sub_0001AD50 and waits for the thread), then plays
  StartOfTransitionOut -> EndOfTransitionOut. Players' logs: ~78 turns
  (~2.6 s) warm, 5-25 s of disc waits under it cold. Now each turn is
  input poll + BeginScene + Present + Sleep(24), no movie drawn, and the
  stop leaves with +0x400C hidden (no transition-out); '[LS] legal screen
  hidden ...: N turns, S s of boot load'. Opt-in since 2026-11-12
  (MC3_SKIP_LEGAL=1): players' profiles went missing at the title after it
  shipped; it skips no file I/O (the left-out per-turn calls set D3D
  state), the cause is not proven. The page asks navigator.storage.persist()
  (disc_cache.js): best-effort OPFS (saves + ~4.4 GiB cache) is evicted
  whole under disk pressure. The
  cold wait stays: its reads (assets_x.dat ~287-310 MB, _3 ~75-132 MB,
  _1 ~3 MB) are one 1 MiB block at a time under the guest lock.
  `MC3_AUTOSTART=cruise` (src/autostart.c, the page's "Start in Cruise",
  2026-11-05, owner test pending) presses from boot into Arcade, Cruise
  with the first profile: each press when its screen's Flash file has
  opened (toolkit `xbox_path_hook`: bg_savegame = title/profiles, bg_main,
  bg_arcade, loadscreen_arcade*), stops in game (state 2/3), on a wrong
  screen or on any input of the player's (`usb_gamepad_host_input`).
  No profile: it stops and says so. Log lines `[AUTOSTART]`.
- Debug switches (toolkit): `RECOMP_CLOCK_RATE=<f>` (+ `RECOMP_CLOCK_ONLY=
  tsc|qpc|tick|sys`) runs the guest clocks at f x real time -- tells clock-
  timed from frame-paced; `RECOMP_IO_MBPS=<n>` (+ `RECOMP_IO_SEEK_MS=<m>`)
  slows reads to a DVD's pace -- for streaming bugs hidden by fast storage.
- Threads: the guest lock serialises guest code. On the Switch each
  hand-over is a scheduler round trip: behind the legal screen (a loader
  thread at work) the main thread waited 66-75% of the time in short waits,
  4 fps. Uncontended lock calls (mutant/critical section) now keep the lock;
  console measurement pending. `MC3_PERF=<s>` reports lock waits, CPU per
  thread (Switch), APU pacing; `pkill -USR1 mcdub3` snapshots the main
  thread's stack (`tools/stackwalk.py`).
- Frame pacing (fixed 2026-10-18): the first flythrough (1.55 s vs 5.5 s on
  hardware), menus and garage ran 2-4x fast on Linux, with every clock and
  the frame time right. The game asks D3D for presentation interval 2
  (30 fps) there and advances per frame, so the presenter must honour D3D's
  flip targets: sub_003E4EF0 (retirement) reports to nv2a_pb_flip_retired.
  That only works once D3D's PGRAPH handler (sub_003E5540) runs under the
  executor's trap lock and reports traps taken: its write-one-to-clear ack
  is a no-op on plain memory, and the trap storm (412k flips, targets
  millions of vblanks ahead) wrecked the flip queue. D3D device 0x003EE718:
  vblank count +0x1C0, last target +0x1C8. `MC3_FLIP_TRACE=1` logs the
  pushbuffer callback (sub_003E52D0; param & 0x1F = command, 1 = flip,
  (param >> 5) & 7 = interval); `MC3_FLIP_PACE=0` / `MC3_TRAP_HOOK=0` undo.

- Career loading-screen hang (found 2026-10-21, fix under test): the main
  thread spun in D3D's fence wait (sub_003DF4D0, loop at 0x003DF607) while
  the loading-screen thread kept drawing. The loop keeps the time handed out
  (+0x2C of the push-buffer state 0x003ECAF0) from before its poll yield;
  the GPU ran past it and `handed - gpu` wrapped. src/games/dub/recomp_manual.c ends
  the loop through a spin hook (recomp_set_spin_hook) once the GPU has
  passed the fence (MC3_FENCE_FIX=0 off). `pkill -USR1 mcdub3` now also
  prints the main thread's kernel call and that fence state.
- Boot check: MC3_XBE_SIZE / MC3_XBE_FNV64 (config/<game>/xbe_facts.h)
  and xbe_editions.h refuse any other XBE.

- Career race crash (found 2026-10-21, fixed 2026-10-22; every career race,
  every host): the race-event listener sub_0023D630 leaves through `jne`
  onto `pop edi; pop esi; ret 4` at 0x23D72D, bytes the detector left in a
  falsely split neighbour (gap_prologue at 0x23D71F), so the target was a
  ret-only stub that skipped both pops. The broadcast loop (sub_00251CD0,
  called by mcPgCareer::ReportRaceResult sub_0024F8C0) then walked on with
  a garbage esi into a null call. The translator now lifts such epilogues
  (21 in MC3; test_stub_epilogue.py). Found with src/games/dub/recomp_manual.c's
  broadcast replacement, which checks every listener call for a broken
  calling convention (`[LISTEN]`, MC3_LISTEN_TRACE=1; MC3_LISTENER_CHECK=0
  runs the game's own loop).

- Saves (2026-10-22): a profile is UDATA/54540079/<12 hex>/<name>.mc3
  (99,162 bytes) + SaveMeta.xbx (UTF-16 "Name=<name>"). The .mc3 starts
  with a format stamp (Xbox 0x02F59447) and ends with HMAC-SHA1 of the
  rest under the kernel's XboxSignatureKey-derived key; the port's keys are
  zero, so its key is all zeros and a real Xbox's profile reads "damaged".
  The Remix writes <name>.mc3Remix instead (same folder scheme; web page
  lists, downloads and imports it, 2026-11-11; its size/stamp are not
  proven, the writer sub_000193F0 streams more than 99,162 bytes, so the
  page checks the layout only; tools/save_import.py is DUB-only, .mc3).
  `make import-save FILE=` (tools/save_import.py: .gsx = gzip + 128-byte
  GSXM header + FATX memory unit with 16 KB clusters; folders; .mc3; .max)
  re-signs and installs, backing up first; verified in game. PS2 saves
  (.max = LZARI, .sps/.xps = SharkPort, plain; profile BxSLUS-nnnnn<name>,
  no signature) can't be used: the same game's PS2 release (SLUS-21029)
  writes stamp 0x02F6DC47, ~61 KB of data (+ 0xCD fill to 67,836), about a
  third of its words in line with the Xbox's; the Remix (SLUS-21355)
  0x00512C4D, about half. The tool names them and refuses. CodeBreaker
  .cbs (RC4 + zlib) is not read.

- Loading-screen circles (fixed 2026-10-24, every host): the loading
  thread (sub_0001B500) advances its Flash movie a fixed 1/30 s a turn
  and, each frame, copies the frame just shown into the next back buffer
  with the 2D engine (NV062 surfaces on subchannel 3, NV09F blit on 2,
  720x480) before drawing the spinner's new segment. The toolkit ignored
  subchannels other than 0, so each buffer got every other segment
  (stutter, circles never closing). The executor now does the blit, in
  guest memory and in nv2a_gl (RECOMP_BLIT=0 off). On fast storage the
  load ends during the screen's intro, and the transition-out plays with
  its shapes off stage: the screen cuts away with the spinner up. At a
  DVD's pace (RECOMP_IO_MBPS=8 RECOMP_IO_SEEK_MS=80) it plays out: that
  is the game, not the port. Tools: RECOMP_GL_FRAMETIME=2 (per frame:
  draws/clears/blits per surface, the one shown), RECOMP_PB_FRAMESTATS=1
  (methods per subchannel, batches culled/drawn, off-screen vertices),
  MC3_LS_TRACE=1 (the loop's turns, its transition labels).

- **Remix** (`GAME=remix`, from 2026-10-24; ADR 0006): *Midnight Club 3:
  DUB Edition Remix*, Europe (Redump "Europe (En,Fr,De,Es,It)"), XBE built
  2005-11-18, region 4, version 6, the same title ID 0x54540079 and XDK
  5849. Its libraries are the DUB Edition's code at new addresses (D3D
  277/281 functions identical, DSOUND 468/473, XACT 353/366, Bink decoder
  0x0037CCD0 -> 0x003B2F40); its game code is 3.57 MB (+222 KB), 10,733 of
  17,839 DUB functions identical, 3,050 changed. Lift: 21,424 functions,
  0 failed, 327 unresolved, 271 unimplemented. Disc: 909 files, 4.5 GB,
  +160 Flash screens, +42 movies, +10 resource packs, same formats.
  Overrides: ported from the DUB Edition's by tools/port_overrides.py
  (123 of 129 addresses by themselves; config/remix/port_map.txt: memmove
  0x002DB4C0, loading-screen thread start 0x0001BC10, heartbeat Sleep
  return 0x001B1654). Data: D3D push-buffer state 0x00422D80, frame time
  0x004B2D70 (config/remix/game.h). Watch: the
  disassembler starts a function at 0x0001BC1C, inside 0x0001BC10 (a false
  split, as the career race crash's).
  Bring-up (2026-10-24): (1) six-digit addresses (0x482AA4) went across
  unported -- port_overrides.py reads 6-8 digits now; (2) fault parsing
  tune/progress/unlockingrules.csv: the cell converter 0x0028B850 (vtable
  method after a switch's tables) was split into ten fragments by the
  gap-prologue pass -- toolkit fix 15dcf3d (padding after switch tables is
  a boundary; DUB's lift changed too, still plays); (3) XACT's DSP command
  wait: the doorbell (DirectSound's command block + 0x810) moves with the
  allocations -- 0x83B10000 widescreen, 0x83A5C000 4:3 in the Remix -- so
  both games register it at run time (overrides of the command wait,
  DUB sub_0038DAA3 / Remix sub_003C3D23, toolkit apu_dsp_ack_add); a fixed
  RECOMP_APU_DSP_ACK hung one layout and zeroed a stray word in the
  other; (4) seeds 0x00027490,
  0x003D9EF3 (the DUB seeds 0x000271C0, 0x003A3C73, found by their bytes).
  (5) the state machine (object at 0x00557ED0, state at +4; DUB
  0x0051886C) never left state 1: "enter state 4" (sub_00021D30) stores
  the state through esi, and the per-frame movie function returned with
  esi/edi popped as zeros -- BinkDoFrame came back 0x8128 bytes of stack
  short because Bink's audio decoder sub_003AE8C0 was split by a false
  start at 0x003AEA35 (data in XGRPH's tail decoded as a call to it).
  Excluded in config/remix/not_functions.json (toolkit --exclude-functions,
  9d7a8a4). (6) seeds 0x0013A770, 0x0016F9C0, 0x0018F9D0 (gap functions
  reached only through pointers; the first two are the DUB seeds
  0x00123420 / 0x00157850's twins). The Remix then plays its boot movies
  and reaches the title screen (PRESS START, city flythrough) at 30 fps.
  The Remix uses SSE (movss/mulss/comiss/cvttss2si) where the DUB Edition
  used x87; the lifter handles what was met so far. Tools: MC3_TABLE_TRACE
  (CSV table loader), MC3_STATE_TRACE (both games: state requests and
  changes), MC3_ABI_CHECK (Remix: callee-saved registers and stack across
  the movie path); tools/match_lifts.py, tools/port_overrides.py.
  USA disc (2026-10-25, ADR 0009): its code is the European one's but for
  one branch, `ja` -> `jae` at 0x001525FC (cmp byte [0x0053F80C], 0x1E,
  in sub_00152420); .rdata differs in 19 bytes (read from the disc). Both
  run on the European lift: config/remix/edition_variants.json, the
  translator's --edition-variants, g_recomp_xbe_variant set by main.c
  ("disc: edition usa, branch variant 1"). `make xbe-edition XBE=...`
  compares any new disc.
  Races (fixed 2026-10-25): (1) from the countdown the camera went to a
  top view and turned for good -- `fucomip; lahf; test ah, 0x44; jnp`
  (the "look around" input == 0 test in sub_002C48C0) lost its lahf, the
  lifter had no AH for x87 compares; two false alias starts on such
  lahfs excluded (config/remix/not_functions.json). (2) The camera
  pulled in and up with the gas: no lifting bug -- every update and draw
  function replayed exact -- but the keyboard's up arrow also pushed left
  stick up, which the Remix reads for its race camera (its chase camera
  is a dolly zoom: SSFov 40-60 deg with speed, distance x0.8). Found by
  playing to the spot and pressing F10: `make run GAME=remix
  MC3_DIFFTEST_CALL=key MC3_DIFFTEST_FUNC=<hex,...>`, then
  `DIFFTEST_ORDER=time tools/difftest.py <capture>` (earliest divergent
  write first); MC3_CAM_TRACE=1 (camera methods per second, projection
  FOV per viewport), MC3_KEYS_TRACE=1.
  60 fps game logic (2026-11-08, `MC3_60FPS=1`, off, untested): races
  run fixed steps (0x534770 = 1, step 0x4B2D58 = 1/(rate*n) = 1/30 from
  sub_0020BB00, called by the race setup sub_00020880); in states 2/3 the
  sub_0020B6F0 override halves it to 1/60 and sub_0041B550 rewrites flip
  interval 2 to 1. Risks: per-frame (not per-step) lerps and Flash HUD run
  2x, replays/ghosts, System Link peers in different modes.
  Drift mode (2026-11-12, v2 assist untested in game; MC3_DRIFT=1 normal,
  =2 strong, live: page "Drift mode" Off/On/Strong = live word 1/2/3;
  src/games/remix/drift.c, drift_step from the sub_0020B6F0 override):
  player 0's car only ([[0x558814]+8]+0x14 = veh). Car physics = AGE
  vehCarSim (vtable 0x44EA64; mcCarSim 0x456550, ctor sub_00121550) at
  [veh+0x50], one per vehicle (vehicle ctor sub_000FF160); tune FileIO
  sub_000A3CA0 (Mass +0x24, CenterOfMass +0x18, InertiaBox +0x34, ...):
  WheelFront [sim+0x54], WheelBack [sim+0x58] = wheel tune objects (vtable
  0x44D6C8, ctor sub_000A69C0, fields sub_000A6DA0: +0x24 SteeringLimit,
  +0x34 BrakeCoef, +0x38 HandbrakeCoef, +0x3C SteeringOffset, +0x64
  OptimumSlipPercent 0.14, +0x68 StaticFric 2.5, +0x6C SlidingFric 2.3
  defaults); wheels point at their axle's ([wheel+4], sub_000A28B0).
  Friction curve sub_000A6220 (from the wheel force sub_000A8770, long and
  lat slip each): up to StaticFric at the optimum slip, SlidingFric past
  it, times the ground grip [wheel+0x118] (recomputed each step). Wheel
  steer angle [wheel+0x140] = -SteeringLimit x input (sub_000A6900). Body
  (phInertialCS) [[[veh+8]+0x68]+0x10]: +4 mass, +0xC inertia, +0x28
  angular speed cap, +0x2C momentum, +0x38 angular momentum, +0x44
  velocity, +0x50 angular velocity (world), +0x5C 3x3 rows right/up/back
  (SetVel sub_003432B0, SetAngVel sub_00343300). Inputs [[veh+4]+0x34]
  +0x6C steer, +0x70 throttle, +0x74 brake, +0x78 handbrake (sub_002C9570).
  Assist per frame from slip angle beta and speed (ideas from nfsu2-web's
  drift assist, no code): rear grip factor fr (Sliding x fr, Static x
  0.6+0.4 fr) gripping 0.92 -> drift 0.62 over 0..6 deg, minus 0.10 x
  throttle, 0.50 for 0.35 s after a handbrake tap or a lift-off with the
  wheel turned, back to 1.05 past beta_max 40 deg (+25); front SteeringLimit
  x up to 1.6 and SlidingFric x up to 1.15 with slip; past beta_max and
  growing, the yaw rate is cut (w and L together); half the speed a slide
  scrubs off per frame is given back (not braking, not a wall); strong
  level looser. Each value re-applied each frame (the upgrade blend
  sub_000A3A90 rewrites tunes), originals back when off. '[DRIFT] car:
  ...' once per car (layout check), '[DRIFT] drift N s, max A deg, ...'
  per drift. Not found: the speed-sensitive steering's owner (vehInput
  tune SSSValue +0x10 / SSSThreshold +0xC, FileIO sub_002C9840, vtable
  0x47681C; also SteerRate/HandbrakeRate/DriftRate). Other tune binders
  seen: sub_00124B70 (bike Lean/Wheelie/Drift/Burnout), sub_0007C350
  (traffic TireGrip), sub_001CAEF0 (AI).
  Drift scoring (2026-11-12, untested in game; src/games/remix/
  drift_score.c, fed by drift_step with Drift mode on or off): a drift =
  slip > 10 deg above 30 km/h, not in free fall, race type 0 only; points/s
  = 0.6 x clamp(slip, 10, 60) x m/s; banked x mult after 1 s under (or
  stopped), mult x1..x5 per 2,500 chain points (next drift within 1.5 s);
  speed lost > 15 m/s^2 (25 braking) or a spin-out (b180, the signed angle,
  crosses side-on by 10 deg from the start's side) loses it; under 100
  points dropped. Online (browser): each 10,000 of the lifetime total
  (profile.bin v4, s_on_drift) pays $10,000 into +0xACC and saves. HUD
  drift_hud_box -> web/pre.js bottom-centre (payout big, green). '[DRIFT]
  score: ...'. Free fall assumes world y up (vertical speed falling > 7
  m/s^2 for 0.1 s).
  Online O1 (2026-11-08, single player, untested; src/games/remix/
  recomp_manual.c "Online (O1)"): main menu (sub_0018C770) item 3
  CM_Networking ('Online') opens the original screen 0x1E, where
  Internet_Play ('Free Roam', callback override sub_00180160) runs Arcade's
  path (sub_0018C530_gen -> screen 0x19, Cruise) in an Online session and
  LAN_Play ('Online Races', sub_001801C0) is the game's System Link,
  untouched. Browser only (2026-11-11, untested in game): the item reads
  'Online (N)', N = the switch's players push freeRoam.total (web/pre.js ->
  mc3_online_count_box each second, -1 = plain 'Online'; set at each menu
  build through the SetLabel override, not live), and picking it runs
  sub_00180160 straight (no screen 0x1E); held White (Q/Shift) while
  picking it opens screen 0x1E for Online Races. Since 2026-11-11
  (agent-run in Chrome: cruise loaded, car drove, White opened 0x1E) the
  browser's sub_00180160 also skips Arcade's setup
  (0x19, shown a frame at most) and Start (0x1B) screens (fr_direct). The
  callback still Goes to 0x19 (sub_0018C530_gen): a main-menu item callback
  that changes no screen hangs the game (main thread looping in the menu's
  Flash event dispatch sub_00201990 under sub_0018CC90, guest lock held, no
  frame; 5428501). The steps run at the next state pass (sub_00020EC0,
  fr_direct_pass, state 7 only): sub_0013AB20 (the profile's
  car into the settings [0x53F2E8]) + sub_0013A590, San Diego Cruise,
  event by name sub_0026B010 + SetEvent, then the Start screen's own
  sub_00184FD0 (stand-in screen, +0x1E0 = 0; 0x4B36C4 put back): copy
  into [0x53F2E4] and Request(6), as the main menu's Request(0x12) item
  does; '[ONLINE] Free Roam: straight into ...'; MC3_FR_DIRECT=0 brings
  the screens back. No Arcade item in the browser (2026-11-11, untested in
  game): main_menu_online_first drops the slot whose callback is
  0x0018C530 and lowers the count (every list routine reads it live);
  MC3_ARCADE=1 keeps it; Free Roam still runs Arcade's path. The Online session: money ([[0x557F0C]+0xC]+0xACC) 2,000,000, also at each
  garage entry (state 5, sub_00021A60); no saves (the writer is
  sub_00022040 -> sub_000193F0; sub_0001A1F0 *reads* the old .mc3 format);
  Career/Tokyo/Arcade (sub_0018C560/580/530) end it and put money and the
  garage blob (0x18340 B at 0x53F308, saved raw) back. The main menu has
  no profile applied (money 0, the progress ctor's; +0xAD0 = 30000 start
  money); its Garage applies it (sub_00019560: reader sub_00019EC0 via
  sub_00022310 op 1, garage -> 0x53F308, career sub_00277E20, then
  sub_0026CBB0), which used to end the session (garage showed the career's
  ~$6k); now that override keeps the session and re-snapshots the loaded
  career. Unlocks (fixed 2026-11-09, untested): the game's unlock flags,
  set while Online, restored after. The cheat handler sub_001923B0's flags
  are 0x53F2ED = every city (sub_00289CC0, rule type 5 'C') and 0x53F2EC =
  every race (sub_0028A1B0, type 6 'RACE_C'): not cars or parts (rule type
  names at 0x4B3A18), so cars stayed locked. Cars: 0x53F2EE (VP checks
  sub_00289D20/D40/D80, the showroom sub_00154710/sub_001550B0); parts,
  paint, rims, perf levels: 0x53F2F2, the garage's own "all unlocked"
  flag; no game code writes either. Log '[ONLINE] unlocks applied: N cars
  (of M in the showroom)', N from sub_00289D80 per showroom car.
  Progression (2026-11-12, browser only, untested in game;
  src/games/remix/online_progress.c): still locked Online were the driving
  abilities (rule types 0xB-0x10, names at 0x4B3A18: ZONEAGROROAR, NITRO,
  SST, BO, 2WD, IAC; checks sub_00289EB0 (special, ret 4) and
  sub_00289E10..E90 (0xC..0x10), 1 if 0x53F2F0 else the rules vs career
  progress; asked at car setup sub_001240F0/sub_002BF920, so they count
  from the next car build) and the reward paints (sub_0014C180 asks the
  GENERIC check sub_0028B4E0 by name, Pearlescent_reward 0x45AFF8 /
  ColorShift_reward 0x45AFE4). Overrides answer 1 while Online for what
  the races won reached: 1 Nitro, 2 SST, 3 BO, 4 Pearlescent, 5 2WD,
  6 IAC, 8 ZONE/AGRO/ROAR, 10 ColorShift. Prize 1st/2nd/3rd $25k/$12k/$6k
  (street race x racers/2, needs >= 2 racers and our finish; rch_result);
  AI race: our place at the race flow's finish state (sub_001C2B00 ->
  sub_001CFC50, the career prize routine, a no-op outside game mode 2,
  wrapped and run as is), racer record race+0x98+i*0xB8 (+0x14 place,
  1-based; i = sub_002694D0). Money into +0xACC and profile.bin saved at
  once (version 3 = + wins; 1 and 2 load with 0). Prompt box: the street
  result's last line, or 8 s back in the cruise (rra_prompt with nothing
  else to say). '[ONLINE] progress: ...'. The garage shows and
  spends [[0x557F0C]+0xC]+0xACC (sub_00152420 prints "$%d.00"; Flash
  var Fortune, read back on updateMoney). Strings: the language's table [0x507E08 +
  4*[0x507E08+0x28]] (buckets {hash, entry, next}, entry+8 = UTF-16; hash
  sub_000EA710 = one-at-a-time on signed chars) is rewritten in place once
  loaded (Xbox Live -> Online, ...; CM_Networking -> "Online").
  sub_000EA750 adds an entry for a missing key: don't use it to probe.
  The main menu copies its labels at build time (SetLabel sub_00348D70 ->
  wcsncpy sub_002DB958), so the rewrite also runs from a sub_0018C770
  override before the menu is built. Sign-in texts (Who_Signed_In, Not
  Signed In, Sign-In Failed) show the page's nick (MC3_PLAYER_NAME, ASCII,
  cut to each text's length; empty -> "Player").

### First lift (2026-10-14)

21,585 functions, 0 failed, 2.98 M lines of C in 87 chunks (`--split 250`).
49 poll loops hinted, 733 MMIO functions (DSOUND, XPP), 328 unresolved call
targets stubbed (`recomp_stubs_unresolved.c`: addresses called but not found
as functions; fix with seeds when they are hit). All 89 files pass a clang C11
syntax check. For scale: Burnout 3 lifts to 22,097 functions.

353 unimplemented instructions (40 mnemonics): no-ops that print `[UNIMPL]`
when reached (`RECOMP_UNIMPL_TRAP=1` stops at the first). Mostly data decoded
as code (D3DX, WMADEC, XON_RD tables); the few likely-real ones are tracked in
TODO.md.

## Status

- 2026-10-11: skeleton; toolkit vendored and verified; lift pipeline
  smoke-tested on the toolkit's test XBE.
- 2026-10-14: disc in `game/`; XBE parsed and fully lifted in the VM (above).
- 2026-10-14: first Linux build OK on Ubuntu 22.04 ARM64 / GCC 11: all 87
  lifted chunks + runtime link into `work/build-linux/mcdub3` (39 MB
  aarch64 ELF). Only warnings, all in the toolkit (printf `%lu` vs `DWORD`,
  `-Wnonnull` on `XBOX_TO_NATIVE`, comments, unused code). Next: first run.
- 2026-10-17: music and sound in menus, garage and races; cars drawn; race view
  and HUD drawn (racing itself not yet: open); garage reachable; movies native on the console; boot
  black screen gone (RECOMP_NET_LINK=0).
- 2026-10-18: frame pacing fixed (flythrough, menus, garage at hardware
  speed on Linux; boot black screen short again). The Switch "1.5x scale
  glitches" (menu, RECOMP_GL_SCALE=1.5) were the slow menu navigation fixed
  on 2026-10-17 (APU EP alive, In-game audio fix 3): closed.
- 2026-10-18 (Switch): legal-screen stall fixed -- MC3's heartbeat thread
  (sub_00196230) polls with Sleep(0) and took the guest lock back at every
  main-thread kernel call; a guest yield now gives way for a quantum
  (RECOMP_YIELD_QUANTUM_MS). Boot black screen fixed -- Mesa 20.1 on the
  Switch has no program binaries (Linux does: progcache-gpu.bin), so the
  shader list (4.8 s) compiles a program a frame behind the legal screen
  (sub_0001B740 -> nv2a_gl_prewarm_start; RECOMP_PROG_PREWARM=boot for the
  old way).
- 2026-11-08 (browser): WebGPU back end (M1-M4) is the default, cruise at
  30 fps after the vblank catch-up fix (was 15-16 on WebGL); Hor+
  widescreen; opt-in 60 fps races (MC3_60FPS); Online menu (Free Roam with
  $2M + every car and part unlocked, no saves; Online Races = System Link);
  free-roam networking (roam core + hub) and remote cars in the Online
  cruise built, first run pending; page: Keys panel by in-game action,
  Stretch, Screen shape, 60 fps setting, deploy config.json with a test
  password. Next: run O3 (MC3_ROAM_FAKE, two browsers), O4 tuned cars.
