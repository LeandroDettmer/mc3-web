#!/usr/bin/env python3
"""Make a mods/ pack that swaps the Remix's in-game speedometer for a custom design.

  hud_speedo_pack.py <design image> <disc dir> <out pack dir>
                     [--id ID --name N --author A --credit C --description D --version V]
                     [--preview out.png] [--alpha ps2|keep]

<design image>  PNG or DDS, square, the PS2 Remix's speedometer texture layout (the PCSX2
                texture-replacement packs, e.g. almightypear's "MC3 Speedometer Pack" at
                384x384 = 3x): any square size, shrunk to the Xbox texture's 128x128.
<disc dir>      the Xbox Remix disc (game-remix-usa): the stock textures are read from
                its assets_x.dat (tools/dave.py).
<out pack dir>  gets texture_x/<name>.tex for each texture below + mod.json (kind
                textures); mods.c serves them over the archive's.

Where the speedometer lives (assets_x.dat, Remix USA):
  texture_x/hud_speed_1.tex, _2, _3   one per top speed on the dial (175 / 200 / 250);
                                      the XBE names them at 0x44FBBC.. (.rdata strings)
  texture_x/garage_hud_speed.tex      the garage's copy (byte-identical to hud_speed_1)
Each is a 128x128 atlas, dial and needle together (tune/hud/dynamic/
speedometer.huddamageshared, TexData): dial 108x108 at (0,0), needle 9x50 at (113,0),
pivot (118,38); needle sweep -130..119 deg. The image is stored transposed (text reads
mirrored); the PS2 texture has the same layout, so a PS2 design goes in pixel for pixel.
The tachometer (hud_tac_1..4: 80x80 dial, needle 8x32 at (106,0)) is not touched.

.tex type 0x0E: 14-byte header (u16 width, height, type, mips 1, u16 1, u32 0), then a
256-entry BGRA palette (1024 bytes) and width x height u8 indices, linear, one level.
The *_proxy.tex (16x16 type 0x10 stand-ins) are left stock.

--alpha ps2 (default): PS2 alpha 0x80 = opaque, so it is doubled (clamped to 255);
--alpha keep: the image's alpha as is.
"""
import argparse
import json
import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dave  # noqa: E402

NAMES = ['texture_x/hud_speed_1.tex', 'texture_x/hud_speed_2.tex', 'texture_x/hud_speed_3.tex',
         'texture_x/garage_hud_speed.tex']


def stock(disc):
    """{name: .tex bytes} of NAMES from <disc>/assets_x.dat."""
    arc = os.path.join(disc, 'assets_x.dat')
    want = {e[1]: e for e in dave.entries(arc) if e[1] in NAMES}
    missing = set(NAMES) - set(want)
    if missing:
        sys.exit('%s: not found: %s' % (arc, ', '.join(sorted(missing))))
    with open(arc, 'rb') as f:
        return {n: dave.read_entry(f, *want[n][2:]) for n in NAMES}


def decode(tex):
    w, h, t = struct.unpack_from('<3H', tex, 0)
    pal = np.frombuffer(tex[14:14 + 1024], np.uint8).reshape(256, 4)[:, [2, 1, 0, 3]]
    return pal[np.frombuffer(tex[14 + 1024:14 + 1024 + w * h], np.uint8).reshape(h, w)]


def design(path, w, h, alpha):
    im = Image.open(path).convert('RGBA')
    if im.width != im.height:
        sys.exit('%s: %dx%d, expected a square image' % (path, im.width, im.height))
    if alpha == 'ps2':
        im.putalpha(im.getchannel('A').point(lambda v: min(255, v * 2)))
    return im.convert('RGBa').resize((w, h), Image.LANCZOS).convert('RGBA')   # premultiplied: no dark fringes


def encode(im, header):
    q = im.quantize(256, method=Image.Quantize.FASTOCTREE)
    pal = np.zeros((256, 4), np.uint8)
    p = np.frombuffer(bytes(q.getpalette('RGBA')), np.uint8).reshape(-1, 4)[:256]
    pal[:len(p)] = p
    return header + pal[:, [2, 1, 0, 3]].tobytes() + np.asarray(q, np.uint8).tobytes()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('design')
    ap.add_argument('disc')
    ap.add_argument('out')
    ap.add_argument('--alpha', choices=('ps2', 'keep'), default='ps2')
    ap.add_argument('--preview')
    base = os.path.splitext(os.path.basename(os.path.normpath(sys.argv[1] if len(sys.argv) > 1 else 'x')))[0]
    for k, v in (('id', None), ('name', 'Speedometer: ' + base), ('author', ''), ('credit', ''),
                 ('description', 'Custom in-game speedometer (' + base + ')'), ('version', '1.0')):
        ap.add_argument('--' + k, default=v)
    a = ap.parse_args()

    texs = stock(a.disc)
    w, h, t, mips = struct.unpack_from('<4H', texs[NAMES[0]], 0)
    assert (w, h, t, mips) == (128, 128, 0x0E, 1), (w, h, t, mips)
    assert all(len(d) == 14 + 1024 + w * h for d in texs.values())
    im = design(a.design, w, h, a.alpha)
    for n in NAMES:
        new = encode(im, texs[n][:14])
        assert len(new) == len(texs[n])
        err = np.abs(decode(new).astype(int) - np.asarray(im, int)).mean()   # self-check: round trip
        assert err < 4, (n, err)
        dst = os.path.join(a.out, *n.split('/'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, 'wb').write(new)
        print('%s: %d bytes, round-trip error %.2f' % (dst, len(new), err))
    meta = {'id': a.id or os.path.basename(os.path.normpath(a.out)), 'name': a.name, 'author': a.author,
            'credit': a.credit, 'description': a.description, 'version': a.version, 'kind': 'textures'}
    open(os.path.join(a.out, 'mod.json'), 'w').write(json.dumps(meta, indent=2) + '\n')
    if a.preview:   # stock dial | new dial, transposed so the text reads (still turned 90 deg)
        old, new = (Image.fromarray(decode(d)).transpose(Image.TRANSPOSE) for d in (texs[NAMES[0]], encode(im, texs[NAMES[0]][:14])))
        sheet = Image.new('RGBA', (w * 2, h), (40, 40, 40, 255))
        sheet.alpha_composite(old, (0, 0))
        sheet.alpha_composite(new, (w, 0))
        sheet.resize((w * 6, h * 3), Image.NEAREST).save(a.preview)
    return 0


if __name__ == '__main__':
    sys.exit(main())
