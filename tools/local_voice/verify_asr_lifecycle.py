#!/usr/bin/env python3
"""ASAN/UBSAN complete C recognizer cancellation, reuse and token asset faults."""
import argparse,hashlib,json,shutil,struct,subprocess,tempfile,wave
from pathlib import Path


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--assets',type=Path,required=True);p.add_argument('--wav',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    exp=root/'app/espdl-quickapp/experimental';desktop=root/'app/espdl-quickapp/overlay/apps/system/desktop'
    sources=[exp/v for v in ('local_asr.c','local_asr_graph.c','local_asr_ops.c','local_asr_fbank.c')]+[desktop/'glass_local_model.c',desktop/'glass_local_sha.c',root/'app/espdl-quickapp/tests/local_asr_host.c']
    exe=out/'asr-lifecycle'
    subprocess.run(['gcc','-std=gnu11','-O1','-g','-ffp-contract=off','-fno-pie','-no-pie','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(desktop),'-I'+str(exp),*[str(s) for s in sources],'-lm','-o',str(exe)],check=True)
    cases=[]
    with tempfile.TemporaryDirectory(prefix='glv-asr-lifecycle-') as temp:
        temp=Path(temp)
        for f in a.assets.iterdir():
            if f.suffix in ('.plan','.data') or f.name=='tokens.bin':shutil.copyfile(f,temp/f.name)
        with wave.open(str(a.wav)) as wav:
            assert wav.getnchannels()==1 and wav.getsampwidth()==2 and wav.getframerate()==16000
            (temp/'input.pcm').write_bytes(wav.readframes(wav.getnframes()))
        def run(label,reject=False):
            result=subprocess.run([str(exe),str(temp),str(temp/'input.pcm'),*(['reject'] if reject else [])],capture_output=True,text=True,timeout=180)
            (out/(label+'.log')).write_text(result.stdout+result.stderr,encoding='utf-8')
            assert result.returncode==0,(label,result.stdout,result.stderr)
            print(label,result.stdout.strip(),flush=True);cases.append(dict(case=label,passed=True))
        run('normal-cancel-oom-reentrant-reuse')
        original=(temp/'tokens.bin').read_bytes()
        variants={'short-header':original[:16],'short-payload':original[:-1]}
        wrong=bytearray(original);wrong[48]^=1
        variants['payload-sha']=wrong
        wrong=bytearray(original);struct.pack_into('<I',wrong,8,5538);variants['vocab-size']=wrong
        wrong=bytearray(original);struct.pack_into('<I',wrong,48+4,0);wrong[16:48]=hashlib.sha256(wrong[48:]).digest();variants['duplicate-offset']=wrong
        wrong=bytearray(original);wrong[48+(5537+1)*4]=0xff;wrong[16:48]=hashlib.sha256(wrong[48:]).digest();variants['bad-utf8']=wrong
        for label,data in variants.items():(temp/'tokens.bin').write_bytes(data);run(label,True)
    report=dict(passed=True,scope='ASAN/UBSAN complete C recognizer with two real-clip reuse runs, cancellation at four checkpoints, OOM and callback rejection/reentrancy, isolated token asset corruption; not P4 audio/lifecycle acceptance',asr_ready=False,board_tested=False,cases=cases,source_sha256={str(s.relative_to(root)):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources})
    (out/'VERIFICATION.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':main()
