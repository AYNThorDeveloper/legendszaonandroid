#!/usr/bin/env python3
"""Every NVN shader of Life is Strange: Double Exposure, read from the verified RomFS.

Unreal keeps the shaders in IoStore containers (Chronos/Content/Paks/pakchunk*-Switch.utoc/.ucas): one
ShaderCodeLibrary chunk per container (FIoStoreShaderCodeArchive: shader hashes, groups, offsets) and one
ShaderCode chunk per shader group. Container blocks and shader groups are both Oodle-compressed. The engine's
global shaders are also in GlobalShaderCache-*.bin in pakchunk0-Switch.pak (pak v11, Oodle blocks).

Oodle is not reimplemented: OodleLZ_Decompress of the game's own main module (main+0x3cab10, found through its
error strings) runs under unicorn, with the module's RELA and RELR relocations applied and memcpy/memmove/memset
supplied as host stubs.

Each Unreal shader blob holds the GLSLC code area (graphics: starting with the 0x30-byte NVN header, magic
0x12345678) followed by 4 bytes and the 0x880-byte NVN control section (code size at +0x704, stage at +0x714).
The shader key is SHA-256(control + code), as nvn_vk_shader.c computes it on the device.

usage: lisde_shaders.py [--romfs DIR] [--dump DIR]   (prints counts; --dump writes <key>.<stage>.control/.code)
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

HERE = Path(__file__).resolve().parent
PORT = HERE.parents[2]
ROMFS = PORT/'Uitgepakt/romfs'
MAIN_ELF = PORT/'Uitgepakt/elf/main.elf'
OODLE_DECOMPRESS = 0x3cab10   # OodleLZ_Decompress in main (Double Exposure 1.0.0)
NVN_MAGIC = bytes.fromhex('78563412')
STAGES = {0: 'vs', 1: 'ps', 5: 'cs'}


class Oodle:
    """OodleLZ_Decompress of the game module under unicorn."""
    BASE, STUB, STACK, HEAP, RET = 0x10000000, 0x7100000, 0x8000000, 0x9000000, 0x7000000
    STACK_SIZE, HEAP_SIZE, DECODER = 0x100000, 0x4000000, 0x800000

    def __init__(self, elf=MAIN_ELF):
        from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
        from unicorn import arm64_const as a
        self.a = a
        data = Path(elf).read_bytes()
        phoff, = struct.unpack_from('<Q', data, 0x20)
        phnum, = struct.unpack_from('<H', data, 0x38)
        phdrs = [struct.unpack_from('<IIQQQQQQ', data, phoff+56*i) for i in range(phnum)]
        loads = [p for p in phdrs if p[0] == 1]
        dynamic = next(p for p in phdrs if p[0] == 2)

        def file_offset(v):
            for _, _, off, va, _, fs, _, _ in loads:
                if va <= v < va+fs:
                    return off+v-va
            raise ValueError('address %#x outside the module' % v)
        u = self.u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        end = max((va+ms+0xfff) & ~0xfff for _, _, _, va, _, _, ms, _ in loads)
        u.mem_map(self.BASE, end)
        for _, _, off, va, _, fs, _, _ in loads:
            u.mem_write(self.BASE+va, data[off:off+fs])
        tags = {}
        for k in range(0, dynamic[5], 16):
            tag, value = struct.unpack_from('<qQ', data, dynamic[2]+k)
            if tag == 0:
                break
            tags.setdefault(tag, value)
        symtab, strtab, syment = file_offset(tags[6]), file_offset(tags[5]), tags.get(11, 24)
        self.stubs = {}

        def symbol(i):
            name, _, _, shndx, value, _ = struct.unpack_from('<IBBHQQ', data, symtab+syment*i)
            if shndx:
                return self.BASE+value
            address = self.STUB+8*i   # an import: a stub the code hook answers
            self.stubs[address] = data[strtab+name:data.index(b'\0', strtab+name)].decode()
            return address
        tables = [(file_offset(tags[7]), tags[8])] + ([(file_offset(tags[23]), tags[2])] if 23 in tags else [])
        for table, size in tables:
            for k in range(0, size, 24):
                where, info, addend = struct.unpack_from('<QQq', data, table+k)
                kind = info & 0xffffffff
                if kind == 1027:   # R_AARCH64_RELATIVE
                    u.mem_write(self.BASE+where, struct.pack('<Q', self.BASE+addend))
                elif kind in (257, 1025, 1026):   # ABS64, GLOB_DAT, JUMP_SLOT
                    u.mem_write(self.BASE+where, struct.pack('<Q', symbol(info >> 32)+addend))
        if 36 in tags:   # DT_RELR
            def rebase(where):
                value, = struct.unpack('<Q', bytes(u.mem_read(self.BASE+where, 8)))
                u.mem_write(self.BASE+where, struct.pack('<Q', value+self.BASE))
            table, where = file_offset(tags[36]), 0
            for k in range(0, tags[35], 8):
                entry, = struct.unpack_from('<Q', data, table+k)
                if not entry & 1:
                    rebase(entry)
                    where = entry+8
                else:
                    for bit in range(63):
                        if entry >> (bit+1) & 1:
                            rebase(where+8*bit)
                    where += 63*8
        u.mem_map(self.STUB, 0x100000)
        u.mem_write(self.STUB, b'\xc0\x03\x5f\xd6'*(0x100000//4))   # ret
        u.hook_add(UC_HOOK_CODE, self._stub, begin=self.STUB, end=self.STUB+0x100000)
        u.mem_map(self.STACK, self.STACK_SIZE)
        u.mem_map(self.HEAP, self.HEAP_SIZE)
        u.mem_map(self.RET, 0x1000)
        u.mem_map(0x6000000, 0x10000)   # thread-local area
        u.reg_write(a.UC_ARM64_REG_TPIDRRO_EL0, 0x6000000)
        u.reg_write(a.UC_ARM64_REG_TPIDR_EL0, 0x6000000)
        u.reg_write(a.UC_ARM64_REG_CPACR_EL1, 0x300000)   # FP/SIMD on

    def _stub(self, u, address, size, _):
        name = self.stubs.get(address)
        dst, src, n = (u.reg_read(self.a.UC_ARM64_REG_X0+i) for i in range(3))
        if name in ('memcpy', 'memmove'):
            u.mem_write(dst, bytes(u.mem_read(src, n)))
        elif name == 'memset':
            u.mem_write(dst, bytes([src & 0xff])*n)
        else:
            raise RuntimeError('Oodle called the import %s' % name)

    def decompress(self, comp, raw_len):
        a, u = self.a, self.u
        decoder, inp = self.HEAP, self.HEAP+self.DECODER
        out = inp+((len(comp)+0xfff) & ~0xfff)
        if out+raw_len > self.HEAP+self.HEAP_SIZE:
            raise ValueError('Oodle block of %d bytes exceeds the emulator heap' % raw_len)
        u.mem_write(inp, comp)
        sp = self.STACK+self.STACK_SIZE-0x1000
        # stack arguments: decBufSize, fpCallback, callbackUserData, decoderMemory, decoderMemorySize, threadPhase
        u.mem_write(sp, struct.pack('<6Q', 0, 0, 0, decoder, self.DECODER, 3))
        # compBuf, compBufSize, rawBuf, rawLen, fuzzSafe, checkCRC, verbosity, decBufBase
        for i, v in enumerate((inp, len(comp), out, raw_len, 1, 0, 0, 0)):
            u.reg_write(a.UC_ARM64_REG_X0+i, v)
        u.reg_write(a.UC_ARM64_REG_SP, sp)
        u.reg_write(a.UC_ARM64_REG_LR, self.RET)
        u.emu_start(self.BASE+OODLE_DECOMPRESS, self.RET)
        n = u.reg_read(a.UC_ARM64_REG_X0)
        if n != raw_len:
            raise ValueError('OodleLZ_Decompress returned %d, expected %d' % (n, raw_len))
        return bytes(u.mem_read(out, raw_len))


class RomFS:
    def __init__(self, root=ROMFS):
        index = json.loads((Path(root)/'index.json').read_text())
        if index.get('status') != 'pass':
            raise ValueError('RomFS has not passed integrity verification')
        self.entries = {r['path']: r for r in index['files']}
        self.stream = (Path(root)/'romfs.bin').open('rb')
        self._oodle = None

    def read(self, path, offset=0, size=None):
        row = self.entries[path]
        size = row['size']-offset if size is None else size
        if offset < 0 or offset+size > row['size']:
            raise ValueError('read outside %s' % path)
        self.stream.seek(row['offset']+offset)
        data = self.stream.read(size)
        if len(data) != size:
            raise ValueError('short RomFS read')
        return data

    def oodle(self, comp, raw_len):
        if self._oodle is None:
            self._oodle = Oodle()
        return self._oodle.decompress(comp, raw_len)


def be40(b):
    return int.from_bytes(b, 'big')


class IoStore:
    """An IoStore container (.utoc version 6, one partition)."""
    def __init__(self, fs, name):
        self.fs, self.ucas = fs, 'Chronos/Content/Paks/%s.ucas' % name
        t = fs.read('Chronos/Content/Paks/%s.utoc' % name)
        if t[:16] != b'-==--==--==--==-' or t[16] != 6:
            raise ValueError('%s: unsupported IoStore TOC' % name)
        header, n, nblocks, blocksize, nmethods, methodlen, self.cbs = struct.unpack_from('<7I', t, 20)
        partitions, = struct.unpack_from('<I', t, 52)
        seeds, = struct.unpack_from('<I', t, 84)
        no_hash, = struct.unpack_from('<I', t, 96)
        if blocksize != 12 or partitions != 1:
            raise ValueError('%s: unexpected TOC layout' % name)
        p = header
        self.ids = [t[p+12*i:p+12*i+12] for i in range(n)]
        p += 12*n
        self.spans = [(be40(t[p+10*i:p+10*i+5]), be40(t[p+10*i+5:p+10*i+10])) for i in range(n)]
        p += 10*n+4*seeds+4*no_hash
        self.blocks = []
        for i in range(nblocks):
            b = t[p+12*i:p+12*i+12]
            self.blocks.append((int.from_bytes(b[0:5], 'little'), int.from_bytes(b[5:8], 'little'),
                                int.from_bytes(b[8:11], 'little'), b[11]))
        p += 12*nblocks
        methods = [t[p+methodlen*i:p+methodlen*(i+1)].rstrip(b'\0').decode() for i in range(nmethods)]
        if methods not in ([], ['Oodle']):
            raise ValueError('%s: compression %s' % (name, methods))
        self.by_id = {cid: i for i, cid in enumerate(self.ids)}

    def chunks(self, kind):
        return [i for i, cid in enumerate(self.ids) if cid[11] == kind]

    def read(self, i):
        offset, length = self.spans[i]
        out = bytearray()
        for b in range(offset//self.cbs, (offset+length-1)//self.cbs+1):
            at, csize, usize, method = self.blocks[b]
            raw = self.fs.read(self.ucas, at, csize)
            out += raw[:usize] if method == 0 else self.fs.oodle(raw, usize)
        start = offset % self.cbs
        return bytes(out[start:start+length])


SHADER_CODE_LIBRARY, SHADER_CODE = 8, 9


def library_shaders(store):
    """(unreal hash, blob) of every shader in the container's FIoStoreShaderCodeArchive(s)."""
    def array(b, p, size):
        n, = struct.unpack_from('<i', b, p)
        return [b[p+4+size*i:p+4+size*(i+1)] for i in range(n)], p+4+size*n
    for li in store.chunks(SHADER_CODE_LIBRARY):
        b = store.read(li)
        if struct.unpack_from('<I', b, 0)[0] != 1:
            raise ValueError('unknown shader archive version')
        p = 4
        _, p = array(b, p, 20)                  # shader map hashes
        hashes, p = array(b, p, 20)
        group_ids, p = array(b, p, 12)
        _, p = array(b, p, 8)                   # shader map entries
        entries, p = array(b, p, 8)
        groups, p = array(b, p, 16)
        indices, p = array(b, p, 4)
        entries = [struct.unpack('<Q', e)[0] >> 34 for e in entries]   # uncompressed offset in the group
        indices = [struct.unpack('<I', x)[0] for x in indices]
        for gi, g in enumerate(groups):
            first, count, usize, csize = struct.unpack('<4I', g)
            raw = store.read(store.by_id[group_ids[gi]])
            if len(raw) != csize:
                raise ValueError('shader group size mismatch')
            if csize < usize:
                raw = store.fs.oodle(raw, usize)
            members = sorted(indices[first:first+count], key=lambda s: entries[s])
            for k, s in enumerate(members):
                end = entries[members[k+1]] if k+1 < len(members) else usize
                yield hashes[s].hex(), raw[entries[s]:end]


