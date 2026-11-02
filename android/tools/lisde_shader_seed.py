#!/usr/bin/env python3
"""Translate every shader of Life is Strange: Double Exposure ahead of play, so the Thor needs no Mac.

The shaders come from lisde_shaders.inventory (IoStore shader archives and the global shader cache of the verified
RomFS). Each is translated for the default bind state with the bridge's own chain (android_shader_seed.translate),
so a seeded variant is byte-identical to what the bridge would answer for that state. The device takes the seeded
variant whose used slots match the bound state, else the nearest one, and notes what it lacked for seed_missing.py.

Double Exposure selects NVN_DEPTH_MODE_NEAR_IS_ZERO ("深度模式 近=0" in every run log), unlike TOTK.

Output (game-derived data: never in the repo): <out>/cache/*.vks, seed.idx, *.fetch and <out>/seed-manifest.json.
Push to the Thor with push_shader_seed.sh <out>.

usage: lisde_shader_seed.py [--out DIR] [--limit N]
"""
import argparse
from pathlib import Path
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import android_shader_seed as chain   # noqa: E402
import lisde_shaders                  # noqa: E402

chain.DEPTH_MINUS_W = False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', type=Path, default=lisde_shaders.PORT/'Uitgepakt/shader-seed')
    ap.add_argument('--limit', type=int, default=0, help='translate only the first N jobs (trial)')
    a = ap.parse_args()
    started = time.time()
    shaders = lisde_shaders.inventory()
    print('%d unique shaders' % len(shaders), flush=True)
    # Vertex and compute shaders first: there are few, and a missing one hides a whole draw or pass.
    order = {'vs': 0, 'cs': 1, 'ps': 2}
    keys = sorted(shaders, key=lambda k: (order[shaders[k][0]], k))
    jobs = [(key, chain.seed.ANSWERS, key + '-' + chain.seed.SUFFIX, 'default') for key in keys]
    if a.limit:
        jobs = jobs[:a.limit]
    chain.translate(a.out, shaders, jobs, dict(game='0100B2301F4A8000'), started)


if __name__ == '__main__':
    main()
