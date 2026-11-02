#!/usr/bin/env python3
"""Validate a real storage-skinning capture and compare GPU positions with CPU linear-blend skinning."""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import numpy as np

# NVN formats used by the recorded skinning vertex layouts.
FORMATS={
 0x03:('u1',1,1),0x08:('<u2',1,1),0x0b:('<u4',1,1),
 0x0f:('u1',2,1),0x14:('<u2',2,1),0x17:('<u4',2,1),
 0x1b:('u1',3,1),0x20:('<u2',3,1),0x23:('<u4',3,1),
 0x27:('u1',4,1),0x2c:('<u2',4,1),0x2f:('<u4',4,1),
 0x05:('<f2',1,1),0x0a:('<f4',1,1),0x11:('<f2',2,1),0x16:('<f4',2,1),
 0x1d:('<f2',3,1),0x22:('<f4',3,1),0x29:('<f2',4,1),0x2e:('<f4',4,1),
 0x25:('u1',4,255),0x2a:('<u2',4,65535),0x26:('i1',4,127),0x2b:('<i2',4,32767),
}

def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('capture',type=Path);ap.add_argument('--output',type=Path)
 args=ap.parse_args();root=args.capture.resolve();out=args.output or root/'analysis';out.mkdir(parents=True,exist_ok=True)
 rows=[json.loads(line) for line in (root/'manifest.jsonl').read_text().splitlines() if line]
 end=next((r for r in rows if r['type'] in ('end','frame_end')),None)
 if end is None:raise ValueError('Capture manifest is not finished')
 resources=[r for r in rows if 'file' in r]
 problems=[]
 for r in resources:
  p=root/r['file']
  if not r.get('copied') or not p.is_file() or p.stat().st_size!=r['bytes']:problems.append(r)
 if problems:raise ValueError('Incomplete capture resources: '+json.dumps(problems[:8]))
 if end['failed'] or len(resources)!=end['files']:raise ValueError('Capture reports failed or missing resources')
 bydraw=defaultdict(list)
 for r in rows:
  if 'draw' in r:bydraw[r['draw']].append(r)
 draws=[r for r in rows if r['type']=='draw'];results=[];groups=defaultdict(list)
 for draw in draws:
  ident=draw['id'];parts=bydraw[ident];attrs={r['location']:r for r in parts if r['type']=='attribute'}
  streams={r['stream']:r for r in parts if r['type']=='stream'};outputs={r['index']:r for r in parts if r['type']=='output'}
  uniforms=[r for r in parts if r['type']=='uniform'];count=draw['count'];first=draw['first']
  record={'draw':ident,'vertices':count,'shader':draw['vs'],'first':first,'cursor':draw['cursor']}
  try:
   palette_info=next(r for r in uniforms if r['kind']==1 and r['reg']==10)
   raw_palette=(root/palette_info['file']).read_bytes()
   if len(raw_palette)%64:raise ValueError('Palette is not a whole number of 4x4 matrices')
   bones=np.frombuffer(raw_palette,dtype='<f4').reshape(-1,4,4).astype(np.float64)
   def attribute(location):
    a=attrs[location];stream=streams[a['stream']];dtype,components,normalization=FORMATS[a['format']]
    raw=(root/stream['file']).read_bytes();dt=np.dtype(dtype);start=a['offset']+(0 if stream['divisor'] else first*stream['stride'])
    stride=0 if stream['divisor'] else stream['stride']
    value=np.ndarray((count,components),dtype=dt,buffer=raw,offset=start,strides=(stride,dt.itemsize)).copy()
    if normalization!=1:
     value=value.astype(np.float64)/normalization
     if dt.kind=='i':value=np.maximum(value,-1)
    return value
   position=attribute(0)[:,:3].astype(np.float64);joints=attribute(12).astype(np.int64);weights=attribute(13).astype(np.float64)
   if joints.shape[1]!=4 or weights.shape[1]!=4:raise ValueError('Expected four joints/weights')
   record['max_joint']=int(joints.max());record['palette_matrices']=len(bones)
   if joints.min()<0 or joints.max()>=len(bones):raise ValueError('Joint index outside captured palette')
   matrices=bones[joints]
   transformed=np.einsum('vc,vkcr->vkr',position,matrices[:,:,:3,:3])+matrices[:,:,3,:3]
   expected=np.einsum('vk,vkr->vr',weights,transformed)
   output=outputs[0];raw=(root/output['file']).read_bytes();start=draw['cursor']*output['stride']
   actual=np.ndarray((count,3),dtype='<f4',buffer=raw,offset=start,strides=(output['stride'],4)).copy().astype(np.float64)
   difference=np.abs(actual-expected);finite=np.isfinite(actual).all(axis=1)&np.isfinite(expected).all(axis=1)
   tolerance=0.0001+0.00001*np.abs(expected)
   bad=(difference>tolerance).any(axis=1)|~finite
   record.update(max_abs_error=float(np.nanmax(difference)),bad_vertices=int(bad.sum()),nonfinite_vertices=int((~finite).sum()),
     weights_sum_min=float(weights.sum(axis=1).min()),weights_sum_max=float(weights.sum(axis=1).max()),
     actual_min=np.nanmin(actual,axis=0).tolist(),actual_max=np.nanmax(actual,axis=0).tolist(),
     palette_address=palette_info['address'],palette_sha256=hashlib.sha256(raw_palette).hexdigest())
   np.savez_compressed(out/f'd{ident:03d}.npz',actual=actual,expected=expected,input_positions=position,joints=joints,weights=weights,bones=bones)
   groups[record['palette_sha256']].append((ident,actual))
  except (ValueError,KeyError,StopIteration,IndexError) as e:record['error']=str(e)
  results.append(record)
 summary={'frame':rows[0]['frame'],'draws':len(draws),'resources':len(resources),'bytes':sum(r['bytes'] for r in resources),
   'analyzed_draws':sum('bad_vertices' in r for r in results),'vertices':sum(r['vertices'] for r in results),
   'bad_vertices':sum(r.get('bad_vertices',0) for r in results),'errors':[r for r in results if 'error' in r],
   'max_abs_error':max((r.get('max_abs_error',0) for r in results),default=0),'palette_groups':len(groups),'per_draw':results}
 (out/'comparison.json').write_text(json.dumps(summary,indent=2)+'\n')
 index=[]
 for number,(key,items) in enumerate(sorted(groups.items(),key=lambda kv:-sum(len(points) for _,points in kv[1])),1):
  points=np.concatenate([p for _,p in items]);filename=f'palette-{number:03d}.ply'
  with (out/filename).open('w') as f:
   f.write('ply\nformat ascii 1.0\nelement vertex %d\nproperty float x\nproperty float y\nproperty float z\nend_header\n'%len(points))
   np.savetxt(f,points,fmt='%.8g')
  index.append({'group':number,'palette_sha256':key,'draws':[i for i,_ in items],'vertices':len(points),'pointcloud':filename})
 (out/'palette-groups.json').write_text(json.dumps(index,indent=2)+'\n')
 print(json.dumps({k:v for k,v in summary.items() if k!='per_draw'},indent=2))

if __name__=='__main__':main()
