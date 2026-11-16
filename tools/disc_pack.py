#!/usr/bin/env python3
"""Packed copies of a disc's most compressible files, read per 1 MiB block (docs/disc-compression.md, phase 1).

    py tools/disc_pack.py game-remix-usa          # writes game-remix-usa.gzb/ beside the disc folder

For each of FILES: <disc>.gzb/<path>.gzb holds the file's 1 MiB blocks one after another, each a gzip member of that
block, or the block's raw bytes when gzip saves under 3% (a stored length equal to the block's length means raw).
<disc>.gzb/index.json keeps, per file, its size, mtime and "z": the stored length of every block. disc_manifest()
(tools/web_serve.py, .deploy/upload.sh) adds that "z" to the file's manifest entry while the file still has that size
and mtime and its .gzb is whole; the page (web/disc_cache.js fetchBlocks) then reads ranges of the .gzb and decodes
each block (DecompressionStream). Files already packed for their size and mtime are skipped. Every block is checked
by decoding it again before it is written. Standard library only. The disc folder itself is never written.
"""
import json
import os
import sys
import zlib
from concurrent.futures import ThreadPoolExecutor

BLOCK = 1 << 20
FILES = ["assets_x.dat_2", "resources/vehicle/decal.xbpf", "resources/vehicle/decal_g.xbpf", "resources/vehicle/rim.xbpf"]
MIN_GAIN = 0.03


def side(disc):
    """the packed copies' folder of a disc folder"""
    return os.path.abspath(disc).rstrip("/\\") + ".gzb"


def pack_block(raw):
    c = zlib.compressobj(9, zlib.DEFLATED, 31)          # wbits 31: a gzip member
    z = c.compress(raw) + c.flush()
    if len(z) > len(raw) * (1 - MIN_GAIN):
        return raw
    if zlib.decompress(z, 31) != raw:
        raise RuntimeError("gzip round trip failed")
    return z


def index(disc):
    try:
        with open(os.path.join(side(disc), "index.json"), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def packed(disc, rel, size, mtime):
    """the stored block lengths of file `rel` when its .gzb fits that size and mtime, else None"""
    e = index(disc).get(rel)
    if not e or e["s"] != size or e["m"] != mtime or len(e["z"]) != -(-size // BLOCK):
        return None
    try:
        return e["z"] if os.path.getsize(os.path.join(side(disc), *rel.split("/")) + ".gzb") == sum(e["z"]) else None
    except OSError:
        return None


def pack(disc, rel):
    src = os.path.join(disc, *rel.split("/"))
    st = os.stat(src)
    if packed(disc, rel, st.st_size, int(st.st_mtime)):
        print(f"{rel}: up to date")
        return
    out = os.path.join(side(disc), *rel.split("/")) + ".gzb"
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(src, "rb") as f:
        blocks = iter(lambda: f.read(BLOCK), b"")
        with ThreadPoolExecutor(os.cpu_count()) as ex, open(out + ".tmp", "wb") as w:
            z = [w.write(b) for b in ex.map(pack_block, blocks)]   # zlib frees the GIL: blocks in parallel, kept in order
    os.replace(out + ".tmp", out)
    idx = index(disc)
    idx[rel] = {"s": st.st_size, "m": int(st.st_mtime), "z": z}
    tmp = os.path.join(side(disc), "index.json.tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(idx, f)
    os.replace(tmp, os.path.join(side(disc), "index.json"))
    raw = sum(n == min(BLOCK, st.st_size - i * BLOCK) for i, n in enumerate(z))
    print(f"{rel}: {st.st_size / BLOCK:.0f} MiB -> {sum(z) / BLOCK:.1f} MiB ({sum(z) / st.st_size:.3f}), {raw} blocks raw")


def main():
    if len(sys.argv) != 2 or not os.path.isfile(os.path.join(sys.argv[1], "default.xbe")):
        sys.exit("usage: disc_pack.py <disc folder with default.xbe>")
    for rel in FILES:
        if os.path.isfile(os.path.join(sys.argv[1], *rel.split("/"))):
            pack(sys.argv[1], rel)
    print(f"packed copies in {side(sys.argv[1])}")


if __name__ == "__main__":
    main()
