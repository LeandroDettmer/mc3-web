#!/usr/bin/env python3
"""Read a Midnight Club 3 (Remix, Xbox) car model and write it as .glb.

  mc3car.py <car folder> <out.glb> [--file vp_x.xbck] [--lod 0|1|2]
            [--parts stk|all|none] [--tex DIR ...] [--report]

<car folder> is resources/vehicle/<car>/ as tools/dave.py extracts it
(assets_x.dat_2): <car>.xbck, <car>_o.xbck, <car>_g.xbck and one
<mesh name>.xbck per part. Shared textures come from --tex folders
(vehicle/shared_texture/texture_x from assets_x.dat_1, texture_x/ from
assets_x.dat), found by <name>.tex. Needs numpy + Pillow.

.xbck = a memory image of the object graph, saved by the dev build at a
fixed address (Remix loader sub_002105F0, called by the car builder
sub_00105AA0 with "$/resources/vehicle/%s/%s" + "" / "_o" / "_g"):
  0x00 u32 base VA of the image; 0x04 u32 kind (car 0x38D030, the value
  sub_00105AA0 passes; part mesh 0x16); 0x08 u32 1; 0x0C u32 image size
  (= file size - 0x80); 0x10-0x7F zero. Image at 0x80 = VA base.
  Pointers are absolute VAs inside [base, base+size); GPU data (vertex
  buffers, textures) is referenced by physical address = VA - 0x80000000.
  Pointers below base are to things outside the file (global shaders,
  shared textures), resolved by name at load. Word 0 of every object is
  a vtable of the dev build (0x5Dxxxx-0x60xxxx, outside the retail XBE):
  the loader rebuilds each object by type, so a writer must reproduce the
  class layouts, not just the bytes. 0xCD fills padding (debug heap),
  arrays made with new[] carry their count in the word before them.
Objects (offsets from the object; vtables as seen in the files):
  Mesh (vt 0x005DB670), 0x20 bytes then its arrays:
    +0x04 u8 0, u8 0, u8 bone, u8 0x1C   +0x08 u32 n groups
    +0x0C u16 *material index [n]  (into the car's material list)
    +0x10 Group *[n] (0x18 each)    +0x14 0   +0x18 D3DVertexBuffer *
    +0x1C u16 *indices (one buffer, triangle strips; 0xCD-padded)
  Group: +0 Prim *, +4 u32 0x00805EC1, +8 u16 0x59, u16 stride 0x1C,
    +0xC u32 0x00044A02, +0x10 u32 0, +0x14 u16 n prims (1)
  Prim (new[] of 1): u16 first index, u16 index count, u32 0
  D3DVertexBuffer: Common 0x00000001, Data (physical), Lock 0
  Vertex, 28 bytes: f32 x,y,z; u32 normal NORMPACKED3 (11:11:10 signed);
    u32 unknown (4 bytes, ~0x80-centred, last byte 0..3); f32 u, v.
    No vertex count is stored: the buffer runs up to the next allocation.
  Mesh table (vt 0x005DD9F8; three per car = LOD H, M, L):
    +0x04 u16 2, u16 n; +0x08 Mesh *[n] (0 = part in its own file
    "<name>.xbck"); +0x0C char *name[n]; +0x10 u32 bone[n]
  Skeleton header (u16 nbones, u16, u16, u16 1, Bone *bones), bones of
    0x44 bytes: +0x00 vec3 world position (rest pose) +0x0C char *name
    +0x10 u16 flags +0x14 Bone *next sibling +0x18 Bone *first child
    +0x1C Bone *parent +0x20 vec3 offset from parent +0x2C vec3 Euler
    rotation (rad) +0x38 u16 index +0x3C u32 DOF flags (0x303FF)
    +0x40 limits * (wheels). The wheel table (u32 i, axle bone, wheel
    bone, arm, vec3 centre) x4 sits by the bone-name hash map.
  Material list (vt 0x005DCF28): +0x04 Material *[n] +0x08 u16 n.
    Material: vt (shader class), u32, u32 shader id, external shader *,
    then param blocks {u32 4, Param *}. Texture param (vt 0x005DBF6C):
    +0x04 u8 2, +0x06 u16 1, +0x08 char *name, +0x0C D3DTexture * (in
    the file) or external (shared .tex, found by name; __envmap__ etc.
    are the engine's).
  D3DTexture: Common 0x00040001, Data (physical), Lock 0, Format (bits
    4-7 dims, 8-15 colour format 0x0C DXT1 / 0x0E DXT3 / 0x0F DXT5,
    16-19 mip levels, 20-23 log2 width, 24-27 log2 height), Size 0.
.tex (shared textures): u16 width, height, type (0x16 DXT1, 0x18 DXT3,
0x1A DXT5, 1 P8 = 256 BGRA palette + swizzled 8-bit pixels), u16 mips,
u16 1, u32 0, then the levels.
Coordinates: y up, front of the car at -z, left-handed (D3D). The .glb
mirrors z (front at +z, right-handed); strips keep D3D's winding.
"""
import argparse, glob, io, json, os, re, struct, sys
import numpy as np
from PIL import Image

