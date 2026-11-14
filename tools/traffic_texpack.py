#!/usr/bin/env python3
"""Turn a PCSX2 texture-replacement pack for the PS2 Remix (SLUS-21355) into a
mods/ overlay of the Xbox Remix's traffic-car textures.

  traffic_texpack.py <png dir> <xbox dir> <out pack dir> [--scale 1|2]
                     [--max-err E] [--force PNGPREFIX=name.tex ...] [--id ID --name N --author A --credit C
                     --description D --version V]

<png dir>  the pack's replacements/ (PCSX2 names <tex hash>-<clut hash>-<TEX0
           bits>.png: they say nothing about the Xbox files, so each PNG is
           matched by picture).
<xbox dir> the traffic textures as tools/dave.py extracts them:
           dave.py extract assets_x.dat_1 'vehicle/va_*.tex' <xbox dir>
           -> <xbox dir>/vehicle/va_<car>/texture_x/<name>.tex
<out>      gets vehicle/va_<car>/texture_x/<name>.tex for every matched texture
           (every folder holding a byte-identical copy) + mod.json.

  traffic_texpack.py xbck <xbox dir> <out pack dir>
           only the step below, on a pack already made.

Resident cars: the traffic resource "$/resources/city/<city>_traffic" (.xbck,
loaded by sub_0006D9F0, a fatal assert if missing) holds the city's car
roster and, built in, the models and textures of its commonest cars (civic,
odessey, tacoma, citybus, deliverytruck, bus_t, the tram/trains, shared
wheels and undercar): those never read the loose .tex. So when <xbox dir>
also has resources/city/*_traffic.xbck (dave.py extract <each assets_x.dat_N>
'resources/city/*_traffic.xbck' <xbox dir>), every built-in texture whose
data equals a stock .tex the pack replaced is patched in place and the
patched .xbck goes into <out>/resources/city/ (same size only: --scale 1).

.tex (loose files in assets_x.dat_1, the roster's other cars): 14-byte header u16 width,
height, type (0x16 DXT1, 0x1A DXT5, 0x18 DXT3, 1 P8), mip count, u16 1, u32 0;
then the levels, DXT blocks in linear order (not swizzled), largest first.

Each PNG is shrunk to the Xbox texture's shape and compared (mean absolute RGB
difference, 0-255) with every Xbox texture of that aspect; the best one under
--max-err wins; of several PNGs for one texture (the PS2's 128 and 256 levels,
a variant's CLUT) the closest is used. --force pairs a PNG (name prefix) with
a texture whatever the score (lookalike wheels, trains whose adverts changed). Output keeps the original's format and
mip count at --scale x its size (2 = 4x the bytes); alpha comes from the
original (the PS2 dumps carry none, or PS2's 0-128 range), so DXT5 masks and
DXT1 punch-through stay the same.
"""
import argparse
import hashlib
import io
import json
import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mc3car  # noqa: E402

TYPES = {0x16: ('DXT1', 8), 0x1A: ('DXT5', 16)}


def read_tex(path):
    d = open(path, 'rb').read()
    w, h, t, mips = struct.unpack_from('<4H', d, 0)
    im, _ = mc3car.tex_file(path)
    return {'w': w, 'h': h, 't': t, 'mips': mips, 'size': len(d), 'im': im.convert('RGBA'),
            'md5': hashlib.md5(d).hexdigest()}


def thumb(im, w, h):
    return np.asarray(im.convert('RGB').resize((w, h), Image.BOX), np.float32)


