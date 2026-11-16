#!/usr/bin/env python3
"""Make the hi-res HUD/map/menu texture pack (mods/hud_hd) for the WebGPU renderer.

  hud_hd_pack.py pack <disc dir> <out pack dir> [--scale 4] [--only GLOB ...] [--id hud_hd]
  hud_hd_pack.py keys <file.tex> ...      the renderer's key of each (stdlib only; the host
                                          test xboxrecomp/src/nv2a_wgpu/wgpu_hires_test.c
                                          checks it against the renderer's decode)

<disc dir>  the Xbox Remix disc (game-remix-usa): stock textures from its assets_x.dat
            (tools/dave.py), texture_x/<prefix>_*.tex for the PREFIXES below (HUD, race
            maps, navigation and menu art: 244 files, 221 without the 16x16 *_proxy
            stand-ins, which are left out).
<out>       gets textures/<key>.png (16 hex digits, the renderer's content key,
            xboxrecomp/src/nv2a_wgpu/wgpu_hires.h), textures/index.tsv (key, name, size,
            type, method) and mod.json (kind textures). mods.c hands textures/ to the
            WebGPU renderer (wgpu_tex.c "Hi-res replacements"); the game keeps the stock
            textures and sizes, so layout code (UV = pixel / texture size) is untouched.

.tex: 14-byte header u16 width, height, type, mip count, u16 1, u32 0, then the data.
Types met here (sizes checked against every file):
  0x16 DXT1, 0x18 DXT3, 0x1A DXT5   blocks, linear (not swizzled), levels largest first
  0x01 P8 (opaque), 0x0E PA8        256 BGRA palette (1024 bytes), then w x h u8 indices
  0x0F P4 (opaque), 0x10 PA4        16 BGRA palette (64 bytes), then w x h / 2 bytes of
                                    4-bit indices, low nibble first
Palettised indices are linear rows (decoded images are smooth that way and noise
swizzled; the speedometer's layout matches its tune file). Their alpha is full range
(0..255), not the PS2's 0..0x80.

Key (wgpu_hires.h): DXT -> level 0's blocks, kind = NV2A colour (0x0C/0x0E/0x0F);
else the palette looked up, B G R A per texel, kind 0. So the title's own storage of
a palettised image (P8 or 4-bit, swizzled or not, 16- or 256-entry palette, or turned
into A8R8G8B8) gives the same key.

Upscale (--scale, default 4): xbrz (pip install xbrz.py) for images of at most 256
colours (the palettised ones, flat UI) when it is installed, else Lanczos on
premultiplied colour + a light unsharp mask; alpha snapped at the ends (< 4 -> 0,
> 251 -> 255), and fully transparent texels keep the straight colour (no dark
fringes once the worker compresses to DXT and the GPU filters).
"""
import argparse
import fnmatch
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dave  # noqa: E402

PREFIXES = ('hud', 'navsys', 'msg', 'illustration', 'icon', 'background', 'bottom', 'barround', 'bar',
            'layer')
DXT = {0x16: (0x0C, 8, 1), 0x18: (0x0E, 16, 2), 0x1A: (0x0F, 16, 3)}   # type: NV2A colour, block bytes, PIL bcn n
M64, PRIME = (1 << 64) - 1, 0x100000001B3


def key(data, w, h, kind):
    """wgpu_hires.h wg_hires_key."""
    k = 0xCBF29CE484222325 ^ (w << 48) ^ (h << 32) ^ kind
    n8 = len(data) // 8 * 8
    for (x,) in struct.iter_unpack('<Q', data[:n8]):
        k = ((k ^ x) * PRIME) & M64
    for b in data[n8:]:
        k = ((k ^ b) * PRIME) & M64
    return k or 1