VT_MESH, VT_TABLE, VT_MATLIST, VT_TEXPARAM = 0x005DB670, 0x005DD9F8, 0x005DCF28, 0x005DBF6C


class Chunk:
    def __init__(s, path):
        s.path, s.d = path, open(path, 'rb').read()
        s.base, s.kind, s.one, s.size = struct.unpack_from('<4I', s.d, 0)
        if s.size != len(s.d) - 0x80:
            raise ValueError('%s: not an .xbck (size %d vs %d)' % (path, s.size, len(s.d) - 0x80))

    def inside(s, va): return s.base <= va < s.base + s.size
    def off(s, va):
        if not s.inside(va): raise ValueError('VA %08x outside %s' % (va, s.path))
        return va - s.base + 0x80
    def va(s, off): return off - 0x80 + s.base
    def u(s, fmt, va): return struct.unpack_from('<' + fmt, s.d, s.off(va))
    def u32(s, va): return s.u('I', va)[0]
    def cstr(s, va):
        o = s.off(va); return s.d[o:s.d.index(b'\0', o)].decode('latin-1')
    def find_vt(s, vt):
        b = struct.pack('<I', vt)
        return [s.va(o) for o in range(0x80, len(s.d) - 3, 4) if s.d[o:o + 4] == b]


def strip_tris(s):
    """D3D triangle strip -> triangle list, degenerates dropped."""
    out = []
    for i in range(len(s) - 2):
        t = (s[i], s[i + 1], s[i + 2]) if i % 2 == 0 else (s[i + 1], s[i], s[i + 2])
        if t[0] != t[1] and t[1] != t[2] and t[0] != t[2]: out.append(t)
    return out


def normpacked3(w):
    w = w.astype(np.int64)
    x = ((w & 0x7FF) ^ 0x400) - 0x400
    y = (((w >> 11) & 0x7FF) ^ 0x400) - 0x400
    z = (((w >> 22) & 0x3FF) ^ 0x200) - 0x200
    return np.stack([x / 1023.0, y / 1023.0, z / 511.0], 1)


