#!/usr/bin/env python3
"""Compare captured skinning palettes across consecutive real game frames."""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import numpy as np


def read_frame(path):
    rows = [json.loads(line) for line in (path / 'manifest.jsonl').read_text().splitlines() if line]
    meta = next(r for r in rows if r.get('type') == 'frame')
    end = next(r for r in rows if r.get('type') == 'frame_end')
    if end['failed']:
        raise ValueError(f"{path}: {end['failed']} capture copies failed")
    palettes = [r for r in rows if r.get('type') == 'uniform' and r.get('kind') == 1 and r.get('reg') == 10]
    raw_by_id = {}
    if meta['frame_index'] == 1:
        for r in palettes:
            if not r.get('copied'):
                raise ValueError(f"{path}: first frame is missing palette for draw {r['draw']}")
            raw_by_id[r['draw']] = (path / r['file']).read_bytes()
        draw_to_key = {draw: hashlib.sha256(raw).hexdigest() for draw, raw in raw_by_id.items()}
        bytes_by_key = {hashlib.sha256(raw).hexdigest(): raw for raw in raw_by_id.values()}
    else:
        for r in palettes:
            if r.get('copied'):
                raw_by_id[r['palette_id']] = (path / r['file']).read_bytes()
        if len(raw_by_id) != len({r['palette_id'] for r in palettes}):
            raise ValueError(f"{path}: one or more unique palettes were not copied")
        draw_to_key = {r['draw']: r['palette_id'] for r in palettes}
        bytes_by_key = raw_by_id
    groups = defaultdict(list)
    for draw, key in draw_to_key.items():
        groups[key].append(draw)
    draw_rows = {r['id']: r for r in rows if r.get('type') == 'draw'}
    signatures = {key: tuple(sorted((draw_rows[i]['count'], draw_rows[i]['vs']) for i in ids))
                  for key, ids in groups.items()}
    return meta, end, rows, groups, bytes_by_key, draw_rows, signatures


def matrices(raw):
    if len(raw) % 64:
        raise ValueError(f'palette has non-matrix length {len(raw)}')
    return np.frombuffer(raw, dtype='<f4').reshape(-1, 4, 4).astype(np.float64)


def diff_metrics(reference, current):
    a, b = matrices(reference), matrices(current)
    if a.shape != b.shape:
        return {'matrix_count_changed': [len(a), len(b)]}
    delta = np.abs(a - b)
    per_bone = delta.reshape(len(a), 16).max(axis=1)
    return {
        'max_matrix_delta': float(delta.max(initial=0)),
        'changed_matrix_values_gt_1e-5': int((delta > 1e-5).sum()),
        'changed_bones_gt_1e-5': int((per_bone > 1e-5).sum()),
        'changed_bones_gt_1e-3': int((per_bone > 1e-3).sum()),
    }


