#!/usr/bin/env python3
"""Shader translation bridge for the Android game loader (development).

The Thor sends one request per line over TCP (adb reverse tcp:27760):
  {"variant", "stage": "vs|ps|cs", "code_hex", "c1", "tex", "img", "attr",
   "fout", "local": [x, y, z] (compute), "depth_minus_w": bool}
The bridge runs the macOS port's prebuilt translator (totk_xlate --serve, the
vendored Ryujinx shader translator) and replies with Vulkan SPIR-V plus a
binding table derived from the SPIR-V resource names by the same rules as
games/totk/tools/totk_xlate/Service.cs (Reflect / DecodeTex):

  OK <spv_bytes> <nbind> <ninput> <noutput> <lx> <ly> <lz>
  B <set> <binding> <kind> <reg> <sreg> <off> <rw> <descriptor_type> <count> <view>
  I <location> <float|sint|uint>      (stage inputs: vertex attributes / fragment varyings)
  O <location> <float|sint|uint>      (stage outputs: vertex varyings / fragment colours)
  <spv bytes>
or
  ERR <message>

kind uses nvn_refl.h numbering (CBUF 1, TEX 3, IMG 4, TBUF 5, SBUFP 6,
SUPPORT 7, C1 8, DRV0 9, DRV2 10, UBOP 12, SBUFU 13). For a combined image
sampler, sreg is the separate sampler slot, or -1 when the sampler comes from
the combined texture handle. view is the VkImageViewType the shader declares
(+256 when multisampled, -1 for non-image resources). Descriptor sets are rewritten so the vertex stage
uses set 0, the fragment stage set 1 and compute set 0.

A request {"variant": V, "strip_inputs": MASK} returns variant V (already
translated) with the fragment inputs at the MASK locations replaced by their
Maxwell default (0, 0, 0, 1 for float vec4, else zero): the Adreno driver
refuses fragment inputs the vertex stage does not write, which NVN allows.

Vertex shaders start by storing (0, 0, 0, 1) to every float vec4 output: on
Maxwell the components a vertex shader never writes read as (0, 0, 0, 1), in
Vulkan they are undefined (zero on Adreno). TOTK multiplies such a w into
alphas and matrices (MacNativeGames PR #91, translator patch 0005).

usage: shader_bridge.py [--port 27760] [--work DIR]
"""
import argparse
import os
import re
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
XLATE = os.path.join(REPO, 'games', 'totk', 'prebuilt', 'macos-arm64', 'totk_xlate')
# Bumped whenever the reply format or the SPIR-V rewriting changes: cached replies of older versions are not reused.
# 3: vertex outputs start at (0, 0, 0, 1); version-2 replies are upgraded without translating again.
# 4: transform-feedback decorations must validate before they can be cached or sent to the device.
REPLY_VERSION = 5

TEX_BASE, SAMP_BASE, IMG_BASE = 0x168, 0x568, 0x120
COMBINED_BASE, COMBINED_SLOTS = 0x20, 32
K_CBUF, K_TEX, K_IMG, K_TBUF, K_SBUFP, K_SUPPORT, K_C1, K_DRV0, K_DRV2, K_UBOP, K_SBUFU = 1, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13
VK_SAMPLER, VK_COMBINED, VK_SAMPLED, VK_STORAGE_IMAGE, VK_UNIFORM_TEXEL, VK_STORAGE_TEXEL, VK_UBO, VK_SSBO = 0, 1, 2, 3, 4, 5, 6, 7


def slot(word, base, cb):
    if cb == 2 and word * 4 >= base and (word * 4 - base) % 8 == 0:
        return (word * 4 - base) // 8
    return None


def decode_tex(name):
    """Service.cs DecodeTex: (texture slot, sampler slot, handle type) from a resource name."""
    m = re.match(r'^[a-z]+_[ti]\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)', name)
    if not m:
        return None
    slots = int(m.group(1)) if m.group(1) else -1
    h = int(m.group(2), 16)
    tb = (slots & 0xffff) if slots >= 0 else 2
    sb = (slots >> 16) - 1 if slots >= 0 and (slots >> 16) else tb
    o0, o1, ty = h & 0x3fff, (h >> 14) & 0x3fff, h >> 28
    words = [o0, o1] if ty else [o0]
    if re.match(r'^[a-z]+_i', name):
        for x in words:
            if IMG_BASE <= x * 4 < TEX_BASE and slot(x, IMG_BASE, tb) is not None:
                return slot(x, IMG_BASE, tb), None, ty
        return None, None, ty
    t = next((slot(x, TEX_BASE, tb) for x in words if slot(x, TEX_BASE, tb) is not None and x * 4 < SAMP_BASE), None)
    if t is None:
        t = next((slot(x, COMBINED_BASE, tb) for x in words
                  if COMBINED_BASE <= x * 4 < COMBINED_BASE + 8 * COMBINED_SLOTS and slot(x, COMBINED_BASE, tb) is not None), None)
    s = next((slot(x, SAMP_BASE, sb) for x in words if x * 4 >= SAMP_BASE and slot(x, SAMP_BASE, sb) is not None), None)
    return t, s, ty