def encode(im, t):
    """One mip level of RGBA -> DXT blocks (Pillow's BCn encoder)."""
    b = io.BytesIO()
    im.save(b, 'DDS', pixel_format=TYPES[t][0])
    d = b.getvalue()
    n = max(1, (im.width + 3) // 4) * max(1, (im.height + 3) // 4) * TYPES[t][1]
    assert d[84:88] == TYPES[t][0].encode() and len(d) == 128 + n, (len(d), n)
    return d[128:]


def build_tex(png, x, scale):
    w, h = x['w'] * scale, x['h'] * scale
    rgb = png.convert('RGB').resize((w, h), Image.LANCZOS)
    if png.mode == 'RGBA' and png.getchannel('A').getextrema() == (0, min(128, png.getchannel('A').getextrema()[1])):
        # PS2-range alpha (0-128): its 0 areas (windows) hold filler colour,
        # so the original's colour stays there
        hole = png.getchannel('A').resize((w, h), Image.NEAREST).point(lambda a: 255 if a == 0 else 0)
        rgb.paste(x['im'].convert('RGB').resize((w, h), Image.LANCZOS), (0, 0), hole)
    alpha = x['im'].getchannel('A')
    if x['t'] == 0x16 and alpha.getextrema()[0] >= 128:
        alpha = None                     # DXT1 without punch-through: opaque
    if alpha is not None:
        alpha = alpha.resize((w, h), Image.LANCZOS if x['t'] == 0x1A else Image.NEAREST)
        if x['t'] == 0x16:               # DXT1: 1-bit alpha, keep it hard
            alpha = alpha.point(lambda a: 255 if a >= 128 else 0)
    im = rgb.convert('RGBA')
    if alpha is not None:
        im.putalpha(alpha)
    out = [struct.pack('<5HI', w, h, x['t'], x['mips'], 1, 0)[:14]]
    for i in range(x['mips']):
        lw, lh = max(1, w >> i), max(1, h >> i)
        out.append(encode(im if i == 0 else im.resize((lw, lh), Image.LANCZOS), x['t']))
    return b''.join(out), im


def patch_traffic(xbox, out):
    """<xbox>/resources/city/<city>_traffic.xbck -> <out>/resources/city/, with every texture
    the pack replaced patched in place (same size: --scale 1 only). Returns files written."""
    rep = {}                             # stock level data -> the pack's (both without the 14-byte header)
    for root, _, names in os.walk(os.path.join(out, 'vehicle')):
        for n in names:
            if n.endswith('.tex'):
                rel = os.path.relpath(os.path.join(root, n), out)
                new, old = open(os.path.join(out, rel), 'rb').read(), open(os.path.join(xbox, rel), 'rb').read()
                if len(new) == len(old) and new != old:
                    rep[old[14:]] = new[14:]
    cdir, written = os.path.join(xbox, 'resources', 'city'), 0
    for n in sorted(os.listdir(cdir)) if os.path.isdir(cdir) else ():
        if not n.endswith('_traffic.xbck'):
            continue
        d = bytearray(open(os.path.join(cdir, n), 'rb').read())
        base, size = struct.unpack_from('<I', d, 0)[0], struct.unpack_from('<I', d, 12)[0]
        assert size == len(d) - 0x80, (n, size, len(d))
        texs = hits = 0
        for o in range(0x80, len(d) - 19, 4):    # D3DTexture: Common 0x00040001, Data (physical), Lock 0, Format, Size 0
            com, data, lock, fmt, sz = struct.unpack_from('<5I', d, o)
            p = (data + 0x80000000 - base + 0x80) & 0xFFFFFFFF
            if com != 0x00040001 or lock or sz or not 0x80 <= p < len(d):
                continue
            texs += 1
            old = next((k for k in rep if d[p:p + len(k)] == k), None)
            if old is not None:
                d[p:p + len(old)] = rep[old]
                hits += 1
        print('%s: %d of its %d textures replaced' % (n, hits, texs))
        if hits:
            dst = os.path.join(out, 'resources', 'city', n)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, 'wb').write(d)
            written += 1
    if not os.path.isdir(cdir):
        print('no %s: the resident traffic cars keep their textures' % cdir)
    return written