def read_mesh(c, va):
    if c.u32(va) != VT_MESH: raise ValueError('no mesh at %08x' % va)
    bone, stride_tag = c.d[c.off(va) + 6], c.d[c.off(va) + 7]
    n, pmat, pgrp, _, pvb, pidx = c.u('6I', va + 8)
    mats = c.u('%dH' % n, pmat)
    common, data, lock = c.u('3I', pvb)
    vbva = data | 0x80000000
    groups = []
    for g in range(n):
        pp, fvf, a, stride, b, z, nprim = c.u('IIHHIIH', pgrp + g * 0x18)
        tris = []
        for p in range(nprim):
            first, cnt, _ = c.u('HHI', pp + p * 8)
            tris += strip_tris(c.u('%dH' % cnt, pidx + first * 2))
        groups.append({'mat': mats[g], 'tris': np.array(tris, np.uint32).reshape(-1, 3),
                       'fvf': fvf, 'stride': stride, 'a': a, 'b': b})
    nv = max(int(g['tris'].max()) for g in groups if len(g['tris'])) + 1
    stride = groups[0]['stride']
    raw = np.frombuffer(c.d, np.uint8, nv * stride, c.off(vbva)).reshape(nv, stride)
    f = raw.view('<f4'); w = raw.view('<u4')
    return {'va': va, 'bone': bone, 'tag': stride_tag, 'groups': groups, 'nv': nv,
            'pos': f[:, 0:3].astype(np.float64), 'nrm': normpacked3(w[:, 3]),
            'extra': w[:, 4].copy(), 'uv': f[:, 5:7].astype(np.float64)}


def euler(r, order):
    cx, cy, cz = np.cos(r); sx, sy, sz = np.sin(r)
    X = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Y = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Z = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    m = {'X': X, 'Y': Y, 'Z': Z}
    return m[order[0]] @ m[order[1]] @ m[order[2]]


def read_skeleton(c, order='ZYX'):
    """Bones by index: name, parent, offset, rot, stored world pos, world 4x4."""
    i = c.d.find(b'vroot\0')
    while i >= 0 and i % 4: i = c.d.find(b'vroot\0', i + 1)
    ref = c.d.find(struct.pack('<I', c.va(i)))
    while ref >= 0 and ref % 4: ref = c.d.find(struct.pack('<I', c.va(i)), ref + 1)
    b0 = c.va(ref) - 0xC
    bones, k = [], 0
    while c.inside(b0 + k * 0x44 + 0x44) and c.u('H', b0 + k * 0x44 + 0x38)[0] == k:
        r = b0 + k * 0x44
        par = c.u32(r + 0x1C)
        bones.append({'name': c.cstr(c.u32(r + 0xC)), 'world0': np.array(c.u('3f', r)),
                      'parent': (par - b0) // 0x44 if par else -1,
                      'off': np.array(c.u('3f', r + 0x20)), 'rot': np.array(c.u('3f', r + 0x2C))})
        k += 1
    for b in bones:
        m = np.eye(4); m[:3, :3] = euler(b['rot'], order); m[:3, 3] = b['off']
        b['world'] = (bones[b['parent']]['world'] @ m) if b['parent'] >= 0 else m
    return bones


def mesh_tables(c):
    out = []
    for t in sorted(c.find_vt(VT_TABLE)):
        _, n = c.u('2H', t + 4); pm, pn, pb = c.u('3I', t + 8)
        out.append([(c.u32(pm + 4 * i), c.cstr(c.u32(pn + 4 * i)), c.u32(pb + 4 * i)) for i in range(n)])
    return out


def materials(c):
    """[{'texs': [(param name, D3DTexture VA or None)]}] in list order."""
    lists = sorted(c.find_vt(VT_MATLIST))
    pl, n = c.u32(lists[0] + 4), c.u('H', lists[0] + 8)[0]
    mats = [c.u32(pl + 4 * i) for i in range(n)]
    params = set(p for p in c.find_vt(VT_TEXPARAM) if c.d[c.off(p) + 4] == 2)
    bounds = sorted(set(mats) | {c.base + c.size})
    out = []
    for m in mats:
        end = bounds[bounds.index(m) + 1]
        texs = [(c.cstr(c.u32(p + 8)), c.u32(p + 12) if c.inside(c.u32(p + 12)) else None)
                for p in sorted(params) if m <= p < end]
        out.append({'va': m, 'texs': texs})
    return out


