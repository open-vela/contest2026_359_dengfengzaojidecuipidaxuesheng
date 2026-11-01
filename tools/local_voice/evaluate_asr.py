#!/usr/bin/env python3
"""Host baseline and concrete residency/operator gate for the specified 14M ASR.

This does not designate a desktop ONNX runtime as an MCU implementation.
"""
import argparse,collections,hashlib,json,resource,tarfile,time,urllib.request,wave
from pathlib import Path
import numpy as np

URL='https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23.tar.bz2'
def main():
 p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
 archive=out/'source.tar.bz2'
 if not archive.exists():
  urllib.request.urlretrieve(URL,archive.with_suffix('.part'));archive.with_suffix('.part').rename(archive)
 with tarfile.open(archive,'r:bz2') as tf:
  for member in tf.getmembers():
   name=Path(member.name)
   if name.is_absolute() or '..' in name.parts or member.issym() or member.islnk():raise ValueError('Unsafe archive member')
   if member.isfile() and (name.name.endswith('.int8.onnx') or name.name in ('tokens.txt','README.md') or name.suffix=='.wav'):
    dest=out/'model'/name.name
    if not dest.exists():dest.parent.mkdir(exist_ok=True);dest.write_bytes(tf.extractfile(member).read())
 import onnx,sherpa_onnx
 from onnx import numpy_helper
 graphs=[]
 for f in sorted((out/'model').glob('*.onnx')):
  model=onnx.load(f);weights=sum(numpy_helper.to_array(v).nbytes for v in model.graph.initializer)
  graphs.append(dict(file=f.name,bytes=f.stat().st_size,sha256=hashlib.sha256(f.read_bytes()).hexdigest(),
    initializer_bytes=weights,operators=dict(collections.Counter((n.domain+':'+n.op_type) for n in model.graph.node)),
    inputs=[dict(name=v.name,dims=[d.dim_value or d.dim_param for d in v.type.tensor_type.shape.dim]) for v in model.graph.input]))
 root=out/'model';before=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss;began=time.monotonic()
 recognizer=sherpa_onnx.OnlineRecognizer.from_transducer(
  tokens=str(root/'tokens.txt'),encoder=str(next(root.glob('encoder*.onnx'))),decoder=str(next(root.glob('decoder*.onnx'))),
  joiner=str(next(root.glob('joiner*.onnx'))),num_threads=1,sample_rate=16000,feature_dim=80,decoding_method='greedy_search',provider='cpu')
 load=time.monotonic()-began;results=[]
 for f in sorted(root.glob('*.wav')):
  with wave.open(str(f)) as w:
   assert w.getnchannels()==1 and w.getsampwidth()==2
   rate=w.getframerate();pcm=np.frombuffer(w.readframes(w.getnframes()),dtype='<i2').astype('float32')/32768
  stream=recognizer.create_stream();began=time.monotonic()
  for at in range(0,len(pcm),1600):
   stream.accept_waveform(rate,pcm[at:at+1600])
   while recognizer.is_ready(stream):recognizer.decode_stream(stream)
  stream.accept_waveform(rate,np.zeros(int(.8*rate),dtype='float32'));stream.input_finished()
  while recognizer.is_ready(stream):recognizer.decode_stream(stream)
  elapsed=time.monotonic()-began
  results.append(dict(file=f.name,text=recognizer.get_result(stream),audio_seconds=len(pcm)/rate,elapsed_seconds=elapsed,rtf=elapsed/(len(pcm)/rate)))
 weight_bytes=sum(v['initializer_bytes'] for v in graphs)
 report=dict(candidate='Chinese Zipformer 14M 2023-02-23',source=URL,archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
  host_runtime_version=sherpa_onnx.__version__,graphs=graphs,host_load_seconds=load,host_peak_rss_kib=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
  pre_session_peak_rss_kib=before,results=results,total_int8_initializer_bytes=weight_bytes,inference_budget_bytes=12*1024*1024,
  resident_weight_gate=weight_bytes<=12*1024*1024,board_tested=False,asr_ready=False,
  conclusion='Stock ONNX resident-weight route exceeds 12 MiB before activations/state. SD streaming is not provided by this runtime; requires a separate quantized operator/state port and latency evaluation. No MCU ASR claim.')
 (out/'ASSESSMENT.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
 print(json.dumps({k:v for k,v in report.items() if k not in ('graphs',)},ensure_ascii=False),flush=True)

if __name__=='__main__':main()
