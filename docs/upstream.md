# What to send upstream

Fixes and findings from this port that other projects should hear about.
Our toolkit has two origins (ADR 0002), so each item names where it belongs:

- **xboxrecomp** ([sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp)):
  the lifter (`tools/`), and the parts of the runtime it already had.
- **nfsu2-sw** ([antoxa2584x/nfsu2-sw](https://github.com/antoxa2584x/nfsu2-sw)):
  the Switch layer we imported: the OpenGL/Vulkan pushbuffer renderers, POSIX
  and Switch audio, the Horizon memory layout and the translator passes.

"Still present" was checked against xboxrecomp v0.13.1 (`193e299`,
2026-10-07) and nfsu2-sw `253bde2` (2026-10-15). Re-check before sending.
Nothing here has been sent yet.

## Patches ready (our commits)

| # | Fix | Our commit | Goes to | Still present upstream |
|---|-----|-----------|---------|------------------------|
| 1 | Lifter: `movsx r32, bp/sp` lifted as a zero extension | `90284dc` | xboxrecomp (and nfsu2-sw, which carries the same lifter) | yes, both |
| 2 | Renderer: texture change check samples 256 words, misses small changes | `7152920` | nfsu2-sw (nv2a_gl, nv2a_vk) | yes |
| 3 | Memory: AArch64 Linux puts the guest base too high for the 4 GB window | `070d587` | nfsu2-sw (POSIX memory layout) | yes |
| 4 | Build: Switch data folder hard-coded to `nfsu2x` | `52f9350` | nfsu2-sw | yes |
| 5 | Repo: `tools/*/output.py` missing, so the lift pipeline can't run | `7d30c5b` | nfsu2-sw | yes |
| 5b | Renderer: fog factor 0 for fixed-function draws and for vertex programs that write no fog, so they become the fog colour | `f289447`, `7e8ab79` | nfsu2-sw (gl_vsh.c, nv2a_gl, nv2a_vk) | yes |
| 5c | Executor: every inline draw taken as pre-transformed (only XYZRHW is) | `28ecb11` | nfsu2-sw (nv2a_pb_exec.c) | yes |
| 5d | Input: keyboard does nothing on Linux (events dropped, key state stubbed) | `91f4c4d` | nfsu2-sw (input/, renderers) | yes |
| 5e | Lifter: a jcc after a join of cmp/test of different widths lifts as `if (_flags)`, never taken | `7cbb099` | xboxrecomp + nfsu2-sw | yes, both |
| 5f | Devices: heap and contiguous-window bus addresses collide; the APU played window bytes for heap PCM. RAM tagged (bit 26) by MmGetPhysicalAddress | `85fb543` | nfsu2-sw (kernel, apu, executor, ohci) | not checked |
| 5g | APU: GP/EP DSP memory read as 0, so DirectSound reset both DSPs from every check (10 ms stall each, half-speed audio). Now storage, stub EP reads alive | `5ff29ce` | nfsu2-sw (apu_core.c) | not checked |
| 5h | Lifter: an unresolved branch target that is a shared epilogue (pops, then `ret N`) became a stub doing only the `ret`, skipping the pops: callee-saved registers and esp corrupt on that path. Straight runs to a ret are now lifted | `44dc50f` | xboxrecomp + nfsu2-sw (translator.py) | yes, both |
| 5h | Executor: viewport scale/offset not mirrored into vertex-program constants c[-38]/c[-37]; D3D vertex shaders put every vertex at (0,0) | `8500473` | nfsu2-sw (nv2a_pb_exec.c) | not checked |
| 5i | Renderers: a narrower clip at a surface's address rebuilt (and wiped) the surface; reuse when it fits, in both dimensions | `43cbb91` | nfsu2-sw (nv2a_gl, nv2a_vk) | not checked |
| 5j | Disassembler: `push ecx` + register save not a prologue, so a seed after a data table was rejected | `e71d79c` | xboxrecomp + nfsu2-sw | not checked |
| 5k | Renderers: set up (window, context, shader cache) lazily at the first flip, i.e. during the first movie; optional `prepare` hook | `ab4cbf0` | nfsu2-sw (backend interface, nv2a_gl) | not checked |
| 5l | Kernel: link up makes XNET wait for DHCP; `RECOMP_NET_LINK=0` reports no cable | `0a4c2a2` | xboxrecomp + nfsu2-sw | not checked |
| 5m | Guest lock: uncontended mutex/critical-section calls handed the lock over (scheduler round trips on Horizon) | `7f79e03` | nfsu2-sw (kernel_bridge.c) | not checked; console measurement pending |
| 5n | Executor: without reported flip retires, FLIP_STALL never waited; `RECOMP_FLIP_VBLANK=1` caps flips at the vblank | `ab7f7eb` | nfsu2-sw (nv2a_pb_exec.c) | not checked |
| 5o | Renderer: uniform arrays uploaded at their declared size; strict drivers (Parallels' GL) refuse the whole call when the compiler kept fewer elements, so `u_tscale` stayed 0 and linear textures (movies) sampled black. Live sizes recorded at link time | `4e16ac9` | nfsu2-sw (nv2a_gl) | not checked |
| 5p | Windows: no desktop path for executor titles (only D3D11/XAudio2/XInput/VEH). `XBOXRECOMP_WIN_SDL_GL`: nv2a_gl + SDL + MMIO registry on the Win32 kernel, title off the main thread, native stack dumps | `0332037` | xboxrecomp + nfsu2-sw | yes |
| 5q | Executor: the 2D engine (subchannels other than 0) ignored, so NV062/NV09F image blits never happened; titles that carry a frame into the next back buffer with a blit and draw only what changed (MC3's loading-screen spinner) showed every other update. Blit in guest memory plus an optional `Nv2aBackend.blit` (nv2a_gl: glBlitFramebuffer) | `a2ac3ea` | xboxrecomp + nfsu2-sw (nv2a_pb_exec.c, nv2a_backend.h, nv2a_gl; nv2a_vk needs its own) | not checked |
| 5r | Disassembler: int3 padding after a function's switch tables (dword table, optional byte index table) was no boundary, so the next function was a gap that the gap-prologue pass split at every ret (lost epilogues, garbage registers in the caller); also case bodies of such switches came out as functions of their own | `15dcf3d` | xboxrecomp + nfsu2-sw (tools/disasm/functions.py) | yes (test_cc_boundary.py) |
| 5s | Lifter: comiss/ucomiss conditions used plain C relations, wrong when an operand is a NaN (jb/jbe/je true on the hardware, jne false, jp/jnp hard-wired); setp/cmovp untranslated. MSVC's `if (a > b)` is `comiss; jbe`, so NaN took the other branch. Also: disasm --exclude-functions (9d7a8a4), apu_dsp_ack_add (4ef8b02) | `8eebe2e` | xboxrecomp + nfsu2-sw (lifter.py, conformance) | yes (conformance sse_*) |
| 5t | Lifter: after fcomi/fucomip (and fnstsw; sahf) every flag test was wrong with a NaN: unordered (g_fp_cmp 2) read as `> 0`, i.e. above. Also cvt(t)ss2si/cvt(t)sd2si as C casts: undefined out of range (x86: 0x80000000; AArch64 saturates), and cvtss2si truncated | `a375ad3` | xboxrecomp + nfsu2-sw (lifter.py, recomp_types.h) | yes (conformance fpu_*comi*, sse_cvt_range; test_flag_fpu_compare.py) |

1. **`movsx` from `bp`/`sp`** (`tools/recomp/lifter.py`, `_lift_movsx`). The
   16-bit source list has ax, bx, cx, dx, si, di but not bp, sp, so
   `movsx ebp, bp` becomes `ebp = LO16(ebp)`, a zero extension: −3 comes
   out as 65533. `_lift_movzx` has all eight. In MC3 this broke Bink's IDCT
   (wrong-colour 8×8 blocks in every movie) plus five sites in game code.
   Two-line fix plus a regression test (`test_lifter_movsx_bp.py`). Found by
   replaying the call on real x86 (see "Tools" below).

2. **Texture change check** (`bytes_hash` in `nv2a_gl.c` and `nv2a_vk.c`).
   It hashes 256 words spread over the texture. For a 640×480 A8R8G8B8 movie
   frame that's one pixel every 4,800 bytes, so a change between samples
   doesn't trigger a re-upload. In MC3's intro, title letters typed out on a
   dark frame stayed stuck in an early animation state for many frames. Fix:
   hash linear (non-swizzled) textures in full, about 0.2 ms per 1.2 MB, and
   keep the sampled hash for swizzled ones. Other titles with CPU-written
   textures (movies, UI) are affected the same way.

3. **Guest memory window on AArch64 Linux** (`xbox_memory_layout.c`). mmap is
   top-down there, so the guest base landed 3.8 GB below the top of the
   48-bit space. Every view at base + guest VA above that (contiguous window,
   NV2A/MCPX/flash apertures) then failed with error 8, and the first NV2A
   register access faulted. x86-64 places mappings lower, which is why
   nfsu2-sw never hit it. Fix: probe for a free 4 GB hole and reserve there.
   POSIX only.

4. **`XBOXRECOMP_APP_NAME`**: a CMake cache variable for the
   `sdmc:/switch/<app>/` folder (pipeline cache, logs), so a second port
   doesn't share nfsu2's files.

5. **Missing `tools/disasm|func_id|recomp/output.py`**. They exist in
   xboxrecomp 49ee746 but not in nfsu2-sw, so the stage-1 tools fail to
   import. Restored from upstream; nothing to change, just add them back.
   Also covered by the draft issue in `docs/drafts/`.

5b. **Fixed-function fog** (`nv2a_gl_vsh_fixed` in `gl_vsh.c`). The shader
   ends with `vFog = 0.0`. D3D's fog blend in the final combiner is
   fog.a × colour + (1 − fog.a) × fog colour, so every fixed-function draw
   with fog on comes out as the plain fog colour. MC3's menu city was flat
   dark grey. Fix: the NV2A fog unit (gen mode, linear/exp/exp2 with
   FOG_PARAMS, factor 1 when fog is off) in GL and Vulkan. Bias constants
   follow xemu; MC3's parameters give a sensible 1 at the camera.

5e. **Mixed-width flag join** (`tools/recomp/translator.py`,
   `_merge_flag_states`). A jcc reached from `cmp eax, 7` on one edge and
   `cmp word [esi+30h], 7` on the other got no flag state, so it lifted as
   `if (_flags)`: never taken. The cmp/test snapshots are already masked
   and sign-extended at each compare's width, so only js/jns depend on it.
   In MC3 this was XACT's sound state check: every sound start was refused,
   with no music or effects at all. Regression tests in `test_flag_join.py`.

5f. **OHCI frame boundary** (`usb/ohci.c`, `ohci_thread`; browser build
   only so far, the same race exists on any host with a guest lock). The
   frame number moved before the held done queue was written back, outside
   the guest lock. XPP cancels a transfer by pausing its ED and, once the
   frame passes its stamp, searching the ED for the transfer's TDs; with
   them still in HcDoneHead it took the dummy tail TD and followed its
   0xFFFFFFFF link forever (MC3 Remix froze at the cruise loading screen
   when XInput closed its pads). Fix: under the guest lock, flush the done
   queue first, no new frame while retired TDs wait behind an unacked WDH.

5g. **Vblank schedule drops late vblanks** (`kernel/kernel_bridge.c`,
   `kernel_vblank_tick`; not committed yet). A tick late by more than half
   a period restarted the schedule at now + period, so every vblank the
   timer thread missed while it waited for the guest lock was lost: 32 Hz
   in the browser (MC3 Remix, interval 2: 16 fps). Fix: catch up (next +=
   period) unless more than 4 periods behind, and deliver one owed vblank
   (ISR + DPC) again in the same lock hold. Any host where the timer
   thread waits for the lock; a no-op when it is on time.

5h. **OHCI never raises FrameNumberOverflow** (`usb/ohci.c`, frame tick).
   XPP keeps a 32-bit frame from it (high half updated in its ISR on FNO);
   without it the frame went back 65536 each wrap, so an ED pause stamped
   just before a wrap waited ~65 s. Fix: set FNO when HcFmNumber bit 15
   toggles. Any XDK title using XInput.

5i. **DSP doorbell not acked while the APU front end is trapped**
   (`apu/apu_core.c`, `apu_dsp.c`). `apu_dsp_ack_add` words were cleared
   in `se_frame` only, which does not run while FECTL is trapped; a
   DirectSound command sent then waited for the driver's trap handler (the
   GP acks on hardware whatever the FE does). Fix: ack the doorbells in the
   trapped wait too.

## Issues to report (no patch from us yet)

| # | Problem | Goes to | Evidence / our workaround |
|---|---------|---------|---------------------------|
| 6 | `pushfd`, `popfd` and `cpuid` are silent no-ops | xboxrecomp + nfsu2-sw | CPU feature checks read garbage. MC3's Bink saw "no MMX", left its pixel routines null and looped forever. Workaround: native `sub_002B9C90` |
| 7 | AC'97 "plain" mode: the channel reset bit is cleared too late | nfsu2-sw | DSOUND reads RR back once (hoisted load) and spins forever. Our `src/ac97_nabm.c` models the NABM registers via MMIO hooks; offer it as the fix |
| 8 | APU streaming voices repeat, skip and run slow | nfsu2-sw | Movie sound measured against FFmpeg's decode: 1,567 repeats of 256–768 samples, 304 skips, 1.2 s of holes, ~84% speed over 70 s. Root cause not found; movies now bypass it (`src/bink_native.c`) |
| 9 | 11 toolkit tests fail on stale harnesses | nfsu2-sw | Their C preambles lack `RECOMP_PREEMPT` and the leaf/register-locals helpers that the nfsu2-sw translator always emits. With those defined, 516 of 517 pass |
| 10 | Template `recomp_manual.c` declares `extern uint32_t g_eax` | xboxrecomp + nfsu2-sw | The registers are thread-local (`RECOMP_TLS`): a TLS mismatch that GCC refuses to link. Fix: include `recomp_types.h` |
| 11 | Git conflict markers committed in docs | nfsu2-sw | `>>>>>>> pr-128` in README.md, CONTRIBUTORS.md, docs/pipeline/06-debugging.md, docs/technical/kernel-replacement.md |

For 8, the quickest reproduction is any title with streamed DirectSound sound.
Record with `RECOMP_AUDIO_DUMP` and compare with the source track using
`tools/audio_diff.py`.

## Tools worth offering

- **Differential tester** (`src/difftest.c`, `tools/difftest.py`, `make
  difftest`): captures guest state around one lifted call, replays the
  original x86 in Unicorn from the same state, and names the first wrongly
  written bytes and the instruction that wrote them. It found #1 in a few
  runs. It's generic apart from the wrappers that arm it.
- **`tools/audio_diff.py`**: follows a reference track through a captured
  output and reports holes, skips, repeats and drift. It found #8.
- **XNet HLE** (`xboxrecomp/src/net/xnet_hle.c`, test
  `xboxrecomp/tests/xnet_hle`; not committed yet): System Link without
  emulating the NIC. A title's XNet thunks (socket, bind, sendto,
  recvfrom, XNetGetTitleXnAddr, XNetXnAddrToInAddr, ...) are overridden to
  call it; datagram sockets (UDP, VDP as UDP) over a pluggable transport,
  host UDP sockets built in (RECOMP_NET_PORT_BASE/_ADDR/_PEERS). No XNet
  security (both ends run it). Generic: any XDK title whose multiplayer is
  System Link over UDP. Goes to xboxrecomp; nfsu2-sw has no use for it.

## How to send

One PR or issue per item, smallest first (#1, #5, #4). Each with the
symptom, the cause, the fix and how it was verified. Our commit messages
already read that way. For nfsu2-sw, settle the licensing question first
(`docs/drafts/nfsu2-sw-licensing-issue.md`): a PR into an unlicensed repo
has unclear terms.
