#!/usr/bin/env python3
"""Compile MIT-licensed runtime support GLSL; never reads game shaders."""
import argparse, shutil, struct, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,default=ROOT/'android/build/shaders')
    a=p.parse_args()
    compiler=shutil.which('glslangValidator')
    if not compiler:p.error('glslangValidator is required to build the runtime support shader')
    with tempfile.TemporaryDirectory(prefix='za-project-shader-') as work:
        spv=Path(work)/'storage_merge.spv'
        subprocess.run([compiler,'-V','--target-env','vulkan1.1',str(ROOT/'android/shaders/storage_merge.comp'),'-o',str(spv)],check=True)
        if shutil.which('spirv-val'):subprocess.run(['spirv-val','--target-env','vulkan1.1',str(spv)],check=True)
        b=spv.read_bytes();words=struct.unpack('<'+str(len(b)//4)+'I',b)
    a.out.mkdir(parents=True,exist_ok=True)
    lines=['/* Generated from MIT-licensed storage_merge.comp; see PokeZ-ARecomp-LICENSE. */','static const uint32_t storage_merge_spv[] = {']
    lines.extend('    '+', '.join(f'0x{x:08x}u' for x in words[i:i+8])+',' for i in range(0,len(words),8))
    lines.append('};')
    (a.out/'storage_merge_spv.h').write_text('\n'.join(lines)+'\n')
if __name__=='__main__':main()
