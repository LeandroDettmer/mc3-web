#!/usr/bin/env python3
"""Read and write Midnight Club 3 (Remix, Xbox) car models.

  mc3car.py <car folder> <out.glb> [--file vp_x.xbck] [--lod 0|1|2]
            [--parts stk|all|none] [--tex DIR ...] [--report]
  mc3car.py roundtrip <car folder>     decode + re-encode every mesh, compare
  mc3car.py build <spec.json>          new car from a .glb on a template car
  mc3car.py check <folder|file.xbck>   Mesh / Group array / VB pointers shared by 2+ table slots

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
Coordinates: y up, front of the car at -z, left at -x (Arm_Front_L,
axl_0; the 350Z's steering wheel side): right-handed. Triangles face out
counter-clockwise (cross(b-a, c-a) . normal > 0). The .glb turns the car
180 degrees about y (front at +z), no mirror.

Loader (traced in the Remix XBE): sub_002105F0 -> sub_002126C0 reads the
0x80 header, refuses kind != the caller's or word 2 != 1, allocates the
image (sub_0020DB20, 0x80-aligned) and reads it whole. sub_00105A40 then
fixes it up in place with delta = new address - stored base: the root at
image offset 0 (sub_00105830) and every type's resource constructor add
delta to each non-null pointer field (GPU Data words too) and overwrite
word 0 with the retail vtable, so the dev vtables in the file are never
used. Meshes: LOD group sub_000E9D80 (4 slots) -> table sub_000E9620
(+4 byte = 2) -> mesh factory [0x507668] (vt 0x4536E4) +8 sub_000DE040
-> mesh sub_000DDF50: +5 |= 2; fixes +0xC, +0x10 (u16 groups at +8),
+0x14 (u16 count at +0xA, 0 here), +0x18 VB (and its Data), +0x1C. Group
sub_000E7790: fixes the prim array, then sets +4 = the vertex declaration
for the 8-byte format at +8 (sub_000DDC50), so 0x00805EC1 is a stale
handle. Format 0x59 = RAGE grcFvf position | normal | diffuse | texture0,
stride 0x1C, 0x00044A02 the channel formats: the vertex's 4th dword is
the diffuse colour: alpha 0..14 = a damage zone by position, B G R - 0x80 =
the vertex's dent vector z y x (mostly inward: 350Z median cos with -normal
0.5; one value per position, else a hit tears the mesh apart).
__licenseplate__ (the game's plate texture) is bottom-up: v grows upward.
The fixup runs once PER TABLE SLOT with no done-guard, so a Mesh (or its Group array or VB,
which it rebases too) reached from two slots is rebased twice: garbage prim counts, a huge draw
(the garage froze). Stock cars share none (check); every slot gets its own Mesh header, Group
array and VB; vertices, indices, material array and prims may be shared.
Nothing checks sizes or vertex counts: any layout with valid pointers
loads. Mesh layout as the dev build wrote it (the writer copies it):
header 0x20, u16 materials, [count] groups at align16(0x20 + 2n) + 4, VB
right after the groups, vertices at align16 (exactly max index + 1),
[count][prims] per group, u16 strips at align16, 0xCD padding.
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


VTX = np.dtype([('pos', '<f4', 3), ('nrm', '<u4'), ('col', '<u4'), ('uv', '<f4', 2)])
DESC = (0x00805EC1, 0x59, 0x1C, 0x00044A02, 0)   # group +4..+0x13 as every MC3 car has it
def _al(x, a=16): return (x + a - 1) // a * a


def decode_mesh(c, va):
    """Mesh at va, losslessly: encode_mesh(decode_mesh(c, va), va) is its bytes."""
    if c.u32(va) != VT_MESH: raise ValueError('no mesh at %08x' % va)
    hdr = bytes(c.d[c.off(va) + 4:c.off(va) + 8])   # 0, 0, bone, 0x1C
    n, pmat, pgrp, p14, pvb, pidx = c.u('6I', va + 8)
    if p14 or n >> 16: raise ValueError('mesh %08x: +0x14 array in use' % va)
    mats = c.u('%dH' % n, pmat)
    groups, nidx = [], 0
    for g in range(n):
        pp, h, fvf, stride, chan, z, nprim, _ = c.u('IIHHIIHH', pgrp + g * 0x18)
        if stride != VTX.itemsize: raise ValueError('mesh %08x: stride %d' % (va, stride))
        strips = []
        for p in range(nprim):
            first, cnt, z2 = c.u('HHI', pp + p * 8)
            if first != nidx or z2: raise ValueError('mesh %08x: strips out of order' % va)
            strips.append(np.array(c.u('%dH' % cnt, pidx + first * 2), np.uint16)); nidx += cnt
        groups.append({'mat': mats[g], 'desc': (h, fvf, stride, chan, z), 'strips': strips})
    nv = max(int(s.max()) for g in groups for s in g['strips'] if len(s)) + 1
    vtx = np.frombuffer(c.d, VTX, nv, c.off(c.u32(pvb + 4) | 0x80000000)).copy()
    return {'hdr': hdr, 'groups': groups, 'vtx': vtx, 'idx_off': pidx - va}


def encode_mesh(m, va, vva=None, iva=None):
    """Bytes of mesh m placed at va (layout as the dev build's; m['idx_off'] if given). With vva / iva
    the vertices / indices are not in the bytes but at those VAs (place_mesh())."""
    gs = m['groups']; n = len(gs)
    o_cnt = _al(0x20 + 2 * n); o_grp = o_cnt + 4; o_vb = o_grp + 0x18 * n; o_vd = _al(o_vb + 12)
    o_prim = o_vd + (0 if vva else VTX.itemsize * len(m['vtx']))
    o_end = o_prim + 4 * n + 8 * sum(len(g['strips']) for g in gs)
    o_idx = m.get('idx_off') or _al(o_end)
    idx = np.concatenate([s for g in gs for s in g['strips']]).astype('<u2')
    if len(idx) > 0xFFFF or len(m['vtx']) > 0x10000: raise ValueError('mesh too big for u16 indices')
    out = bytearray(b'\xcd' * (o_end if iva else o_idx + 2 * len(idx)))   # ends with the indices: the next object may be 4-aligned
    struct.pack_into('<I4s6I', out, 0, VT_MESH, m['hdr'], n, va + 0x20, va + o_grp, 0, va + o_vb, iva or va + o_idx)
    struct.pack_into('<%dH' % n, out, 0x20, *[g['mat'] for g in gs])
    struct.pack_into('<I', out, o_cnt, n)
    p, first = o_prim, 0
    for k, g in enumerate(gs):
        struct.pack_into('<IIHHIIHH', out, o_grp + 0x18 * k, va + p + 4, *g['desc'], len(g['strips']), 0xCDCD)
        struct.pack_into('<I', out, p, len(g['strips'])); p += 4   # new[] count
        for s in g['strips']:
            struct.pack_into('<HHI', out, p, first, len(s), 0); p += 8; first += len(s)
    struct.pack_into('<3I', out, o_vb, 1, (vva or va + o_vd) & 0x7FFFFFFF, 0)   # D3DVertexBuffer, Data physical
    if not vva: out[o_vd:o_prim] = np.asarray(m['vtx'], VTX).tobytes()
    if not iva: out[o_idx:o_idx + 2 * len(idx)] = idx.tobytes()
    return bytes(out)


def place_mesh(img, m):
    """Mesh m into img's freed holes (Img.place), vertices, indices and the rest each where it fits:
    the loader fixes every pointer field on its own, so the arrays need not follow the header."""
    vb = np.asarray(m['vtx'], VTX).tobytes()
    ib = np.concatenate([s for g in m['groups'] for s in g['strips']]).astype('<u2').tobytes()
    vva = img.place(len(vb), lambda va: vb); iva = img.place(len(ib), lambda va: ib)
    return img.place(len(encode_mesh(m, 0, vva, iva)), lambda va: encode_mesh(m, va, vva, iva))


def clone_mesh(img, va):
    """A copy of the Mesh at va with its own header, Group array (+ count word) and VB header;
    the vertices, indices, material array and prims stay shared (they are never rebased twice)."""
    o = va - img.base; n = struct.unpack_from('<I', img.d, o + 8)[0]
    h = list(struct.unpack_from('<I4s6I', img.d, o)); pg, pvb = h[4], h[6]
    g = bytes(img.d[pg - img.base:pg - img.base + 0x18 * n]); vb = bytes(img.d[pvb - img.base:pvb - img.base + 12])
    o_vb = 0x24 + 0x18 * n
    def make(v):
        h[4], h[6] = v + 0x24, v + o_vb
        b = bytearray(b'\xcd' * (o_vb + 12))
        struct.pack_into('<I4s6I', b, 0, *h); struct.pack_into('<I', b, 0x20, n)
        b[0x24:o_vb] = g; b[o_vb:] = vb
        return bytes(b)
    return img.place(o_vb + 12, make)


def unshare(img, slots):
    """Every slot VA in slots gets its own Mesh: later slots pointing at a Mesh already seen get clone_mesh()."""
    seen = set()
    for sv in slots:
        mp = struct.unpack_from('<I', img.d, sv - img.base)[0]
        if not mp: continue
        if mp in seen: img.put(sv, struct.pack('<I', clone_mesh(img, mp)))
        else: seen.add(mp)


def check(paths):
    """Per car file: Mesh, Group array and VB pointers reached from 2+ slots (or meshes). Returns the count."""
    files = [f for p in paths for f in (sorted(glob.glob(os.path.join(p, '*.xbck'))) if os.path.isdir(p) else [p])]
    tot = 0
    for f in files:
        c = Chunk(f)
        if c.kind == 0x16: continue
        ts = mesh_tables(c)
        if not ts: continue
        ms = [e[0] for t in ts for e in t if e[0]]
        dup = {'mesh': len(ms) - len(set(ms))}
        for k, off in (('groups', 0x10), ('vb', 0x18)):
            ps = [c.u32(m + off) for m in set(ms)]; dup[k] = len(ps) - len(set(ps))
        n = sum(dup.values()); tot += n
        print('%-40s %d slots: shared %s' % (os.path.basename(f), len(ms), ', '.join('%s %d' % kv for kv in dup.items()) if n else '0'))
    print('total shared:', tot)
    return tot


def read_mesh(c, va):
    m = decode_mesh(c, va); v = m['vtx']
    groups = [{'mat': g['mat'], 'tris': np.array([t for s in g['strips'] for t in strip_tris(s.tolist())],
                                                 np.uint32).reshape(-1, 3)} for g in m['groups']]
    return {'va': va, 'bone': m['hdr'][2], 'tag': m['hdr'][3], 'groups': groups, 'nv': len(v),
            'pos': v['pos'].astype(np.float64), 'nrm': normpacked3(v['nrm']),
            'extra': v['col'].copy(), 'uv': v['uv'].astype(np.float64)}


def pack_normal(n):
    n = n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
    q = lambda a, s, b: np.clip(np.round(a * s), -s, s).astype(np.int64) & b
    return (q(n[:, 0], 1023, 0x7FF) | q(n[:, 1], 1023, 0x7FF) << 11 | q(n[:, 2], 511, 0x3FF) << 22).astype(np.uint32)


def stripify(tris):
    """Triangles (CCW out, as strip_tris() gives them) -> one strip, pieces joined by degenerates."""
    tris = [tuple(int(v) for v in t) for t in tris if len(set(t)) == 3]
    edges = {}
    for i, (a, b, c) in enumerate(tris):
        for e, x in (((a, b), c), ((b, c), a), ((c, a), b)): edges.setdefault(e, []).append((i, x))
    used, out = [False] * len(tris), []
    for i0, t in enumerate(tris):
        if used[i0]: continue
        used[i0] = True; s = list(t)
        while True:   # strip_tris(): triangle k is (s[k], s[k+1], s[k+2]) if k even, else (s[k+1], s[k], s[k+2])
            e = (s[-2], s[-1]) if len(s) % 2 == 0 else (s[-1], s[-2])
            nxt = next(((i, x) for i, x in edges.get(e, ()) if not used[i]), None)
            if not nxt: break
            used[nxt[0]] = True; s.append(nxt[1])
        if out: out += [out[-1], s[0]] + [s[0]] * (len(out) % 2)   # next piece starts on an even triangle
        out += s
    return np.array(out, np.uint16)


class Img:
    """An .xbck being edited; pointers are VAs at the stored base, new data goes at the end."""
    def __init__(s, path=None, base=None, kind=None):
        b = open(path, 'rb').read() if path else struct.pack('<4I', base, kind, 1, 0) + bytes(0x70)
        s.h, s.d = bytearray(b[:0x80]), bytearray(b[0x80:])
        s.base = struct.unpack_from('<I', b)[0]
    def put(s, va, b):
        o = va - s.base
        if not (0 <= o and o + len(b) <= len(s.d)): raise ValueError('put %08x outside the image' % va)
        s.d[o:o + len(b)] = b
    def add(s, make):
        """make(va) -> bytes; placed 16-aligned at the end (0x80 for textures: pass a lambda that pads)."""
        s.d += b'\xcd' * (-len(s.d) % 16); va = s.base + len(s.d); s.d += make(va); return va
    holes = ()
    def free(s, va, n):
        """[va, va+n) holds nothing referenced any more: 0xCD it and let place() reuse it."""
        s.put(va, b'\xcd' * n); s.holes = sorted(list(s.holes) + [[va, va + n]])
    def place(s, n, make):
        """make(va) -> n bytes, in the smallest freed hole that holds it 16-aligned, else at the end."""
        fit = [h for h in s.holes if _al(h[0]) + n <= h[1]]
        if not fit: return s.add(make)
        h = min(fit, key=lambda h: h[1] - h[0]); va = _al(h[0])
        s.put(va, make(va)); s.holes.remove(h)
        s.holes = sorted(s.holes + [x for x in ([h[0], va], [va + n, h[1]]) if x[1] - x[0] >= 16])
        return va
    def blob(s):
        s.d += b'\xcd' * (-len(s.d) % 16); struct.pack_into('<I', s.h, 0xC, len(s.d)); return bytes(s.h + s.d)
    def save(s, path):
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        open(path, "wb").write(s.blob())


def move_bones(img, bones, targets, keep=()):
    """targets {bone index: new world position}; children follow. Writes the bone records and every
    other copy of a moved bone's old position (wheel table, exhaust and light lists) outside the
    skeleton and the VA ranges in keep. Returns {index: new position}."""
    R = [b['world'][:3, :3] for b in bones]
    t, moved = {}, {}
    for k, b in enumerate(bones):
        p = b['parent']; assert p < k, 'bones not parent-first'
        if k in targets: new = np.asarray(targets[k], np.float64)
        elif p in moved: new = R[p] @ b['off'] + t[p]
        else: t[k] = b['world0']; continue
        t[k] = moved[k] = new
        off = R[p].T @ (new - t[p]) if p >= 0 else new
        img.put(b['va'], struct.pack('<3f', *new)); img.put(b['va'] + 0x20, struct.pack('<3f', *off))
    olds = {}
    for k, new in moved.items(): olds.setdefault(tuple(np.round(bones[k]['world0'], 4)), set()).add(tuple(np.round(new, 6)))
    f = np.frombuffer(bytes(img.d[:len(img.d) // 4 * 4]), '<f4')
    trip = np.lib.stride_tricks.sliding_window_view(f, 3)
    skip = [(bones[0]['va'], bones[-1]['va'] + 0x44)] + list(keep)
    for old, news in olds.items():
        if len(news) > 1 or not any(old): continue   # bones that shared a place but moved apart: copies stay
        with np.errstate(invalid='ignore'): hit = np.nonzero(np.all(np.abs(trip - old) < 6e-5, axis=1))[0]
        for i in hit:
            va = img.base + 4 * int(i)
            if not any(a <= va < b for a, b in skip): img.put(va, struct.pack('<3f', *next(iter(news))))
    return moved


def put_texture(img, c, va, im):
    """Overwrite the texels of the embedded texture at va (a texture param's +0xC) with im,
    scaled to its size and compressed to its DXT format. An opaque im in a DXT5 texture turns it
    into DXT1 (the D3D format word; the texture object's +6 byte is not the format: the 350Z has
    3 on a DXT1, 9 and 1 on DXT5s, so it stays) and frees the second half of its texels."""
    tv = next(v for v in range(va, va + 0x100, 4) if c.u32(v) == 0x00040001 and c.inside(c.u32(v + 4) | 0x80000000))
    data, fmt = c.u32(tv + 4), c.u32(tv + 12)
    cf, w, h, mips = (fmt >> 8) & 0xFF, 1 << ((fmt >> 20) & 0xF), 1 << ((fmt >> 24) & 0xF), (fmt >> 16) & 0xF
    if mips != 1: raise NotImplementedError('shortcut: one mip level only (the 350Z trim has one)')
    im = im.convert('RGBA').resize((w, h), Image.LANCZOS)
    if cf == 0x0F and np.asarray(im)[..., 3].min() == 255:
        cf = 0x0C; img.put(tv + 12, struct.pack('<I', fmt & ~0xFF00 | 0x0C00))
        img.free((data | 0x80000000) + w * h // 2, w * h // 2)
    b = io.BytesIO(); im.save(b, 'DDS', pixel_format={0x0C: 'DXT1', 0x0F: 'DXT5'}[cf])
    img.put(data | 0x80000000, b.getvalue()[128:128 + w * h // (2 if cf == 0x0C else 1)])
    return w, h, cf


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
        bones.append({'va': r, 'name': c.cstr(c.u32(r + 0xC)), 'world0': np.array(c.u('3f', r)),
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
    turn = np.diag([-1.0, 1.0, -1.0])   # front -z -> front +z (glTF), a rotation: winding kept
    stats = []
    for m, ch, name, bone, part in items:
        w = bones[bone]['world']
        pos = (m['pos'] @ w[:3, :3].T + w[:3, 3]) @ turn
        nrm = m['nrm'] @ w[:3, :3].T @ turn
        nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
        ia = [accessor(pos.astype(np.float32), 'VEC3', mm=True), accessor(nrm.astype(np.float32), 'VEC3'),
              accessor(m['uv'].astype(np.float32), 'VEC2')]
        prims, ntri = [], 0
        for g in m['groups']:
            if not len(g['tris']): continue
            ntri += len(g['tris'])
            prims.append({'attributes': {'POSITION': ia[0], 'NORMAL': ia[1], 'TEXCOORD_0': ia[2]},
                          'indices': accessor(g['tris'].ravel(), 'SCALAR', 5125, 34963), 'material': material(g['mat'])})
        gmesh.append({'name': name, 'primitives': prims}); gnodes.append({'name': name, 'mesh': len(gmesh) - 1})
        # winding check: face normal vs the vertices' normals (counter-clockwise = 1.0)
        t = np.concatenate([g['tris'] for g in m['groups']]).astype(np.int64)
        fn = np.cross(pos[t[:, 1]] - pos[t[:, 0]], pos[t[:, 2]] - pos[t[:, 0]])
        agree = float(np.mean(np.sum(fn * nrm[t].sum(1), 1) > 0)) if len(t) else 1.0   # empty mesh
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


def roundtrip(folder):
    """Decode and re-encode every mesh of a car folder: part files are rebuilt whole, car files
    get their meshes rewritten in place. Returns the number of files that differ."""
    bad, own = 0, []
    def enc(m, va):   # also notes meshes whose index buffer is not where the default layout puts it
        b = encode_mesh(m, va)
        if b != encode_mesh(dict(m, idx_off=None), va): own.append('%s %08x' % (os.path.basename(f), va))
        return b
    for f in sorted(glob.glob(os.path.join(folder, '*.xbck'))):
        c = Chunk(f)
        if c.kind == 0x16:
            img = Img(base=c.base, kind=c.kind); m = decode_mesh(c, c.base)
            img.add(lambda va: enc(m, va)); what = 'rebuilt from the decoded mesh'
        else:
            img, n = Img(f), 0
            for mp in sorted({e[0] for t in mesh_tables(c) for e in t if e[0]}):
                img.put(mp, enc(decode_mesh(c, mp), mp)); n += 1
            what = '%d meshes re-encoded in place' % n
        a, b = np.frombuffer(c.d, np.uint8), np.frombuffer(img.blob(), np.uint8)
        nd = int(np.count_nonzero(a[:min(len(a), len(b))] != b[:min(len(a), len(b))])) + abs(len(a) - len(b))
        bad += nd > 0
        print('%-90s %s: %s' % (os.path.basename(f)[:90], what, 'identical' if not nd else '%d bytes differ' % nd))
    print('index buffer off the default layout (kept as found):', ', '.join(own) or 'none')
    return bad


def read_glb(path):
    """Primitives of a .glb: [{'name', 'mat', 'tex', 'pos', 'nrm', 'uv', 'tris'}] (vertices as stored)."""
    b = open(path, 'rb').read(); jl = struct.unpack_from('<I', b, 12)[0]
    g = json.loads(b[20:20 + jl]); bin_ = b[28 + jl:]
    def acc(i):
        a = g['accessors'][i]; v = g['bufferViews'][a['bufferView']]
        k = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3}[a['type']]
        return np.frombuffer(bin_, {5126: '<f4', 5125: '<u4', 5123: '<u2'}[a['componentType']], a['count'] * k,
                             v.get('byteOffset', 0) + a.get('byteOffset', 0)).reshape(a['count'], k)
    out = []
    for m in g['meshes']:
        for p in m['primitives']:
            mat = g['materials'][p['material']] if 'material' in p else {}
            t = mat.get('pbrMetallicRoughness', {}).get('baseColorTexture')
            at = p['attributes']
            out.append({'name': m.get('name', ''), 'mat': mat.get('name', ''),
                        'tex': g['images'][g['textures'][t['index']]['source']].get('name', '') if t else '',
                        'pos': acc(at['POSITION']).astype(np.float64), 'nrm': acc(at['NORMAL']).astype(np.float64),
                        'uv': acc(at['TEXCOORD_0']).astype(np.float64), 'tris': acc(p['indices']).reshape(-1, 3).astype(np.int64)})
    return out


def _canon(t):
    i = t.index(min(t)); return t[i:] + t[:i]


def _nearest(a, b):
    out = np.empty(len(a), np.int64)
    for i in range(0, len(a), 512): out[i:i + 512] = ((a[i:i + 512, None] - b[None]) ** 2).sum(2).argmin(1)
    return out


# Longest strip a group may have. Stock cars (94, every .xbck) top out at 2,096 indices a group;
# D3D (DrawIndexedVertices sub_00417820) copies a group's indices into the push buffer inline,
# 0x1FF dwords per reservation, with no kick-off in between, and a 14,409-index group froze
# the garage for good: MakeSpace sub_00416710 -> fence wait sub_004165A0 waited (0x004166F4,
# event device+0x1DCC) on a fence the GPU could never reach. Stay inside the stock range.
MAX_STRIP = 2048


def split_strips(T):
    """Triangles -> strips of at most MAX_STRIP indices (consecutive triangles batched)."""
    out, i, B = [], 0, MAX_STRIP // 2
    while i < len(T):
        b = B
        while True:
            s = stripify(T[i:i + b])
            if len(s) <= MAX_STRIP: break
            b = b * 3 // 4
        out.append(s); i += b
    return out


def empty_mesh(bone, mat=1):
    """One vertex, one degenerate strip: draws nothing (for slots the new car has no part for)."""
    return {'hdr': bytes([0, 0, bone, 0x1C]), 'vtx': np.zeros(1, VTX),
            'groups': [{'mat': mat, 'desc': DESC, 'strips': [np.zeros(3, np.uint16)]}]}


def decimate(P, TW, wpos, targets, seam=100.0, unmapped=1e-4, weight=None):
    """Quadric edge collapse (Garland-Heckbert, half-edge: a vertex moves onto a neighbour, so
    every position stays on the source surface). P welded positions; TW triangles as wedge ids
    (a wedge = one position + one set of attributes: uv, normal, material); wpos[w] its position.
    Open edges, material edges and uv/normal seams carry penalty planes (seam x |edge|^2), so
    silhouettes, outlines and seams go last; a collapse that would leave a wedge of the moving
    vertex without a counterpart on the target costs `unmapped` more and copies its attributes.
    weight: per-triangle factor on its quadrics (shapes to keep). targets: triangle counts, descending. Returns ([tris as wedge ids per target], wsrc): wedge w
    has the attributes of source wedge wsrc[w] and the position wpos[w] (wpos grows in place)."""
    import heapq
    TW = [list(t) for t in TW]; wsrc = list(range(len(wpos)))
    TP = np.array([[wpos[w] for w in t] for t in TW])
    a, b, c = P[TP[:, 0]], P[TP[:, 1]], P[TP[:, 2]]
    fn = np.cross(b - a, c - a); ar = np.linalg.norm(fn, axis=1)
    pl = np.concatenate([fn / np.maximum(ar, 1e-20)[:, None], np.zeros((len(fn), 1))], 1)
    pl[:, 3] = -np.sum(pl[:, :3] * a, 1)
    Q = np.zeros((len(P), 4, 4))
    wt = np.ones(len(TW)) if weight is None else np.asarray(weight, np.float64)
    K = (0.5 * ar * wt)[:, None, None] * pl[:, :, None] * pl[:, None, :]
    for k in range(3): np.add.at(Q, TP[:, k], K)
    sides = {}
    for t, (tp, tw) in enumerate(zip(TP.tolist(), TW)):
        for k in range(3):
            i, j = tp[k], tp[(k + 1) % 3]
            sides.setdefault((min(i, j), max(i, j)), []).append((t, tw[k] if i < j else tw[(k + 1) % 3], tw[(k + 1) % 3] if i < j else tw[k]))
    for (i, j), ss in sides.items():
        if len(ss) == 2 and ss[0][1:] == ss[1][1:]: continue   # inner edge, same wedges both sides
        e = P[j] - P[i]
        for t, _, _ in ss:
            n = np.cross(e, pl[t, :3]); ln = np.linalg.norm(n)
            if ln < 1e-12: continue
            p = np.append(n / ln, -np.dot(n / ln, P[i])); k = seam * wt[t] * np.dot(e, e) * np.outer(p, p)
            Q[i] += k; Q[j] += k
    vt = [set() for _ in P]
    for t, tp in enumerate(TP.tolist()):
        for v in tp: vt[v].add(t)
    alive, nalive, ver = [True] * len(TW), len(TW), [0] * len(P)
    Ph = np.concatenate([P, np.ones((len(P), 1))], 1)

    def corner(t, v): return next(k for k in range(3) if wpos[TW[t][k]] == v)
    PL = P.tolist()   # plain floats: numpy on 3-vectors costs more than the arithmetic

    def nrm(a, b, c):
        ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]; vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
        return uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx

    def cost(u, v):
        shared = vt[u] & vt[v]
        if not shared: return None
        for t in vt[u] - shared:
            p = [PL[wpos[w]] for w in TW[t]]; n0 = nrm(*p); p[corner(t, u)] = PL[v]; n1 = nrm(*p)
            d, l0, l1 = sum(x * y for x, y in zip(n0, n1)), sum(x * x for x in n0), sum(x * x for x in n1)
            if l1 < 1e-6 * l0 or d < 0 or d * d < 0.09 * l0 * l1: return None   # flip or sliver
        mp = {TW[t][corner(t, u)] for t in shared}
        miss = len({TW[t][corner(t, u)] for t in vt[u] - shared} - mp)
        h = Ph[v]; return float(h @ (Q[u] + Q[v]) @ h) + unmapped * miss

    heap = []
    def push(u, v):
        cst = cost(u, v)
        if cst is not None: heapq.heappush(heap, (cst, u, v, ver[u], ver[v]))
    for (i, j) in sides: push(i, j); push(j, i)
    out = []
    for target in targets:
        while nalive > target and heap:
            cst, u, v, vu, vv = heapq.heappop(heap)
            if vu != ver[u] or vv != ver[v]: continue
            now = cost(u, v)
            if now is None: continue
            if now > cst + 1e-9 * abs(cst) + 1e-30: heapq.heappush(heap, (now, u, v, vu, vv)); continue
            shared = vt[u] & vt[v]
            mp = {TW[t][corner(t, u)]: TW[t][corner(t, v)] for t in shared}
            for t in shared:
                alive[t] = False; nalive -= 1
                for w in TW[t]: vt[wpos[w]].discard(t)
            for t in list(vt[u]):
                k = corner(t, u); w = TW[t][k]
                if w not in mp:   # no counterpart on v: same attributes, v's position
                    mp[w] = len(wpos); wpos.append(v); wsrc.append(wsrc[w])
                TW[t][k] = mp[w]; vt[v].add(t)
            vt[u] = set(); Q[v] += Q[u]; ver[u] += 1; ver[v] += 1
            nb = {wpos[w] for t in vt[v] for w in TW[t]} - {v}
            for n in nb: push(v, n); push(n, v)
        out.append(np.array([TW[t] for t in range(len(TW)) if alive[t]], np.int64).reshape(-1, 3))
    return out, wsrc


def hull(pos, paint=5, glass=7, n=6):
    """Low LOD: the car lofted through n cross-sections along z, each a hexagon (bottom, shoulder,
    roof corners) from the vertices around it; sides, roof and end caps, no floor.
    Upper sides are glass where the cabin stands (more than 0.25 m over the shoulder)."""
    z0, z1 = pos[:, 2].min(), pos[:, 2].max(); L = z1 - z0
    secs = []
    for s in np.linspace(0, 1, n):
        zc = z0 + L * min(max(s, 0.04), 0.96)
        q = pos[np.abs(pos[:, 2] - zc) < 0.08 * L]
        w, yb, yt = np.percentile(np.abs(q[:, 0]), 99), np.percentile(q[:, 1], 1), np.percentile(q[:, 1], 99)
        side = q[np.abs(q[:, 0]) > 0.9 * w]; ys = min(np.percentile(side[:, 1], 95) if len(side) else yt, yt)
        top = q[q[:, 1] > ys + 0.6 * (yt - ys)]; wt = np.percentile(np.abs(top[:, 0]), 95) if len(top) > 4 else w * 0.7
        z = z0 + L * s
        secs.append(np.array([[-w, yb, z], [-w, ys, z], [-wt, yt, z], [wt, yt, z], [w, ys, z], [w, yb, z]]))
    V, F = [], []   # F: (vertex ids CCW out, material)
    def quad(p, mat):
        k = len(V); V.extend(p); F.append(((k, k + 1, k + 2), mat)); F.append(((k, k + 2, k + 3), mat))
    for a, b in zip(secs, secs[1:]):   # z grows from a to b
        cab = (a[2, 1] + b[2, 1]) / 2 - (a[1, 1] + b[1, 1]) / 2 > 0.25
        for i in range(5):
            mat = glass if cab and i in (1, 3) else paint
            quad([a[i], a[i + 1], b[i + 1], b[i]], mat)
    for s, flip in ((secs[0], True), (secs[-1], False)):   # caps: front (z min) faces -z
        k = len(V); V.extend(s)
        for i in range(1, 5):
            F.append(((k, k + i + 1, k + i) if not flip else (k, k + i, k + i + 1), paint))
    V = np.array(V); out = []
    for (i, j, k), mat in F:   # orient each face away from the car's axis
        nrm = np.cross(V[j] - V[i], V[k] - V[i]); cen = (V[i] + V[j] + V[k]) / 3 - [0, (V[:, 1].min() + V[:, 1].max()) / 2, (z0 + z1) / 2]
        out.append(((i, j, k) if np.dot(nrm, cen) >= 0 else (i, k, j), mat))
    return V, out


def lod_meshes(groups_by_mat, vmax=3907, gmax=28):
    """{material: (pos, nrm, uv, tris)} -> meshes [{'mats': [...], 'pos', 'nrm', 'uv', 'groups'}],
    materials kept whole, at most vmax vertices and gmax groups a mesh."""
    out, cur = [], None
    for mi, (p, nr, uv, T) in sorted(groups_by_mat.items()):
        strips = split_strips(T)
        if cur is None or len(cur['pos']) + len(p) > vmax or len(cur['groups']) + len(strips) > gmax:
            cur = {'pos': np.zeros((0, 3)), 'nrm': np.zeros((0, 3)), 'uv': np.zeros((0, 2)), 'groups': [], 'mats': []}; out.append(cur)
        b = len(cur['pos'])
        cur['groups'] += [{'mat': mi, 'desc': DESC, 'strips': [(s.astype(np.int64) + b).astype(np.uint16)]} for s in strips]
        cur['pos'] = np.concatenate([cur['pos'], p]); cur['nrm'] = np.concatenate([cur['nrm'], nr])
        cur['uv'] = np.concatenate([cur['uv'], uv]); cur['mats'].append(mi)
    return out


def build(spec_path):
    """New car from a .glb on a template car (see the spec: .scratch, per car). Every template file
    (<t>.xbck, _o, _g) keeps its object graph. The .glb is welded and decimated (decimate()) to each
    spec 'lods' entry's triangle count ('hull': a box loft instead), split into meshes of at most a
    stock car's vertices and groups and put in that LOD's table slots; every template mesh not kept
    is freed and the new meshes fill those holes first (file sizes stay the template's). Unused slots
    get empty meshes, bones move, the trim texture becomes an atlas (DXT1 when opaque), the cull
    sphere grows to the new body, null part slots get empty part files."""
    sp = json.load(open(spec_path))
    tpl, out, car = sp['template'], sp['out'], sp['car']
    tname = os.path.basename(os.path.normpath(tpl))
    ax, shift = np.array(sp['axes'], np.float64), np.array(sp['offset'], np.float64)
    rules = [(re.compile(r[0]), r[1], r[2] if len(r) > 2 else None) for r in sp['materials']]
    src = [m for m in read_glb(sp['glb']) if not re.search(sp['drop'], m['name']) and len(m['tris'])]
    plate = {i for i, mt in enumerate(materials(Chunk(os.path.join(tpl, tname + '.xbck')))) if any(n == '__licenseplate__' for n, _ in mt['texs'])}
    for m in src:
        m['pos'] = m['pos'] * ax + shift; m['nrm'] = m['nrm'] * ax
        if np.prod(ax) < 0: m['tris'] = m['tris'][:, ::-1]   # a mirror turns the winding
        m['to'], m['cell'] = next((t, cell) for rx, t, cell in rules if rx.search('%s|%s|%s' % (m['name'], m['mat'], m['tex'])))
        lo = np.floor(m['uv'].min(0))
        if np.all(m['uv'].max(0) - lo <= 1): m['uv'] = m['uv'] - lo   # mesh within one tile: bring it to 0..1
        if m['to'] in plate: m['uv'][:, 1] = 1 - m['uv'][:, 1]   # glTF v runs down, the game's plate texture up
    # atlas: each cell holds the part of its source the meshes use
    A, grid = sp['atlas']['size'], sp['atlas']['grid']; cs = A // grid
    atlas = Image.new('RGBA', (A, A), (128, 128, 128, 255))
    for k, cell in enumerate(dict.fromkeys(m['cell'] for m in src if m['cell'])):
        ms = [m for m in src if m['cell'] == cell]
        im = Image.open(os.path.join(sp['atlas']['dir'], cell)).convert('RGBA'); W, H = im.size
        uv = np.clip(np.concatenate([m['uv'] for m in ms]), 0, 1)
        x0, y0 = int(uv[:, 0].min() * W), int(uv[:, 1].min() * H)
        x1, y1 = max(int(np.ceil(uv[:, 0].max() * W)), x0 + 1), max(int(np.ceil(uv[:, 1].max() * H)), y0 + 1)
        cx, cy = (k % grid) * cs, (k // grid) * cs
        atlas.paste(im.crop((x0, y0, x1, y1)).resize((cs, cs), Image.LANCZOS), (cx, cy))
        for m in ms:
            t = (np.clip(m['uv'], 0, 1) * [W, H] - [x0, y0]) / [x1 - x0, y1 - y0]
            m['uv'] = (np.array([cx, cy]) + 2 + np.clip(t, 0, 1) * (cs - 4)) / A   # 2-texel inset against bleeding
    # weld (positions 0.1 mm; a wedge = position + uv + normal + material), decimate to each LOD
    Vp = np.concatenate([m['pos'] for m in src]); Vn = np.concatenate([m['nrm'] for m in src])
    Vuv = np.concatenate([m['uv'] for m in src]); Vm = np.concatenate([np.full(len(m['pos']), m['to']) for m in src])
    offs = np.cumsum([0] + [len(m['pos']) for m in src])
    Vt = np.concatenate([m['tris'] + o for m, o in zip(src, offs)])
    _, first, pid = np.unique(np.round(Vp / 1e-4).astype(np.int64), axis=0, return_index=True, return_inverse=True)
    pid = pid.ravel(); P = Vp[first]
    # materials whose shader reads no texture by uv (spec 'nouv': plain, envmap, procedural paint) drop it
    Vuv[np.isin(Vm, sp.get('nouv', []))] = 0
    # normals: at one position, uv and material, those within 60 degrees become one (averaged)
    Vn = Vn / np.maximum(np.linalg.norm(Vn, axis=1, keepdims=True), 1e-9)
    _, grp = np.unique(np.concatenate([pid[:, None], np.round(Vuv / 1e-4), Vm[:, None]], 1).astype(np.int64), axis=0, return_inverse=True)
    cl, heads = np.zeros(len(Vp), np.int64), {}
    for i in np.argsort(grp.ravel(), kind='stable').tolist():
        hs = heads.setdefault(int(grp.ravel()[i]), [])
        k = next((h for h in hs if Vn[h] @ Vn[i] > 0.5), None)
        if k is None: hs.append(i); k = i
        cl[i] = k
    acc = np.zeros_like(Vn); np.add.at(acc, cl, Vn); Vn = acc[cl]
    _, wfirst, wid = np.unique(cl, return_index=True, return_inverse=True); wid = wid.ravel()
    tp = pid[Vt]; ok = (tp[:, 0] != tp[:, 1]) & (tp[:, 1] != tp[:, 2]) & (tp[:, 0] != tp[:, 2])
    wpos = pid[wfirst].tolist()
    lods = sp['lods']
    prot = sp.get('protect', {})   # {material: quadric weight}: lights keep their shapes longer
    res, wsrc = decimate(P, wid[Vt][ok], wpos, [l['tris'] for l in lods if 'tris' in l],
                         weight=[prot.get(str(mi), 1.0) for mi in Vm[Vt[ok][:, 0]]])
    wsrc, wpos = np.array(wsrc), np.array(wpos)
    lo, hi = np.percentile(Vp, 0.2, 0), np.percentile(Vp, 99.8, 0)   # robust box (an antenna is not the car)
    hi[0] = max(hi[0], -lo[0]); lo[0] = -hi[0]
    built, it = [], iter(res)
    for l in lods:
        byMat = {}
        if 'tris' in l:
            T = next(it); tm = Vm[wfirst[wsrc[T[:, 0]]]]
            for mi in np.unique(tm):
                used, loc = np.unique(T[tm == mi], return_inverse=True); s = wfirst[wsrc[used]]
                a = np.concatenate([P[wpos[used]], Vn[s], Vuv[s]], 1)   # collapses leave equal copies: one each
                _, one, back = np.unique(np.round(a / 1e-5).astype(np.int64), axis=0, return_index=True, return_inverse=True)
                byMat[int(mi)] = (a[one, :3], a[one, 3:6], a[one, 6:], back.ravel()[loc].reshape(-1, 3))
        else:   # 'hull': [paint, glass] materials of a box loft of the whole source
            V, F = hull(Vp, l['hull'][0], l['hull'][1])
            nr = np.zeros((len(V), 3))
            for (i, j, k), _ in F: nr[[i, j, k]] = np.cross(V[j] - V[i], V[k] - V[i])
            for mi in sorted({f[1] for f in F}):
                T = np.array([f[0] for f in F if f[1] == mi]); used, loc = np.unique(T, return_inverse=True)
                byMat[mi] = (V[used], nr[used], np.zeros((len(used), 2)), loc.reshape(-1, 3))
        meshes = lod_meshes(byMat)
        for m in meshes:
            m['vtx'] = np.zeros(len(m['pos']), VTX)
            m['vtx']['pos'], m['vtx']['nrm'], m['vtx']['uv'] = m['pos'], pack_normal(m['nrm']), m['uv']
            m['vmat'] = np.concatenate([np.full(len(byMat[mi][0]), mi) for mi in m['mats']])
        built.append(meshes)
        print('LOD %s: %d tris; %s' % (l['slots'], sum(len(t[3]) for t in byMat.values()), ', '.join(
            'mesh %d verts %d groups (longest strip %d)' % (len(m['pos']), len(m['groups']), max(len(g['strips'][0]) for g in m['groups']))
            for m in meshes)))
    parts, pbase, zoned = {}, Chunk(glob.glob(os.path.join(tpl, 'vroot_*.xbck'))[0]).base, False
    for suf in sp.get('files', ['', '_o', '_g']):
        tf = os.path.join(tpl, tname + suf + '.xbck'); c = Chunk(tf); img = Img(tf)
        bones = read_skeleton(c)
        slots = []
        for t in sorted(c.find_vt(VT_TABLE)):
            n = c.u('2H', t + 4)[1]; pm, pn, pb = c.u('3I', t + 8)
            slots += [(pm + 4 * i, c.u32(pm + 4 * i), c.cstr(c.u32(pn + 4 * i)), c.u32(pb + 4 * i)) for i in range(n)]
        if not zoned:   # diffuse = dent vector + zone (docstring), by position only so a hit keeps seams shut:
            # zone and dent length of the template body's nearest vertex in box units, direction = -(our normal
            # averaged over every vertex at that position); the template's directions fit its own surface only
            tv = np.concatenate([decode_mesh(c, mp)['vtx'] for _, mp, name, _ in slots if mp and re.search(sp['zone_from'], name)])
            tq = (tv['pos'].astype(np.float64) - tv['pos'].min(0)) / (tv['pos'].max(0) - tv['pos'].min(0))
            tcol = tv['col'].astype(np.int64)
            tlen = np.linalg.norm(np.stack([(tcol >> s & 255) - 128 for s in (16, 8, 0)], 1), axis=1)
            ms = [m for meshes in built for m in meshes]
            ap = np.concatenate([m['pos'] for m in ms]); an = np.concatenate([m['nrm'] for m in ms])
            an = an / np.maximum(np.linalg.norm(an, axis=1, keepdims=True), 1e-9)
            _, key = np.unique(np.round(ap / 1e-5).astype(np.int64), axis=0, return_inverse=True); key = key.ravel()
            ns = np.zeros((key.max() + 1, 3)); np.add.at(ns, key, an)
            ns /= np.maximum(np.linalg.norm(ns, axis=1, keepdims=True), 1e-9)
            near = _nearest((ap - lo) / (hi - lo), tq)
            byte = np.clip(np.round(128 - ns[key] * tlen[near][:, None]), 0, 255).astype(np.int64)
            col = (byte[:, 0] << 16 | byte[:, 1] << 8 | byte[:, 2] | tcol[near] & 0xFF000000).astype(np.uint32)
            b = 0
            for m in ms: m['vtx']['col'] = col[b:b + len(m['pos'])]; b += len(m['pos'])
            zoned = True
        targets = {}
        for rx, op, v in sp['bones']:
            for k, b in enumerate(bones):
                if not re.search(rx, b['name']): continue
                w = b['world0']
                targets[k] = (np.array([w[i] if v[i] is None else v[i] for i in range(3)]) if op == 'set'
                              else w + [v[0] * np.sign(w[0]), v[1], v[2]])   # add: x outward
        keep = [(mp, mp + len(encode_mesh(decode_mesh(c, mp), mp))) for _, mp, name, _ in slots if mp and re.search(sp['keep'], name)]
        moved = move_bones(img, bones, targets, keep)
        for mp in sorted({mp for _, mp, _, _ in slots if mp} - {a for a, _ in keep}):   # every mesh the new car drops
            img.free(mp, len(encode_mesh(decode_mesh(c, mp), mp)))
        tva = next(t for m in materials(c) for n, t in m['texs'] if n == sp['atlas']['param'] and t)
        tw, th, cf = put_texture(img, c, tva, atlas)
        filled, empties = set(), {}
        for l, meshes in zip(lods, built):
            ss = [s for s in slots if re.search(l['slots'], s[2])]
            if len(meshes) > len(ss): raise ValueError('%s: %d meshes, %d slots' % (l['slots'], len(meshes), len(ss)))
            for m, (slot, _, name, bone) in zip(meshes, ss):
                img.put(slot, struct.pack('<I', place_mesh(img, {'hdr': bytes([0, 0, bone, 0x1C]), 'groups': m['groups'], 'vtx': m['vtx']})))
                filled.add(slot)
        for slot, mp, name, bone in slots:
            if slot in filled or re.search(sp['keep'], name): continue
            if mp:
                if bone not in empties:
                    e = empty_mesh(bone); empties[bone] = img.place(len(encode_mesh(e, 0)), lambda va: encode_mesh(e, va))
                img.put(slot, struct.pack('<I', empties[bone]))
            else: parts[name] = bone
        unshare(img, [sl for sl, _, _, _ in slots])   # one Mesh, Group array and VB per slot (see the docstring)
        # the drawable's LOD group: tables H M L, 0, LOD distances, bucket masks, then the cull sphere
        # (x y z r) at +0x30; grown to hold the new car, never shrunk
        g = c.d.find(struct.pack('<4I', *sorted(c.find_vt(VT_TABLE)), 0)); sva = c.va(g) + 0x30
        sph = np.array(c.u('4f', sva))
        r = 1.02 * max(float(np.linalg.norm(m['pos'] - sph[:3], axis=1).max()) for ms in built for m in ms)
        if r > sph[3]: img.put(sva + 12, struct.pack('<f', r))
        for a, b in sp.get('rename', []):
            if len(a) != len(b): raise ValueError('rename %s: same length only' % a)
            img.d = bytearray(img.d.replace(a.encode() + b'\0', b.encode() + b'\0'))
        img.save(os.path.join(out, car + suf + '.xbck'))
        print('%s%s.xbck: %d bytes (template %d); %d bones moved, %d empty meshes, atlas %dx%d %s, '
              'cull sphere r %.3f -> %.3f, %d B of holes left' % (
              car, suf, len(img.d) + 0x80, len(c.d) + 0x80, len(moved), len(empties), tw, th,
              {0x0C: 'DXT1', 0x0F: 'DXT5'}[cf], sph[3], max(r, sph[3]), sum(h[1] - h[0] for h in img.holes)))
    for name, bone in parts.items():
        p = Img(base=pbase, kind=0x16); p.add(lambda va: encode_mesh(empty_mesh(bone), va)); p.save(os.path.join(out, name + '.xbck'))
    print('%d empty part files' % len(parts))
    if sp.get('preview'):
        export(out, sp['preview'], texdirs=sp.get('tex', []), report=True)


if __name__ == '__main__':
    if sys.argv[1:2] == ['roundtrip']: sys.exit(1 if roundtrip(sys.argv[2]) else 0)
    if sys.argv[1:2] == ['check']: sys.exit(1 if check(sys.argv[2:]) else 0)
    if sys.argv[1:2] == ['build']: build(sys.argv[2]); sys.exit(0)
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
