#!/usr/bin/env python3
"""SoRX pak that can be renamed: the startup check in data/scripts/loading.c
(loadPak) opened "Paks/SORX.pak" or "Paks/1.0.0.pak" by name and shut the
game down when neither was found (or when its first token was neither PACK
nor empty). The check is removed; the header gets the standard PACK magic.

Usage: sorx_renamable.py <SORX.pak (original or fixed)> <out.pak>

The other names the game's scripts use (Saves/SORX.set, Saves/SORX.sco,
the translation files) are its own data files, read and written under
fixed names: they do not depend on the pak's name and are left as they are.
The old loading.c stays in the data area, unreferenced.
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pak_fix_magic import read_table

src, dst = sys.argv[1:3]
TARGET = 'data\\scripts\\loading.c'
size = os.path.getsize(src)
f = open(src, 'rb')
ents = read_table(f, size)
f.seek(size - 4)
start = struct.unpack('<I', f.read(4))[0]
names = [e[0] for e in ents]
if names.count(TARGET) != 1:
    sys.exit('%s: no single %s' % (src, TARGET))
_, off, ln = ents[names.index(TARGET)]
f.seek(off)
code = f.read(ln).decode('latin-1')
if 'Paks/SORX.pak' not in code:
    sys.exit('%s: loadPak already patched (or another game version)' % src)
a = code.index('void loadPak()')
b = code.index('\r\n}', code.index('setlocalvar("packInit", 1);')) + 3
new = '\r\n'.join([
    'void loadPak()',
    '{//Load *.pak file',
    ' //(port) the check of "Paks/SORX.pak" / "Paks/1.0.0.pak" is gone: it shut',
    ' //the game down when the pak had another name or another place, and read',
    ' //the whole pak into memory to look at its first word. The pak can now be',
    ' //renamed and loaded from anywhere.',
    '',
    '\tif(getlocalvar("packInit") == NULL()){',
    '\t\tsetlocalvar("packInit", 1);',
    '\t}',
    '}'])
code2 = code[:a] + new + code[b:]
data = code2.encode('latin-1')
# copy the data area, the new loading.c at its end, then the updated table
out = open(dst, 'wb')
out.write(b'PACK')
f.seek(4)
left = start - 4
while left > 0:
    chunk = f.read(min(left, 1 << 24))
    out.write(chunk)
    left -= len(chunk)
noff = start
out.write(data)
table = bytearray()
for name, o, l in ents:
    if name == TARGET:
        o, l = noff, len(data)
    nb = name.encode('latin-1')
    table += struct.pack('<III', 12 + len(nb) + 1, o, l) + nb + b'\0'
tstart = noff + len(data)
out.write(table + struct.pack('<I', tstart))
out.close()
print('%s: %d files, loading.c %d -> %d bytes, written %s' % (src, len(ents), ln, len(data), dst))
