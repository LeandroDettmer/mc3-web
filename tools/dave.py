#!/usr/bin/env python3
"""List and extract Midnight Club 3 "Dave" archives (assets_x.dat*, banks.dat).

  dave.py list <archive>
  dave.py extract <archive> <name-or-glob> <outdir>

Format (the Remix's reader sub_002B3D20, Dave branch at 0x002B4013):
  header  u32 magic, count, toc_bytes, names_bytes
          "DAVE" = plain names; "Dave" = packed names (flags 0x5587C0/C1)
  0x800   TOC, count x 16 bytes: name_off (into the name table),
          data_off (absolute), size (unpacked), stored (bytes on disc)
          (sub_002B3900 decrypts it only when the key at 0x558318 is
          set; no disc seen needs it)
  then    name table (names_bytes)
"Dave" names (packer sub_002B33C0, char packer sub_002B3210): each name
is 6-bit codes, LSB first, ended by code 0; it may start with two codes
0x38 + (n & 7), 0x20 + (n >> 3): the first n chars of the previous
entry's name. Letters come back lowercase ('\\' was stored as '/').
Data: stored != size means raw deflate (inflateInit2 -15, sub_002B4310
called from 0x002B3751).
"""
import fnmatch
import os
import struct
import sys
import zlib

CHARS = {1: ' ', 2: '#', 3: '$', 4: '(', 5: ')', 6: '-', 7: '.', 8: '/',
         0x14: '_', 0x2F: '~', 0x30: '?'}
CHARS.update({0x0A + i: chr(0x30 + i) for i in range(10)})
CHARS.update({0x15 + i: chr(0x61 + i) for i in range(26)})


def packed_name(table, off):
    """6-bit codes from table[off] up to (not including) code 0."""
    out, acc, bits = [], 0, 0
    while True:
        while bits < 6:
            acc |= table[off] << bits
            off += 1
            bits += 8
        code = acc & 63
        acc >>= 6
        bits -= 6
        if code == 0:
            return out
        out.append(code)


def entries(path):
    """[(index, name, data_off, size, stored)] in TOC order."""
    with open(path, 'rb') as f:
        magic, count, toc_bytes, names_bytes = struct.unpack('<4sIII', f.read(16))
        if magic not in (b'DAVE', b'Dave'):
            sys.exit('%s: not a Dave archive (magic %r)' % (path, magic))
        f.seek(0x800)
        toc = f.read(toc_bytes)
        table = f.read(names_bytes)
    out, prev = [], ''
    for i in range(count):
        name_off, data_off, size, stored = struct.unpack_from('<4I', toc, i * 16)
        if magic == b'DAVE':
            name = table[name_off:table.index(b'\0', name_off)].decode('latin-1')
        else:
            codes = packed_name(table, name_off)
            keep = 0
            if codes and codes[0] >= 0x38:
                keep = (codes[0] - 0x38) | ((codes[1] - 0x20) << 3)
                codes = codes[2:]
            name = prev[:keep] + ''.join(CHARS.get(c, '?') for c in codes)
        prev = name
        out.append((i, name, data_off, size, stored))
    return out


def read_entry(f, data_off, size, stored):
    f.seek(data_off)
    data = f.read(stored)
    if stored != size:
        data = zlib.decompress(data, -15)
    if len(data) != size:
        raise ValueError('size %d, expected %d' % (len(data), size))
    return data


def main(argv):
    if len(argv) == 3 and argv[1] == 'list':
        for i, name, off, size, stored in entries(argv[2]):
            print('%d\t%s\t0x%X\t%d\t%d' % (i, name, off, size, stored))
        return 0
    if len(argv) == 5 and argv[1] == 'extract':
        pat = argv[3].lower().replace('\\', '/')
        # names ending in '/' are folder entries (size 0): nothing to write
        hits = [e for e in entries(argv[2])
                if fnmatch.fnmatchcase(e[1], pat) and not e[1].endswith('/')]
        with open(argv[2], 'rb') as f:
            for i, name, off, size, stored in hits:
                # code 0x30 ('?': any char the packer did not know) is lost
                dst = os.path.join(argv[4], *name.replace('?', '_').split('/'))
                os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
                with open(dst, 'wb') as o:
                    o.write(read_entry(f, off, size, stored))
                print('%s\t%d' % (name, size))
        print('%d file(s)' % len(hits), file=sys.stderr)
        return 0 if hits else 1
    print(__doc__.split('\n\n')[0], file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
