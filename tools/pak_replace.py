#!/usr/bin/env python3
"""Replace files inside an OpenBOR .pak without unpacking it.

Usage: pak_replace.py <in.pak> <out.pak> <path in pak>=<file> [...]

  path in pak   as stored, e.g. data/videos/intro.webm (/ or \\, any case)

Every file is written again in its old order (the replaced ones with their
new contents), then the file table in its old order; bytes no entry points
to are dropped. The magic is written as PACK.
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pak_fix_magic import read_table

if len(sys.argv) < 4:
    sys.exit(__doc__)
src, dst = sys.argv[1:3]
repl = {}
for a in sys.argv[3:]:
    k, _, v = a.partition('=')
    repl[k.replace('/', '\\').lower()] = v
size = os.path.getsize(src)
f = open(src, 'rb')
ents = read_table(f, size)
f.seek(size - 4)
start = struct.unpack('<I', f.read(4))[0]
names = {n.lower() for n, _, _ in ents}
miss = [k for k in repl if k not in names]
if miss:
    sys.exit('not in the pak: ' + ', '.join(miss))
out = open(dst, 'wb')
out.write(b'PACK' + struct.pack('<I', 0))
pos, new = 8, {}
# every file written again in its old order: the replaced ones with their
# new contents, nothing unreferenced is kept
for name, o, l in sorted(ents, key=lambda e: e[1]):
    k = name.lower()
    if k in repl:
        d = open(repl[k], 'rb').read()
    else:
        f.seek(o)
        d = f.read(l)
    out.write(d)
    new[k] = (pos, len(d))
    pos += len(d)
table = bytearray()
for name, o, l in ents:
    o, l = new[name.lower()]
    nb = name.encode('latin-1')
    table += struct.pack('<III', 12 + len(nb) + 1, o, l) + nb + b'\0'
out.write(table + struct.pack('<I', pos))
out.close()
print('%s: %d files, %d replaced, written %s' % (src, len(ents), len(repl), dst))
