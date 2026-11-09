# Contributing: development guide

The technical source of truth: how the project turns the Xbox game into a
WebAssembly module that runs in the browser, what sits where, and the
day-to-day loop. Decisions and their reasons: [docs/decisions/](docs/decisions/).
Engineering notes (rules, MC3 facts, status): [CLAUDE.md](CLAUDE.md).

## Quick start

You need your own disc of *Midnight Club 3: DUB Edition Remix* (USA or
Europe), as an ISO or as extracted files.

### Prerequisites

Linux, or Windows with WSL 2 (Ubuntu 24.04). In WSL, clone into the Linux
file system or a fast drive, and check out with `core.autocrlf=input`: the
scripts must keep LF line endings.

- **Python 3.10+** with `capstone` (the lift's x86 disassembler). `make setup`
  (Ubuntu) creates `work-<game>/venv` with it, and installs the native Linux
  build's packages too.
- **CMake, Ninja**, and optionally **ccache** (tools/build_web.sh uses it when
  installed: an unchanged lifted chunk is not recompiled).
- **[Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)**:
  `source <emsdk>/emsdk_env.sh`, or `EMSDK=<dir>` (default `/opt/emsdk`).
- **Rust 1.85+** for the server (`web/server`), **Node.js 20+** for the
  Cloudflare Worker tests (`web/lan-worker`, `web/races-worker`).
- **Google Chrome** to play.

### From disc to browser

```sh
make setup                                            # once (Ubuntu / WSL): Python venv + packages
make extract GAME=remix EDITION=usa ISO=<file.iso>    # unpacks into game-remix-usa/
make lift GAME=remix                                  # XBE -> C in work-remix/gen (~15 min, ~3 M lines)
make web GAME=remix                                   # browser build: work-remix/build-web
make web-serve GAME=remix                             # http://localhost:3000/ (Python dev server)
```

Or serve with the Rust server, which also carries online play (System
Link, race rooms, free roam):

```sh
cd web/server && cargo build --release
target/release/mc3-server --web .. --build ../../work-remix/build-web \
    --disc ../../game-remix-usa --port 3000
```

Open http://localhost:3000/ in Chrome and press **Play**. Player's guide:
[docs/web/PLAYING.md](docs/web/PLAYING.md); server options:
[web/server/README.md](web/server/README.md).

`make` with no target lists every target, grouped. Every target takes
`GAME=remix` (the browser port's game) or `GAME=dub` (the default; the DUB
Edition builds too, at an earlier stage). Logs land in `work-<game>/logs/`.

### Building into your own folder

`BUILD_DIR=<folder> tools/build_web.sh` (or `make web BUILD_DIR=...`) builds
anywhere; build folders share the ccache. `JOBS=n` sets the compile jobs
(~1.5 GB of RAM each on the lifted chunks; the default is sized from free
memory). A fresh build compiles ~90 lifted chunks: several minutes.

A clean checkout's file dates can trip the "lift is stale" check (the
config is newer than the lifted C). When the lift inputs did not change,
`touch` the generated files, or set `MC3_SKIP_LIFT_CHECK=1`.

## How it works: two stages

```
STAGE 1: LIFT (Python)                                STAGE 2: BUILD (C, Emscripten)
game-remix-usa/default.xbe ─► xboxrecomp/tools ─► work-remix/gen/*.c ─► emcc ─► work-remix/build-web/
 (x86 machine code)            parse, disassemble,  (the game as C)   + src/*.c       mcdub3remix.js + .wasm
                               identify, translate                    + toolkit runtime
                                                                      + web/*.js (pre-js)
```

**Stage 1, the lift (`make lift`, `make regen`, `make relift`).** The
toolkit's recompiler is a set of Python programs (`xboxrecomp/tools/`). They
read the Xbox executable, disassemble it (with *capstone*), find every
function, recognise library code, recover how each function takes its
arguments, and translate every x86 function into a C function. Output:
`work-<game>/gen/` (~90 files, ~3 M lines) plus intermediate results in
`work-<game>/analysis/`. It runs once, then again only when the C must be
regenerated: after changing seeds (`config/<game>/seed_functions.json`,
`make regen`), or the set of hand-written overrides in
`src/games/<game>/recomp_manual.c` / the translator (`make relift`, skips the
analysis steps).

For the browser, `work-remix/gen` is lifted with `RECOMP_INLINE=16` and the
`g_guest` memory alias (CLAUDE.md, Browser port): regenerate the same way or
the inlined leaves are lost.

