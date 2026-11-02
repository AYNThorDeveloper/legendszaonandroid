#!/usr/bin/env python3
"""Every NVN shader of Pokémon Legends: Z-A, read from the verified RomFS (2.0.2 merged with the base game).

Trinity keeps its files in arc/data.trpfs: "ONEPACK\\0", the offset of a FlatBuffer {pack hashes, pack offsets}, and
at each offset a pack (FlatBuffer {file hashes, files}); a file is a table {.., compression (i8, -1 none), ..,
decompressed size (u64), data [ubyte]}. Compressed files are Oodle streams.

Oodle is not reimplemented: OodleLZ_Decompress of the game's own main (2.0.2 +0x1a9c9e0, the function that reports
"OODLE ERROR : OodleLZ_Decompress: ..." errors) runs under unicorn, as lisde_shaders.Oodle does for Double Exposure,
here from the flat NSO image (relocations from MOD0's dynamic section).

Shaders are BNSH files. Only files whose data shows "BNSH" (in the literals of an Oodle stream, or plainly) are
decompressed; their NVN programs are read with lisde_shaders.nvn_shaders (code area with the 0x12345678 header, the
0x880-byte control section 4 bytes after it). The key is SHA-256(control + code), as on the device.

usage: za_shaders.py [--dump DIR] [--limit-packs N]   (prints counts)
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import time

HERE = Path(__file__).resolve().parent
PORT = HERE.parents[2]
sys.path.insert(0, str(HERE))
import lisde_shaders   # noqa: E402

ROMFS = PORT/'Uitgepakt/romfs-2.0.2'
MAIN_NSO = PORT/'Uitgepakt/exefs-2.0.2/main'
OODLE_DECOMPRESS = 0x1a9c9e0   # OodleLZ_Decompress in main 2.0.2


def nso_image(path=MAIN_NSO):
    import lz4.block
    b = Path(path).read_bytes()
    if b[:4] != b'NSO0':
        raise ValueError('%s is not an NSO' % path)
    flags, = struct.unpack_from('<I', b, 0xC)
    segs = [struct.unpack_from('<IIII', b, 0x10+i*0x10) for i in range(3)]
    csz = struct.unpack_from('<3I', b, 0x60)
    img = bytearray(max(s[1]+s[2] for s in segs))
    for i, (fo, mo, size, _) in enumerate(segs):
        raw = b[fo:fo+csz[i]]
        img[mo:mo+size] = lz4.block.decompress(raw, uncompressed_size=size) if flags >> i & 1 else raw[:size]
    return bytes(img)+bytes(segs[2][3])   # the data segment's .bss (its fourth header word), zeroed


class Oodle(lisde_shaders.Oodle):
    """lisde_shaders.Oodle with the module mapped from a flat NSO image (addresses = offsets). Trinity files reach
    ~73 MB decompressed: the heap is 512 MiB, above the module."""
    HEAP, HEAP_SIZE = 0x80000000, 0x20000000

    def __init__(self, path=MAIN_NSO):
        from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
        from unicorn import arm64_const as a
        self.a = a
        data = nso_image(path)
        mod0, = struct.unpack_from('<I', data, 4)
        dyn = mod0+struct.unpack_from('<i', data, mod0+4)[0]
        tags = {}
        for k in range(0, 0x2000, 16):
            tag, value = struct.unpack_from('<qQ', data, dyn+k)
            if tag == 0:
                break
            tags.setdefault(tag, value)
        u = self.u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(self.BASE, (len(data)+0xfff) & ~0xfff)
        u.mem_write(self.BASE, data)
        symtab, strtab, syment = tags[6], tags[5], tags.get(11, 24)
        self.stubs = {}

        def symbol(i):
            name, _, _, shndx, value, _ = struct.unpack_from('<IBBHQQ', data, symtab+syment*i)
            if shndx:
                return self.BASE+value
            address = self.STUB+8*i
            self.stubs[address] = data[strtab+name:data.index(b'\0', strtab+name)].decode()
            return address
        tables = [(tags[7], tags[8])] + ([(tags[23], tags[2])] if 23 in tags else [])
        for table, size in tables:
            for k in range(0, size, 24):
                where, info, addend = struct.unpack_from('<QQq', data, table+k)
                kind = info & 0xffffffff
                if kind == 1027:
                    u.mem_write(self.BASE+where, struct.pack('<Q', self.BASE+addend))
                elif kind in (257, 1025, 1026):
                    u.mem_write(self.BASE+where, struct.pack('<Q', symbol(info >> 32)+addend))
        if 36 in tags:
            def rebase(where):
                value, = struct.unpack('<Q', bytes(u.mem_read(self.BASE+where, 8)))
                u.mem_write(self.BASE+where, struct.pack('<Q', value+self.BASE))
            where = 0
            for k in range(0, tags[35], 8):
                entry, = struct.unpack_from('<Q', data, tags[36]+k)
                if not entry & 1:
                    rebase(entry)
                    where = entry+8
                else:
                    for bit in range(63):
                        if entry >> (bit+1) & 1:
                            rebase(where+8*bit)
                    where += 63*8
        u.mem_map(self.STUB, 0x100000)
        u.mem_write(self.STUB, b'\xc0\x03\x5f\xd6'*(0x100000//4))
        u.hook_add(UC_HOOK_CODE, self._stub, begin=self.STUB, end=self.STUB+0x100000)
        u.mem_map(self.STACK, self.STACK_SIZE)
        u.mem_map(self.HEAP, self.HEAP_SIZE)
        u.mem_map(self.RET, 0x1000)
        u.mem_map(0x6000000, 0x10000)
        u.reg_write(a.UC_ARM64_REG_TPIDRRO_EL0, 0x6000000)
        u.reg_write(a.UC_ARM64_REG_TPIDR_EL0, 0x6000000)
        u.reg_write(a.UC_ARM64_REG_CPACR_EL1, 0x300000)

    def decompress(self, comp, raw_len):
        global OODLE_DECOMPRESS
        saved, lisde_shaders.OODLE_DECOMPRESS = lisde_shaders.OODLE_DECOMPRESS, OODLE_DECOMPRESS
        try:
            return super().decompress(comp, raw_len)
        finally:
            lisde_shaders.OODLE_DECOMPRESS = saved


class Flat:
    """Just enough FlatBuffers to walk Trinity's tables."""

    def __init__(self, buf, base=0):
        self.b, self.base = buf, base

    def u32(self, p): return struct.unpack_from('<I', self.b, p)[0]
    def root(self): return self.base+self.u32(self.base)

    def field(self, table, i):
        vt = table-struct.unpack_from('<i', self.b, table)[0]
        vsize = struct.unpack_from('<H', self.b, vt)[0]
        if 4+2*i >= vsize:
            return None
        off = struct.unpack_from('<H', self.b, vt+4+2*i)[0]
        return table+off if off else None

    def ref(self, p): return p+self.u32(p)

    def vector(self, table, i):
        p = self.field(table, i)
        if p is None:
            return 0, 0
        v = self.ref(p)
        return self.u32(v), v+4


