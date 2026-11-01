#!/usr/bin/env python3
"""Sanitize RAM capture collection; device adapter remains separately gated."""
import argparse,hashlib,json,subprocess
from pathlib import Path


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    d=root/'app/espdl-quickapp/overlay/apps/system/desktop';sources=[d/'glass_local_capture.c',d/'glass_local_model.c',d/'glass_local_sha.c',root/'app/espdl-quickapp/tests/local_capture_host.c'];exe=out/'capture-collector'
    subprocess.run(['gcc','-std=gnu11','-O1','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(d),*[str(s) for s in sources],'-lm','-o',str(exe)],check=True)
    result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30);(out/'run.log').write_text(result.stdout+result.stderr,encoding='utf-8');print(result.stdout+result.stderr,flush=True);result.check_returncode()
    (out/'VERIFICATION.json').write_text(json.dumps(dict(passed=True,scope='Host ASAN/UBSAN actual RAM collector with deterministic PCM source; native adapter, DMA, microphone and app integration not verified/enabled',asr_ready=False,microphone_tested=False,source_sha256={str(s.relative_to(root)):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources}),indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':main()
