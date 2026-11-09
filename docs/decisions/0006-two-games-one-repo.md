# 6. Two games in one repo: the DUB Edition and the Remix

Date: 2026-10-24

## Context

*Midnight Club 3: DUB Edition Remix* (Xbox, built 2005-11-18) is a later
build of the same game: the same title ID (0x54540079), the same XDK
(5849), the same libraries (D3D, Bink, DirectSound, XACT, WMA, pads, byte
for byte the same size), but every section at new addresses and 222 KB more
game code. A lift is tied to one XBE, and every override and seed is an
address, so the Remix needs its own lift, overrides and seeds.

`tools/match_lifts.py` paired the two lifts: the libraries almost entirely
identical code (D3D 277 of 281 functions, DirectSound 468 of 473, XACT 353
of 366), the game code 10,733 of 17,839 functions identical and 3,050
changed. Of the DUB Edition's overrides all but two mapped by themselves.
So almost every fix found for the DUB Edition (renderer, audio, frame
pacing, saves, the Switch) holds for the Remix, and most of its overrides
carry over by address.

A separate repository would have been quicker to start, but every later
fix would then have to be carried over by hand.

## Decision

One repository, two games, selected with `GAME=dub` (the default) or
`GAME=remix` on every `make` target. What depends on a game lives in that
game's own place; everything else is shared:

| | DUB Edition | Remix |
|---|---|---|
| config (XBE facts, editions, seeds, `game.env`) | `config/dub/` | `config/remix/` |
| overrides | `src/games/dub/recomp_manual.c` | `src/games/remix/recomp_manual.c` |
| discs | `game/`, `game-<edition>/` | `game-remix/`, `game-remix-<edition>/` |
| generated (lift, builds, logs, saves) | `work-dub/` | `work-remix/` |
| app name, executable | `mcdub3` | `mcdub3remix` |
| Switch folder, NRO | `mcdub3-nx/`, `mcdub3-nx-<edition>.nro` | `mcdub3remix-nx/`, `mcdub3remix-nx-<edition>.nro` |
| app identity (NACP, NSP, Mac, Windows) | `config/dub/app.env` | `config/remix/app.env` (both + `switch/app.env`, ADR 0008) |

- Only the selected game's files go into a lift and a build: a fix in
  `src/games/remix/` never reaches the DUB Edition, and the reverse. A fix
  in shared code reaches both.
- The Remix's overrides start as a port of the DUB Edition's:
  `tools/port_overrides.py` rewrites every address through the function
  pairing, places inside identical functions, and data read through the
  twin instruction; `config/remix/port_map.txt` adds pairs found by hand.
  Once the Remix runs, its file is its own and is edited directly; the
  port is a starting point, not a build step.
- Editions work within each game as before (`EDITION=eur`).
- `work/` from before the split becomes `work-dub/` (moved once by the
  Makefile and `tools/env.sh`).
- Saves don't collide: the port keeps profiles beside each game's disc
  (`<disc>/UDATA`, `work-<game>/save`), the Switch in each game's own folder.

## Consequences

- Restructure once: paths in the scripts, CMake (`MC3_GAME`), the
  Makefile, CI (DUB Edition only until the Remix is released).
- Two lifts to keep in step after a translator change (`make relift` per
  game).
- Shared code must not assume DUB Edition addresses; address-bound logic
  stays in the per-game override files (or takes its addresses from there).
