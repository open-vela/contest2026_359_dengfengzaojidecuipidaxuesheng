#!/usr/bin/env python3
"""Seal the SD release after all exporters ran; do not modify source models."""
import argparse, hashlib, json, struct
from pathlib import Path

REQUIRED = ['edgeidiot/model.bin','edgeidiot/weights.bin','edgeidiot/tokenizer.bin',
            'tts-v2/model.bin','tts-v2/weights.bin','tts-v2/frontend.bin','wake/weights.bin']

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('release',type=Path);a=p.parse_args()
    root=a.release
    for name in REQUIRED:
        if not (root/name).is_file(): raise SystemExit('Missing required resource: '+name)
    manifest=json.loads((root/'manifest.json').read_text(encoding='utf-8'))
    manifest['models_ready']=['edgeidiot','tts-v2','wake']
    manifest['asr_ready']=False
    files=[]
    for f in sorted(root.rglob('*')):
        if not f.is_file() or f.name in ('manifest.json','manifest.bin'): continue
        files.append(dict(path=f.relative_to(root).as_posix(),bytes=f.stat().st_size,
                          sha256=hashlib.sha256(f.read_bytes()).hexdigest()))
    manifest['files']=files
    (root/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    # Runtime directory is a fixed little-endian ABI, bounded before allocation.
    entries=[]
    for f in files:
        path=f['path'].encode()
        if len(path)>=96: raise SystemExit('Resource name too long')
        entries.append(struct.pack('<96sI32s',path,f['bytes'],bytes.fromhex(f['sha256'])))
    (root/'manifest.bin').write_bytes(struct.pack('<4sIII',b'GLV1',1,len(entries),0)+b''.join(entries))
    print(json.dumps(dict(release=root.name,files=len(files),bytes=sum(f['bytes'] for f in files))))

if __name__=='__main__':main()