def nvn_shaders(blob):
    """(stage, control, code) for each NVN control section in an Unreal shader blob or a shader cache file.
    Graphics code starts with the NVN magic and its control section follows 4 bytes after the code; compute code
    has no header, so its control section is found by the compute stage word and a plausible thread group."""
    found, seen = [], set()

    def take(p, q, size):
        if p not in seen:
            seen.add(p)
            found.append((STAGES[struct.unpack_from('<I', blob, p+0x714)[0]], blob[p:p+0x880], blob[q:q+size]))
    q = blob.find(NVN_MAGIC)
    while q >= 0:
        for p in range(q+0x34, len(blob)-0x880+1, 4):
            c1, size = struct.unpack_from('<II', blob, p+0x700)
            stage, = struct.unpack_from('<I', blob, p+0x714)
            if size == p-4-q and stage in (0, 1) and 0x30 <= c1 <= size:
                take(p, q, size)
                break
        q = blob.find(NVN_MAGIC, q+4)
    word = struct.pack('<I', 5)
    at = blob.find(word, 0x714)
    while at >= 0:
        p = at-0x714
        if p+0x880 <= len(blob):
            c1, size = struct.unpack_from('<II', blob, p+0x700)
            local = struct.unpack_from('<3I', blob, p+0x748)
            if 0 < size <= p-4 and c1 <= size and all(local) and local[0]*local[1]*local[2] <= 1024:
                take(p, p-4-size, size)
        at = blob.find(word, at+1)
    return found