| Step        | Tool (`xboxrecomp/tools/`) | Does                                                        | Output                          |
| ----------- | -------------------------- | ----------------------------------------------------------- | ------------------------------- |
| parse       | `xbe_parser`               | reads the XBE header: sections, entry point, kernel imports | `work-<game>/analysis/xbe_*`    |
| disassemble | `disasm` (capstone)        | finds every function and jump target                        | `work-<game>/analysis/disasm/`  |
| identify    | `func_id`                  | recognises library code (C runtime, XDK)                    | `work-<game>/analysis/func_id/` |
| ABI         | `abi_analysis`             | recovers calling conventions and argument counts            | `work-<game>/analysis/abi/`     |
| translate   | `recomp`                   | turns each x86 function into a C function                   | `work-<game>/gen/`              |
| (helpers)   | `seed_from_log`, `xiso`    | seed candidates from a run log; ISO unpacking               | `config/`, `game*/`             |

**Stage 2, the build (`make web`).** Emscripten compiles three things into
one WebAssembly module (wasm32, pthreads, PROXY_TO_PTHREAD):

- `work-<game>/gen/`: the game itself, as generated C;
- `xboxrecomp/src/`: the toolkit's runtime, which stands in for the Xbox:
  kernel, memory map, GPU (WebGPU back end `nv2a_wgpu`, WebGL 2 `nv2a_gl`),
  sound chip (AudioWorklet ring), controllers, network (XNet over a WebSocket);
- `src/`: our own code: boot (`main.c`), hand-written function replacements
  (`games/<game>/recomp_manual.c`), the browser file system (`web_fs.c`).

The page's JavaScript (`web/`) is linked in as pre-js files. The disc is
never copied into the build: the page reads it from the server with HTTP
Range requests (and caches it in OPFS on remote servers).

**Why two stages:** lifting is slow and only depends on the XBE, seeds and
overrides; building is the part you repeat all day. An ordinary code change
never re-runs the lift.

## How our changes sit on top of the game

**The generated C in `work-<game>/gen/` is never edited.** It is a build
product, rewritten by every lift and never committed. The game's
`default.xbe` is never edited either. Everything we change lives in our own
files, applied on top of the untouched base every time:

```
   game*/default.xbe  (the base: never changes)
          │
          │  stage 1 lift, steered by our inputs:
          │    config/<game>/seed_functions.json   extra function starts
          │    src/games/<game>/recomp_manual.c    which functions NOT to generate
          │    xboxrecomp/tools/                   translator fixes
          ▼
   work-<game>/gen/*.c  (disposable output)
          │
          │  stage 2 build, linked with our code:
          │    src/games/<game>/recomp_manual.c    our versions of those functions
          │    src/main.c, src/web_fs.c            boot, data patches, browser glue
          │    xboxrecomp/src/                     the runtime: the "Xbox hardware"
          │    web/*.js                            page side: audio, disc, network, input
          ▼
   <app>.js + <app>.wasm
```

| To change                                           | Put it in                                                                                                    | How it takes effect                                                                                                    |
| --------------------------------------------------- | ------------------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------- |
| a game function (bug fix, native replacement, stub) | `src/games/<game>/recomp_manual.c`: define `void sub_XXXXXXXX(void)`                                         | the lift skips that function (`--exclude-manual`); the linker uses ours, for direct and indirect calls                 |
| a game function, but keep the original              | declare `extern void sub_XXXXXXXX_gen(void);`, define `sub_XXXXXXXX` and call `sub_XXXXXXXX_gen()` inside it | the lift emits the original body under the `_gen` name, so ours wraps it                                               |
| keep an override around but switched off            | wrap it in `#if 0 … #endif`                                                                                  | the lift ignores it and generates the original again (relift)                                                          |
| a missing function                                  | `config/<game>/seed_functions.json` (with a note)                                                            | the disassembler treats it as a function start; it gets lifted like the rest                                           |
| game data or settings (a constant, a limit)         | `src/main.c`, after the memory is mapped: write to the guest address                                         | patched in memory at boot; the XBE file stays untouched                                                                |
| how the Xbox hardware or kernel behaves             | `xboxrecomp/src/` (commit as `xboxrecomp:`)                                                                  | rebuilt and linked                                                                                                     |
| how x86 is translated                               | `xboxrecomp/tools/recomp/` (commit as `xboxrecomp:`)                                                         | the next lift produces different C                                                                                     |
| the start page, settings, online UI                 | `web/` (index.html, *.js)                                                                                    | page files are served as they are; pre-js files (pre.js, audio.js, disc_cache.js, lan.js, net.js, ...) need `make web` |

