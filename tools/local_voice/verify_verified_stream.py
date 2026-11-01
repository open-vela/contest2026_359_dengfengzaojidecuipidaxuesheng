#!/usr/bin/env python3
"""Real C manifest/SHA/stream ownership tests; no hardware or original files changed."""
import argparse,hashlib,json,shutil,struct,subprocess,tempfile
from pathlib import Path

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    repo=Path(__file__).resolve().parents[2];d=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
    template=out/'fixture';template.mkdir()
    weights=struct.pack('<f',1)+bytes([0x11]*16)+struct.pack('<f',2)+bytes([0x11]*16)
    tensor=b'sample.weight'.ljust(80,b'\0')+struct.pack('<9I',2,32,0,0,2,1,0,40,20)
    index=b'GLM1'+struct.pack('<III',1,1,40)+hashlib.sha256(weights).digest()+tensor
    files={'edgeidiot/model.bin':index,'edgeidiot/weights.bin':weights,'edgeidiot/tokenizer.bin':b'tokenizer',
           'tts-v2/model.bin':index,'tts-v2/weights.bin':weights,'tts-v2/frontend.bin':b'frontend','wake/weights.bin':b'wake'}
    manifest=b'GLV1'+struct.pack('<III',1,len(files),0)
    for name,data in files.items():
        path=template/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
        manifest+=name.encode().ljust(96,b'\0')+struct.pack('<I',len(data))+hashlib.sha256(data).digest()
    (template/'manifest.bin').write_bytes(manifest)
    sources=[d/f'glass_local_{n}.c' for n in ('model','sha','assets')]+[repo/'app/espdl-quickapp/tests/local_verified_stream_host.c']
    exe=out/'verified-stream'
    subprocess.run(['gcc','-std=gnu11','-O2','-g','-ffp-contract=off','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(d),*map(str,sources),'-Wl,--wrap=fread','-lm','-o',str(exe)],check=True)
    cases=('baseline','reuse','corrupt','truncate','replace','missing','index','cancel','budget')
    for case in cases:
        # Linux VFS semantics are required for the open-stream rename test;
        # Windows DrvFS prevents replacing a pathname with an open reader.
        with tempfile.TemporaryDirectory(prefix='glv-verified-') as folder:
            fixture=Path(folder)/case;shutil.copytree(template,fixture)
            r=subprocess.run([str(exe),str(fixture),case],capture_output=True,text=True,timeout=30)
        (out/f'{case}.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,flush=True);r.check_returncode()
    deps=[*sources,d/'glass_local_model.h',d/'glass_local_assets.h',Path(__file__)]
    (out/'VERIFICATION.json').write_text(json.dumps(dict(passed=True,cases=cases,sanitizers=['address','undefined'],
        source_sha256={str(f.relative_to(repo)):sha(f) for f in deps},scope='Request-local verified stream transfer, digest/index/path identity binding, cancellation/errors/budget and exact model arithmetic'),indent=2)+'\n')
if __name__=='__main__':main()