def unswizzle(a, w, h):
    """Xbox Morton (swizzled) layout -> linear, for a 2D array of texels."""
    xs, ys = np.zeros(w, np.int64), np.zeros(h, np.int64)
    bit, mx, my = 1, 1, 1
    while mx < w or my < h:
        if mx < w:
            xs += np.where(np.arange(w) & mx, bit, 0); mx <<= 1; bit <<= 1
        if my < h:
            ys += np.where(np.arange(h) & my, bit, 0); my <<= 1; bit <<= 1
    return a[(ys[:, None] + xs[None, :]).ravel()].reshape(h, w, *a.shape[1:])


def decode(kind, w, h, data):
    """kind: 'dxt1' 'dxt3' 'dxt5' or ('p8', palette)."""
    if isinstance(kind, tuple):
        pal = np.frombuffer(kind[1], np.uint8).reshape(256, 4)[:, [2, 1, 0, 3]]
        px = unswizzle(np.frombuffer(data, np.uint8, w * h), w, h)
        return Image.fromarray(pal[px], 'RGBA')
    n = {'dxt1': 1, 'dxt3': 2, 'dxt5': 3}[kind]
    size = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * (8 if n == 1 else 16)
    return Image.frombytes('RGBA', (w, h), bytes(data[:size]), 'bcn', n)


def d3d_texture(c, va):
    # the param points at the engine's texture object; its D3DTexture
    # (Common 0x00040001) follows within 0x100 bytes (+0x80 for the 350Z)
    va = next(v for v in range(va, va + 0x100, 4) if c.u32(v) == 0x00040001 and c.inside(c.u32(v + 4) | 0x80000000))
    common, data, lock, fmt, size = c.u('5I', va)
    cf, w, h = (fmt >> 8) & 0xFF, 1 << ((fmt >> 20) & 0xF), 1 << ((fmt >> 24) & 0xF)
    kind = {0x0C: 'dxt1', 0x0E: 'dxt3', 0x0F: 'dxt5'}.get(cf)
    info = {'w': w, 'h': h, 'fmt': '%02X' % cf, 'mips': (fmt >> 16) & 0xF}
    if not kind: return None, info
    o = c.off(data | 0x80000000)
    return decode(kind, w, h, c.d[o:o + w * h * 2]), info


def tex_file(path):
    d = open(path, 'rb').read()
    w, h, t, mips = struct.unpack_from('<4H', d, 0)
    info = {'w': w, 'h': h, 'fmt': 'tex%02X' % t, 'mips': mips}
    if t == 1: return decode(('p8', d[14:14 + 1024]), w, h, d[14 + 1024:]), info
    kind = {0x16: 'dxt1', 0x18: 'dxt3', 0x1A: 'dxt5'}.get(t)
    return (decode(kind, w, h, d[14:]) if kind else None), info


def load_car(folder, fname=None, lod=0, parts='stk', order='ZYX'):
    car = os.path.basename(os.path.normpath(folder))
    c = Chunk(os.path.join(folder, fname or car + '.xbck'))
    bones = read_skeleton(c, order)
    mats = materials(c)
    items = []   # (mesh dict, chunk, name, bone, is_part)
    for mp, name, bone in mesh_tables(c)[lod]:
        if name.startswith('neonglow'): continue   # the underglow quad, lit only with neon fitted
        part = '_bone_' in name or name.startswith(('vroot_hood_', 'vroot_tail_lights_'))
        if part and parts == 'none': continue
        if part and parts == 'stk' and '_stk_' not in name: continue
        if mp:
            items.append((read_mesh(c, mp), c, name, bone, part))
        else:
            pc = Chunk(os.path.join(folder, name + '.xbck'))
            items.append((read_mesh(pc, pc.base + 0), pc, name, bone, part))
    return c, bones, mats, items


def find_tex(name, dirs):
    for d in dirs:
        hit = glob.glob(os.path.join(d, '**', name + '.tex'), recursive=True)
        if hit: return hit[0]