def pak_file(fs, pak, wanted):
    """One file of a pak (v11, unencrypted index, Oodle blocks)."""
    row = fs.entries[pak]
    footer = fs.read(pak, row['size']-0x100)
    m = footer.rfind(struct.pack('<I', 0x5A6F12E1))
    if m < 17 or footer[m-1] != 0:
        raise ValueError('pak footer not found or index encrypted')
    version, index_at, index_size = struct.unpack_from('<IQQ', footer, m+4)
    if version != 11:
        raise ValueError('pak version %d' % version)
    names = footer[m+44:].split(b'\0')
    methods = [n.decode() for n in names if n]
    index, p = fs.read(pak, index_at, index_size), 0

    def fstring():
        nonlocal p
        n, = struct.unpack_from('<i', index, p)
        p += 4
        if n < 0:
            s = index[p:p-2*n].decode('utf-16-le')[:-1]
            p -= 2*n
        else:
            s = index[p:p+n-1].decode()
            p += n
        return s
    mount = fstring()
    p += 4+8                                     # entry count, path hash seed
    if struct.unpack_from('<i', index, p)[0]:
        p += 4+8+8+20
    else:
        p += 4
    if not struct.unpack_from('<i', index, p)[0]:
        raise ValueError('pak without full directory index')
    dir_at, dir_size = struct.unpack_from('<qq', index, p+4)
    p += 4+8+8+20
    encoded_size, = struct.unpack_from('<i', index, p)
    encoded = index[p+4:p+4+encoded_size]
    d = fs.read(pak, dir_at, dir_size)

    def dstring():
        nonlocal q
        n, = struct.unpack_from('<i', d, q)
        s = d[q+4:q+4+n-1].decode()
        q += 4+n
        return s
    directories, = struct.unpack_from('<i', d, 0)
    q = 4
    for _ in range(directories):
        directory = dstring()
        count, = struct.unpack_from('<i', d, q)
        q += 4
        for _ in range(count):
            name = dstring()
            location, = struct.unpack_from('<i', d, q)
            q += 4
            if (mount+directory+name).endswith(wanted):
                return read_pak_entry(fs, pak, encoded, location, methods)
    raise KeyError(wanted)


