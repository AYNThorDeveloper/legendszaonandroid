#!/usr/bin/env python3
"""Build a bounded binary index for the native read-only RomFS backend."""
import argparse,hashlib,json,struct
from pathlib import Path
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--version', choices=('v0','v112','v121','v143'), default='v0')
# --romfs-dir: another merged RomFS (Z-A 1.0.2 test build); --out-dir: where its romfs_config.h goes (default build/<version>)
parser.add_argument('--romfs-dir', type=Path);parser.add_argument('--out-dir', type=Path);args=parser.parse_args();version=args.version
out=args.out_dir or root/('build' if version=='v0' else f'build/{version}');out.mkdir(parents=True,exist_ok=True)
folder=args.romfs_dir or root.parents[2]/'Uitgepakt/romfs-2.0.2';manifest=json.loads((folder/'index.json').read_text())
assert manifest['status']=='pass' and (folder/'romfs.bin').stat().st_size==manifest['romfs_size']
strings=bytearray();rows=bytearray()
for item in manifest['files']:
    name=item['path'].encode()+b'\0';rows.extend(struct.pack('<QQII',item['offset'],item['size'],len(strings),len(name)));strings.extend(name)
index=struct.pack('<8sQQ',b'TOTKRFS1',len(manifest['files']),24+len(rows))+rows+strings
(folder/'runtime-index.bin').write_bytes(index)
(out/'romfs_config.h').write_text(f'#define ROMFS_IMAGE_SIZE {manifest["romfs_size"]}ULL\n#define ROMFS_IMAGE_SHA256 "{manifest["romfs_sha256"]}"\n#define ROMFS_INDEX_SHA256 "{hashlib.sha256(index).hexdigest()}"\n')
print(json.dumps({'files':len(manifest['files']),'index_bytes':len(index)}))