def model_bounds(npz_path, raw_palette):
    with np.load(npz_path) as data:
        pos = data['input_positions'].astype(np.float64)
        joints = data['joints'].astype(np.int64)
        weights = data['weights'].astype(np.float64)
    bones = matrices(raw_palette)
    if joints.max(initial=0) >= len(bones):
        raise ValueError(f'{npz_path}: joint exceeds captured palette')
    selected = bones[joints]
    transformed = np.einsum('vc,vkcr->vkr', pos, selected[:, :, :3, :3]) + selected[:, :, 3, :3]
    out = np.einsum('vk,vkr->vr', weights, transformed)
    return {'min': out.min(axis=0).tolist(), 'max': out.max(axis=0).tolist()}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('sequence', type=Path, help='folder containing frame-01, frame-02, ...')
    ap.add_argument('--output', type=Path)
    args = ap.parse_args()
    root = args.sequence.resolve()
    folders = sorted(root.glob('frame-*'))
    if len(folders) < 2:
        raise SystemExit(f'Need at least two frame folders under {root}')
    frames = [read_frame(p) for p in folders]
    _, _, _, base_groups, base_bytes, _, base_signatures = frames[0]
    base_summary = json.loads((folders[0] / 'analysis' / 'comparison.json').read_text())
    draw_vertices = {r['draw']: r['vertices'] for r in base_summary['per_draw']}
    groups = []
    for key, draw_ids in sorted(base_groups.items(), key=lambda item: -sum(draw_vertices.get(i, 0) for i in item[1])):
        ref_raw = base_bytes[key]
        signature = base_signatures[key]
        entry = {
            'reference_palette_sha256': key,
            'reference_draws': sorted(draw_ids),
            'reference_vertices': sum(draw_vertices.get(i, 0) for i in draw_ids),
            'draw_signature': [list(item) for item in signature],
            'frames': [],
        }
        for ix, (meta, end, rows, candidate_groups, current_bytes, _, candidate_signatures) in enumerate(frames):
            if ix == 0:
                match_key = key
            else:
                matches = [ck for ck, sig in candidate_signatures.items() if sig == signature]
                match_key = matches[0] if len(matches) == 1 else None
            if match_key is None or match_key not in current_bytes:
                matches = [ck for ck, sig in candidate_signatures.items() if sig == signature]
                entry['frames'].append({'frame_index': meta['frame_index'], 'frame': meta['frame'],
                                        'candidate_groups': len(matches), 'ambiguous': len(matches) > 1,
                                        'missing': not matches})
                continue
            raw = current_bytes[match_key]
            record = {
                'frame_index': meta['frame_index'], 'frame': meta['frame'],
                'matched_draws': len(candidate_groups[match_key]), 'palette_sha256': hashlib.sha256(raw).hexdigest(),
                'exactly_same_as_first': raw == ref_raw,
            }
            if ix:
                record.update(diff_metrics(ref_raw, raw))
            entry['frames'].append(record)
        groups.append(entry)

    largest = max(groups, key=lambda x: x['reference_vertices'])
    largest_draw = max(largest['reference_draws'], key=lambda i: draw_vertices.get(i, 0))
    pose = {'candidate_draw': largest_draw, 'candidate_group_draws': largest['reference_draws'], 'frames': []}
    for ix, (meta, _, _, candidate_groups, current_bytes, _, candidate_signatures) in enumerate(frames):
        if ix == 0:
            key = largest['reference_palette_sha256']
        else:
            matches = [ck for ck, sig in candidate_signatures.items() if sig == tuple(tuple(x) for x in largest['draw_signature'])]
            key = matches[0] if len(matches) == 1 else None
        if key is None or key not in current_bytes:
            continue
        pose['frames'].append({'frame_index': meta['frame_index'], 'frame': meta['frame'],
            'bounds': model_bounds(folders[0] / 'analysis' / f'd{largest_draw:03d}.npz', current_bytes[key])})

    summary = {
        'sequence': root.name,
        'frame_count': len(frames),
        'frames': [{'frame_index': m['frame_index'], 'frame': m['frame'], 'draws': e['draws'], 'files': e['files']}
                   for m, e, *_ in frames],
        'largest_model_candidate': {'draws': largest['reference_draws'], 'vertices': largest['reference_vertices'], 'sample_draw': largest_draw, 'pose_bounds': pose['frames']},
        'palette_groups': groups,
    }
    out = args.output or root / 'temporal-analysis.json'
    out.write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({
        'sequence': summary['sequence'], 'frame_count': summary['frame_count'],
        'frames': summary['frames'],
        'largest_model_candidate': summary['largest_model_candidate'],
        'palette_groups': [{
            'vertices': g['reference_vertices'], 'draws': g['reference_draws'],
            'frames': [{'frame_index': f['frame_index'], 'same': f.get('exactly_same_as_first'),
                        'changed_bones_gt_1e-5': f.get('changed_bones_gt_1e-5'),
                        'max_matrix_delta': f.get('max_matrix_delta'), 'matched_draws': f.get('matched_draws'),
                        'candidate_groups': f.get('candidate_groups'), 'ambiguous': f.get('ambiguous')}
                       for f in g['frames']]
        } for g in groups],
    }, indent=2))


if __name__ == '__main__':
    main()
