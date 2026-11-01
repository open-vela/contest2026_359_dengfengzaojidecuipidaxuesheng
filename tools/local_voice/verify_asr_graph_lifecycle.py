#!/usr/bin/env python3
"""Sanitize complete C graph cancellation, reclamation and invalid SD plans.

All destructive I/O faults are limited to fresh temporary fixture copies.
Never pass an on-device asset directory to the C fault harness.
"""
import argparse,hashlib,json,shutil,struct,subprocess,tempfile
from pathlib import Path


def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--graphs',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    exp=root/'app/espdl-quickapp/experimental';desktop=root/'app/espdl-quickapp/overlay/apps/system/desktop'
    sources=[exp/'local_asr_graph.c',exp/'local_asr_ops.c',desktop/'glass_local_model.c',desktop/'glass_local_sha.c',root/'app/espdl-quickapp/tests/local_asr_graph_host.c']
    exe=out/'asr-graph-lifecycle'
    subprocess.run(['gcc','-std=gnu11','-O1','-g','-ffp-contract=off','-fno-pie','-no-pie','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(desktop),'-I'+str(exp),*[str(v) for v in sources],'-lm','-o',str(exe)],check=True)
    results=[]
    def test(stem,label,reject=False):
        result=subprocess.run([str(exe),str(stem),*(['reject'] if reject else [])],text=True,capture_output=True,timeout=90)
        (out/(label+'.log')).write_text(result.stdout+result.stderr,encoding='utf-8')
        assert result.returncode==0,(label,result.stdout,result.stderr)
        results.append(dict(test=label,passed=True));print(label,result.stdout.strip(),flush=True)
    with tempfile.TemporaryDirectory(prefix='glv-asr-graph-') as temp:
        temp=Path(temp)
        for plan in sorted(a.graphs.glob('*.plan')):
            dest=temp/plan.stem;shutil.copyfile(plan,dest.with_suffix('.plan'));shutil.copyfile(plan.with_suffix('.data'),dest.with_suffix('.data'))
            test(dest,plan.stem+'-lifecycle')
        plan=next(a.graphs.glob('decoder*.plan'));original=plan.read_bytes();data=plan.with_suffix('.data').read_bytes()
        nt=struct.unpack_from('<I',original,16)[0];node_at=80+nt*72
        variants={}
        def word(label,at,value):
            b=bytearray(original);struct.pack_into('<I',b,at,value);variants[label]=b
        word('header-tensor-count',16,0xffffffff);word('header-node-count',20,0xffffffff)
        word('header-input-count',24,65);word('header-data-size',32,0xffffffff)
        word('tensor-rank',80+4,9);word('tensor-overflow',80+12,0xffffffff)
        word('tensor-type',80,0xffffffff);word('tensor-conflicting-flags',80+8,3)
        # First decoder constant is the embedding weight (tensor 2).
        meta=json.loads(plan.with_suffix('.json').read_text(encoding='utf-8'))
        ci=next(i for i,t in enumerate(meta['tensors']) if t['flags']&1)
        word('constant-offset',80+ci*72+44,0xffffffff);word('constant-format',80+ci*72+52,3)
        word('constant-row-shape',80+ci*72+56,0xffffffff)
        word('node-arity',node_at+4,0);word('node-output-arity',node_at+8,2)
        word('node-future-input',node_at+12,nt-1);word('node-output-duplicate',node_at+44,0)
        word('node-opcode',node_at,0xffffffff)
        variants['truncated-plan']=original[:-1];variants['trailing-plan']=original+b'\0'
        b=bytearray(original);b[36]^=1;variants['payload-sha']=b
        for name,value in variants.items():
            stem=temp/name;stem.with_suffix('.plan').write_bytes(value);stem.with_suffix('.data').write_bytes(data);test(stem,name,True)
    result=dict(passed=True,scope='ASAN/UBSAN host full-graph lifecycle and isolated corrupt/truncated assets; no real SD removal or P4 verification',source_sha256={str(s.relative_to(root)):sha(s) for s in sources},cases=results,asr_ready=False,board_tested=False)
    (out/'VERIFICATION.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':main()
