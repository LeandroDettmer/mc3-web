# Disc compression per 1 MiB block (study, 2026-11-15)

Question: can the disc in R2 be stored compressed, per 1 MiB block, so players download fewer bytes, without more
R2 GetObject requests? Study only. No game or page code changed, nothing uploaded.

## Short answer

**Partly.** Use gzip only, and only for the files where it pays: `assets_x.dat_2`, `resources/vehicle/decal.xbpf`,
`decal_g.xbpf` and `rim.xbpf`. Store them as one packed object per file plus a block index in the manifest. This
keeps every request the same, needs no wasm, and cuts **~1.2 TB/day** of today's 5.71 TB (~22%). It is about one day
of work. A later zstd step (wasm decoder) adds **~0.4 TB/day** more, mostly from `assets_x.dat`. Do it only if the
first step shows a gain in players' disc waits.

**It saves no money.** R2 egress is free, and R2 bills requests (Class B), which this keeps the same. The gain is the
player's: fewer bytes on slow links (phones read ~1.4 s a block from R2), shorter cold car builds and a smaller
first fill. It also means less mobile data.

## Measured (node 24 zlib, every 1 MiB block of each file)

Ratio = compressed / raw bytes, summed over the blocks. One gzip member or brotli stream per block (blocks compressed
alone, as a Range read needs). Brotli q11 was run on every 8th block. Video: every 4th block, every 8th for q11, over
all 106 `.bik`. "gzip9 blocks" = per-block gzip9 ratio histogram: < 0.5 / 0.5-0.75 / 0.75-0.9 / 0.9-0.98 / >= 0.98.

