#!/usr/bin/env python3
"""Translate every shader of Pokémon Legends: Z-A ahead of play, so the Thor needs no Mac.

The shaders come from za_shaders: the BNSH files inside the Trinity packs of the verified RomFS (decompressed with the
game's own Oodle). Reading them from a dump directory (one decompressed file per BNSH-holding Trinity file, written by
a full pass over data.trpfs) is the fast path; without --files the packs are read directly. Each shader is translated
for the default bind state with the bridge's own chain (android_shader_seed.translate, bridge.handle: including the
32-lane warp emulation), so a seeded variant is byte-identical to what the bridge would answer for that state. The
device takes the seeded variant whose used slots match the bound state, else the nearest one, and notes what it
lacked for seed_missing.py.

Z-A selects NVN_DEPTH_MODE_NEAR_IS_ZERO ("深度模式 近=0" in its run logs), as Double Exposure does.

Output (game-derived data: never in the repo): <out>/cache/*.vks, seed.idx, *.fetch and <out>/seed-manifest.json.
Push to the Thor with push_shader_seed.sh <out>.

Parallel: --shard K/N translates every N-th shader into <out>/shard-K; --merge combines the shards into <out>/cache.
The inventory is built once and kept in <out>/inventory.pickle.

usage: za_shader_seed.py [--files DIR] [--out DIR] [--limit N] [--shard K/N | --merge]
"""
import argparse
import json
import os
from pathlib import Path
import pickle
import shutil
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import android_shader_seed as chain   # noqa: E402
import za_shaders                     # noqa: E402

chain.DEPTH_MINUS_W = False


def load_inventory(files, out):
    path = out/'inventory.pickle'
    if path.exists():
        return pickle.loads(path.read_bytes())
    shaders = za_shaders.inventory_files(files) if files.is_dir() else za_shaders.inventory()
    out.mkdir(parents=True, exist_ok=True)
    path.with_suffix('.tmp').write_bytes(pickle.dumps(shaders))
    os.replace(path.with_suffix('.tmp'), path)
    return shaders


def merge(out):
    """The shards' caches into <out>/cache, one seed.idx, one manifest."""
    cache = out/'cache'
    cache.mkdir(parents=True, exist_ok=True)
    index, manifests = set(), []
    for shard in sorted(out.glob('shard-*')):
        for f in (shard/'cache').iterdir():
            if f.name == 'seed.idx':
                index.update(line for line in f.read_text().splitlines() if line)
            elif f.suffix in ('.vks', '.fetch'):
                shutil.copyfile(f, cache/f.name)
        manifests.append(json.loads((shard/'seed-manifest.json').read_text()))
    (cache/'seed.idx').write_text('\n'.join(sorted(index)) + '\n')
    (cache/'seed.depth').write_text('0\n')   # NEAR_IS_ZERO: the device uses the seed only in this depth mode
    failed = {}
    for m in manifests:
        for k, v in m['failed'].items():
            failed[k] = failed.get(k, 0) + v
    total = dict(manifests[0], shards=len(manifests), variants=len(index), failed=failed,
                 jobs=sum(m['jobs'] for m in manifests), written=sum(m['written'] for m in manifests),
                 fetch_proofs=sum(m['fetch_proofs'] for m in manifests),
                 bytes=sum(p.stat().st_size for p in cache.glob('*.vks')), seconds=max(m['seconds'] for m in manifests))
    (out/'seed-manifest.json').write_text(json.dumps(total, indent=1) + '\n')
    print(json.dumps(total, indent=1))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--files', type=Path, default=za_shaders.PORT/'Uitgepakt/bnsh-files')
    ap.add_argument('--out', type=Path, default=za_shaders.PORT/'Uitgepakt/shader-seed')
    ap.add_argument('--limit', type=int, default=0, help='translate only the first N jobs (trial)')
    ap.add_argument('--shard', default='', help='K/N: every N-th shader from the K-th, into <out>/shard-K')
    ap.add_argument('--merge', action='store_true')
    a = ap.parse_args()
    if a.merge:
        merge(a.out)
        return
    started = time.time()
    shaders = load_inventory(a.files, a.out)
    print('%d unique shaders' % len(shaders), flush=True)
    # Vertex and compute shaders first: a missing one hides a whole draw or pass.
    order = {'vs': 0, 'cs': 1, 'ps': 2}
    keys = sorted(shaders, key=lambda k: (order[shaders[k][0]], k))
    out = a.out
    if a.shard:
        k, n = (int(x) for x in a.shard.split('/'))
        keys = keys[k::n]
        out = a.out/('shard-%d' % k)
    jobs = [(key, chain.seed.ANSWERS, key + '-' + chain.seed.SUFFIX, 'default') for key in keys]
    if a.limit:
        jobs = jobs[:a.limit]
    chain.translate(out, shaders, jobs, dict(game='0100F43008C44000'), started)


if __name__ == '__main__':
    main()
