#!/usr/bin/env python3
"""Bind host ASR evidence and audit the actual RV32 operator objects."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path


def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--artifacts',type=Path,required=True);p.add_argument('--toolchain',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    root=Path(__file__).resolve().parents[2];r=a.artifacts.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    evidence=['asr-c-graph7/VERIFICATION.json','asr-c-graph-lifecycle1/VERIFICATION.json','asr-c-fbank1/VERIFICATION.json','asr-c-transcription1/VERIFICATION.json','asr-c-lifecycle1/VERIFICATION.json']
    for rel in evidence:
        report=json.loads((r/rel).read_text(encoding='utf-8'));assert report['passed'] and not report['asr_ready']
        hashes=report.get('source_sha256')
        if isinstance(hashes,dict):
            for file,digest in hashes.items():
                if file=='app/espdl-quickapp/experimental/local_asr_graph.h':continue
                assert sha(root/file)==digest,('changed tested C source',file,rel)
        elif hashes:assert sha(root/'app/espdl-quickapp/experimental/local_asr_fbank.c')==hashes
    # Header-only change since graph test is documentation of the caller's
    # manifest obligation. Bind the tested declaration prefix byte-for-byte.
    graph=json.loads((r/evidence[0]).read_text(encoding='utf-8'))
    header=root/'app/espdl-quickapp/experimental/local_asr_graph.h'
    tested=header.read_text(encoding='utf-8').replace('No Python/ONNX runtime is needed by this API.\n * Caller must validate the external resource manifest (including plan SHA)\n * before open. This loader additionally bounds the plan and hashes data.','No Python/ONNX runtime is needed by this API.')
    assert hashlib.sha256(tested.encode()).hexdigest()==graph['source_sha256'][str(header.relative_to(root))]
    records=[]
    for obj in sorted((r/'asr-c-transcription1').glob('*-rv32.o')):
        dis=subprocess.check_output([str(a.toolchain/'riscv32-esp-elf-objdump'),'-dr',str(obj)],text=True)
        (out/(obj.stem+'.txt')).write_text(dis,encoding='utf-8');assert not re.search(r'\bf(?:madd|msub|nmadd|nmsub)\.s\b',dis)
        function='';calls=[]
        for line in dis.splitlines():
            match=re.match(r'^[0-9a-f]+ <([^>]+)>:',line)
            if match and not match[1].startswith('.L'):function=match[1]
            match=re.search(r'R_RISCV_CALL(?:_PLT)?\s+(\S+)',line)
            if match and (re.search(r'__[a-z0-9_]*df[a-z0-9_]*',match[1]) or match[1] in ('cos','sin','pow')):
                assert function=='glv_asr_fbank_create',(obj.name,function,match[1]);calls.append(dict(function=function,target=match[1]))
        stack=obj.with_suffix('.su').read_text(encoding='utf-8');frames=[int(v.split('\t')[1]) for v in stack.splitlines()]
        assert max(frames)<=2048
        records.append(dict(object=obj.name,sha256=sha(obj),max_local_stack_bytes=max(frames),double_math_init_only=calls))
    assert len(records)==4
    sources=list((root/'app/espdl-quickapp/experimental').glob('*'))
    sources.extend((root/'tools/local_voice').glob('*asr*.py'))
    result=dict(passed=True,scope='Host evidence/source bindings plus actual RV32 ilp32f object checks; not a final firmware link, P4 heap/timing, or ASR deployment',plan_complete=False,asr_ready=False,board_tested=False,evidence={v:sha(r/v) for v in evidence},rv32_objects=records,sources={str(v.relative_to(root)):sha(v) for v in sources if v.is_file()},next_required='Integrate persistent worker/audio ownership and resource-manifest verification, build/link a new candidate, then test P4 file-based Chinese inference/cancel/reserve before enabling microphone/UI ASR')
    (out/'VERIFICATION.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print('ASR_NATIVE_SOURCE_AND_OBJECT_AUDIT_OK',flush=True)


if __name__=='__main__':main()
