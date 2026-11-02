#!/usr/bin/env python3
"""Render the user's ten retained V2 sentences with FP32, Q8, and the C kernels."""
import argparse,json,re,subprocess,sys,time,wave
from pathlib import Path
import numpy as np
from verify_engines import dequantized

def save(path,audio):
 with wave.open(str(path),'wb') as f:
  f.setnchannels(1);f.setsampwidth(2);f.setframerate(24000);f.writeframes((np.clip(audio,-1,1)*32767).astype('<i2').tobytes())
def main():
 p=argparse.ArgumentParser();p.add_argument('--release',type=Path,required=True);p.add_argument('--source',type=Path,required=True)
 p.add_argument('--reference',type=Path,required=True);p.add_argument('--native',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 import torch
 sys.path.insert(0,str(a.source));from baker4m.infer import Synthesizer,frontend
 from baker4m.synthesis_controls import SynthesisControls,render_controlled
 out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);cases=json.loads(a.reference.read_text())['sentences']
 engine=Synthesizer(a.source/'weights/student_acoustic_spectrum.pt',device='cpu',threads=2)
 controls=dict(speed=.9,volume=1,pause_scale=1.2,vowel_scale=1.05,pad_start_ms=125)
 originals={};report=[]
 for case in cases:
  audio,_=engine.synthesize(case['text'],**controls);originals[case['id']]=audio;save(out/(case['id']+'-fp32.wav'),audio)
 state,_=dequantized(a.release/'tts-v2');engine.acoustic.load_state_dict({k[9:]:v for k,v in state.items() if k.startswith('acoustic.')})
 engine.vocoder.load_state_dict({k[8:]:v for k,v in state.items() if k.startswith('vocoder.')})
 for case in cases:
  stem=case['id'];text=case['text'];symbols=frontend(text,engine.vocab,engine.lexicon);ids=[engine.vocab[v] for v in symbols]
  native=subprocess.run([str(a.native),'frontend',str(a.release),text],text=True,capture_output=True,check=True)
  got_ids=list(map(int,native.stdout.split(':',1)[1].split()));assert ids==got_ids,(stem,'pronunciation mismatch')
  kind=[2 if v=='sp' else 1 if re.search('[1-5]$',v) else 0 for v in symbols]
  phones=out/'phones.bin';np.asarray(list(zip(ids,kind)),dtype='<u4').tofile(phones)
  began=time.monotonic()
  for mode,args in [('mel',[a.release/'tts-v2',phones,out/'mel.bin']),('vocode',[a.release/'tts-v2',out/'mel.bin',out/'audio.f32'])]:
   r=subprocess.run([str(a.native),mode,*map(str,args)],text=True,capture_output=True)
   with (out/'native.log').open('a') as f:f.write(stem+' '+r.stderr)
   if r.returncode:raise RuntimeError(r.stdout+r.stderr)
  c=np.fromfile(out/'audio.f32',dtype='<f4');raw=(out/'mel.bin').read_bytes();frames=int.from_bytes(raw[:4],'little');mel=np.frombuffer(raw[4:],dtype='<f4').reshape(80,frames)
  with torch.inference_mode():
   reference,_=render_controlled(engine.acoustic,torch.tensor([ids]),symbols,.9,SynthesisControls(pause_scale=1.2,vowel_scale=1.05,pad_start_ms=125))
   expected=engine.vocoder(torch.from_numpy(mel.copy())[None])[0,0].numpy()
  corr=float(np.corrcoef(c,expected)[0,1]);mel_error=float(np.max(np.abs(mel-reference[0].T.numpy())))
  assert corr>.999 and mel_error<.015 and np.isfinite(c).all(),(stem,corr,mel_error)
  q8,_=engine.synthesize(text,**controls);audio=np.concatenate([np.zeros(3000,dtype='f4'),c]);save(out/(stem+'-c-q8.wav'),audio);save(out/(stem+'-python-q8.wav'),q8)
  original=originals[stem]
  # Quantization can change duration rounding. Do not time-warp or hide that
  # difference behind an unaligned waveform correlation score.
  row=dict(id=stem,text=text,original_samples=len(original),q8_samples=len(q8),c_samples=len(audio),
   original_peak=float(np.max(np.abs(original))),q8_peak=float(np.max(np.abs(q8))),c_peak=float(np.max(np.abs(c))),
   c_vs_q8_same_mel_correlation=corr,c_mel_max_abs=mel_error,host_native_seconds=time.monotonic()-began,
   listening_accepted=False)
  report.append(row);print(json.dumps(row,ensure_ascii=False),flush=True)
 (out/'VERIFICATION.json').write_text(json.dumps(dict(passed=True,native_sanitized=True,cases=report,scope='numerical integrity; human listening and board speaker acceptance pending'),ensure_ascii=False,indent=2)+'\n')

if __name__=='__main__':main()