def export(folder, out, fname=None, lod=0, parts='stk', texdirs=(), report=False):
    c, bones, mats, items = load_car(folder, fname, lod, parts)
    # self-checks: bone chain reproduces the stored rest positions; mesh bone byte = table bone
    err = max(np.abs(b['world'][:3, 3] - b['world0']).max() for b in bones)
    assert err < 1e-3, 'bone world positions off by %g' % err
    for m, _, name, bone, _ in items:
        assert m['bone'] == bone, '%s: bone %d vs table %d' % (name, m['bone'], bone)
    # textures used
    imgs, texinfo = {}, {}
    def texture(mi):
        for pname, tva in mats[mi]['texs'] if mi < len(mats) else []:
            if pname.startswith('__') or pname == 'none': continue
            if pname in imgs: return pname
            im, info = (d3d_texture(c, tva) if tva else
                        tex_file(find_tex(pname, texdirs)) if find_tex(pname, texdirs) else (None, None))
            texinfo[pname] = (info, 'embedded' if tva else 'shared .tex')
            if im is not None:
                imgs[pname] = im; return pname
        return None
    # glb
    bin_, views, acc, gnodes, gmesh, gmat, gtex, gimg = bytearray(), [], [], [], [], [], [], []
    def add(data, target=None):
        while len(bin_) % 4: bin_.append(0)
        views.append({'buffer': 0, 'byteOffset': len(bin_), 'byteLength': len(data), **({'target': target} if target else {})})
        bin_.extend(data); return len(views) - 1
    def accessor(a, typ, ct=5126, target=34962, mm=False):
        e = {'bufferView': add(a.tobytes(), target), 'componentType': ct, 'count': len(a), 'type': typ}
        if mm: e.update(min=a.min(0).tolist(), max=a.max(0).tolist())
        acc.append(e); return len(acc) - 1
    matidx, imgidx = {}, {}
    def material(mi):
        if mi in matidx: return matidx[mi]
        names = [p for p, _ in (mats[mi]['texs'] if mi < len(mats) else [])]
        paint = '__metalflake__' in names or '__decal__' in names   # car paint: colour comes from the profile
        t = None if paint else texture(mi)   # (a paint's own textures are options: carbon hood, vinyls)
        e = {'name': 'mat%d %s' % (mi, ' '.join(names)), 'doubleSided': True,
             'pbrMetallicRoughness': {'metallicFactor': 0, 'roughnessFactor': 0.6}}
        if t:
            if t not in imgidx:
                b = io.BytesIO(); imgs[t].save(b, 'PNG')
                gimg.append({'bufferView': add(b.getvalue()), 'mimeType': 'image/png', 'name': t})
                gtex.append({'source': len(gimg) - 1}); imgidx[t] = len(gtex) - 1
            e['pbrMetallicRoughness']['baseColorTexture'] = {'index': imgidx[t]}
            if 'window' in t or 'glass' in ' '.join(names): e['alphaMode'] = 'BLEND'
        elif paint:
            e['pbrMetallicRoughness'].update(baseColorFactor=[0.55, 0.05, 0.05, 1], metallicFactor=0.3)
        gmat.append(e); matidx[mi] = len(gmat) - 1; return matidx[mi]
    flip = np.diag([1.0, 1.0, -1.0])   # LH, front -z  ->  RH, front +z
    stats = []
    for m, ch, name, bone, part in items:
        w = bones[bone]['world']
        pos = (m['pos'] @ w[:3, :3].T + w[:3, 3]) @ flip
        nrm = m['nrm'] @ w[:3, :3].T @ flip
        nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
        ia = [accessor(pos.astype(np.float32), 'VEC3', mm=True), accessor(nrm.astype(np.float32), 'VEC3'),
              accessor(m['uv'].astype(np.float32), 'VEC2')]
        prims, ntri = [], 0
        for g in m['groups']:
            if not len(g['tris']): continue
            ntri += len(g['tris'])
            # the mirror turns D3D's clockwise front faces counter-clockwise: reverse to keep them front
            prims.append({'attributes': {'POSITION': ia[0], 'NORMAL': ia[1], 'TEXCOORD_0': ia[2]},
                          'indices': accessor(g['tris'][:, ::-1].ravel(), 'SCALAR', 5125, 34963), 'material': material(g['mat'])})
        gmesh.append({'name': name, 'primitives': prims}); gnodes.append({'name': name, 'mesh': len(gmesh) - 1})
        # winding check: face normal vs the vertices' normals (after the mirror both should agree)
        t = np.concatenate([g['tris'] for g in m['groups']]).astype(np.int64)[:, ::-1]
        fn = np.cross(pos[t[:, 1]] - pos[t[:, 0]], pos[t[:, 2]] - pos[t[:, 0]])
        agree = float(np.mean(np.sum(fn * nrm[t].sum(1), 1) > 0))
        stats.append((name, bones[bone]['name'], m['nv'], ntri, len(m['groups']), part, agree,
                      float(np.abs(np.linalg.norm(m['nrm'], axis=1) - 1).max())))
    while len(bin_) % 4: bin_.append(0)
    g = {'asset': {'version': '2.0', 'generator': 'tools/mc3car.py'}, 'scene': 0,
         'scenes': [{'nodes': list(range(len(gnodes)))}], 'nodes': gnodes, 'meshes': gmesh,
         'materials': gmat, 'textures': gtex, 'images': gimg, 'samplers': [],
         'accessors': acc, 'bufferViews': views, 'buffers': [{'byteLength': len(bin_)}]}
    for t in gtex: t['sampler'] = 0
    g['samplers'] = [{'wrapS': 10497, 'wrapT': 10497}]
    js = json.dumps(g, separators=(',', ':')).encode(); js += b' ' * (-len(js) % 4)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'wb') as f:
        f.write(struct.pack('<4sII', b'glTF', 2, 12 + 8 + len(js) + 8 + len(bin_)))
        f.write(struct.pack('<I4s', len(js), b'JSON') + js + struct.pack('<I4s', len(bin_), b'BIN\0') + bytes(bin_))
    if report: print_report(c, bones, mats, items, stats, texinfo)
    return stats