class RomFS:
    def __init__(self, root=ROMFS):
        index = json.loads((Path(root)/'index.json').read_text())
        if index.get('status') != 'pass':
            raise ValueError('RomFS has not passed integrity verification')
        self.entries = {r['path']: r for r in index['files']}
        self.stream = (Path(root)/'romfs.bin').open('rb')

    def read(self, path, offset, size):
        row = self.entries[path]
        if offset < 0 or offset+size > row['size']:
            raise ValueError('read outside %s' % path)
        self.stream.seek(row['offset']+offset)
        data = self.stream.read(size)
        if len(data) != size:
            raise ValueError('short RomFS read')
        return data


def packs(fs):
    """(index, offset, size) of every pack in data.trpfs."""
    path = 'arc/data.trpfs'
    head = fs.read(path, 0, 16)
    if head[:8] != b'ONEPACK\0':
        raise ValueError('data.trpfs is not a ONEPACK')
    table, = struct.unpack_from('<Q', head, 8)
    total = fs.entries[path]['size']
    raw = fs.read(path, table, total-table)
    fb = Flat(raw)
    root = fb.root()
    n, at = fb.vector(root, 1)
    offsets = list(struct.unpack_from('<%dQ' % n, raw, at))
    ends = sorted(offsets)+[table]
    following = {o: ends[k+1] for k, o in enumerate(sorted(offsets))}
    return [(i, o, following[o]-o) for i, o in enumerate(offsets)]