def main():
    if len(sys.argv) == 4 and sys.argv[1] == 'xbck':
        patch_traffic(sys.argv[2], sys.argv[3])
        return
    ap = argparse.ArgumentParser()
    ap.add_argument('pngs'); ap.add_argument('xbox'); ap.add_argument('out')
    ap.add_argument('--scale', type=int, default=1, choices=(1, 2))
    ap.add_argument('--max-err', type=float, default=40.0)
    ap.add_argument('--force', action='append', default=[])
    for k, v in (('id', 'enhanced_traffic'), ('name', 'Enhanced Traffic'),
                 ('author', 'victor, almightypear'),
                 ('credit', ''),
                 ('description', 'Traffic vehicles retextured (PS2 Remix "MC3 Enhanced Traffic" '
                                 'texture pack, converted to the Xbox textures)'),
                 ('version', '1.0')):
        ap.add_argument('--' + k, default=v)
    a = ap.parse_args()

    # Xbox textures, one entry per distinct file (copies in several car folders)
    xs, where = {}, {}
    vroot = os.path.join(a.xbox, 'vehicle')
    for car in sorted(os.listdir(vroot)):
        tdir = os.path.join(vroot, car, 'texture_x')
        for n in sorted(os.listdir(tdir)) if os.path.isdir(tdir) else ():
            if not n.endswith('.tex') or n == 'vp_shadow_ambient.tex':
                continue
            x = read_tex(os.path.join(tdir, n))
            if x['t'] not in TYPES:
                continue
            key = (n, x['md5'])
            xs.setdefault(key, x)
            where.setdefault(key, []).append('vehicle/%s/texture_x/%s' % (car, n))

    # match every PNG to its closest Xbox texture of the same aspect
    best = {}                            # xbox key -> (err, png name)
    rows = []
    for f in sorted(os.listdir(a.pngs)):
        if not f.lower().endswith('.png'):
            continue
        im = Image.open(os.path.join(a.pngs, f)); im.load()
        asp = im.width / im.height
        cands = []
        for key, x in xs.items():
            if abs(asp - x['w'] / x['h']) > 0.1:
                continue
            tw, th = (64, 64 * x['h'] // x['w'])
            err = float(np.abs(thumb(im, tw, th) - thumb(x['im'], tw, th)).mean())
            cands.append((err, key))
        cands.sort()
        if not cands:
            rows.append((f, None, None, None)); continue
        err, key = cands[0]
        second = cands[1][0] if len(cands) > 1 else None
        rows.append((f, key, err, second))
        if err <= a.max_err and (key not in best or err < best[key][0]):
            best[key] = (err, f)

    for fp in a.force:
        pre, tex = fp.split('=')
        f = next(n for n in sorted(os.listdir(a.pngs)) if n.startswith(pre))
        key = next(k for k in xs if k[0] == tex)
        for k in [k for k, v in best.items() if v[1] == f]:
            del best[k]
        best[key] = (-1.0, f)
        rows = [(r[0], key, -1.0, None) if r[0] == f else r for r in rows]
    print('%-52s %-30s %6s %6s' % ('png', 'xbox texture', 'err', '2nd'))
    for f, key, err, second in rows:
        used = key in best and best[key][1] == f
        print('%-52s %-30s %6s %6s %s' % (f[:52], key[0] if key else '-',
              '%.1f' % err if err is not None else '-', '%.1f' % second if second else '-',
              ('FORCED' if err == -1.0 else 'USED') if used else
              ('rejected' if err is None or err > a.max_err else 'dup')))

    total_old = total_new = files = 0
    for key, (err, f) in sorted(best.items()):
        x = xs[key]
        data, want = build_tex(Image.open(os.path.join(a.pngs, f)), x, a.scale)
        # self-check: the written file decodes back to the picture we encoded
        tmp = os.path.join(a.out, '.check.tex')
        os.makedirs(a.out, exist_ok=True)
        open(tmp, 'wb').write(data)
        back, info = mc3car.tex_file(tmp)
        os.remove(tmp)
        assert (info['w'], info['h']) == (x['w'] * a.scale, x['h'] * a.scale), info
        chk = float(np.abs(thumb(back, 64, 64 * x['h'] // x['w'])
                           - thumb(want, 64, 64 * x['h'] // x['w'])).mean())
        assert chk < 6, (key, chk)
        for rel in where[key]:
            dst = os.path.join(a.out, *rel.split('/'))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, 'wb').write(data)
            total_old += x['size']; total_new += len(data); files += 1
        print('%-30s %3dx%-3d %s mips %d  %6d -> %6d B  x%d copies  (%s, err %.1f, check %.1f)'
              % (key[0], x['w'], x['h'], TYPES[x['t']][0], x['mips'], x['size'], len(data),
                 len(where[key]), f[:24], err, chk))
    missing = sorted(k[0] for k in xs if k not in best)
    meta = {'id': a.id, 'name': a.name, 'author': a.author, 'credit': a.credit,
            'description': a.description, 'version': a.version, 'kind': 'textures'}
    json.dump(meta, open(os.path.join(a.out, 'mod.json'), 'w'), indent=2)
    print('%d textures, %d files: %d B stock -> %d B (scale %d)'
          % (len(best), files, total_old, total_new, a.scale))
    print('not replaced: %s' % ', '.join(missing))
    patch_traffic(a.xbox, a.out)


if __name__ == '__main__':
    main()
