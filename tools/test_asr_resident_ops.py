#!/usr/bin/env python3
"""Sanitize full-RAM Q4/Q8 operators against the previous accelerated path."""
import argparse, hashlib, json, os, subprocess
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[1])
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--fixture',type=Path,default=Path(__file__).resolve().parents[1]/'app/espdl-quickapp/tests/asr_resident_ops_host.c')
    a=p.parse_args();repo=a.repo.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    e=repo/'app/espdl-quickapp/experimental';d=repo/'app/espdl-quickapp/overlay/apps/system/desktop';v=e/'esp_dsp'
    lock=json.loads((v/'UPSTREAM.json').read_text(encoding='utf-8'))
    for f in lock['files']:assert hashlib.sha256((v/f['path']).read_bytes()).hexdigest()==f['sha256'],f['path']
    flags=['gcc','-std=gnu11','-O2','-g','-ffp-contract=off','-fsanitize=address,undefined','-fno-pie','-no-pie',
           '-DGLV_ASR_ACCEL_ENABLED','-DGLV_ASR_OFFICIAL_DSP','-I'+str(v/'compat'),'-I'+str(e),'-I'+str(d)]
    renames=['-Ddsps_'+op+'_f32_ansi=glv_esp_dsp_'+op+'_f32_ansi' for op in ('add','sub','mul')]
    sources=[e/(n+'.c') for n in ('local_asr_ops','local_asr_accel','local_asr_parallel','local_asr_dsp')]
    sources += [d/'glass_local_model.c',d/'glass_local_sha.c',a.fixture.resolve()]
    sources += [v/('dsps_'+op+'_f32_ansi.c') for op in ('add','sub','mul')]
    executable=out/'asr-resident-ops'
    subprocess.run([*flags,*renames,*map(str,sources),'-lm','-pthread','-o',str(executable)],check=True)
    env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'}
    run=subprocess.run([str(executable)],capture_output=True,encoding='utf-8',env=env,timeout=120)
    (out/'test.log').write_text(run.stdout+run.stderr,encoding='utf-8');print(run.stdout+run.stderr,end='');run.check_returncode()
    assert 'RESIDENT_OPS_OK cases=28' in run.stdout
    (out/'RESULT.json').write_text(json.dumps({'passed':True,'cases':28,'scope':'Host ASan/UBSan; bit identity to old accelerated math, serial/parallel, odd/padded widths, cancellation, NaN scale rejection, zero row I/O and zero live bytes; not board performance',
        'source_sha256':{str(s.relative_to(repo)) if s.is_relative_to(repo) else str(s):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources}},indent=2)+'\n',encoding='utf-8')
if __name__=='__main__':main()
