#!/usr/bin/env python3
"""Plan/install a private local Z-A build on an explicitly selected Android device."""
import argparse, hashlib
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial',required=True)
    p.add_argument('--fonts',required=True,type=Path,help='Your private shared-fonts directory, including fonts.list')
    p.add_argument('--apply',action='store_true')
    a=p.parse_args();private=ROOT.parent/'Uitgepakt';fonts=a.fonts.resolve()
    apk=ROOT/'android/game_activity/build/ZA-Game-Loader.apk'
    modules=[private/'rewritten-2.0.2'/f'{n}.patched.bin' for n in ('main','sdk')]
    romfs=private/'romfs-2.0.2'
    for item in [apk,*modules,romfs/'romfs.bin',romfs/'runtime-index.bin',fonts/'fonts.list']:
        if not item.is_file():p.error('Missing private installation input: '+str(item))
    rows=(fonts/'fonts.list').read_text().splitlines()
    if len(rows)!=6:p.error('fonts.list must describe all six system fonts')
    for font_type,row in enumerate(rows):
        fields=row.split()
        if len(fields)!=3 or fields[0]!=str(font_type) or Path(fields[1]).name!=fields[1] or not (fonts/fields[1]).is_file():p.error('Invalid or incomplete fonts.list')
        if hashlib.sha256((fonts/fields[1]).read_bytes()).hexdigest()!=fields[2]:p.error('Shared font digest mismatch')
    device='/data/local/tmp/za-android-202'
    print('Install private local APK, modules, merged RomFS/index and your shared fonts on '+a.serial+'.')
    print('No game launch, save transfer, driver installation or existing environment-file replacement.')
    if not a.apply:print('Plan only. Add --apply to execute.');return
    def run(*args):subprocess.run(['adb','-s',a.serial,*map(str,args)],check=True)
    run('install','-r',apk)
    run('shell','mkdir','-p',device)
    for item in [*modules,romfs/'romfs.bin',romfs/'runtime-index.bin']:run('push',item,device+'/')
    run('shell','mkdir','-p',device+'/shared-fonts')
    run('push',fonts/'fonts.list',device+'/shared-fonts/')
    for row in (fonts/'fonts.list').read_text().splitlines():run('push',fonts/row.split()[1],device+'/shared-fonts/')
if __name__=='__main__':main()