def pack_files(blob):
    """(file hash, compression, decompressed size, data) for each file of a pack."""
    fb = Flat(blob)
    root = fb.root()
    nh, hat = fb.vector(root, 0)
    hashes = struct.unpack_from('<%dQ' % nh, blob, hat) if nh else ()
    nf, fat = fb.vector(root, 1)
    for k in range(nf):
        t = fb.ref(fat+4*k)
        comp = fb.field(t, 1)
        size = fb.field(t, 3)
        n, dat = fb.vector(t, 4)
        yield (hashes[k] if k < nh else 0, struct.unpack_from('<b', blob, comp)[0] if comp else 0,
               struct.unpack_from('<Q', blob, size)[0] if size else n, blob[dat:dat+n])


STAGE_NAMES = {0: 'vs', 1: 'ps', 5: 'cs'}


def bnsh_shaders(data):
    """(stage, control, code) for each shader of each BNSH in a decompressed Trinity file. A BNSH's shader info
    records hold {control offset (u64), code offset (u64), code size (u32)}, offsets from the BNSH header; the code
    size equals the control section's own (+0x704), which identifies the records. Graphics code starts with the NVN
    header (0x12345678); the device hashes control and code from that pointer."""
    found, seen = [], set()
    b0 = data.find(b'BNSH')
    while b0 >= 0:
        nxt = data.find(b'BNSH', b0+4)
        B = memoryview(data)[b0:nxt if nxt > 0 else len(data)]
        n = len(B)
        for i in range(0, n-24, 8):
            c, k = struct.unpack_from('<QQ', B, i)
            if not (0 < c <= n-0x880 and 0 < k < n and c & 3 == 0):
                continue
            size, = struct.unpack_from('<I', B, c+0x704)
            if not size or size != struct.unpack_from('<I', B, i+16)[0] or k+size > n:
                continue
            stage, = struct.unpack_from('<I', B, c+0x714)
            if stage not in STAGE_NAMES or (b0+c, b0+k) in seen:
                continue
            if stage != 5 and bytes(B[k:k+4]) != lisde_shaders.NVN_MAGIC:
                continue
            seen.add((b0+c, b0+k))
            found.append((STAGE_NAMES[stage], bytes(B[c:c+0x880]), bytes(B[k:k+size])))
        b0 = nxt
    return found


def inventory_files(directory):
    """inventory() from the decompressed files a dump left in directory (<pack>_<hash>.bin)."""
    shaders = {}
    for f in sorted(Path(directory).glob('*.bin')):
        for stage, control, code in bnsh_shaders(f.read_bytes()):
            shaders.setdefault(hashlib.sha256(control+code).hexdigest(), (stage, control, code, f.stem))
    return shaders


def inventory(root=ROMFS, limit_packs=0, progress=True):
    """{key: (stage, control, code, source)} for every shader of the game."""
    fs = RomFS(root)
    oodle = None
    shaders, files, started = {}, 0, time.time()
    plist = packs(fs)
    if limit_packs:
        plist = plist[:limit_packs]
    for i, off, size in plist:
        blob = fs.read('arc/data.trpfs', off, size)
        if b'BNSH' not in blob:
            continue
        for fhash, comp, raw_len, data in pack_files(blob):
            if b'BNSH' not in data:
                continue
            if comp != -1 and raw_len != len(data):
                if oodle is None:
                    oodle = Oodle()
                data = oodle.decompress(data, raw_len)
            files += 1
            for stage, control, code in bnsh_shaders(data):
                shaders.setdefault(hashlib.sha256(control+code).hexdigest(),
                                   (stage, control, code, 'pack%d:%016x' % (i, fhash)))
        if progress and i % 500 == 0:
            print('pack %d/%d: %d shader files, %d shaders, %.0f s' % (i, len(plist), files, len(shaders),
                  time.time()-started), flush=True)
    return shaders


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--romfs', type=Path, default=ROMFS)
    ap.add_argument('--dump', type=Path)
    ap.add_argument('--limit-packs', type=int, default=0)
    a = ap.parse_args()
    shaders = inventory(a.romfs, a.limit_packs)
    counts = {}
    for stage, *_ in shaders.values():
        counts[stage] = counts.get(stage, 0)+1
    print('%d unique shaders: %s' % (len(shaders), counts))
    if a.dump:
        a.dump.mkdir(parents=True, exist_ok=True)
        for key, (stage, control, code, _) in shaders.items():
            (a.dump/('%s.%s.control' % (key, stage))).write_bytes(control)
            (a.dump/('%s.%s.code' % (key, stage))).write_bytes(code)


if __name__ == '__main__':
    main()
