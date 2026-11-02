#!/usr/bin/env python3
"""Summarise an indirect capture ("captureind"): per indirect draw the arguments as the GPU held them right before the
draw, grouped by the compute dispatch that preceded it. Zero instance counts, implausible counts and index ranges are
listed, so a culling pass that drops (or garbles) whole object groups stands out."""
import argparse
import json
import struct
from collections import Counter, defaultdict
from pathlib import Path


def records(path, indexed, stride, n):
    raw = path.read_bytes()
    out = []
    for i in range(n):
        at = i * stride
        if indexed:
            if at + 20 > len(raw): break
            out.append(dict(zip(('count', 'instances', 'first', 'base_vertex', 'first_instance'), struct.unpack_from('<IIIiI', raw, at))))
        else:
            if at + 16 > len(raw): break
            out.append(dict(zip(('count', 'instances', 'first', 'first_instance'), struct.unpack_from('<IIII', raw, at))))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__); ap.add_argument('capture', type=Path)
    root = ap.parse_args().capture
    rows = [json.loads(l) for l in (root / 'manifest.jsonl').read_text().splitlines() if l.strip()]
    end = next((r for r in rows if r['type'] == 'frame_end'), None)
    print('frame', rows[0].get('frame'), 'subgroup', rows[0].get('subgroup'), '| end:', end)
    last_cs, dispatches = '-', Counter()
    groups = defaultdict(lambda: {'draws': 0, 'records': 0, 'zero': 0, 'visible': 0, 'instances': 0, 'odd': []})
    by_vs = defaultdict(lambda: [0, 0])
    total = Counter()
    for r in rows:
        if r['type'] == 'dispatch':
            last_cs = r['cs'][:8]; dispatches[r['cs']] += 1; continue
        if r['type'] != 'draw': continue
        total['draws'] += 1
        if not r['copied']: total['not_copied'] += 1; continue
        n = r['max_draws']
        if r['count'] != '0':
            if not r['count_copied']: total['count_not_copied'] += 1; continue
            n = min(n, struct.unpack('<I', (root / r['count_file']).read_bytes()[:4])[0])
        recs = records(root / r['file'], r['itype'] >= 0, r['stride'], n)
        g = groups[last_cs]; g['draws'] += 1
        for k, x in enumerate(recs):
            g['records'] += 1; total['records'] += 1
            if x['instances'] == 0 or x['count'] == 0:
                g['zero'] += 1; total['zero'] += 1; by_vs[r['vs'][:12]][0] += 1
            else:
                g['visible'] += 1; g['instances'] += x['instances']; by_vs[r['vs'][:12]][1] += 1
            if x['instances'] > 65536 or x['count'] > 1 << 24 or x.get('first', 0) > 1 << 28:
                g['odd'].append((r['id'], k, x))
    print('dispatches this frame:', sum(dispatches.values()), 'distinct:', len(dispatches))
    for cs, n in dispatches.most_common(12): print('  %s x%d' % (cs, n))
    print('indirect draws %(draws)d, records %(records)d, zero-instance %(zero)d' % total,
          '(%.0f%%)' % (100 * total['zero'] / max(1, total['records'])), {k: v for k, v in total.items() if k not in ('draws', 'records', 'zero')})
    print('\nby preceding dispatch (cs): draws / records / zero / visible / instances')
    for cs, g in sorted(groups.items(), key=lambda kv: -kv[1]['records']):
        print('  %-8s %5d %6d %6d %6d %8d%s' % (cs, g['draws'], g['records'], g['zero'], g['visible'], g['instances'],
              '  ODD %d e.g. %s' % (len(g['odd']), g['odd'][:2]) if g['odd'] else ''))
    print('\nvertex shaders with the most zero-instance records: vs zero/visible')
    for vs, (z, v) in sorted(by_vs.items(), key=lambda kv: -kv[1][0])[:20]:
        print('  %s %5d / %5d' % (vs, z, v))


if __name__ == '__main__':
    main()
