#!/usr/bin/env python3
# wix-suite-extract.py SUITE_DIR OUT_DIR [PREFIX...]
#
# Unpacks the Windows Live 11 Suite installer (a WiX bundle: "Ableton Live 11 Suite
# Installer.exe" with Setup.msi inside, and the files in "... Installer Data N.cab")
# into OUT_DIR/Program and OUT_DIR/Resources, as Live's Windows folder has them.
# With PREFIXes (paths under the program folder, e.g. "Resources/Core Library"),
# only files under them are kept.
#
# Needs cabextract (Setup.msi is LZX-compressed inside the .exe). The MSI's file
# tables (File, Component, Directory) are read here from its compound file, with
# no other library: the cabinets name every file by an MSI key only.
import os, struct, subprocess, sys, tempfile

# --- compound file (OLE) -------------------------------------------------------
class Cfb:
    def __init__(self, data):
        self.d = d = data
        assert d[:8] == b'\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1', 'not a compound file'
        self.ss = 1 << struct.unpack_from('<H', d, 0x1e)[0]
        self.mss = 1 << struct.unpack_from('<H', d, 0x20)[0]
        nfat, = struct.unpack_from('<I', d, 0x2c)
        dirstart, = struct.unpack_from('<I', d, 0x30)
        self.cutoff, = struct.unpack_from('<I', d, 0x38)
        minifat, nminifat, difat, ndifat = struct.unpack_from('<IIII', d, 0x3c)
        fatsecs = list(struct.unpack_from('<109I', d, 0x4c))[:nfat]
        while difat not in (0xfffffffe, 0xffffffff) and len(fatsecs) < nfat:
            s = self.sec(difat); n = self.ss // 4 - 1
            fatsecs += list(struct.unpack_from('<%dI' % n, s, 0)); difat, = struct.unpack_from('<I', s, n * 4)
        fatsecs = fatsecs[:nfat]
        self.fat = []
        for s in fatsecs: self.fat += struct.unpack_from('<%dI' % (self.ss // 4), self.sec(s), 0)
        dirdata = self.chain(dirstart)
        self.entries = []
        for o in range(0, len(dirdata), 128):
            nl, = struct.unpack_from('<H', dirdata, o + 0x40)
            name = dirdata[o:o + max(nl - 2, 0)].decode('utf-16le', 'surrogatepass')
            typ = dirdata[o + 0x42]
            start, size = struct.unpack_from('<IQ', dirdata, o + 0x74)
            self.entries.append((name, typ, start, size))
        root = self.entries[0]
        self.ministream = self.chain(root[2])[:root[3]]
        self.minifat = []
        if nminifat:
            mf = self.chain(minifat)
            self.minifat = list(struct.unpack_from('<%dI' % (len(mf) // 4), mf, 0))
    def sec(self, n): return self.d[(n + 1) * self.ss:(n + 2) * self.ss]
    def chain(self, s):
        out = []
        while s not in (0xfffffffe, 0xffffffff):
            out.append(self.sec(s)); s = self.fat[s]
        return b''.join(out)
    def minichain(self, s):
        out = []
        while s not in (0xfffffffe, 0xffffffff):
            out.append(self.ministream[s * self.mss:(s + 1) * self.mss]); s = self.minifat[s]
        return b''.join(out)
    def streams(self):
        for name, typ, start, size in self.entries:
            if typ == 2:
                data = self.chain(start) if size >= self.cutoff else self.minichain(start)
                yield name, data[:size]

# MSI stream names are packed: two characters per UTF-16 unit (0x3800..0x47ff),
# one (0x4800..0x483f), 0x4840 = table prefix "!".
ALPHA = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._'
def msi_name(n):
    out = []
    for ch in n:
        c = ord(ch)
        if 0x3800 <= c < 0x4800: c -= 0x3800; out.append(ALPHA[c & 0x3f]); out.append(ALPHA[(c >> 6) & 0x3f])
        elif 0x4800 <= c < 0x4840: out.append(ALPHA[c - 0x4800])
        elif c == 0x4840: out.append('!')
        else: out.append(ch)
    return ''.join(out)

# --- MSI tables --------------------------------------------------------------------
def msi_files(msi):
    st = {msi_name(n): d for n, d in Cfb(open(msi, 'rb').read()).streams()}
    pool, data = st['!_StringPool'], st['!_StringData']
    _, flags = struct.unpack_from('<HH', pool, 0)
    refsz = 3 if flags & 0x8000 else 2
    strs = ['']; off = 0; i = 1; n = len(pool) // 4
    while i < n:
        ln, refs = struct.unpack_from('<HH', pool, i * 4)
        if ln == 0 and refs:
            i += 1; ln2, _ = struct.unpack_from('<HH', pool, i * 4); ln = (refs << 16) | ln2; strs.append(None)
        strs.append(data[off:off + ln].decode('cp1252', 'replace')); off += ln; i += 1
    def colsize(t):
        if (t & ~0x1000) == 0x0900: return 2
        if t & 0x0800: return refsz
        return 2 if (t & 0xff) <= 2 else 4
    def table(raw, types):
        rs = sum(colsize(t) for t in types); rows = len(raw) // rs if rs else 0; cols = []; o = 0
        for t in types:
            sz = colsize(t); vals = []
            for r in range(rows):
                v = int.from_bytes(raw[o + r * sz:o + (r + 1) * sz], 'little')
                if t & 0x0800 and (t & ~0x1000) != 0x0900: vals.append(strs[v] if v else None)
                else: vals.append(None if v == 0 else v ^ (0x8000 if sz == 2 else 0x80000000))
            cols.append(vals); o += sz * rows
        return [list(r) for r in zip(*cols)]
    schema = {}
    for t, num, name, typ in table(st['!_Columns'], [0x0d00, 0x0502, 0x0d00, 0x0502]):
        schema.setdefault(t, []).append((num, name, typ))
    def load(t):
        sc = sorted(schema[t]); return [dict(zip([c[1] for c in sc], r)) for r in table(st['!' + t], [c[2] for c in sc])]
    long_ = lambda s: s.split('|')[-1]
    dirs = {r['Directory']: r for r in load('Directory')}; cache = {}
    def dpath(k):
        if k in cache: return cache[k]
        r = dirs[k]; nm = long_(r['DefaultDir'].split(':')[0]); par = r['Directory_Parent']
        p = '' if not par or par == k else dpath(par)
        p = p if nm in ('.', 'SourceDir') else (p + '/' + nm if p else nm)
        cache[k] = p; return p
    comp = {r['Component']: r for r in load('Component')}
    return {r['File']: dpath(comp[r['Component_']]['Directory_']) + '/' + long_(r['FileName']) for r in load('File')}

def main():
    suite, out = sys.argv[1], sys.argv[2]; prefixes = sys.argv[3:]
    exe = next(os.path.join(suite, f) for f in os.listdir(suite) if f.lower().endswith('installer.exe'))
    cabs = sorted(f for f in os.listdir(suite) if f.lower().endswith('.cab'))
    work = tempfile.mkdtemp(dir=out if os.path.isdir(out) else None, prefix='.wix-')
    # Setup.msi: the attached container's entry whose MSI tables open
    subprocess.run(['cabextract', '-q', '-d', work, exe], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    msi = None
    for f in sorted(os.listdir(work)):
        p = os.path.join(work, f)
        with open(p, 'rb') as fh:
            if fh.read(8) == b'\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1':
                try: files = msi_files(p); msi = p; break
                except Exception: pass
    if not msi: sys.exit('no Setup.msi in ' + exe)
    raw = os.path.join(work, 'raw'); os.makedirs(raw)
    for c in cabs:   # each cabinet stands alone (no spanning)
        subprocess.run(['cabextract', '-q', '-d', raw, os.path.join(suite, c)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    placed = 0
    for key, path in files.items():
        rel = path.split('Ableton/Live/', 1)[-1]
        if prefixes and not any(rel == p or rel.startswith(p.rstrip('/') + '/') for p in prefixes): continue
        src = os.path.join(raw, key)
        if not os.path.exists(src): continue
        dst = os.path.join(out, rel); os.makedirs(os.path.dirname(dst), exist_ok=True); os.replace(src, dst); placed += 1
    subprocess.run(['rm', '-rf', '--', work], check=False)
    print(placed)

main()
