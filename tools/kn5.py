#!/usr/bin/env python3
"""Read an Assetto Corsa .kn5 model and write a .glb plus its textures as PNG.

usage: kn5.py model.kn5 outdir [--max-tex 512] [--tex-from full.kn5] [--skin dir] [--report]

kn5 layout (public): 'sc6969', int32 version (+int32 if > 5), textures
(active, name, size, DDS bytes), materials (name, shader, props, texture
slots), then a node tree: dummies carry a row-major 4x4, meshes carry
vertices (pos, normal, uv, tangent) + u16 indices + material id.
Coordinates stay as AC has them (x right, y up, z forward, left-handed);
world transforms are baked into the vertices. Needs numpy + Pillow.
"""
import argparse, io, json, mmap, os, struct, sys
import numpy as np
from PIL import Image


class R:
    def __init__(s, b): s.b, s.p = b, 0
    def u(s, f):
        n = struct.calcsize(f); v = struct.unpack_from('<' + f, s.b, s.p); s.p += n
        return v if len(v) > 1 else v[0]
    def raw(s, n):
        v = s.b[s.p:s.p + n]; s.p += n
        if len(v) != n: raise ValueError('kn5 truncated at %d' % s.p)
        return v
    def str(s): return s.raw(s.u('i')).decode('utf-8', 'replace')


def parse(path, tex_only=False):
    f = open(path, 'rb'); r = R(mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ))
    if r.raw(6) != b'sc6969': raise ValueError('not a kn5')
    ver = r.u('i')
    if ver > 5: r.u('i')
    textures = {}
    for _ in range(r.u('i')):
        r.u('i'); name = r.str(); textures[name] = r.raw(r.u('i'))
    if tex_only: return {'textures': textures}
    materials = []
    for _ in range(r.u('i')):
        m = {'name': r.str(), 'shader': r.str()}
        r.u('H')                       # alphaBlendMode, alphaTested
        if ver > 4: r.u('i')           # depthMode
        m['props'] = {}
        for _ in range(r.u('i')):
            k = r.str(); m['props'][k] = r.u('f'); r.raw(36)
        m['tex'] = {}
        for _ in range(r.u('i')):
            slot = r.str(); r.u('i'); m['tex'][slot] = r.str()
        materials.append(m)
    nodes = []  # flat list, parent index, world matrix (row-vector convention)

    def node(parent, pw):
        t = r.u('i'); name = r.str(); nch = r.u('i'); r.u('B')
        n = {'name': name, 'type': t, 'parent': parent}
        if t == 1:
            loc = np.array(r.u('16f'), np.float64).reshape(4, 4)
            w = loc @ pw
        elif t in (2, 3):
            r.raw(3)  # castShadow, visible, transparent
            if t == 3:
                for _ in range(r.u('i')): r.str(); r.raw(64)
            nv = r.u('i'); step = 11 if t == 2 else 19
            v = np.frombuffer(r.raw(nv * step * 4), '<f4').reshape(nv, step)
            ni = r.u('i'); idx = np.frombuffer(r.raw(ni * 2), '<u2').astype(np.uint32)
            n['mat'] = r.u('i'); r.u('i'); n['lod'] = r.u('2f'); r.raw(16); r.u('B')
            w = pw
            p = np.c_[v[:, 0:3], np.ones(nv)] @ w
            nm = v[:, 3:6] @ w[:3, :3]
            nm /= np.maximum(np.linalg.norm(nm, axis=1, keepdims=True), 1e-9)
            n.update(pos=p[:, :3].astype('f4'), nrm=nm.astype('f4'), uv=v[:, 6:8].astype('f4'), idx=idx)
        else:
            raise ValueError('unknown node type %d at %d' % (t, r.p))
        n['world'] = w
        me = len(nodes); nodes.append(n)
        for _ in range(nch): node(me, w)

    node(-1, np.eye(4))
    return {'version': ver, 'textures': textures, 'materials': materials, 'nodes': nodes}


def tex_png(dds, mx):
    im = Image.open(io.BytesIO(dds)); im.load()
    im = im.convert('RGBA')
    if max(im.size) > mx:
        k = mx / max(im.size); im = im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.LANCZOS)
    return im