def read_pak_entry(fs, pak, encoded, location, methods):
    flags, = struct.unpack_from('<I', encoded, location)
    p = location+4
    block = flags & 0x3f
    if block == 0x3f:
        block, = struct.unpack_from('<I', encoded, p)
        p += 4
    else:
        block <<= 11
    nblocks, encrypted, method = (flags >> 6) & 0xffff, (flags >> 22) & 1, (flags >> 23) & 0x3f
    if encrypted:
        raise ValueError('encrypted pak entry')

    def number(small):
        nonlocal p
        v, = struct.unpack_from('<I' if small else '<Q', encoded, p)
        p += 4 if small else 8
        return v
    offset, usize = number(flags >> 31 & 1), number(flags >> 30 & 1)
    size = number(flags >> 29 & 1) if method else usize
    if not method:
        return fs.read(pak, offset+53, usize)
    sizes = [struct.unpack_from('<I', encoded, p+4*i)[0] for i in range(nblocks)] if nblocks > 1 else [size]
    if methods[method-1] != 'Oodle':
        raise ValueError('pak compression %s' % methods[method-1])
    at = offset+(8+8+8+4+20)+(4+16*nblocks)+1+4   # past the serialized FPakEntry that precedes the data
    out, left = bytearray(), usize
    for s in sizes:
        out += fs.oodle(fs.read(pak, at, s), min(block, left))
        left -= min(block, left)
        at += s
    return bytes(out)


