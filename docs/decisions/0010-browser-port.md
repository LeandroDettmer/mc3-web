# 10. The browser port: Emscripten and WebGL 2, the disc over HTTP, System Link through the server

Date: 2026-11-05

## Context

The native hosts (Linux, macOS, Windows, Switch) all draw through OpenGL-style calls: the renderer `nv2a_gl` is the
one that Direct3D is translated to, and every native build runs it. The browser has to run the same game from the
same lifted C, with no install on the player's PC.

Three things differ in a browser:

- **Memory.** The native host maps guest memory with mmap aliases (RAM mirrors, the tiled aperture). WebAssembly has
  no mmap, so those aliases cannot exist.
- **Files.** A disc is several gigabytes (3.4 GB extracted for the DUB Edition, 4.5 GB for the Remix). It cannot be
  shipped with the page, uploaded, or copied into the build. The page reads it from the server the player runs.
- **Online.** A browser has no UDP sockets. System Link needs another path to the other players.

nfsu2-web (MIT, th0mk) solved the same three problems for another game, with a Direct3D 9 renderer on WebGPU. Its
design is reused where it fits (the start page, the AudioWorklet's ring buffer).

## Decision

1. **Emscripten, WebGL 2, pthreads.** The game's C is built with Emscripten (`tools/build_web.sh`): WebAssembly, the
   game's threads as Web Workers (`PROXY_TO_PTHREAD`) on shared memory, WASMFS for the file system, SDL2 from
   Emscripten's port. The renderer is `nv2a_gl` on WebGL 2 (a GLSL ES 3.00 rewrite of the shaders, `gl_es.c`).
   **Not** wasi-sdk with WebGPU: the renderer is GL on every host, and a WebGPU renderer would be a second renderer
   to keep in step with the first, with its own shader and state code. The Vulkan renderer stays off in this build.

2. **Guest memory through a region table.** On `__EMSCRIPTEN__`, `XBOX_PTR` maps Xbox addresses through a 1 MB region
   table: RAM mirrors and the tiled aperture fold onto their storage. Native builds expand to the same base plus
   offset as before. Raw `g_xbox_mem_offset` uses go through `xbox_ptr()`.

3. **Disc over HTTP range, saves in OPFS.** The page reads the disc from the server with HTTP range requests
   (`web/wasmfs_fetch.js`, which replaces WASMFS's fetch backend: its chunking left stale data). The server
   (`tools/web_serve.py`) sends the COOP/COEP headers that threads need, and the Range support. Profiles are saved in
   the browser's private storage, OPFS (`src/web_fs.c`), under `UDATA/54540079/`. The disc is never copied into the
   build and never leaves the player's server.

4. **Sound through an AudioWorklet.** The APU writes float samples into a single-producer, single-consumer ring
   (`xboxrecomp/src/apu/apu_web.c`), read by an AudioWorklet. The design is nfsu2-web's.

5. **Movies: the lifted Bink decoder.** The browser build has no FFmpeg (`MC3_NATIVE_BINK=OFF`), so the lifted decoder
   plays the movies. The movie upscale switch therefore has no effect in the browser yet.

6. **Online: System Link through the server.** The game's System Link runs on the XNet HLE
   (`xboxrecomp/src/net/xnet_hle.*`), which has a pluggable transport. Native builds send it as UDP on the local
   network (`MC3_NETPLAY=1`, Remix). The browser's transport is a WebSocket to a virtual LAN switch on the server,
   carrying the same XNet datagrams; players on one server meet as on one LAN. Then comes a free-roam hub for
   everyone on a server, without a limit. **Status: in progress.** The start page's switch only opens the menu today.

7. **One repository, a fork.** This repository is a fork of mcdub3-nx keeping its history. The browser port sits on
   the same code: `GAME=dub` and `GAME=remix` apply to the browser as to every other target (ADR 0006). Changes to the
   toolkit go in `xboxrecomp/` with the `xboxrecomp:` prefix (ADR 0002). The upstream Switch build and release
   workflows are parked in `.github/workflows-disabled/`: they need a self-hosted runner that holds the discs. The
   tests in `ci.yml` run without game data and stay on.

## Consequences

- The browser build needs Emscripten and a lift made from the player's own disc (`make lift`). Windows users build in
  WSL 2 with the clone in the Linux file system.
- The renderer has one GL code path to keep correct, and the native builds keep their own. A change to `nv2a_gl`
  reaches both.
- No disc is downloaded in full: the browser reads the parts it needs from the player's server, on demand.
- Online waits on the browser's transport and the hub. Until then, the Remix's System Link works only in the native
  builds.
- Credit: the start page and the AudioWorklet's design follow nfsu2-web (MIT). See [NOTICE.md](../../NOTICE.md).
