# 2. Vendor the toolkit as our own snapshot, layered by origin

Superseded in part by [0007](0007-toolkit-fork-without-upstream-history.md)
(2026-10-25): the history is no longer imported and the subtree merge
below no longer applies; the layers stay as described.

Date: 2026-10-11

## Context

- Upstream xboxrecomp (sp00nz + ~10 contributors, MIT) moves fast: 509 commits
  since 2026-09-01. It has no Switch support.
- nfsu2-sw (one author) carries the Switch layer in its vendored copy:
  OpenGL/Vulkan pushbuffer renderers, Joy-Con input, Horizon memory/threads,
  translator passes (+16.9k/−8.1k lines over upstream). Its own `src/` has no
  licence; the toolkit files keep upstream's MIT.
- We don't want to depend on either project's pace.

## Decision

`xboxrecomp/` is a git subtree, built as labelled layers:

1. Upstream `49ee746` (2026-09-27), imported with history: the commit
   nfsu2-sw's copy was forked from.
2. nfsu2-sw's delta in six commits by area (renderers, input/USB, APU, kernel,
   recompiler, build/docs), authored as Anton Artemov, verified byte-identical
   to `antoxa2584x/nfsu2-sw@6e19726:xboxrecomp`. It includes upstream PR #128
   (unmerged upstream), whose conflicts with `49ee746` were resolved there.
3. Our own commits, prefixed `xboxrecomp:`.

nfsu2-sw's `src/` is reference only; ours is written from the toolkit's MIT
template and the toolkit API.

## Updating

```sh
git fetch xboxrecomp-upstream
git merge -X subtree=xboxrecomp xboxrecomp-upstream/main
```

A trial merge on 2026-10-11 (101 upstream commits ahead) conflicted in 22
files, mostly kernel, APU and recompiler files the Switch layer also changes.
Merge deliberately, when an upstream fix is wanted, not routinely.

Keep our toolkit edits small and in new files where possible, and offer
generic fixes upstream; the less we carry alone, the cheaper each merge.

## Upstream changes taken selectively

| Upstream | Ours | Why |
|---|---|---|
| 21d9ce7 Lift switch tables whose displacement is not slot 0 | dc8be7d | jump tables inside CRT/XDK routines lifted as calls to non-functions (MC3: memmove, XNet checksum) |
| c576d24 Measure jump tables down from the displacement | aa5f978 | same bug class, disassembler side |
| 85d4f30 (#173) Lift arms that follow their inline jump tables | 8fc4bd7 | tables placed after the function's code (XNet checksum at 0x003A7F90) |
| f4e3186 (#134) Lifter: a table counted down from its last slot keeps that slot | e06ca81 | completes c576d24 |

Imported with `git format-patch -- tools | git am --directory=xboxrecomp`,
original authors kept. A later full upstream merge sees the same changes.
Toolkit tests (tools/recomp, tools/disasm): no new failures; 11 tests fail
on the Mac both before and after (host-compiled C tests), to re-check on Linux.

## Known gaps found while vendoring

- nfsu2-sw's published toolkit lacks `tools/{disasm,func_id,recomp}/output.py`
  (the pipeline can't import); restored from `49ee746`.
- The renderers hard-coded `sdmc:/switch/nfsu2x/`; now `XBOXRECOMP_APP_NAME`.

## Open

Ask nfsu2-sw to license `src/` or upstream the Switch layer into xboxrecomp
(draft: `docs/drafts/nfsu2-sw-licensing-issue.md`; not posted).