Browser-only C sits behind `__EMSCRIPTEN__` (or in a web-only file), so the
native Linux debug build keeps its behaviour.

### Regen or just build?

| You changed                                                                                | Run                | Why                                                                             |
| ------------------------------------------------------------------------------------------ | ------------------ | ------------------------------------------------------------------------------- |
| `src/*.c`, `xboxrecomp/src/` (runtime), `web/*.js`                                         | `make web`         | plain C / JS; the lift is unaffected                                            |
| the *body* of an existing override in `src/games/<game>/recomp_manual.c`                   | `make web`         | same set of functions replaced; only our code changed                           |
| *added or removed* an override in `src/games/<game>/recomp_manual.c`                       | `make relift web`  | the lift must drop (or restore) the lifted body; fast: skips the analysis steps |
| `xboxrecomp/tools/recomp/` (translator)                                                    | `make relift web`  | same analysis, different C                                                      |
| `config/<game>/seed_functions.json`, `xboxrecomp/tools/disasm`, `func_id`, `abi_analysis`  | `make regen web`   | the analysis itself changes (which functions exist)                             |
| the disc                                                                                   | `make lift web`    | new base                                                                        |

The build checks this for you (`mc3_check_lift_fresh` in tools/env.sh): it
stops, and names the step to run, when the seeds are newer than the lifted C
or the set of overrides differs from the one the last lift saw (read with the
toolkit's own scanner, `xboxrecomp/tools/recomp/manual_scan.py`). A relift or
regen rewrites `work-<game>/gen/`, so the next build recompiles the chunks
whose text changed (ccache skips the rest).

### Files and saves in the browser

The game only ever uses Xbox paths (`D:\...`, `\Device\CdRom0\...`,
`\Device\Harddisk0\Partition1\UDATA\...`); `xboxrecomp/src/kernel/kernel_path.c`
maps them to two host folders. In the browser (`src/web_fs.c`) the disc is
mounted at `/game` through a fetch backend (`web/wasmfs_fetch.js`, HTTP
Range), and saves at `/opfs/save` (Origin Private File System,
`MC3_SAVE_DIR`). The page's **Saves** tab imports a real Xbox profile and
re-signs it with the port's all-zero key (the same format
`tools/save_import.py` handles; CLAUDE.md, Saves).

## Two games: GAME=dub and GAME=remix

