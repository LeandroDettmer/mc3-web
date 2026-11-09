# Notices and credits

This file lists whose work this project builds on, and under which licence each part is used. It covers the code in
this repository. The game itself is not in it: this repository contains no game code or data.

## This project

- **mc3-web** (this fork): the browser port, its start page, its tooling and its documentation are MIT, as
  [LICENSE](LICENSE) says. Maintained by Leandro Dettmer.
- **mcdub3-nx** by Laure Ladastra: the native port this repository forks, with its history. MIT, the same
  [LICENSE](LICENSE). Source: <https://github.com/laureladastra/mcdub3-nx>.

## The browser renderer: nfsu2-web

- **nfsu2-web** by th0mk, MIT. Source: <https://github.com/th0mk/nfsu2-web>. Ideas and code for the WebGPU renderer
  (`web/wgpu.js`, `web/wgpu_fixed.js`, `xboxrecomp/src/nv2a_wgpu/`: the command encoder, vertex pulling, the
  frame arena) are ported from it, and the browser port's other nfsu2-web parts (lock, guest-memory alias) likewise.
  Each file reusing its code names th0mk in its header.

## The toolkit: xboxrecomp

`xboxrecomp/` is the Xbox static recompilation toolkit.

- **xboxrecomp** by Ned Heller (sp00nz) and its contributors, MIT. Licence: [xboxrecomp/LICENSE](xboxrecomp/LICENSE).
  Contributors: [xboxrecomp/CONTRIBUTORS.md](xboxrecomp/CONTRIBUTORS.md). Source: <https://github.com/sp00nznet/xboxrecomp>.

- **LGPL-2.1-or-later, extracted from xemu.** These files keep their own licence and copyright:

  | File | Copyright |
  | --- | --- |
  | `xboxrecomp/src/apu/apu_core.c`, `apu_vp.c` | espes (2012); Jannik Vogel (2018–2019); Matt Borgerson (2019–2025) |
  | `xboxrecomp/src/apu/apu_dsp.c` | espes (2012); Matt Borgerson (2019–2025) |
  | `xboxrecomp/src/apu/apu_regs.h` | espes (2012); Jannik Vogel (2018–2019); Matt Borgerson (2019–2021) |
  | `xboxrecomp/src/apu/apu_state.h` | espes (2012); Jannik Vogel (2018–2019); Matt Borgerson (2019–2025) |
  | `xboxrecomp/src/apu/apu_debug.h`, `fpconv.h` | Matt Borgerson (2020–2025) |
  | `xboxrecomp/src/nv2a/nv2a_regs.h` | espes (2012); Jannik Vogel (2015) |

  The full licence text is in [xboxrecomp/LICENSES/LGPL-2.1.txt](xboxrecomp/LICENSES/LGPL-2.1.txt). The LGPL asks that
  the notices stay, that the source stays available, and that a user can relink against a modified version. The source
  of these files is in this repository, so those conditions hold. The full attribution is in
  [xboxrecomp/NOTICE](xboxrecomp/NOTICE). Source of xemu: <https://github.com/xemu-project/xemu>.

- **Algorithms, independently implemented and credited:** Xbox Z-order (Morton) unswizzling in
  `xboxrecomp/src/d3d/d3d8_swizzle.h` (the masked-increment technique is Fabian Giesen's); NV2A register-combiner
  translation in `xboxrecomp/src/d3d/d3d8_combiners.h`. Both cite xemu's code as a reference for the hardware's
  behaviour.

- **Reference material, no code taken:** xemu, Cxbx-Reloaded and xboxdevwiki, for hardware behaviour, register offsets
  and kernel export ordinals.

## The browser port

- **nfsu2-web** by th0mk, MIT. Source: <https://github.com/th0mk/nfsu2-web>. The start page's layout and design follow
  it, and so does the audio ring buffer feeding the AudioWorklet (`xboxrecomp/src/apu/apu_web.c`). The server
  (`web/server`) takes its WebSocket crate unchanged (`web/server/crates/ws`, with its licence) and its virtual LAN
  switch's design (`web/server/crates/lan`), and System Link's mailbox follows its net mailbox
  (`xboxrecomp/src/net/xnet_web.c`, `web/net.js`). Free roam is ported from its free-roam multiplayer, without its
  NFSU2 game layer: the core's wire format, session and interpolation (`xboxrecomp/src/net/roam/roam_core.c`), its
  platform layer and browser endpoint (`roam_plat.h`, `roam_web.c`, `roam_api.c`) and its hub
  (`web/server/crates/roam`). The touch controls and the phone page follow its touch controls and device helpers
  (`web/touch.js`, `xboxrecomp/src/input/touch_pad_web.c`: a virtual controller merged with a real one, a thumb
  stick where the thumb lands, sliding between pedals, the Wake Lock, phone detection and defaults, the device
  checks' memory and WebGPU notes). nfsu2-web is itself
  based on NFSU2Recomp by Talha Müderrisoğlu (MIT).

- **nfsu2-sw** by antoxa2584x. Its source is unlicensed, so this project reads it for ideas only and copies none of it.
  Source: <https://github.com/antoxa2584x/nfsu2-sw>.

## Third-party software used to build or run

| Software | Licence | Used for |
| --- | --- | --- |
| [Emscripten](https://emscripten.org) | MIT (its compiler is LLVM, under LLVM's licence) | the browser build's toolchain; not shipped in this repository |
| [SDL2](https://www.libsdl.org) | zlib | the native hosts; Emscripten's SDL2 port in the browser |
| [Capstone](https://www.capstone-engine.org) | BSD | the lifter's disassembly |
| [FFmpeg](https://ffmpeg.org) | LGPL or GPL, depending on how it is built | the native Bink movie decoder (`MC3_NATIVE_BINK`); not in the browser build |

Each of these keeps its own licence; this project only uses them.

## Trademarks

*Midnight Club* is a trademark of Take-Two Interactive / Rockstar Games. Xbox is a trademark of Microsoft. Nintendo
Switch is a trademark of Nintendo. The project is not affiliated with or endorsed by any of them. See the
[Legal section of the README](README.md#legal).
