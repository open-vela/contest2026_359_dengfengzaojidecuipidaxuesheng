#!/usr/bin/env python3
"""Check tiled native kernels against all retained C/Q8 PCM references."""
import argparse,hashlib,json,subprocess,time,wave
from pathlib import Path
import numpy as np

def main():
 p=argparse.ArgumentParser();p.add_argument('--native',type=Path,required=True);p.add_argument('--release',type=Path,required=True);p.add_argument('--baseline',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);rows=json.loads((a.baseline/'VERIFICATION.json').read_text())['cases'];report=dict(passed=False,cases=[])
 for row in rows:
  stem=row['id'];raw=out/(stem+'.f32');began=time.monotonic()
  r=subprocess.run([str(a.native),'synthesize',str(a.release),row['text'],str(raw)],capture_output=True,text=True,check=True)
  with (out/'native.log').open('a') as log:log.write(stem+' '+r.stderr)
  audio=np.fromfile(raw,dtype='<f4');assert np.isfinite(audio).all()
  pcm=(np.clip(audio,-1,1)*32767).astype('<i2').tobytes()
  with wave.open(str(a.baseline/(stem+'-c-q8.wav')),'rb') as wav:
   assert wav.getframerate()==24000 and wav.getnchannels()==1 and wav.getsampwidth()==2
   before=wav.readframes(wav.getnframes())
  assert pcm==before,(stem,'PCM changed')
  with wave.open(str(out/(stem+'-c-q8.wav')),'wb') as wav:
   wav.setnchannels(1);wav.setsampwidth(2);wav.setframerate(24000);wav.writeframes(pcm)
  result=dict(id=stem,samples=len(audio),bit_exact_pcm=True,sha256=hashlib.sha256(pcm).hexdigest(),native_seconds=time.monotonic()-began)
  if stem==rows[-1]['id'] and (a.baseline/'audio.f32').exists():
   original=np.fromfile(a.baseline/'audio.f32',dtype='<f4');assert np.array_equal(audio[3000:],original),'Last-case raw float waveform changed';result['bit_exact_raw_float']=True
  report['cases'].append(result);print(stem,'PCM_BIT_EXACT',round(result['native_seconds'],3),flush=True)
 report['passed']=True;report['scope']='All ten retained native C/Q8 PCM references; last case also raw FP32 sample comparison; no time warping'
 (out/'VERIFICATION.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