def inventory(root=ROMFS):
    """{key: (stage, control, code, source)} for every shader of the game."""
    fs = RomFS(root)
    shaders = {}

    def add(blob, source):
        for stage, control, code in nvn_shaders(blob):
            shaders.setdefault(hashlib.sha256(control+code).hexdigest(), (stage, control, code, source))
    for name in sorted(p[len('Chronos/Content/Paks/'):-5] for p in fs.entries if p.endswith('.utoc')):
        store = IoStore(fs, name)
        for unreal_hash, blob in library_shaders(store):
            add(blob, '%s:%s' % (name, unreal_hash))
    add(pak_file(fs, 'Chronos/Content/Paks/pakchunk0-Switch.pak', 'GlobalShaderCache-SP_GLSL_SWITCH_MOBILE.bin'),
        'pakchunk0-Switch.pak:GlobalShaderCache')
    return shaders


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--romfs', type=Path, default=ROMFS)
    ap.add_argument('--dump', type=Path)
    a = ap.parse_args()
    shaders = inventory(a.romfs)
    counts = {}
    for stage, _, _, _ in shaders.values():
        counts[stage] = counts.get(stage, 0)+1
    print('%d unique shaders: %s' % (len(shaders), counts))
    if a.dump:
        a.dump.mkdir(parents=True, exist_ok=True)
        for key, (stage, control, code, _) in shaders.items():
            (a.dump/('%s.%s.control' % (key, stage))).write_bytes(control)
            (a.dump/('%s.%s.code' % (key, stage))).write_bytes(code)


if __name__ == '__main__':
    main()