def export(path, outdir, mx=512, skin_dir=None, tex_from=None):
    k = parse(path)
    if tex_from: k['textures'].update(parse(tex_from, True)['textures'])  # LODs hold no textures; sandero.kn5 does
    os.makedirs(outdir, exist_ok=True)
    base = os.path.splitext(os.path.basename(path))[0]
    meshes = [n for n in k['nodes'] if 'pos' in n]
    # textures actually used by a mesh's material (diffuse-like slots)
    used = {}
    for n in meshes:
        for slot, t in k['materials'][n['mat']]['tex'].items():
            used[t] = 1
    pngs = {}
    for name in used:
        src = k['textures'].get(name)
        if src is None and skin_dir and os.path.exists(os.path.join(skin_dir, name)):
            src = open(os.path.join(skin_dir, name), 'rb').read()
        if src is None: continue
        try:
            im = tex_png(src, mx)
        except Exception as e:
            print('texture %s: %s' % (name, e), file=sys.stderr); continue
        fn = os.path.splitext(name)[0] + '.png'
        im.save(os.path.join(outdir, fn)); pngs[name] = (fn, im.size, len(src))
    # glb
    bin_, views, acc, gnodes, gmesh, gmat, gtex, gimg = bytearray(), [], [], [], [], [], [], []
    def add(data, target=None):
        while len(bin_) % 4: bin_.append(0)
        views.append({'buffer': 0, 'byteOffset': len(bin_), 'byteLength': len(data), **({'target': target} if target else {})})
        bin_.extend(data); return len(views) - 1
    imgidx, matidx = {}, {}
    for name, (fn, sz, _) in pngs.items():
        b = open(os.path.join(outdir, fn), 'rb').read()
        gimg.append({'bufferView': add(b), 'mimeType': 'image/png', 'name': fn}); gtex.append({'source': len(gimg) - 1})
        imgidx[name] = len(gtex) - 1
    def material(i):
        if i in matidx: return matidx[i]
        m = k['materials'][i]; d = m['tex'].get('txDiffuse')
        e = {'name': m['name'], 'pbrMetallicRoughness': {'metallicFactor': 0, 'roughnessFactor': 1}, 'doubleSided': True}
        if d in imgidx: e['pbrMetallicRoughness']['baseColorTexture'] = {'index': imgidx[d]}
        gmat.append(e); matidx[i] = len(gmat) - 1; return matidx[i]
    for n in meshes:
        if not len(n['idx']): continue
        p = n['pos']; vi = add(p.tobytes(), 34962)
        acc.append({'bufferView': vi, 'componentType': 5126, 'count': len(p), 'type': 'VEC3', 'min': p.min(0).tolist(), 'max': p.max(0).tolist()})
        a = [len(acc) - 1]
        acc.append({'bufferView': add(n['nrm'].tobytes(), 34962), 'componentType': 5126, 'count': len(p), 'type': 'VEC3'}); a.append(len(acc) - 1)
        acc.append({'bufferView': add(n['uv'].tobytes(), 34962), 'componentType': 5126, 'count': len(p), 'type': 'VEC2'}); a.append(len(acc) - 1)
        acc.append({'bufferView': add(n['idx'].tobytes(), 34963), 'componentType': 5125, 'count': len(n['idx']), 'type': 'SCALAR'}); a.append(len(acc) - 1)
        gmesh.append({'name': n['name'], 'primitives': [{'attributes': {'POSITION': a[0], 'NORMAL': a[1], 'TEXCOORD_0': a[2]}, 'indices': a[3], 'material': material(n['mat'])}]})
        gnodes.append({'name': n['name'], 'mesh': len(gmesh) - 1})
    while len(bin_) % 4: bin_.append(0)
    g = {'asset': {'version': '2.0', 'generator': 'tools/kn5.py'}, 'scene': 0, 'scenes': [{'nodes': list(range(len(gnodes)))}],
         'nodes': gnodes, 'meshes': gmesh, 'materials': gmat, 'textures': gtex, 'images': gimg,
         'accessors': acc, 'bufferViews': views, 'buffers': [{'byteLength': len(bin_)}]}
    js = json.dumps(g, separators=(',', ':')).encode(); js += b' ' * (-len(js) % 4)
    out = os.path.join(outdir, base + '.glb')
    with open(out, 'wb') as f:
        f.write(struct.pack('<4sII', b'glTF', 2, 12 + 8 + len(js) + 8 + len(bin_)))
        f.write(struct.pack('<I4s', len(js), b'JSON') + js + struct.pack('<I4s', len(bin_), b'BIN\0') + bytes(bin_))
    return k, pngs, out


def report(k, pngs):
    ms = [n for n in k['nodes'] if 'pos' in n]
    allp = np.concatenate([n['pos'] for n in ms]); lo, hi = allp.min(0), allp.max(0)
    print('version', k['version'], 'nodes', len(k['nodes']), 'meshes', len(ms),
          'tris', sum(len(n['idx']) // 3 for n in ms))
    print('bbox min', lo.round(3).tolist(), 'max', hi.round(3).tolist(), 'size xyz', (hi - lo).round(3).tolist())
    print('-- nodes (indent = depth; meshes: tris verts material shader/diffuse)')
    for i, n in enumerate(k['nodes']):
        d, p = 0, n['parent']
        while p >= 0: d += 1; p = k['nodes'][p]['parent']
        if 'pos' in n:
            m = k['materials'][n['mat']]
            c = (n['pos'].min(0) + n['pos'].max(0)) / 2
            print('%s%s  mesh tris=%d v=%d mat=%s [%s] diff=%s ctr=%s' % ('  ' * d, n['name'], len(n['idx']) // 3, len(n['pos']),
                  m['name'], m['shader'], m['tex'].get('txDiffuse'), c.round(3).tolist()))
        else:
            print('%s%s  dummy pos=%s' % ('  ' * d, n['name'], n['world'][3, :3].round(3).tolist()))
    print('-- textures (kn5 dds bytes -> png size)')
    for name, data in k['textures'].items():
        print(name, len(data), pngs.get(name, ('', None))[1])


if __name__ == '__main__':
    ap = argparse.ArgumentParser(); ap.add_argument('kn5'); ap.add_argument('out')
    ap.add_argument('--max-tex', type=int, default=512); ap.add_argument('--skin', help='skin folder for textures not in the kn5')
    ap.add_argument('--tex-from', help='kn5 holding the textures (only its texture block is read)')
    ap.add_argument('--report', action='store_true')
    a = ap.parse_args()
    k, pngs, out = export(a.kn5, a.out, a.max_tex, a.skin, a.tex_from)
    print('wrote', out, os.path.getsize(out), 'bytes')
    if a.report: report(k, pngs)
