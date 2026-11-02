#!/usr/bin/env python3
"""Rewrite a translated compute shader (.vks: text header + SPIR-V) so every invocation is its own one-lane warp.

Z-A's GPU culling counts visible instances with warp-aggregated atomics: ballot over the warp, the highest active
lane adds the popcount, the others take their slot from SubgroupLtMask and a shuffle of the leader's base. That only
works when the host subgroup behaves like a 32-lane Maxwell warp. With one lane per warp the same code is correct on
any subgroup size: lane = 0, LtMask = 0, ballot(p) = (p ? 1 : 0, 0, 0, 0), shuffle(v, id) = v. Each invocation then does
its own atomic add. Usage: warp1_rewrite.py in.vks out.vks (validated with spirv-val)."""
import struct
import subprocess
import sys
import tempfile

LANE, LT_MASKS = 41, range(4416, 4421)


def rewrite(spv):
    w = list(struct.unpack('<%dI' % (len(spv) // 4), spv))
    bound = w[3]
    insts, i = [], 5
    while i < len(w):
        n = w[i] >> 16; insts.append(w[i:i + n]); i += n

    def new_id():
        nonlocal bound
        bound += 1; return bound - 1

    def I(op, *a): return [((len(a) + 1) << 16) | op, *a]
    lane_vars, mask_vars = set(), set()
    for x in insts:
        if x[0] & 0xffff == 71 and x[2] == 11:
            if x[3] == LANE: lane_vars.add(x[1])
            elif x[3] in LT_MASKS: mask_vars.add(x[1])
    uint = next(x[1] for x in insts if x[0] & 0xffff == 21 and x[2] == 32 and x[3] == 0)
    uvec4 = next(x[1] for x in insts if x[0] & 0xffff == 23 and x[2] == uint and x[3] == 4)
    c0, c1, null4 = new_id(), new_id(), new_id()
    consts = [I(43, uint, c0, 0), I(43, uint, c1, 1), I(46, uvec4, null4)]
    mask_ptrs = set()
    out, placed, n = [], False, {'lane': 0, 'mask': 0, 'ballot': 0, 'shuffle': 0}
    for x in insts:
        op = x[0] & 0xffff
        if op == 54 and not placed:
            out.extend(consts); placed = True
        if op == 65 and x[3] in mask_vars:          # OpAccessChain into a mask: its loads become 0 too
            mask_ptrs.add(x[2]); out.append(x); continue
        if op == 61 and (x[3] in lane_vars or x[3] in mask_ptrs):   # OpLoad lane index or one mask word -> 0
            out.append(I(83, x[1], x[2], c0)); n['mask' if x[3] in mask_ptrs else 'lane'] += 1
        elif op == 61 and x[3] in mask_vars:        # OpLoad SubgroupXxMask -> 0
            out.append(I(83, x[1], x[2], null4)); n['mask'] += 1
        elif op == 339:                             # OpGroupNonUniformBallot type id scope pred
            bit = new_id()
            out.append(I(169, uint, bit, x[4], c1, c0))            # OpSelect
            out.append(I(80, x[1], x[2], bit, c0, c0, c0)); n['ballot'] += 1   # OpCompositeConstruct
        elif op == 345:                             # OpGroupNonUniformShuffle type id scope value index
            out.append(I(83, x[1], x[2], x[4])); n['shuffle'] += 1
        elif op in range(333, 367) and op not in (339, 345):
            raise ValueError('subgroup operation %d not handled' % op)
        else:
            out.append(x)
    hdr = w[:5]; hdr[3] = bound
    for x in out: hdr.extend(x)
    return struct.pack('<%dI' % len(hdr), *hdr), n


def main():
    src, dst = sys.argv[1], sys.argv[2]
    d = open(src, 'rb').read()
    at = d.find(struct.pack('<I', 0x07230203))
    head = d[:at].decode()
    first, rest = head.split('\n', 1)
    f = first.split()
    if f[0] != 'OK' or int(f[1]) != len(d) - at: raise ValueError('unexpected .vks header: ' + first)
    spv, n = rewrite(d[at:])
    with tempfile.NamedTemporaryFile(suffix='.spv') as t:
        t.write(spv); t.flush()
        val = subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', t.name], capture_output=True, text=True)
        if val.returncode: raise ValueError('invalid SPIR-V: ' + (val.stderr or val.stdout)[:300])
    f[1] = str(len(spv))
    open(dst, 'wb').write((' '.join(f) + '\n' + rest).encode() + spv)
    print('%s: %s, %d -> %d bytes, spirv-val ok' % (dst, n, len(d), len(spv) + at + len(f[1]) - len(first.split()[1])))


if __name__ == '__main__':
    main()
