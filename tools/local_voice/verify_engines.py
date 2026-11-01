#!/usr/bin/env python3
"""Actual C vs. fixed Python model arithmetic and tokenizer, no board/network."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import zipfile

import numpy as np


def dequantized(folder):
    import torch
    directory=json.loads((folder/'model.json').read_text())
    data=(folder/'weights.bin').read_bytes();state={}
    for row in directory['tensors']:
        raw=memoryview(data)[row['offset']:row['offset']+row['bytes']]
        shape=row['shape']
        if row['dtype']=='f32':x=np.frombuffer(raw,dtype='<f4').copy()
        elif row['dtype']=='q4_32':
            b=np.frombuffer(raw,dtype='u1').reshape(-1,20)
            scale=b[:,:4].copy().view('<f4').reshape(-1,1)
            q=np.empty((len(b),32),dtype='i1');q[:,0::2]=b[:,4:]&15;q[:,1::2]=b[:,4:]>>4;q[q>=8]-=16
            x=(q.astype('f4')*scale).reshape(-1)
        else:
            b=np.frombuffer(raw,dtype='u1').reshape(shape[0],row['row_stride'])
            scale=b[:,:4].copy().view('<f4').reshape(-1,1)
            x=(b[:,4:].view('i1').astype('f4')*scale).reshape(-1)
        state[row['name']]=torch.from_numpy(x.reshape(shape).copy())
    return state,directory['metadata']


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--release',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--tts-source',type=Path,required=True);p.add_argument('--tts-checkpoint',type=Path,required=True)
    p.add_argument('--part',choices=['tokenizer','frontend','llm','tts','all'],default='all')
    p.add_argument('--no-sanitize',action='store_true')
    a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    desktop=root/'app/espdl-quickapp/overlay/apps/system/desktop';exe=out/'local-voice-host'
    flags=['-std=gnu11','-O2','-g','-ffp-contract=off','-fno-omit-frame-pointer']
    if not a.no_sanitize:flags+=['-fsanitize=address,undefined','-fno-pie','-no-pie']
    sources=[desktop/('glass_local_'+name+'.c') for name in ['model','sha','tokenizer','llm','tts','frontend']]
    command=['gcc',*flags,'-I'+str(desktop),*map(str,sources),str(root/'app/espdl-quickapp/tests/local_voice_host.c'),'-lm','-o',str(exe)]
    subprocess.run(command,check=True)
    def run(args):
        r=subprocess.run([str(exe),*map(str,args)],capture_output=True,text=True)
        with (out/'native.log').open('a') as log:log.write(r.stderr)
        if r.returncode:print(r.stdout+r.stderr);r.check_returncode()
        return r.stdout
    report={'sanitizers':not a.no_sanitize,'passed':False}
    if a.part in ('frontend','all'):
        tests=json.loads((a.release/'checks/frontend.json').read_text())
        result=run(['frontend',a.release,*[row['text'] for row in tests]])
        actual=[list(map(int,line.split(':',1)[1].split())) for line in result.splitlines()]
        assert len(actual)==len(tests)
        for row,got in zip(tests,actual):assert got==row['ids'],(row['text'],got,row['ids'])
        report['frontend_cases']=len(tests);print('FRONTEND_EXACT_OK',len(tests),flush=True)
    if a.part in ('tokenizer','all'):
        from tokenizers import Tokenizer
        tok=Tokenizer.from_str((a.release/'edgeidiot/tokenizer.source.json').read_text())
        tests=[row['text'] for row in json.loads((a.release/'checks/tokenizer.json').read_text())]
        tests+=['<|im_start|>user\n你好<|im_end|>\n<|im_start|>assistant\n','x\t  y\n\n',"we're I'll isn't 're",'한글 각 ㄱㅏ','Å Å A\u030a','\u0301\u0323A\u030a','①㍿ Ⅷ', 'a\u0315\u0300','\u1100\u1161\u11a8']
        import random
        rng=random.Random(717)
        alphabet='你好世界天气学习中文１２３ABCabc\n\t \r\u0301\u0323\u0315。！？.,\'’😀éÅ각Å'
        tests+=[''.join(rng.choice(alphabet) for _ in range(rng.randint(1,90))) for _ in range(256)]
        actual=[]
        for at in range(0,len(tests),16):
            result=run(['tokenize',a.release/'edgeidiot/tokenizer.bin',*tests[at:at+16]])
            for line in result.splitlines():
                size,ids=line.split(':',1);ids=list(map(int,ids.split()));assert int(size)==len(ids),(line,at)
                actual.append(ids)
        for text,ids in zip(tests,actual):
            expected=tok.encode(text,add_special_tokens=False).ids
            assert ids==expected,(repr(text),ids,expected)
        assert len(actual)==len(tests);report['tokenizer_cases']=len(tests)
        print('TOKENIZER_EXACT_OK',len(tests),flush=True)
    if a.part in ('llm','all'):
        import torch
        from transformers import LlamaConfig,LlamaForCausalLM
        from tokenizers import Tokenizer
        torch.set_num_threads(1);state,config=dequantized(a.release/'edgeidiot')
        cfg=LlamaConfig(**config);cfg._attn_implementation='eager'
        model=LlamaForCausalLM(cfg).eval();missing=model.load_state_dict(state,strict=False)
        assert not missing.unexpected_keys and set(missing.missing_keys)<={'lm_head.weight'}
        model.tie_weights();tok=Tokenizer.from_str((a.release/'edgeidiot/tokenizer.source.json').read_text())
        prompt='<|im_start|>user\n你好<|im_end|>\n<|im_start|>assistant\n'
        ids=tok.encode(prompt,add_special_tokens=False).ids
        run(['logits',a.release/'edgeidiot',prompt,out/'llm-logits.f32',0])
        got=np.fromfile(out/'llm-logits.f32',dtype='<f4').reshape(-1,8192)
        with torch.inference_mode():expected=model(torch.tensor([ids]),use_cache=False).logits[0].numpy()
        assert got.shape==expected.shape
        error=float(np.max(np.abs(got-expected)));relative=float(np.linalg.norm(got-expected)/np.linalg.norm(expected))
        np.testing.assert_allclose(got,expected,atol=.003,rtol=.003)
        report.update(llm_max_abs=error,llm_relative_l2=relative,llm_tokens=len(ids))
        prefixes=sorted({1,7,8,9,16,17,len(ids)})
        for prefix in prefixes:
            if prefix>len(ids):continue
            run(['prefill',a.release/'edgeidiot',prompt,out/'prefill-logits.f32',prefix])
            batched=np.fromfile(out/'prefill-logits.f32',dtype='<f4')
            assert np.array_equal(batched,got[prefix-1]),('prefill differs from sequential',prefix,float(np.max(np.abs(batched-got[prefix-1]))))
        report['prefill_bit_exact_prefixes']=[n for n in prefixes if n<=len(ids)]
        run(['generate',a.release/'edgeidiot',prompt,out/'llm-reply.txt',4])
        print('LLM_Q4_REFERENCE_OK',error,relative,flush=True)
    if a.part in ('tts','all'):
        import torch
        torch.set_num_threads(2);sys.path.insert(0,str(a.tts_source))
        from baker4m.infer import Synthesizer,frontend
        from baker4m.synthesis_controls import SynthesisControls,render_controlled
        engine=Synthesizer(a.tts_checkpoint,device='cpu',threads=2)
        state,metadata=dequantized(a.release/'tts-v2')
        engine.acoustic.load_state_dict({k[9:]:v for k,v in state.items() if k.startswith('acoustic.')})
        engine.vocoder.load_state_dict({k[8:]:v for k,v in state.items() if k.startswith('vocoder.')})
        text='你好。';symbols=frontend(text,engine.vocab,engine.lexicon)
        ids=[engine.vocab[p] for p in symbols]
        kind=[2 if p=='sp' else 1 if re.search('[1-5]$',p) else 0 for p in symbols]
        np.array(list(zip(ids,kind)),dtype='<u4').tofile(out/'phones.bin')
        run(['mel',a.release/'tts-v2',out/'phones.bin',out/'mel.bin'])
        raw=(out/'mel.bin').read_bytes();frames=int.from_bytes(raw[:4],'little');got=np.frombuffer(raw[4:],dtype='<f4').reshape(80,frames)
        with torch.inference_mode():
            mel,duration=render_controlled(engine.acoustic,torch.tensor([ids]),symbols,.9,SynthesisControls(pause_scale=1.2,vowel_scale=1.05,pad_start_ms=125))
        expected=mel[0].T.numpy()
        assert got.shape==expected.shape, (got.shape,expected.shape,duration.tolist())
        error=float(np.max(np.abs(got-expected)));relative=float(np.linalg.norm(got-expected)/np.linalg.norm(expected))
        print('TTS_MEL_ERROR',error,relative,flush=True)
        np.testing.assert_allclose(got,expected,atol=.015,rtol=.015)
        # First prove vocoder tiling against identical mel conditioning. The
        # acoustic path's separate error is not hidden inside the waveform test.
        with torch.inference_mode():wav=engine.vocoder(torch.from_numpy(got.copy())[None])[0,0].numpy()
        run(['vocode',a.release/'tts-v2',out/'mel.bin',out/'tts.f32'])
        audio=np.fromfile(out/'tts.f32',dtype='<f4');assert audio.shape==wav.shape
        correlation=float(np.corrcoef(audio,wav)[0,1]);peak_error=float(np.max(np.abs(audio-wav)))
        assert np.isfinite(audio).all() and correlation>.999,(correlation,peak_error)
        report.update(tts_frames=frames,tts_mel_max_abs=error,tts_mel_relative_l2=relative,tts_vocoder_correlation=correlation,tts_vocoder_max_abs=peak_error)
        import wave
        with wave.open(str(out/'v2-c-reference.wav'),'wb') as f:
            f.setnchannels(1);f.setsampwidth(2);f.setframerate(24000)
            f.writeframes(np.rint(np.clip(np.concatenate([np.zeros(3000),audio]),-1,1)*32767).astype('<i2').tobytes())
        print('TTS_NATIVE_REFERENCE_OK',report,flush=True)
    report['passed']=True;(out/'VERIFICATION.json').write_text(json.dumps(report,ensure_ascii=False,indent=2))


if __name__=='__main__':main()