def content(tex):
    """(w, h, type, kind, level-0 bytes the key covers)."""
    w, h, t = struct.unpack_from('<3H', tex, 0)
    if t in DXT:
        colour, block, _ = DXT[t]
        n = ((w + 3) // 4) * ((h + 3) // 4) * block
        return w, h, t, colour, tex[14:14 + n]
    if t in (0x01, 0x0E):
        pal, idx = tex[14:14 + 1024], tex[14 + 1024:14 + 1024 + w * h]
        assert len(tex) == 14 + 1024 + w * h, ('size', len(tex))
    elif t in (0x0F, 0x10):
        pal, packed = tex[14:14 + 64], tex[14 + 64:14 + 64 + w * h // 2]
        assert len(tex) == 14 + 64 + w * h // 2, ('size', len(tex))
        idx = bytes(v for b in packed for v in (b & 15, b >> 4))
    else:
        raise ValueError('.tex type 0x%02X' % t)
    entries = [pal[i * 4:i * 4 + 4] for i in range(len(pal) // 4)]
    return w, h, t, 0, b''.join(entries[i] for i in idx)


def rgba(tex):
    """Level 0 as a PIL RGBA image, and its colour count (capped)."""
    import numpy as np
    from PIL import Image
    w, h, t, kind, data = content(tex)
    if kind:
        im = Image.frombytes('RGBA', (w, h), bytes(data), 'bcn', DXT[t][2])
    else:
        im = Image.fromarray(np.frombuffer(data, np.uint8).reshape(h, w, 4)[:, :, [2, 1, 0, 3]].copy(), 'RGBA')
    colours = im.getcolors(257)
    return im, len(colours) if colours else 257


def upscale(im, colours, scale):
    """(image scale x larger, method)."""
    import numpy as np
    from PIL import Image, ImageFilter
    size = (im.width * scale, im.height * scale)
    out, method = None, 'lanczos'
    if colours <= 256:
        try:
            import xbrz
            out, method = xbrz.scale_pillow(im, scale).convert('RGBA'), 'xbrz'
        except Exception:      # not installed (or an older API): Lanczos below
            out = None
    if out is None:
        out = im.convert('RGBa').resize(size, Image.LANCZOS).convert('RGBA')
        a = out.getchannel('A')
        rgb = out.convert('RGB').filter(ImageFilter.UnsharpMask(radius=2, percent=50, threshold=2))
        out = Image.merge('RGBA', (*rgb.split(), a))
    px = np.array(out)
    a = px[:, :, 3]
    a[a < 4] = 0
    a[a > 251] = 255
    clear = a == 0
    if clear.any():   # straight colour under transparent texels: no dark fringes when filtered
        straight = np.array(im.convert('RGB').resize(size, Image.LANCZOS))
        px[:, :, :3][clear] = straight[clear]
    return Image.fromarray(px, 'RGBA'), method


def wanted(name, only):
    if not name.startswith('texture_x/') or not name.endswith('.tex') or name.endswith('_proxy.tex'):
        return False
    base = name[len('texture_x/'):]
    if only:
        return any(fnmatch.fnmatchcase(base, g) for g in only)
    return base.split('_')[0] in PREFIXES


def cmd_pack(a):
    arc = os.path.join(a.disc, 'assets_x.dat')
    hits = [e for e in dave.entries(arc) if wanted(e[1], a.only)]
    if not hits:
        sys.exit('%s: no texture matched' % arc)
    tdir = os.path.join(a.out, 'textures')
    os.makedirs(tdir, exist_ok=True)
    rows, done, total = [], {}, 0
    with open(arc, 'rb') as f:
        for _, name, off, size, stored in hits:
            tex = dave.read_entry(f, off, size, stored)
            w, h, t, kind, data = content(tex)
            k = key(data, w, h, kind)
            if k not in done:
                im, colours = rgba(tex)
                big, method = upscale(im, colours, a.scale)
                path = os.path.join(tdir, '%016x.png' % k)
                big.save(path, optimize=True)
                done[k] = (method, os.path.getsize(path))
                total += done[k][1]
            rows.append('%016x\t%s\t%dx%d\t0x%02X\t%s\t%d' % (k, name, w, h, t, *done[k]))
            print(rows[-1])
    with open(os.path.join(tdir, 'index.tsv'), 'w') as o:
        o.write('key\tname\tsize\ttype\tmethod\tpng_bytes\n' + '\n'.join(rows) + '\n')
    meta = {'id': a.id, 'name': 'HD HUD, maps and menus',
            'author': '', 'credit': '',
            'description': 'The HUD, race maps and menu art upscaled %dx from the game\'s own (WebGPU only). '
                           'Best with a higher Resolution.' % a.scale,
            'version': '1.0', 'kind': 'textures'}
    with open(os.path.join(a.out, 'mod.json'), 'w') as o:
        o.write(json.dumps(meta, indent=2) + '\n')
    print('%d textures, %d distinct, %.1f MB of PNG in %s' % (len(rows), len(done), total / 1e6, tdir),
          file=sys.stderr)
    return 0


def cmd_keys(a):
    for p in a.files:
        w, h, t, kind, data = content(open(p, 'rb').read())
        print('%016x\t%dx%d\t0x%02X\t%s' % (key(data, w, h, kind), w, h, t, p))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('pack')
    p.add_argument('disc')
    p.add_argument('out')
    p.add_argument('--scale', type=int, default=4)
    p.add_argument('--only', nargs='*', help='file name globs inside texture_x/ instead of PREFIXES')
    p.add_argument('--id', default='hud_hd')
    k = sub.add_parser('keys')
    k.add_argument('files', nargs='+')
    a = ap.parse_args()
    return cmd_pack(a) if a.cmd == 'pack' else cmd_keys(a)


if __name__ == '__main__':
    sys.exit(main())
