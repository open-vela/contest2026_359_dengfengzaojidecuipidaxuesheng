#!/usr/bin/env python3
"""Bind byte-identical ASR I/O changes, lifecycle evidence and RV32 objects."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path


def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--artifacts',type=Path,required=True);p.add_argument('--toolchain',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    root=Path(__file__).resolve().parents[2];r=a.artifacts.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    evidence=['asr-io-equivalence1/VERIFICATION.json','asr-batched-transcription1/VERIFICATION.json','asr-batched-lifecycle1/VERIFICATION.json','asr-batched-worker1/VERIFICATION.json','asr-batched-graph-lifecycle1/VERIFICATION.json']
    for name in evidence:
        report=json.loads((r/name).read_text(encoding='utf-8'));assert report['passed']
        for path,digest in report['source_sha256'].items():assert sha(root/path)==digest,(name,path)
    old=r/'candidate8/source/app/espdl-quickapp/experimental';new=root/'app/espdl-quickapp/experimental'
    for name in ('local_asr_ops.c','local_asr_ops.h','local_asr_fbank.c','local_asr_fbank.h'):assert (old/name).read_bytes()==(new/name).read_bytes(),name
    assert (old/'local_asr.c').read_text(encoding='utf-8').split('static int decoder(',1)[1]==(new/'local_asr.c').read_text(encoding='utf-8').split('static int decoder(',1)[1]
    records=[]
    for obj in sorted((r/'asr-batched-transcription1').glob('*-rv32.o')):
        dis=subprocess.check_output([str(a.toolchain/'riscv32-esp-elf-objdump'),'-dr',str(obj)],text=True);(out/(obj.stem+'.txt')).write_text(dis,encoding='utf-8')
        assert not re.search(r'\bf(?:madd|msub|nmadd|nmsub)\.s\b',dis)
        function=''
        for line in dis.splitlines():
            m=re.match(r'^[0-9a-f]+ <([^>]+)>:',line)
            if m and not m[1].startswith('.L'):function=m[1]
            m=re.search(r'R_RISCV_CALL(?:_PLT)?\s+(\S+)',line)
            if m and (re.search(r'__[a-z0-9_]*df[a-z0-9_]*',m[1]) or m[1] in ('cos','sin','pow')):assert function=='glv_asr_fbank_create',(obj.name,function,m[1])
        stack=max(int(v.split('\t')[1]) for v in obj.with_suffix('.su').read_text(encoding='utf-8').splitlines());assert stack<=2048
        records.append(dict(object=obj.name,sha256=sha(obj),max_local_stack_bytes=stack))
    assert len(records)==4
    before=json.loads((r/'asr-sequential-transcription1/VERIFICATION.json').read_text(encoding='utf-8'))['cases'];after=json.loads((r/'asr-batched-transcription1/VERIFICATION.json').read_text(encoding='utf-8'))['cases']
    native_failure=json.loads((r/'device-asr8-sample0/BOARD-ASR.json').read_text(encoding='utf-8'));assert not native_failure['passed'] and native_failure['current']['error']==-5
    result=dict(passed=True,scope='Host byte-equivalent SD sequential read/shared-RHS batching/bounded packed joiner cache; native retest required',asr_ready=False,board_tested=False,asr_io_optimized=True,evidence_sha256={v:sha(r/v) for v in evidence},rv32_objects=records,host_io_comparison=[dict(file=x['file'],before=x['weight_file_io'],after=y['weight_file_io'],memory_peak_after=y['peak_bytes']) for x,y in zip(before,after)],native_candidate8_failure=dict(report_sha256=sha(r/'device-asr8-sample0/BOARD-ASR.json'),error=-5,data_token='ff',elapsed_ms=native_failure['current']['elapsed_ms'],touch_cancel_seconds=native_failure['touch_cancel']['observed_stop_seconds'],root_cause_cured=False),sources={str(v.relative_to(root)):sha(v) for v in new.iterdir() if v.is_file() and v.suffix in ('.c','.h')})
    (out/'VERIFICATION.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print('ASR_IO_EQUIVALENCE_LIFECYCLE_AND_RV32_OK',flush=True)


if __name__=='__main__':main()
