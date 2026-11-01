#!/usr/bin/env python3
"""Compile the capture adapter with actual prepared NuttX flags (no flashing)."""
import argparse,hashlib,json,os,shlex,subprocess
from pathlib import Path


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--apps',type=Path,required=True);p.add_argument('--toolchain',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    root=Path(__file__).resolve().parents[2];desktop=root/'app/espdl-quickapp/overlay/apps/system/desktop';out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    env=dict(os.environ);env['PATH']=str(a.toolchain)+':'+env['PATH']
    command=['make','-s','-C',str(a.apps/'system/desktop'),'APPDIR='+str(a.apps),'TOPDIR='+str(a.apps.parent/'nuttx'),'CROSSDEV='+str(a.toolchain/'riscv32-esp-elf-'),'--eval=local-capture-flags:;$(info CAPTURE_CC=$(CC))$(info CAPTURE_CFLAGS=$(CFLAGS))','local-capture-flags']
    result=subprocess.run(command,capture_output=True,text=True,check=True,env=env)
    cc=shlex.split(next(v.removeprefix('CAPTURE_CC=') for v in result.stdout.splitlines() if v.startswith('CAPTURE_CC=')))
    flags=shlex.split(next(v.removeprefix('CAPTURE_CFLAGS=') for v in result.stdout.splitlines() if v.startswith('CAPTURE_CFLAGS=')))
    assert '-mabi=ilp32f' in flags
    source=[desktop/'glass_local_capture.c',desktop/'glass_local_capture_device.c'];objects=[]
    for file in source:
        target=out/(file.stem+'.o');cmd=[*cc,*flags,'-I'+str(desktop),'-O2','-fstack-usage','-c',str(file),'-o',str(target)]
        result=subprocess.run(cmd,capture_output=True,text=True,env=env);(out/(file.stem+'.log')).write_text(result.stdout+result.stderr,encoding='utf-8');result.check_returncode()
        stack=target.with_suffix('.su').read_text(encoding='utf-8');maximum=max(int(v.split('\t')[1]) for v in stack.splitlines());assert maximum<=4096
        objects.append(dict(file=target.name,sha256=hashlib.sha256(target.read_bytes()).hexdigest(),max_local_stack_bytes=maximum))
    source.extend([desktop/'glass_local_capture.h',desktop/'glass_voice_capture.inc',desktop/'glass_music_pcm.h',desktop/'glass_music.h'])
    report=dict(passed=True,scope='Actual RV32 ilp32f/NuttX headers and flags compile only; no final link, microphone or DMA hardware validation',asr_ready=False,microphone_tested=False,config_sha256=hashlib.sha256((a.apps.parent/'nuttx/.config').read_bytes()).hexdigest(),objects=objects,source_sha256={str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in source})
    (out/'VERIFICATION.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print('LOCAL_CAPTURE_NATIVE_NUTTX_OBJECTS_OK',flush=True)


if __name__=='__main__':main()