def print_report(c, bones, mats, items, stats, texinfo):
    print('%s: base %08x kind %X image %d B; %d bones, %d materials, %d mesh tables'
          % (os.path.basename(c.path), c.base, c.kind, c.size, len(bones), len(mats), len(mesh_tables(c))))
    for i, t in enumerate(mesh_tables(c)):
        print('  table %d: %d entries, %d in this file' % (i, len(t), sum(1 for e in t if e[0])))
    tot = {False: 0, True: 0}
    for name, bone, nv, nt, ng, part, agree, nerr in stats:
        tot[part] += nt
        print('  %-70s bone %-14s v %5d tris %5d groups %2d wind %.2f |n|-1 %.3f' % (name[:70], bone, nv, nt, ng, agree, nerr))
    print('tris: body %d + parts %d = %d' % (tot[False], tot[True], tot[False] + tot[True]))
    for n, (info, src) in sorted(texinfo.items()):
        print('  texture %-22s %s %s' % (n, src, info))
    allp = np.concatenate([m['pos'] @ bones[b]['world'][:3, :3].T + bones[b]['world'][:3, 3] for m, _, _, b, _ in items])
    print('bbox (game axes) min', allp.min(0).round(3).tolist(), 'max', allp.max(0).round(3).tolist())


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('folder'); ap.add_argument('out')
    ap.add_argument('--file', help='which .xbck of the folder (default <car>.xbck; _o, _g)')
    ap.add_argument('--lod', type=int, default=0, help='mesh table: 0 high, 1 medium, 2 low')
    ap.add_argument('--parts', default='stk', choices=['stk', 'all', 'none'])
    ap.add_argument('--tex', action='append', default=[], help='folder searched for <name>.tex')
    ap.add_argument('--report', action='store_true')
    a = ap.parse_args()
    export(a.folder, a.out, a.file, a.lod, a.parts, a.tex, a.report)
    print('wrote', a.out, os.path.getsize(a.out), 'bytes')