The repo builds two games ([ADR 0006](docs/decisions/0006-two-games-one-repo.md)):
*Midnight Club 3: DUB Edition* (`GAME=dub`, the default) and *DUB Edition
Remix* (`GAME=remix`, the browser port's focus).

| | DUB Edition | Remix |
|---|---|---|
| config | `config/dub/` | `config/remix/` |
| overrides | `src/games/dub/recomp_manual.c` | `src/games/remix/recomp_manual.c` |
| discs | `game/`, `game-<edition>/` | `game-remix/`, `game-remix-<edition>/` |
| generated | `work-dub/` | `work-remix/` |
| module | `mcdub3.js` / `.wasm` | `mcdub3remix.js` / `.wasm` |

A fix for one game goes in its own `config/<game>/` or `src/games/<game>/`;
the other game never builds it. Shared code takes no game's addresses: what
it needs comes from `config/<game>/game.h`.

Editions ([ADR 0009](docs/decisions/0009-edition-variants.md)): each game
runs all its editions on one lift. The Remix's USA disc differs from its
European (lifted) one in a single branch condition, which the lift carries
both ways. To check a new disc:

```sh
make xbe-edition GAME=remix XBE=game-remix-usa/default.xbe
```

It compares that `default.xbe` with the lifted one and prints what to add:
an `X(...)` line in `config/<game>/xbe_editions.h` (same code), plus the
branches in `config/<game>/edition_variants.json` and a relift (branch
conditions only), or a lift of its own (other code differences; not
supported yet).

The Remix's overrides started as a port of the DUB Edition's, every address
rewritten by `tools/match_lifts.py` (pairs the two lifts' functions) and
`tools/port_overrides.py` (rewrites a file through that map;
`config/remix/port_map.txt` holds the hand-found addresses). Use them to
carry a fix over, or to find a function in the other build.

## Project layout

Legend: **[git]** tracked · **[ignored]** gitignored (local only) ·
**[generated]** written by a tool, safe to delete and regenerate.

```
mc3-web/
├── Makefile                  [git] the development loop; `make` lists targets
├── CMakeLists.txt            [git] stage 2: one C project (browser; native Linux for debugging)
├── README.md, CONTRIBUTING.md, CLAUDE.md, NOTICE.md, LICENSE
├── TODO.md                   [ignored] personal work list
│
├── web/                      [git] the browser side
│   ├── index.html              start page: Play, Settings, Controls, Saves, Online, About
│   ├── pre.js, audio.js, disc_cache.js, lan.js, net.js, rtc.js, wgpu.js, touch.js, ...
│   ├── wasmfs_fetch.js         WASMFS backend: the disc over HTTP Range
│   ├── server/                 Rust server (mc3-server): page, build, disc, /lan switch, roam hub
│   ├── lan-worker/             the /lan switch on a Cloudflare Worker + Durable Object
│   └── races-worker/           the race-room list on a Cloudflare Worker
│
├── src/                      [git] OUR C code, compiled with the lifted code
│   ├── main.c                  boot: load XBE, map Xbox memory, start devices, jump to the game
│   ├── web_fs.c                browser file system: disc at /game, saves at /opfs/save
│   ├── games/<game>/           per game (dub, remix): overrides, online, free roam
│   ├── ac97_nabm.c, autostart.c, difftest.c, bink_native.c (native only), ...
│   └── switch_nx.c, windows_host.c   inherited native hosts, not built for the web
│
├── config/<game>/            [git] per-game inputs to stage 1 and 2 (dub, remix)
│   ├── game.env                names and folders: module name, disc folders
│   ├── game.h                  the guest addresses shared code needs
│   ├── seed_functions.json     extra function starts for the disassembler
│   ├── not_functions.json      false function starts to exclude (remix)
│   ├── edition_variants.json   branch conditions that differ per edition (remix)
│   ├── xbe_editions.h          every disc (edition) the lift runs, checked at boot
│   ├── xbe_facts.h             entry point + title ID, written by `make lift`
│   └── port_map.txt            remix only: hand-found DUB -> Remix addresses
│
├── tools/                    [git] scripts behind the make targets
│   ├── env.sh                  shared paths (game*/, work-<game>/), Python with capstone, lift check
│   ├── setup_ubuntu.sh         make setup
│   ├── extract_disc.sh         make extract: ISO -> game*/
│   ├── first_lift.sh, regen.sh make lift / regen / relift: the stage-1 pipeline
│   ├── build_web.sh            make web / web-node: Emscripten build
│   ├── web_serve.py            make web-serve: dev server (COOP/COEP, Range)
│   ├── web_promote.sh, web_release_build.sh   copy a finished build to the served folder
│   ├── wgsl_check.sh           validate the WebGPU back end's WGSL with naga
│   ├── xbe_edition.py, match_lifts.py, port_overrides.py, trace_wrappers.py
│   ├── save_import.py          make import-save (native saves; same format as the page's import)
│   └── build_linux.sh, run_linux.sh, difftest.py, profile.sh, record.sh, ...   native Linux debugging
│
├── docs/
│   ├── decisions/            [git] ADRs: why things are the way they are
│   ├── web/PLAYING.md        [git] the player's guide
│   ├── races-protocol.md, webgpu-plan.md, upstream.md
│
├── xboxrecomp/               [git] THE TOOLKIT, our fork (ADR 0002, 0007), edited in place
│   ├── tools/                  stage 1, Python: xbe_parser, disasm, func_id, abi_analysis, recomp, ...
│   ├── src/                    stage 2, C runtime: kernel/, platform/, nv2a/, nv2a_gl/, nv2a_wgpu/,
│   │                           apu/, audio/, usb/, input/, net/, d3d/, video/
│   ├── templates/runtime/      recomp_types.h: the register model the generated C includes
│   └── docs/  tests/           toolkit documentation and C tests
│
├── game-remix[-<edition>]/   [ignored] the extracted disc (never committed)
└── work-<game>/              [ignored] EVERYTHING GENERATED; delete any part and rebuild it
    ├── analysis/             stage-1 intermediate results
    ├── gen/                  stage-1 OUTPUT: the game as C (~90 files, ~3 M lines)
    ├── build-web/            make web: <app>.js, <app>.wasm, index.html
    ├── build-web-node/       make web-node
    ├── build-linux*/         native Linux builds (optional)
    ├── logs/                 every make log, timestamped
    └── venv/                 Python + capstone (make setup)
```

| Want to              | Delete                                                         | Then         |
| -------------------- | -------------------------------------------------------------- | ------------ |
| Rebuild from scratch | `make clean-build` (`work-<game>/build-*`)                     | `make web`   |
| Re-lift from scratch | `make clean-gen` (`work-<game>/gen`, `work-<game>/analysis`)   | `make lift`  |
| Reset Python         | `work-<game>/venv`                                             | `make setup` |

## Tests

Everything here runs without the game:

```sh
make toolkit-test                                   # lifter, disassembler (pytest; CI runs these)
(cd web/server && cargo test --workspace)           # server crates: ws, lan switch, roam hub
(cd web/lan-worker && node --test test/*.test.mjs)  # /lan switch on a Worker
(cd web/races-worker && node --test test/*.test.mjs)
tools/wgsl_check.sh                                 # WGSL of the WebGPU back end (needs naga)
```

`.github/workflows/ci.yml` runs the toolkit tests (minus
`ci/known-test-failures.txt`) and a syntax check of `tools/` on every push.

`make web-node GAME=remix` builds a Node.js variant (NODERAWFS, host files):
a headless boot test without a browser.

## Debugging in the browser

- Every `RECOMP_*` / `MC3_*` runtime switch can be set from the page
  (Settings), or in the URL: `?RECOMP_GPU=webgl`, `?MC3_DISC_CACHE=0`, ...
  CLAUDE.md lists them with their meaning.
- `MC3_PERF=<s>` prints per-second performance lines (guest lock, vblanks,
  renderer); `RECOMP_WGPU_STATS=1` the WebGPU back end's.
- The page batches the game's log into the browser console.

## Native Linux build (optional, for debugging)

The same lifted C also builds as a native Linux program with OpenGL and
SDL2. It is not shipped; it is the place for tools the browser lacks: gdb
on every `sub_XXXXXXXX`, `perf`, frame dumps, and **difftest** (replay one
captured call on real x86 in Unicorn and name the first wrongly written
bytes: the tool to reach for when output is subtly wrong).

```sh
make setup                  # once: SDL2, GL/epoxy, FFmpeg, gdb, perf, ...
make build run GAME=remix   # work-remix/build-linux/mcdub3remix, in a window (keyboard = pad 1)
make run-gdb                # under gdb (bt = host chain; call print_guest_state())
make snapshot && make report  # a hang: guest call stack after 30 s, explained
make hoststacks             # every thread's host backtrace: who waits on what
make profile                # perf: hottest functions per thread
make record                 # frames + audio of a headless run -> work-<game>/capture/
make difftest CALL=n FUNC=<hex>   # replay call n of a wrapped function on x86, diff vs lifted
make trap                   # stop at the first untranslated instruction reached
```

The disc for native runs is `game*/` (tools/env.sh picks it; `EDITION=`
or `MC3_GAME_DIR=` choose another); saves go to `work-<game>/save`
(`MC3_SAVE_DIR`). Real Xbox profiles: `make import-save FILE=<.gsx | folder |
.mc3>` (re-signs and installs; backs up first). Native-only features: FFmpeg
movie decoding (`src/bink_native.c`), Vulkan (`make build-vk`), and HD
texture packs (`hd/` beside the disc; `make hd-dump`, `make hd-tools`,
`make hd-upscale`; xboxrecomp/src/nv2a_gl/hd_pack.c).

The macOS, Windows and Nintendo Switch hosts that mcdub3-nx shipped are not
maintained here; their platform code inside `xboxrecomp/` and `src/` stays
so the toolkit keeps one shape, but nothing builds or tests it.

## Commits and pull requests

- Never commit game data (ISO, `default.xbe`, disc files) or generated C.
- Toolkit changes (`xboxrecomp/`) go in their own commits, subject prefixed
  `xboxrecomp:`; note upstreamable fixes in [docs/upstream.md](docs/upstream.md).
- Every seed added to `config/<game>/seed_functions.json` gets a note.
- Record decisions in `docs/decisions/` (short ADRs).
- Code reused from nfsu2-web (MIT, th0mk) carries a credit in the file
  header and in NOTICE.md.
