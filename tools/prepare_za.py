#!/usr/bin/env python3
"""Prepare private Z-A 2.0.2 output from the user's merged dump."""
import argparse, hashlib, json, os, shutil, subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
def run(*args, env=None):
    subprocess.run([str(x) for x in args], check=True, cwd=ROOT, env=env)
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--exefs',type=Path,required=True)
    p.add_argument('--romfs',type=Path,required=True)
    a=p.parse_args(); ex=a.exefs.resolve(); rom=a.romfs.resolve()
    expected=json.loads((ROOT/'games/totk/assets/expected-za-2.0.2.json').read_text())
    manifest_path=ex/'extraction-manifest.json'
    if not manifest_path.is_file():manifest_path=ex.parent/'extraction-manifest.json'
    manifest=json.loads(manifest_path.read_text())
    if manifest['program']['title_id']!='0100F43008C44000':raise ValueError('Wrong game title')
    hashes={x['name']:x['sha256'] for x in manifest['files']}
    if hashes != expected['exefs_sha256']:raise ValueError('Unsupported ExeFS version; expected the documented 2.0.2 dump')
    for name in ('main','sdk','rtld','main.npdm'):
        if hashlib.sha256((ex/name).read_bytes()).hexdigest()!=hashes[name]:raise ValueError('Manifest mismatch: '+name)
    private=ROOT.parent/'Uitgepakt'
    extracted=private/'exefs-2.0.2'; rewritten=private/'rewritten-2.0.2'; folder=private/'romfs-2.0.2'
    if any(x.exists() for x in (extracted,rewritten,folder)):raise ValueError('Private outputs exist; use a fresh directory')
    extracted.mkdir(parents=True)
    for name in expected['exefs_sha256']:shutil.copy2(ex/name,extracted/name)
    shutil.copy2(manifest_path,extracted/'extraction-manifest.json')
    folder.mkdir(parents=True)
    (folder/'romfs.bin').symlink_to(rom)
    sys.path.insert(0,str(ROOT/'games/totk/scripts'))
    from extract_romfs import index_romfs
    h=hashlib.sha256()
    with rom.open('rb') as f:
        for b in iter(lambda:f.read(8*1024*1024),b''):h.update(b)
    if rom.stat().st_size!=expected['romfs']['size'] or h.hexdigest()!=expected['romfs']['sha256']:raise ValueError('Unsupported or corrupt merged RomFS')
    (folder/'index.json').write_text(json.dumps(dict(status='pass',romfs_size=rom.stat().st_size,romfs_sha256=h.hexdigest(),files=index_romfs(rom))))
    env=os.environ.copy();env.update(DM_RULESET='native',DM_PRIVATE_GATES='0',DM_THREAD_POINTER_REGISTER='tpidr_el0')
    for name in ('main','sdk'):run(sys.executable,ROOT/'games/totk/third_party/d2r/patch_nso.py',extracted/name,rewritten,name,env=env)
    run(sys.executable,ROOT/'games/totk/scripts/prepare_romfs_backend.py','--version','v143','--romfs-dir',folder)
    run(sys.executable,ROOT/'android/prepare_android_runtime.py','--version','2.0.2')
    print('Private preparation complete. Input hashes match the supported local 2.0.2 reference dump.')
if __name__=='__main__':main()