class Spirv:
    """Just enough SPIR-V parsing for resource and interface reflection."""

    def __init__(self, data):
        if len(data) < 20 or len(data) % 4:
            raise ValueError('not SPIR-V')
        self.words = list(struct.unpack('<%dI' % (len(data) // 4), data))
        if self.words[0] != 0x07230203:
            raise ValueError('bad SPIR-V magic')
        self.names, self.deco, self.types, self.vars, self.consts, self.caps = {}, {}, {}, {}, {}, set()
        self.deco_at = []   # (word index of the operand, id, decoration) for rewriting sets / bindings
        i = 5
        while i < len(self.words):
            op, n = self.words[i] & 0xffff, self.words[i] >> 16
            w = self.words[i:i + n]
            if n == 0:
                raise ValueError('zero-length SPIR-V instruction')
            if op == 17:
                self.caps.add(w[1])
            elif op == 5:
                self.names[w[1]] = self._string(w[2:])
            elif op == 71:
                self.deco.setdefault(w[1], {})[w[2]] = w[3] if n > 3 else True
                if w[2] in (33, 34):
                    self.deco_at.append((i + 3, w[1], w[2]))
            elif op in (19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 32):
                self.types[w[1]] = (op, w[2:])
            elif op == 43:
                self.consts[w[2]] = w[3]
            elif op == 59:
                self.vars[w[2]] = (w[1], w[3])
            i += n

    @staticmethod
    def _string(ws):
        b = struct.pack('<%dI' % len(ws), *ws)
        return b.split(b'\0', 1)[0].decode('utf-8', 'replace')

    def pointee(self, var):
        ptr_type, storage = self.vars[var]
        op, ops = self.types[ptr_type]
        return storage, ops[1]

    def view_type(self, image_ops):
        dim, arrayed, ms = image_ops[1], image_ops[3], image_ops[4]
        base = {0: (0, 4), 1: (1, 5), 2: (2, 2), 3: (3, 6), 4: (1, 5), 6: (1, 5)}.get(dim)
        if base is None:
            return -1
        return base[1 if arrayed else 0] + (256 if ms else 0)

    def descriptor(self, var):
        """(descriptor type, count, view type) of a resource variable."""
        storage, t = self.pointee(var)
        count = 1
        while self.types[t][0] in (28, 29):   # (runtime) arrays of descriptors
            op, ops = self.types[t]
            count *= self.consts.get(ops[1], 1) if op == 28 else 1
            t = ops[0]
        op, ops = self.types[t]
        if op == 27:
            image = self.types[ops[0]][1]
            return (VK_UNIFORM_TEXEL if image[1] == 5 else VK_COMBINED), count, self.view_type(image)
        if op == 26:
            return VK_SAMPLER, count, -1
        if op == 25:
            dim, sampled = ops[1], ops[5]
            if dim == 5:
                return (VK_STORAGE_TEXEL if sampled == 2 else VK_UNIFORM_TEXEL), count, -1
            return (VK_STORAGE_IMAGE if sampled == 2 else VK_SAMPLED), count, self.view_type(ops)
        if op == 30:
            if storage == 12 or 3 in self.deco.get(t, {}):
                return VK_SSBO, count, -1
            return VK_UBO, count, -1
        raise ValueError('unsupported resource type %d for %s' % (op, self.names.get(var, var)))

    def scalar_class(self, t):
        while self.types[t][0] in (23, 28):   # vectors, arrays
            t = self.types[t][1][0]
        op, ops = self.types[t]
        if op == 21:
            return 'sint' if ops[1] else 'uint'
        return 'float'

    def interface(self, storage_class):
        out = []
        for var, (ptr, storage) in self.vars.items():
            d = self.deco.get(var, {})
            if storage != storage_class or 30 not in d or 11 in d:   # Location set, not a BuiltIn
                continue
            out.append((d[30], self.scalar_class(self.types[ptr][1][1])))
        return sorted(out)

    def strip_interlock(self):
        """Removes SPV_EXT_fragment_shader_interlock (absent from the Adreno driver):
        the critical section runs without per-pixel ordering. Returns True if removed."""
        out, i, removed = self.words[:5], 5, False
        while i < len(self.words):
            op, n = self.words[i] & 0xffff, self.words[i] >> 16
            w = self.words[i:i + n]
            drop = (op == 17 and w[1] in (5363, 5372, 5378)) or op in (5364, 5365) \
                or (op == 16 and w[2] in (5366, 5367, 5368, 5369, 5370, 5371)) \
                or (op == 10 and self._string(w[1:]) == 'SPV_EXT_fragment_shader_interlock')
            if drop:
                removed = True
            else:
                out += w
            i += n
        self.words = out
        self.deco_at = []
        return removed

    def zero_inputs(self, mask):
        """Turns Input variables at the given locations into zero-initialized
        Private variables (SPIR-V 1.3: removed from the entry point interface).
        Access chains into them get Private pointer types."""
        targets = {v for v, (ptr, st) in self.vars.items() if st == 1 and self.deco.get(v, {}).get(30, -1) >= 0
                   and 11 not in self.deco.get(v, {}) and (mask >> self.deco[v][30]) & 1}
        if not targets:
            return
        # pass 1: ids derived from the targets and the pointee types needing Private pointers
        derived, pointees = set(targets), {self.types[self.vars[v][0]][1][1] for v in targets}
        i = 5
        while i < len(self.words):
            op, n = self.words[i] & 0xffff, self.words[i] >> 16
            if op in (65, 66) and self.words[i + 3] in derived:
                derived.add(self.words[i + 2])
                pointees.add(self.types[self.words[i + 1]][1][1])
            i += n
        bound = self.words[3]
        new_ptr = {}
        for t in sorted(pointees):
            new_ptr[t] = bound
            bound += 1
        nulls, decls = {}, []
        for t, pid in new_ptr.items():
            decls += [(4 << 16) | 32, pid, 6, t]
        for v in sorted(targets):   # id order: the device's C port (nvn_vk_shader.c) emits the same words
            t = self.types[self.vars[v][0]][1][1]
            if t in nulls:
                continue
            if self.vec4_float(t):   # Maxwell default of an unwritten attribute
                d, nulls[t], bound = self.default_vec4(t, bound)
                decls += d
            else:
                nulls[t] = bound
                decls += [(3 << 16) | 46, t, bound]
                bound += 1
        # pass 2: rewrite
        out, i, inserted = self.words[:5], 5, False
        while i < len(self.words):
            op, n = self.words[i] & 0xffff, self.words[i] >> 16
            w = list(self.words[i:i + n])
            if op == 15:   # OpEntryPoint: drop the targets from the interface
                k = 3
                while True:   # the name is a nul-terminated string
                    word = w[k]
                    k += 1
                    if any(((word >> sh) & 0xff) == 0 for sh in (0, 8, 16, 24)):
                        break
                w = w[:k] + [x for x in w[k:] if x not in targets]
                w[0] = (len(w) << 16) | op
            elif op == 71 and w[1] in targets:
                i += n
                continue
            elif op == 59 and w[2] in targets:
                if not inserted:
                    out += decls
                    inserted = True
                t = self.types[w[1]][1][1]
                w = [(5 << 16) | 59, new_ptr[t], w[2], 6, nulls[t]]
            elif op in (65, 66) and w[3] in derived:
                w[1] = new_ptr[self.types[w[1]][1][1]]
            out += w
            i += n
        out[3] = bound
        self.words = out
        self.deco_at = []

    def vec4_float(self, t):
        """True for a 32-bit float vec4 type id."""
        op, ops = self.types.get(t, (0, []))
        if op != 23 or ops[1] != 4:
            return False
        e = self.types.get(ops[0], (0, []))
        return e[0] == 22 and e[1][0] == 32

    def default_vec4(self, t, bound):
        """Declarations of (0, 0, 0, 1) of vec4 type t: (words, id, new bound)."""
        f = self.types[t][1][0]
        zero, one, vec = bound, bound + 1, bound + 2
        return [(4 << 16) | 43, f, zero, 0, (4 << 16) | 43, f, one, 0x3f800000,
                (7 << 16) | 44, t, vec, zero, zero, zero, one], vec, bound + 3

    def init_outputs(self):
        """Vertex stage: stores (0, 0, 0, 1) to every float vec4 user output at the start of the entry point.
        The shader's own stores follow, so only components it never writes keep the default. Returns the count."""
        outs = [v for v, (ptr, st) in self.vars.items() if st == 3 and 30 in self.deco.get(v, {})
                and 11 not in self.deco.get(v, {}) and self.vec4_float(self.types[ptr][1][1])]
        entry = None
        i = 5
        while i < len(self.words):
            op, n = self.words[i] & 0xffff, self.words[i] >> 16
            if op == 15:
                entry = self.words[i + 2]
                break
            i += n
        if not outs or entry is None:
            return 0
        decls, consts, bound = [], {}, self.words[3]
        for v in outs:
            t = self.types[self.vars[v][0]][1][1]
            if t not in consts:
                d, consts[t], bound = self.default_vec4(t, bound)
                decls += d
        out, i, state = self.words[:5], 5, 0   # 0: globals, 1: in the entry function, 2: done
        while i < len(self.words):
            op, n = self.words[i] & 0xffff, self.words[i] >> 16
            w = self.words[i:i + n]
            if op == 54 and decls:   # first function: the constants go just before it
                out += decls
                decls = []
            if state == 0 and op == 54 and w[2] == entry:
                state = 1
            elif state == 1 and op == 248:   # first block: after its OpVariables
                out += w
                i += n
                while (self.words[i] & 0xffff) == 59:
                    k = self.words[i] >> 16
                    out += self.words[i:i + k]
                    i += k
                for v in outs:
                    out += [(3 << 16) | 62, v, consts[self.types[self.vars[v][0]][1][1]]]
                state = 2
                continue
            out += w
            i += n
        if state != 2:
            raise ValueError('vertex output initialisation found no entry block')
        out[3] = bound
        self.words = out
        self.deco_at = []
        return len(outs)

    def rewrite_set(self, new_set):
        for at, var, kind in self.deco_at:
            if kind == 34:
                self.words[at] = new_set
        return struct.pack('<%dI' % len(self.words), *self.words)


def classify(stage, spv, res):
    """Service.cs Reflect on SPIR-V names; returns binding rows."""
    rows = []
    sbufs = sorted(res.get('sbufs', []), key=lambda b: b['binding'])
    base = {'vs': 0x110, 'cs': 0x310, 'ps': 0x510}[stage]
    sb_index = 0
    resources = sorted((spv.deco[v].get(33, 0), v) for v, (p, st) in spv.vars.items()
                       if st in (0, 2, 12) and 33 in spv.deco.get(v, {}))
    for binding, var in resources:
        name = spv.names.get(var, '')
        dtype, count, view = spv.descriptor(var)
        row = dict(binding=binding, kind=0, reg=-1, sreg=-1, off=0, rw=0, dtype=dtype, count=count, view=view)
        m = re.match(r'^[a-z]+_c(\d+)$', name)
        if name == 'support_buffer':
            row['kind'] = K_SUPPORT
        elif m:
            n = int(m.group(1))
            if 3 <= n <= 16:
                row.update(kind=K_CBUF, reg=n - 3)
            elif n == 1:
                row['kind'] = K_C1
            elif n in (0, 2):
                row['kind'] = K_DRV0 if n == 0 else K_DRV2
            else:
                raise ValueError('constant bank %d in %s' % (n, name))
        elif re.match(r'^[a-z]+_s(\d+)$', name):
            b = sbufs[sb_index] if sb_index < len(sbufs) else None
            sb_index += 1
            cb_slot = b['cb_slot'] if b else 0
            off = b['cb_offset'] * 4 if b and cb_slot == 0 else None
            row['rw'] = 1 if b and (b['flags'] & 1) else 0   # Ryujinx BufferUsageFlags.Write
            if stage == 'cs' and off is not None and 0x210 <= off < 0x310 and (off - 0x210) % 16 == 0:
                row.update(kind=K_UBOP, reg=(off - 0x210) // 16)
            elif b and 3 <= cb_slot <= 16:
                row.update(kind=K_SBUFU, reg=cb_slot - 3, off=b['cb_offset'] * 4)
            elif off is not None and 0 <= off - base < 0x100 and (off - base) % 16 == 0:
                row.update(kind=K_SBUFP, reg=(off - base) // 16)
            else:
                raise ValueError('storage buffer %s: address source c%d+%#x not mapped' % (name, cb_slot, (b or {}).get('cb_offset', 0) * 4))
        elif re.match(r'^[a-z]+_t\w*?_cb\d+_40000000$', name) and dtype in (VK_UNIFORM_TEXEL, VK_STORAGE_TEXEL):
            # LISDE: a bindless texture buffer the translator turned into an array indexed by texture pool id
            # (Ryujinx BindlessElimination, TextureHandleType.Direct). reg -2: element i is pool id i.
            row.update(kind=K_TBUF, reg=-2)
        elif re.match(r'^[a-z]+_t\w*?_cb\d+_40000000$', name) and dtype == 2:   # bindless textures: array of sampled images
            row.update(kind=K_TEX, reg=-2)
        elif re.match(r'^[a-z]+_s\w*?_cb\d+_40000000$', name) and dtype == 0:   # their samplers: element i = sampler pool id i
            row.update(kind=11, reg=-2)
        else:
            d = decode_tex(name)
            if d is None or d[0] is None:
                raise ValueError('resource %s has no NVN slot' % name)
            t, s, ty = d
            if re.match(r'^[a-z]+_i', name):
                row.update(kind=K_IMG, reg=t)
            elif dtype in (VK_UNIFORM_TEXEL, VK_STORAGE_TEXEL):
                row.update(kind=K_TBUF, reg=t)
            else:
                row.update(kind=K_TEX, reg=t, sreg=s if ty else -1)
        rows.append(row)
    return rows


class Translator:
    def __init__(self, work):
        self.work = work
        self.lock = threading.Lock()
        self.proc = subprocess.Popen([XLATE, '--serve'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

    def run(self, job):
        import json
        with self.lock:
            self.proc.stdin.write(json.dumps({'job': job, 'out': self.work}) + '\n')
            self.proc.stdin.flush()
            line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError('translator exited')
        if not line.startswith('ok '):
            raise RuntimeError(line.strip()[:300])
        with open(os.path.join(self.work, job['key'] + '.spv'), 'rb') as f:
            spv = f.read()
        with open(os.path.join(self.work, job['key'] + '.res.json')) as f:
            res = json.load(f)
        return spv, res


def split_reply(reply):
    """(header and table lines, SPIR-V bytes) of a reply."""
    lines, pos = [], 0
    head = reply[:reply.index(b'\n')].decode().split()
    total = int(head[2]) + int(head[3]) + int(head[4])
    for _ in range(total + 1):
        end = reply.index(b'\n', pos)
        lines.append(reply[pos:end].decode())
        pos = end + 1
    return lines, reply[pos:]


# Global-section operand layout (word indices into one instruction; w[0] is the opcode word) for order_globals.
_GLOBAL_IDS = {   # opcode: (result id index, id operand indices, first index of a trailing id list or None)
    19: (1, (), None), 20: (1, (), None), 21: (1, (), None), 22: (1, (), None), 23: (1, (2,), None), 24: (1, (2,), None),
    25: (1, (2,), None), 26: (1, (), None), 27: (1, (2,), None), 28: (1, (2, 3), None), 29: (1, (2,), None),
    30: (1, (), 2), 31: (1, (), None), 32: (1, (3,), None), 33: (1, (), 2),
    41: (2, (1,), None), 42: (2, (1,), None), 43: (2, (1,), None), 44: (2, (1,), 3), 45: (2, (1,), None), 46: (2, (1,), None),
    48: (2, (1,), None), 49: (2, (1,), None), 50: (2, (1,), None), 51: (2, (1,), 3), 52: (2, (1,), 4),
    59: (2, (1, 4), None), 1: (2, (1,), None),
}


def order_globals(data):
    """SPIR-V with its types / constants / global variables ordered so every id is defined before it is used.
    The translator can emit an OpConstantComposite before the constants it lists (Double Exposure pixel shaders,
    26 Oct 2026), which spirv-val rejects and Turnip fails to compile. The order of everything else is kept; a module
    with an instruction this does not know in that section (or OpTypeForwardPointer) is returned unchanged."""
    words = list(struct.unpack('<%dI' % (len(data) // 4), data))
    insts, i = [], 5
    while i < len(words):
        n = words[i] >> 16
        if not n:
            return data
        insts.append((i, n)); i += n
    ops = [words[a] & 0xffff for a, _ in insts]
    first = next((k for k, op in enumerate(ops) if op in _GLOBAL_IDS), None)
    end = next((k for k, op in enumerate(ops) if op == 54), len(insts))   # OpFunction
    if first is None:
        return data
    section = insts[first:end]
    if any(words[a] & 0xffff not in _GLOBAL_IDS for a, _ in section):
        return data
    defined, deps = {}, []
    for k, (a, n) in enumerate(section):
        res, idx, tail = _GLOBAL_IDS[words[a] & 0xffff]
        w = words[a:a + n]
        defined[w[res]] = k
        d = [w[j] for j in idx if j < n] + (list(w[tail:]) if tail is not None else [])
        deps.append(d)
    late = [k for k, d in enumerate(deps) if any(defined.get(x, -1) > k for x in d)]
    if not late:
        return data
    import heapq
    users = [[] for _ in section]; need = [0] * len(section)
    for k, d in enumerate(deps):
        for x in set(d):
            j = defined.get(x)
            if j is not None and j != k:
                users[j].append(k); need[k] += 1
    ready = [k for k in range(len(section)) if not need[k]]; heapq.heapify(ready); order = []
    while ready:
        k = heapq.heappop(ready); order.append(k)
        for u in users[k]:
            need[u] -= 1
            if not need[u]:
                heapq.heappush(ready, u)
    if len(order) != len(section):
        raise ValueError('SPIR-V global section has a dependency cycle')
    out = words[:section[0][0]]
    for k in order:
        a, n = section[k]; out += words[a:a + n]
    out += words[section[-1][0] + section[-1][1]:]
    print('[bridge] reordered %d global instructions defined after their use' % len(late), flush=True)
    return struct.pack('<%dI' % len(out), *out)


_IMAGE_OPERAND_IDS = [(0x1, 1), (0x2, 1), (0x4, 2), (0x8, 1), (0x10, 1), (0x20, 1), (0x40, 1), (0x80, 1), (0x100, 1),
                      (0x200, 1), (0x400, 0), (0x800, 0), (0x1000, 0), (0x2000, 0), (0x4000, 0), (0x10000, 1)]


def emulate_dynamic_offsets(data):
    """Texture instructions whose ConstOffset is not a constant, rewritten the way Ryujinx's TexturePass does when the
    host has no non-constant offsets: samples add offset / textureSize(lod) to the coordinates, fetches add the offset
    to the integer coordinates, gathers use the Offset operand (allowed for gathers). The translator's accessor reports
    non-constant offsets as supported (fine for Metal, not for Vulkan), and then also emits the offset vector as a global
    OpConstantComposite of function values (Double Exposure, 26 Oct 2026); that composite is rebuilt in the function."""
    words = list(struct.unpack('<%dI' % (len(data) // 4), data))
    insts, i = [], 5
    while i < len(words):
        n = words[i] >> 16
        insts.append(words[i:i + n]); i += n
    CONST = {41, 42, 43, 44, 45, 46, 48, 49, 50, 51, 52}
    gconst, types, rtype = {}, {}, {}
    for w in insts:
        op = w[0] & 0xffff
        if op in CONST: gconst[w[2]] = w
        elif 19 <= op <= 39 and op != 39: types[w[1]] = w
        if op in (61, 86, 83, 100) and len(w) > 2: rtype[w[2]] = w[1]
    def is_const(x):
        w = gconst.get(x)
        return w is not None and ((w[0] & 0xffff) not in (44, 51) or all(is_const(c) for c in w[3:]))
    targets = [k for k, w in enumerate(insts) if (w[0] & 0xffff) in (87, 88, 89, 90, 95, 96, 97)
               and len(w) > (6 if (w[0] & 0xffff) in (89, 90, 96, 97) else 5)]
    work = []
    for k in targets:
        w = insts[k]; op = w[0] & 0xffff; mpos = 6 if op in (89, 90, 96, 97) else 5
        mask = w[mpos]
        if not mask & 0x8:
            continue
        at, opos, lpos = mpos + 1, None, None
        for bit, cnt in _IMAGE_OPERAND_IDS:
            if mask & bit:
                if bit == 0x8: opos = at
                if bit == 0x2: lpos = at
                at += cnt
        if not is_const(w[opos]):
            work.append((k, mpos, opos, lpos))
    if not work:
        return data
    bound = words[3]
    def new_id():
        nonlocal bound
        bound += 1; return bound - 1
    added_types = []
    def find_type(pred, make):
        for t, tw in types.items():
            if pred(tw): return t
        t = new_id(); tw = make(t); types[t] = tw; added_types.append(tw); return t
    t_int = find_type(lambda tw: tw[0] & 0xffff == 21 and tw[2] == 32 and tw[3] == 1, lambda t: [(4 << 16) | 21, t, 32, 1])
    t_float = find_type(lambda tw: tw[0] & 0xffff == 22 and tw[2] == 32, lambda t: [(3 << 16) | 22, t, 32])
    def vec(comp, n):
        if n == 1: return comp
        return find_type(lambda tw: tw[0] & 0xffff == 23 and tw[2] == comp and tw[3] == n, lambda t: [(4 << 16) | 23, t, comp, n])
    zero_int = None
    for x, cw in gconst.items():
        if cw[0] & 0xffff == 43 and cw[1] == t_int and cw[3] == 0: zero_int = x
    added_consts = []
    if zero_int is None:
        zero_int = new_id(); added_consts.append([(4 << 16) | 43, t_int, zero_int, 0])
    def I(op, *ops): return [((len(ops) + 1) << 16) | op] + list(ops)
    def ncomp(t):
        tw = types[t]; return tw[3] if tw[0] & 0xffff == 23 else 1
    inserts, removed_globals, need_query, need_gather = {}, set(), False, False
    for k, mpos, opos, lpos in work:
        w = list(insts[k]); op = w[0] & 0xffff; pre = []
        off = w[opos]
        if off in gconst:   # a "constant" of function values: build it here
            gw = gconst[off]; nid = new_id(); pre.append(I(80, gw[1], nid, *gw[3:])); removed_globals.add(off); off = nid
        if op in (96, 97):   # gathers: a dynamic Offset is valid Vulkan for gathers (ImageGatherExtended)
            w[mpos] = (w[mpos] & ~0x8) | 0x10; w[opos] = off; need_gather = True
            # Offset comes after ConstOffset in operand order; with only one of them present the position is the same
        else:
            si = w[3]; sit = rtype.get(si)
            if sit is None: raise ValueError('dynamic texture offset: sampled image %d has no known type' % si)
            itw = types[sit]
            img_t = itw[2] if itw[0] & 0xffff == 27 else sit   # OpTypeSampledImage -> OpTypeImage (fetch takes the image)
            iw = types[img_t]; dim, arrayed = iw[3], iw[5]
            n = {0: 1, 1: 2, 2: 3, 4: 2}.get(dim)
            if n is None: raise ValueError('dynamic texture offset on image dim %d' % dim)
            coord = w[4]
            m = n + (1 if arrayed else 0)   # non-projective coordinates: the image's dimensions, then the layer
            ct = vec(t_int if op == 95 else t_float, m)
            if op == 95:   # fetch: integer coordinates
                comp = t_int; offs = off
                if m == n: nc = new_id(); pre.append(I(128, ct, nc, coord, offs))   # OpIAdd
                else:
                    parts = []
                    for c in range(m):
                        e = new_id(); pre.append(I(81, comp, e, coord, c))
                        if c < n:
                            oc = offs
                            if n > 1: oc = new_id(); pre.append(I(81, t_int, oc, offs, c))
                            s2 = new_id(); pre.append(I(128, comp, s2, e, oc)); e = s2
                        parts.append(e)
                    nc = new_id(); pre.append(I(80, ct, nc, *parts))
            else:
                need_query = True
                img = coord_img = new_id()
                if op in (87, 88, 89, 90): pre.append(I(100, img_t, img, si))   # OpImage
                if lpos is not None:
                    li = new_id(); pre.append(I(110, t_int, li, w[lpos]))   # OpConvertFToS
                else:
                    li = zero_int   # implicit LOD: the base level size (Ryujinx uses the base level too)
                k_comp = (2 if dim == 3 else n) + (1 if arrayed else 0)
                sz = new_id(); pre.append(I(103, vec(t_int, k_comp), sz, img, li))   # OpImageQuerySizeLod
                if k_comp != n:
                    ex = []
                    for c in range(n): e = new_id(); pre.append(I(81, t_int, e, sz, c)); ex.append(e)
                    if n > 1: s2 = new_id(); pre.append(I(80, vec(t_int, n), s2, *ex)); sz = s2
                    else: sz = ex[0]
                fv = vec(t_float, n)
                szf = new_id(); pre.append(I(111, fv, szf, sz))
                of = new_id(); pre.append(I(111, fv, of, off))
                dl = new_id(); pre.append(I(136, fv, dl, of, szf))   # OpFDiv
                if m == n: nc = new_id(); pre.append(I(129, ct, nc, coord, dl))   # OpFAdd
                else:
                    parts = []
                    for c in range(m):
                        e = new_id(); pre.append(I(81, t_float, e, coord, c))
                        if c < n:
                            dc = dl
                            if n > 1: dc = new_id(); pre.append(I(81, t_float, dc, dl, c))
                            s2 = new_id(); pre.append(I(129, t_float, s2, e, dc)); e = s2
                        parts.append(e)
                    nc = new_id(); pre.append(I(80, ct, nc, *parts))
            w[4] = nc
            w[mpos] &= ~0x8
            del w[opos]
            w[0] = (len(w) << 16) | op
        inserts[k] = pre; insts[k] = w
    DEBUG = (5, 6, 7, 8, 71, 72)   # names, strings, lines, decorations
    used = set()
    for k, w in enumerate(insts):
        if k in inserts or (w[0] & 0xffff) in CONST or (w[0] & 0xffff) in DEBUG: continue
        used.update(w[1:])
    for g in removed_globals:
        if g in used: raise ValueError('dynamic texture offset vector %d has other uses' % g)
    out_insts, caps_end, first_fn = [], None, None
    for k, w in enumerate(insts):
        op = w[0] & 0xffff
        if op in CONST and w[2] in removed_globals: continue
        if op in (5, 71) and w[1] in removed_globals: continue
        if op == 54 and first_fn is None:
            first_fn = len(out_insts); out_insts += added_types + added_consts
        out_insts += inserts.get(k, []); out_insts.append(w)
        if op == 17: caps_end = len(out_insts)
    for need, cap in ((need_query, 50), (need_gather, 25)):   # ImageQuery, ImageGatherExtended
        if need and not any((w[0] & 0xffff) == 17 and w[1] == cap for w in out_insts):
            out_insts.insert(caps_end, I(17, cap)); caps_end += 1
    out = words[:5]; out[3] = bound
    for w in out_insts: out += w
    print('[bridge] %d dynamic texture offsets emulated' % len(work), flush=True)
    return struct.pack('<%dI' % len(out), *out)


def emulate_warp32(data):
    """Maxwell warp (32 lanes) semantics on the Adreno's 64/128-lane subgroups.

    The translator assumes a 32-lane host subgroup (IGpuAccessor.QueryHostSubgroupSize = 32): it reads the lane index
    raw, takes component 0 of ballots and lane masks and shuffles by the guest lane. Z-A's GPU culling counts visible
    instances with ballot + SubgroupLtMask in 64-thread workgroups; on wider subgroups lanes 32+ saw the masks of lanes
    0-31, the counts came out wrong and the scene pass and the culling compute grew (Hexveil's PokeZ-ARecomp 7253341 /
    95d48f2, the same symptom on AMD). This applies what Ryujinx emits for a larger subgroup: lane = lane & 31, ballot
    and mask component lane >> 5, shuffle id (lane & ~31) | (id & 31). Returns (data, changed)."""
    w = list(struct.unpack('<%dI' % (len(data) // 4), data))
    bound = w[3]

    def new_id():
        nonlocal bound
        r = bound; bound += 1; return r

    def I(op, *args):
        return [((len(args) + 1) << 16) | op, *args]
    insts, i = [], 5
    while i < len(w):
        n = w[i] >> 16
        insts.append(w[i:i + n]); i += n
    lane = None; masks = set(); uint = None; uint_in = None; ballots = set()
    has_ops = False
    for x in insts:
        op = x[0] & 0xffff
        if op == 71 and x[2] == 11:
            if x[3] == 41: lane = x[1]
            elif 4416 <= x[3] <= 4420: masks.add(x[1])
        elif op == 21 and x[2] == 32 and x[3] == 0: uint = x[1]
        elif op == 339: ballots.add(x[2]); has_ops = True
        elif op in (345, 334, 335, 336): has_ops = True
    if not has_ops and lane is None:
        return data, False
    if uint is None:
        raise ValueError('subgroup operations without a 32-bit unsigned type')
    for x in insts:
        if (x[0] & 0xffff) == 32 and x[2] == 1 and x[3] == uint: uint_in = x[1]
    c5, c31, cnot31 = new_id(), new_id(), new_id()
    pre_func = [I(43, uint, c5, 5), I(43, uint, c31, 31), I(43, uint, cnot31, 0xffffffe0)]
    new_deco = []
    if lane is None:   # ballots or shuffles without a lane index read: add the built-in
        if uint_in is None:
            uint_in = new_id(); pre_func.insert(0, I(32, uint_in, 1, uint))
        lane = new_id()
        pre_func.append(I(59, uint_in, lane, 1))
        new_deco.append(I(71, lane, 11, 41))
    out, warned, first_func, last_deco = [], set(), None, None
    for x in insts:
        op = x[0] & 0xffff
        if op == 71: last_deco = len(out)
        if op == 54 and first_func is None:
            first_func = len(out); out.extend(pre_func)
        if op == 15 and new_deco and lane not in x[3:]:   # OpEntryPoint: the new built-in joins the interface
            x = list(x) + [lane]; x[0] = (len(x) << 16) | 15
        if first_func is None:
            out.append(x); continue

        def lane_shift():
            raw, comp = new_id(), new_id()
            out.append(I(61, uint, raw, lane)); out.append(I(194, uint, comp, raw, c5))
            return raw, comp
        if op == 61 and x[3] in masks:   # a whole lane mask loaded: its .x is read like a ballot's
            ballots.add(x[2]); out.append(x)
        elif op == 61 and x[3] == lane and x[1] == uint:   # lane index: the lane within its 32-lane warp
            raw = new_id()
            out.append(I(61, uint, raw, lane)); out.append(I(199, uint, x[2], raw, c31))
        elif op == 65 and x[3] in masks and len(x) == 5:   # SubgroupXxMask[0]: the warp's component
            _, comp = lane_shift()
            out.append(I(65, x[1], x[2], x[3], comp))
        elif op == 81 and x[3] in ballots and len(x) == 5 and x[4] == 0:   # ballot(..).x: the warp's component
            _, comp = lane_shift()
            out.append(I(77, x[1], x[2], x[3], comp))
        elif op == 345:   # shuffle by guest lane: within the warp
            raw, hi, lo, nid = new_id(), new_id(), new_id(), new_id()
            out.append(I(61, uint, raw, lane)); out.append(I(199, uint, hi, raw, cnot31))
            out.append(I(199, uint, lo, x[5], c31)); out.append(I(197, uint, nid, hi, lo))
            out.append(I(345, x[1], x[2], x[3], x[4], nid))
        else:
            if op in (334, 335, 336) and op not in warned:
                warned.add(op); print('[bridge] WARNING: subgroup vote op %d left as is (64/128-lane result)' % op, flush=True)
            out.append(x)
    if new_deco:
        out[last_deco + 1:last_deco + 1] = new_deco
    w2 = w[:5]; w2[3] = bound
    for x in out: w2.extend(x)
    return struct.pack('<%dI' % len(w2), *w2), True


def validate(work, name, data):
    check = os.path.join(work, name + '.spv')
    with open(check, 'wb') as f:
        f.write(data)
    val = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', check], capture_output=True, text=True)
    if val.returncode:
        raise ValueError('rewritten SPIR-V is invalid: ' + (val.stderr or val.stdout).strip()[:200])


def vertex_defaults(work, name, data):
    """Vertex SPIR-V with outputs initialised to (0, 0, 0, 1) (validated when anything changed)."""
    spv = Spirv(data)
    if not spv.init_outputs():
        return data
    data = struct.pack('<%dI' % len(spv.words), *spv.words)
    validate(work, name + '-defaults', data)
    return data


def apply_xfb(data, xfb):
    """Decorate vertex shader outputs for VK_EXT_transform_feedback and shrink output vector types to match XFB component counts."""
    if not xfb:
        return data
    words = list(struct.unpack('<%dI' % (len(data) // 4), data))
    bound = words[3]
    def new_id():
        nonlocal bound
        r = bound; bound += 1; return r
    def I(op, *args):
        return [((len(args) + 1) << 16) | op, *args]

    insts = []
    i = 5
    while i < len(words):
        n = words[i] >> 16
        insts.append(words[i:i + n])
        i += n

    locs = {}
    out_vars = {}
    ptr_types = {}
    vec_types = {}
    entry_id = None
    for k, w in enumerate(insts):
        op = w[0] & 0xffff
        if op == 15 and w[1] == 0:
            entry_id = w[2]
        elif op == 71 and len(w) >= 4 and w[2] == 30:
            locs[w[3]] = w[1]
        elif op == 59 and len(w) >= 4 and w[3] == 3:
            out_vars[w[2]] = (k, w[1])
        elif op == 32 and len(w) >= 4 and w[2] == 3:
            ptr_types[w[3]] = w[1]
        elif op == 23 and len(w) >= 4:
            vec_types[(w[2], w[3])] = w[1]

    by_var = {}
    for item in xfb:
        if (not isinstance(item, dict) or not 0 <= int(item.get('buf', -1)) < 4 or
                not 1 <= int(item.get('comp', 0)) <= 4 or int(item.get('stride', 0)) <= 0 or
                int(item.get('off', -1)) < 0 or int(item.get('off', -1)) + 4 * int(item.get('comp', 0)) > int(item.get('stride', 0))):
            raise ValueError('invalid transform-feedback output description')
        vid = locs.get(item['loc'])
        if vid is None or vid not in out_vars:
            raise ValueError('transform-feedback location %s is absent from the translated vertex outputs' % item['loc'])
        if vid in by_var:
            raise ValueError('transform-feedback captures one output variable more than once')
        by_var[vid] = item
    if len(by_var) != len(xfb):
        raise ValueError('not every transform-feedback output was mapped')
    if entry_id is None:
        raise ValueError('transform-feedback vertex entry point is missing')

    inv_ptr = {pid: elem for elem, pid in ptr_types.items()}
    inv_vec = {vid: (comp_t, cnt) for (comp_t, cnt), vid in vec_types.items()}

    added_globals = []
    new_var_elem_type = {}
    value_types = {}
    # These instructions have a result type followed by a result id. Keep a
    # small type index so stores can be converted when an XFB varying is
    # narrower than the translated output.
    typed_result_ops = set([1, 12, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,
                            52, 55, 57, 59, 61, 65, 66, 67, 68, 69, 70, 77,
                            78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89,
                            90, 91, 92, 93, 94, 95, 96, 97, 98, 245])
    for w in insts:
        op = w[0] & 0xffff
        if op in typed_result_ops and len(w) >= 3:
            value_types[w[2]] = w[1]
        elif 109 <= op <= 242 and len(w) >= 3:
            value_types[w[2]] = w[1]

    for vid, item in by_var.items():
        k, ptype = out_vars[vid]
        elem = inv_ptr.get(ptype)
        comp = item['comp']
        if elem in inv_vec:
            scalar_t, cnt = inv_vec[elem]
            if comp > cnt:
                raise ValueError('transform-feedback output at location %s needs %d components but the translated output has %d' %
                                 (item['loc'], comp, cnt))
            if comp == cnt:
                # The translated output already has the right type. In
                # particular, do not replace its OpStore value: doing so
                # collapses captured positions to zero.
                continue
            if comp == 1:
                want_elem = scalar_t
            else:
                want_elem = vec_types.get((scalar_t, comp))
                if want_elem is None:
                    want_elem = new_id()
                    vec_types[(scalar_t, comp)] = want_elem
                    added_globals.append(I(23, want_elem, scalar_t, comp))
            want_ptr = ptr_types.get(want_elem)
            if want_ptr is None:
                want_ptr = new_id()
                ptr_types[want_elem] = want_ptr
                added_globals.append(I(32, want_ptr, 3, want_elem))
            insts[k][1] = want_ptr
            new_var_elem_type[vid] = (want_elem, comp, scalar_t, cnt)

    alias = {}
    new_insts = []
    caps_end = 0
    exec_mode_pos = None
    decos_end = None
    first_fn = None

    for k, w in enumerate(insts):
        op = w[0] & 0xffff
        if op == 65 and len(w) >= 5 and w[3] in new_var_elem_type and new_var_elem_type[w[3]][1] == 1:
            alias[w[2]] = w[3]
            continue
        if op == 62 and len(w) >= 3 and w[1] in new_var_elem_type:
            want_elem, comp, scalar_t, _ = new_var_elem_type[w[1]]
            source = w[2]
            source_type = value_types.get(source)
            if source_type != want_elem:
                if comp == 1 and source_type in inv_vec and inv_vec[source_type][0] == scalar_t:
                    converted = new_id()
                    new_insts.append(I(81, scalar_t, converted, source, 0))
                    source = converted
                elif comp > 1 and source_type in inv_vec and inv_vec[source_type][0] == scalar_t and inv_vec[source_type][1] >= comp:
                    converted = new_id()
                    new_insts.append(I(79, want_elem, converted, source, source, *range(comp)))
                    source = converted
                else:
                    raise ValueError('cannot preserve transform-feedback store for location %s (value type %s)' %
                                     (by_var[w[1]]['loc'], source_type))
            w = [w[0], w[1], source] + w[3:]
        elif alias and op not in (5, 6, 7, 8, 71, 72):
            w = [w[0]] + [alias.get(x, x) for x in w[1:]]
        new_insts.append(w)
        if op == 17:
            caps_end = len(new_insts)
        elif op in (15, 16):
            exec_mode_pos = len(new_insts)
        elif op in (71, 72):
            decos_end = len(new_insts)
        elif op == 54 and first_fn is None:
            first_fn = len(new_insts) - 1

    if added_globals and first_fn is not None:
        for g in reversed(added_globals):
            new_insts.insert(first_fn, g)
    if decos_end is not None:
        decos = []
        for vid, item in by_var.items():
            decos.append(I(71, vid, 36, item['buf']))
            decos.append(I(71, vid, 37, item['stride']))
            decos.append(I(71, vid, 35, item['off']))
        for d in reversed(decos):
            new_insts.insert(decos_end, d)
    if exec_mode_pos is not None and entry_id is not None:
        if not any((w[0] & 0xffff) == 16 and w[1] == entry_id and w[2] == 11 for w in new_insts):
            new_insts.insert(exec_mode_pos, I(16, entry_id, 11))
    if not any((w[0] & 0xffff) == 17 and w[1] == 53 for w in new_insts):
        new_insts.insert(caps_end, I(17, 53))

    out = words[:5]
    out[3] = bound
    for w in new_insts:
        out += w
    return order_globals(struct.pack('<%dI' % len(out), *out))


def upgrade_reply(tr, variant, stage, old, xfb=None):
    """A version-2 reply in the current format: vertex outputs get their defaults."""
    if stage != 'vs':
        return old
    lines, data = split_reply(old)
    data = vertex_defaults(tr.work, variant, data)
    if xfb:
        data = apply_xfb(data, xfb)
        validate(tr.work, variant + '-xfb', data)
    head = lines[0].split()
    head[1] = str(len(data))
    return ('\n'.join([' '.join(head)] + lines[1:]) + '\n').encode() + data


def strip_inputs(tr, rq):
    variant, mask = rq['variant'], int(rq['strip_inputs'])
    out = os.path.join(tr.work, '%s-i%x.reply%d' % (variant, mask, REPLY_VERSION))
    if os.path.exists(out):
        with open(out, 'rb') as f:
            return f.read()
    with open(os.path.join(tr.work, '%s.reply%d' % (variant, REPLY_VERSION)), 'rb') as f:
        reply = f.read()
    lines, data = split_reply(reply)
    head = lines[0].split()
    spv = Spirv(data)
    spv.zero_inputs(mask)
    data = struct.pack('<%dI' % len(spv.words), *spv.words)
    check = os.path.join(tr.work, '%s-i%x.spv' % (variant, mask))
    with open(check, 'wb') as f:
        f.write(data)
    val = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', check], capture_output=True, text=True)
    if val.returncode:
        raise ValueError('input stripping produced invalid SPIR-V: ' + (val.stderr or val.stdout).strip()[:200])
    kept = [l for l in lines[1:] if not (l.startswith('I ') and (mask >> int(l.split()[1])) & 1)]
    head[1] = str(len(data))
    head[3] = str(sum(1 for l in kept if l.startswith('I ')))
    result = ('\n'.join([' '.join(head)] + kept) + '\n').encode() + data
    with open(out, 'wb') as f:
        f.write(result)
    return result


def handle(tr, rq):
    import json
    if 'strip_inputs' in rq:
        return strip_inputs(tr, rq)
    variant, stage = rq['variant'], rq['stage']
    if stage not in ('vs', 'ps', 'cs'):
        raise ValueError('stage %s has no translation chain' % stage)
    done = os.path.join(tr.work, '%s.reply%d' % (variant, REPLY_VERSION))
    if os.path.exists(done):   # the variant name covers the code and every answer
        with open(done, 'rb') as f:
            return f.read()
    old = os.path.join(tr.work, '%s.reply2' % variant)
    if os.path.exists(old):
        with open(old, 'rb') as f:
            reply = upgrade_reply(tr, variant, stage, f.read(), rq.get('xfb'))
        with open(done + '.tmp', 'wb') as f:
            f.write(reply)
        os.replace(done + '.tmp', done)
        return reply
    code = bytes.fromhex(rq['code_hex'])
    path = os.path.join(tr.work, variant + '.code')
    with open(path, 'wb') as f:
        f.write(code)
    job = {'key': variant, 'stage': stage, 'code': path, 'c1': rq.get('c1', ''), 'debug': True, 'reduced': True,
           'tex': rq.get('tex', {}), 'img': rq.get('img', {}), 'attr': rq.get('attr', {}), 'fout': rq.get('fout', {}),
           'depth_minus_w': bool(rq.get('depth_minus_w'))}
    if rq.get('local'):
        job['local'] = rq['local']
    data, res = tr.run(job)
    data = emulate_dynamic_offsets(order_globals(data))
    spv = Spirv(data)
    if 10 in spv.caps:   # Float64: the Adreno driver has no shaderFloat64; use the translator's lowering
        job['softfloat64'] = True
        data, res = tr.run(job)
        data = emulate_dynamic_offsets(order_globals(data))
        spv = Spirv(data)
        if 10 in spv.caps:
            raise ValueError('FP64 lowering still emitted Float64')
    if 11 in spv.caps:
        raise ValueError('shader needs Int64, which the Adreno driver lacks')
    rows = classify(stage, spv, res)
    if spv.strip_interlock():
        print('[bridge] %s: fragment shader interlock removed (no ordering guarantee on this device)' % variant, flush=True)
        spv = Spirv(struct.pack('<%dI' % len(spv.words), *spv.words))
    data = spv.rewrite_set(1 if stage == 'ps' else 0)
    data, warp = emulate_warp32(data)
    if warp:
        validate(tr.work, variant + '-warp32', data)
        spv = Spirv(data)
        print('[bridge] %s: 32-lane warp emulated on wider subgroups' % variant, flush=True)
    if stage == 'vs':
        data = vertex_defaults(tr.work, variant, data)
        if rq.get('xfb'):
            data = apply_xfb(data, rq['xfb'])
            validate(tr.work, variant + '-xfb', data)
            spv = Spirv(data)
            print('[bridge] %s: transform feedback applied (%d outputs)' % (variant, len(rq['xfb'])), flush=True)
    try:   # reported, not fatal: Turnip accepts some modules spirv-val rejects (e.g. a missing *WithoutFormat capability)
        validate(tr.work, variant + '-final', data)
    except ValueError as e:
        print('[bridge] WARNING %s %s: %s' % (stage, variant, e), flush=True)
    inputs = spv.interface(1) if stage in ('vs', 'ps') else []
    outputs = spv.interface(3) if stage in ('vs', 'ps') else []
    local = rq.get('local') or [0, 0, 0]
    head = ['OK %d %d %d %d %d %d %d' % (len(data), len(rows), len(inputs), len(outputs), local[0], local[1], local[2])]
    for r in rows:
        head.append('B %d %d %d %d %d %d %d %d %d %d' % (1 if stage == 'ps' else 0, r['binding'], r['kind'], r['reg'], r['sreg'], r['off'], r['rw'], r['dtype'], r['count'], r['view']))
    head += ['I %d %s' % x for x in inputs] + ['O %d %s' % x for x in outputs]
    reply = ('\n'.join(head) + '\n').encode() + data
    with open(done + '.tmp', 'wb') as f:
        f.write(reply)
    os.replace(done + '.tmp', done)
    return reply


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        import json
        for line in self.rfile:
            if not line.strip():
                continue
            try:
                rq = json.loads(line)
                reply = handle(self.server.translator, rq)
                print('[bridge] %s %s ok (%d bytes)' % (rq.get('stage'), rq.get('variant'), len(reply)), flush=True)
            except Exception as e:   # reported to the device, which stops with the message
                msg = ('%s: %s' % (type(e).__name__, e)).replace('\n', ' ')[:400]
                print('[bridge] FAIL %s' % msg, flush=True)
                reply = ('ERR %s\n' % msg).encode()
            self.wfile.write(reply)
            self.wfile.flush()


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=27760)
    ap.add_argument('--work', default=None)
    a = ap.parse_args()
    work = a.work or tempfile.mkdtemp(prefix='totk-shader-bridge-')
    os.makedirs(work, exist_ok=True)
    if not os.access(XLATE, os.X_OK):
        sys.exit('translator missing: %s' % XLATE)
    srv = Server(('127.0.0.1', a.port), Handler)
    srv.translator = Translator(work)
    print('[bridge] listening on 127.0.0.1:%d, work %s' % (a.port, work), flush=True)
    srv.serve_forever()


if __name__ == '__main__':
    main()