| File | MiB | gzip 6 | gzip 9 | brotli q5 | brotli q11 | zstd 19 | gzip9 blocks | Today TB/day |
|---|---:|---:|---:|---:|---:|---:|---|---:|
| assets_x.dat | 297 | 0.926 | 0.926 | 0.762 | 0.748 | 0.759 | 4/19/11/192/72 | 1.08 |
| assets_x.dat_1 | 126 | 0.989 | 0.989 | 0.991 | 0.990 | | 0/0/4/5/118 | 0.29 |
| assets_x.dat_2 | 233 | 0.489 | 0.488 | 0.293 | 0.275 | 0.281 | 181/3/50/0/0 | 0.74 |
| assets_x.dat_3 | 134 | 0.993 | 0.993 | 0.998 | 0.992 | | 0/0/0/7/127 | 0.37 |
| audio/XStreams/mx.xwb | 364 | 0.970 | 0.970 | 0.969 | 0.959 | | 0/0/3/316/45 | 1.20 |
| audio/XStreams/dx.xwb | 61 | 0.876 | 0.876 | 0.871 | 0.795 | | 0/0/60/1/0 | ? |
| audio/XStreams/City_Walla.xwb | 47 | 0.900 | 0.900 | 0.881 | 0.858 | | 0/0/29/19/0 | ? |
| resources/vehicle/decal.xbpf | 262 | 0.260 | 0.256 | 0.209 | 0.176 | 0.194 | 263/0/0/0/0 | 0.43 |
| resources/vehicle/decal_g.xbpf | 334 | 0.051 | 0.049 | 0.043 | 0.039 | | 334/0/0/0/0 | 0.50 |
| resources/vehicle/rim.xbpf | 21 | 0.220 | 0.219 | 0.189 | 0.168 | | 22/0/0/0/0 | ? |
| banks.dat | 78 | 0.860 | 0.860 | 0.546 | 0.481 | | 0/0/77/1/0 | ? |
| video/*.bik (106) | 1645 | 0.992 | 0.992 | 0.993 | 0.977 | | 1/1/0/16/451 | ? |

zstd 19 was measured on 32 blocks a file (every 7th): it is close to brotli q11 everywhere it matters. zstd 3 gives
0.766 on assets_x.dat and 0.315 on _2. "?" = not in the dispatcher's top-7 list. Those files and the rest of the
disc make up the other ~1.1 TB/day.

What the numbers say:

- **Music, video, the city packs `_1`/`_3`**: already compressed (WMA, Bink, packed data). 1-3% at best. Leave them raw.
- **The decals and rims** are mostly empty texture space. gzip already gets 0.05-0.26, and brotli gains little more.
- **`assets_x.dat`, `_2` and `banks.dat`** repeat at distances beyond deflate's 32 KB window, but inside the block.
  Brotli and zstd see the whole 1 MiB block and take `assets_x.dat` from 0.93 to 0.75, and `_2` from 0.49 to 0.28.
  gzip cannot get there at any level.

### Decode cost (one core, Ryzen 5 5500, node 24)

| | gunzip (zlib) | DecompressionStream('gzip') | brotli | zstd |
|---|---:|---:|---:|---:|
| assets_x.dat, ms per MiB | 2.1 | 6.4 | 5.9 | 1.4 |
| assets_x.dat_2, ms per MiB | 3.2 | 6.3 | 3.7 | 2.1 |
| decal.xbpf, ms per MiB | 2.8 | 5.3 | 3.8 | 1.7 |

DecompressionStream's extra time is stream overhead per call. A phone is ~3-5x slower: ~20-30 ms a block for gzip.
A wasm zstd decoder is ~1.5-2x native: ~3 ms desktop, ~10-15 ms phone. The block it saves takes 300-1400 ms on the
network, so decoding is not the limit. Decoding runs on the disc I/O thread (the fetch backend's own pthread worker),
never on the game or page thread. A 16-block fill run costs at most ~0.5 s of that thread on a phone, spread over the
run.

## Estimated saving (today's traffic, ratio applied to each file's bytes)

| File | Today TB/day | gzip 9 saves | zstd/brotli saves |
|---|---:|---:|---:|
| assets_x.dat | 1.08 | 0.08 (not worth packing with gzip) | 0.27 |
| assets_x.dat_2 | 0.74 | 0.38 | 0.53 |
| decal_g.xbpf | 0.50 | 0.48 | 0.48 |
| decal.xbpf | 0.43 | 0.32 | 0.35 |
| mx.xwb, _1, _3 | 1.86 | ~0.04 (skip) | ~0.06 (skip) |
| **Packed set** | | **~1.18 (gzip: _2 + decals)** | **~1.63 (zstd: + assets_x.dat)** |

Add rim.xbpf (0.22), banks.dat (0.86 gzip, 0.48 zstd) and dx.xwb (0.88 / 0.80) in proportion to their share of the
remaining ~1.1 TB. They are not in the top list, so this is likely under 0.1 TB/day more.

**Caveat:** the decal traffic (0.93 TB/day for two files of 596 MiB, ~1,600 full reads a day) looks like re-fetching.
Another scout is fixing that in wasmfs_fetch.js now. If that fix lands, the decal share of the saving shrinks with it.

## Design

### Storage in R2: one packed object per file, index in the manifest

Rejected: **one object per block** (with `Content-Encoding`, so the browser decodes it, brotli included). Every run
the client asks for in one request today would become N requests. That is 3 per readahead run, up to 16 per fill
run, and the music head runs. GetObject would rise ~5-16x. That breaks the hard rule: R2 bills requests.

Rejected: **whole-file `Content-Encoding`**. It allows no random access, and a Range read of an encoded body cannot
be decoded.

Chosen: **`discs/remix-usa/<path>.gzb`**, the file's blocks one after another, each one either:

- a gzip member of that block, or
- the block's raw bytes, when gzip saves under 3%.

A stored length equal to the block's raw length means "raw". A gzip member of a 1 MiB block that saves at least 3% is
always shorter, so no flag is needed. The raw `<path>` objects stay for old pages, local servers and fallback. Extra
R2 storage is ~0.25 GB (the gzipped _2, decals and rim): cents a month.

The index is a new manifest field per packed file, `"z": [stored length of block 0, 1, ...]`. The client builds the
offsets from it. Old pages ignore the field: `C.manifest` returns the objects, and `C.key` reads only `p` and `s`. The
manifest is already fetched once a session, so the index costs **no extra request**. Its size is ~850 numbers, ~6 KB.
Files without `z` are read raw, as today.

### Requests: the same count

A run of blocks `bi .. bi+n-1` is contiguous in the packed object too. It is still **one** Range request:
`bytes=off[bi]-(off[bi+n]-1)`. fetchRun, fetchRuns, readahead (RA_RUN 3), fill runs (PREFETCH_RUN 16), the music
heads and the stall retry all keep their request count. The retry restarts from the first block not yet in, at
`off[first missing]`.

### Client (web/disc_cache.js + web/wasmfs_fetch.js, both in the module: `make web`)

- `describe()` / `fileInfo()`: when the manifest entry has `z`, set `fi.z` = offsets (N+1 entries) and
  `src = sig.url(rel + '.gzb')`. The signer needs no change: the Worker signs any path under `discs/<name>/` (it
  does not check the manifest), and its server fallback serves any key with a single Range.
- `fetchBlocks(url, bi, n, size, {z})`: Range in packed offsets. It cuts the body by stored length, not BLOCK. Each
  finished piece is decoded before `onBlock`, in order: raw when its length equals `blockLen`, else
  `new Response(new Blob([piece]).stream().pipeThrough(new DecompressionStream('gzip'))).arrayBuffer()`. Then it
  checks that the result has `blockLen` bytes. A decode error counts as a failed try, so it is retried like a cut
  connection. The read's own block (first in the range) is still handed over first.
- A 404 on `.gzb` (a manifest newer than the bucket) drops `z` for that file for the session and reads raw.
- OPFS, the memory LRU and `pinned` keep **decoded** 1 MiB blocks. Cache keys (path + size) and the scheme stay as
  they are. Blocks already cached stay valid, and reading the cache costs no CPU.
- The predownload worker uses the same `fetchBlocks` and gets it for free.
- A local or LAN server (web_serve.py, mc3-server): its manifest has no `z`, so it reads raw. To test the packed
  path on :3000, web_serve.py needs an opt-in (a `.gzb` + `z` from the same pack tool) and `?MC3_DISC_CACHE=1`.

Roughly 40 lines in `fetchBlocks`, 5 in `wasmfs_fetch.js`, and a relink.

### Upload (.deploy/upload.sh disc)

A pack step, `tools/disc_pack.py`, runs before `rclone copy`. For each file in a fixed list (`assets_x.dat_2`,
`resources/vehicle/decal.xbpf`, `decal_g.xbpf`, `rim.xbpf`) it:

1. compresses each block with `zlib.compressobj(9, wbits=31)`, keeping it raw when that saves under 3%;
2. writes `.deploy/.upload/z/<path>.gzb`;
3. adds `z` to that file's entry in `$tmp/$name.manifest`.

Then `rclone copy .deploy/.upload/z "$remote/discs/$name"`, with the manifest uploaded **after** the `.gzb` objects,
so no page sees a `z` whose object is not up yet. The pack step needs a round-trip check (decode every block, compare
with the disc), and a node test of the client's cut-and-decode against the same files.

### Phase 2 (only if phase 1 helps): zstd

Pack `assets_x.dat` too, and `_2` as zstd (0.28 instead of 0.49), with the decoder in wasm. Browsers'
DecompressionStream has gzip/deflate only: brotli and zstd are not available in every browser we support. Brotli's
own `Content-Encoding` decoding works only on whole responses, which needs one object per block (rejected above).

Pick zstd over brotli. Its decoder is faster (1.4 vs 5.9 ms/MiB), it needs no 122 KB built-in dictionary, and it
gives nearly the same ratio. The decoder would be the zstd decompress-only single-file source (`zstddeclib.c`, BSD),
compiled into the module behind `__EMSCRIPTEN__` and called from the disc thread. The disc thread is one of the
module's pthreads, so input and output go through `malloc`'d wasm buffers. It adds ~100 KB of wasm and a codec byte
in the index (`"zc": "zstd"`). Extra saving: ~0.27 TB/day (assets_x.dat) + ~0.15 TB/day (_2 beyond gzip), plus
banks.dat. Effort ~1-2 more days, including testing on phones.

## Recommendation

| Step | Saves | Requests | Effort | Risk |
|---|---|---|---|---|
| gzip: _2, decal, decal_g, rim | ~1.2 TB/day (~22%) | unchanged | ~1 day (pack tool, ~45 lines of client, upload order) | low: raw fallback, old pages unaffected |
| zstd wasm: + assets_x.dat, _2 as zstd | ~0.4 TB/day more | unchanged | ~1-2 days | medium: wasm decoder on phones, module size |
| music, video, _1, _3 | ~0 | | | not worth it |

Do phase 1, after the decal re-fetch fix lands, so the saving can be measured on its own (`[WEB] disc wait` lines,
cold car builds). Judge phase 2 on players' disc waits in `assets_x.dat`, not on TB: the bytes cost nothing in R2.
