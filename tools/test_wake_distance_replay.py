#!/usr/bin/env python3
"""Offline paired replay; no microphone, network, training or device writes."""
from pathlib import Path
import argparse,ctypes,hashlib,json,shutil,subprocess,sys,time
import numpy as np
p=argparse.ArgumentParser(description='Paired production wake replay; synthetic development evidence only.')
p.add_argument('--reference',type=Path,required=True,help='Frozen pre-change desktop wake sources')
p.add_argument('--corpus',type=Path,required=True)
p.add_argument('--model',type=Path,required=True,help='Authenticated nihao_openvela.gkw file')
p.add_argument('--output',type=Path,required=True)
p.add_argument('--continuous-minutes',type=int,default=15)
p.add_argument('--profiles-only',action='store_true')
a=p.parse_args()
if not 1<=a.continuous_minutes<=60:p.error('continuous minutes must be 1..60')
h=a.output.resolve();h.mkdir(parents=True,exist_ok=True)
r=Path(__file__).resolve().parents[1]
from wake_streaming import stage_sources
from wake_augmentation import PROFILES,audio_profile
from test_wake_scenarios import Detector
desktop=r/'app/espdl-quickapp/overlay/apps/system/desktop';corpus=a.corpus.resolve()
stage_sources(a.reference.resolve(),h/'reference');stage_sources(desktop,h/'candidate')
runtime=h/'runtime';(runtime/'custom').mkdir(parents=True,exist_ok=True)
shutil.copy2(a.model,runtime/'custom/nihao_openvela.gkw')
libs={}
for name in ['reference','candidate']:
    source=h/name;library=h/(name+'.so')
    subprocess.run(['gcc','-shared','-fPIC','-O3','-I'+str(source),'-I'+str(desktop),*[str(source/f) for f in ['glass_custom_wake.c','glass_wake_features.c','qpk_espdl_sha256.c']],'-lm','-o',str(library)],check=True)
    lib=ctypes.CDLL(str(library));lib.glass_custom_wake_create.argtypes=[ctypes.c_char_p,ctypes.POINTER(ctypes.c_void_p)]
    lib.glass_custom_wake_feed.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_int16),ctypes.c_size_t];lib.glass_custom_wake_destroy.argtypes=[ctypes.c_void_p]
    libs[name]=lib
def measure(name,sound,chunks=(320,)):
    detector=Detector(libs[name],runtime)
    try:return detector.feed(sound,chunks)
    finally:detector.close()
rows=json.loads((corpus/'corpus.json').read_text())['records'];rows=[v for v in rows if v['split'] in ['validation','test']]
audio={}
for row in rows:
    raw=(corpus/row['pcm']).read_bytes();assert hashlib.sha256(raw).hexdigest()==row['sha256'];audio[row['pcm']]=np.frombuffer(raw,dtype='<i2').copy()
report=dict(model_unchanged=True,ratio_threshold_unchanged=True,synthetic_only=True,physical_room_tested=False,
    model_sha256=hashlib.sha256((runtime/'custom/nihao_openvela.gkw').read_bytes()).hexdigest(),profiles={},augmented={},continuous={},chunk_invariance={},differences=[])
def save():(h/'comparison.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
def stats():return {name:dict(positive=0,detected=0,negative=0,false=0) for name in libs}
def evaluate(sound,row,target,profile):
    found={n:len(measure(n,sound)) for n in libs}
    for n,count in found.items():
        positive=bool(row['label']);target[n]['positive' if positive else 'negative']+=1;target[n]['detected' if positive else 'false']+=int(count==1 if positive else count>0)
    if found['reference']!=found['candidate']:
        report['differences'].append(dict(profile=profile,pcm=row['pcm'],label=row['label'],events=found))
rng=np.random.default_rng(20261021)
for profile in [*PROFILES,'far_quiet']:
    result=stats()
    for row in rows:
        sound=audio_profile(audio[row['pcm']],'quiet' if profile=='far_quiet' else profile,rng)
        if profile=='far_quiet':sound=np.round(sound*.2).astype('<i2')
        evaluate(sound,row,result,profile)
    report['profiles'][profile]=result;save();print('PAIRED',profile,result,flush=True)
for seed in ([] if a.profiles_only else [20261016,20261022,20261023]):
    rng=np.random.default_rng(seed);result=stats()
    for row in rows:
        for variant in range(6):
            sound=audio[row['pcm']].astype(float)
            if variant:
                rate=rng.uniform(.85,1.15);sound=np.interp(np.arange(0,len(sound),rate),np.arange(len(sound)),sound)
                delay=int(rng.uniform(.008,.045)*16000)
                if len(sound)>delay:sound[delay:]+=rng.uniform(0,.3)*sound[:-delay].copy()
                sound*=rng.uniform(.25,1.6);rms=np.sqrt(np.mean(sound**2)+1)
                sound+=rng.normal(0,rms*10**(-rng.uniform(12,35)/20),len(sound))
            sound=np.pad(np.clip(sound,-32768,32767).astype('<i2'),(9600,19200))
            evaluate(sound,row,result,'augmented-'+str(seed))
    report['augmented'][str(seed)]=result;save();print('AUGMENTED',seed,result,flush=True)
# A deterministic 15-minute continuous stream for each of two speech beds.
for kind in ([] if a.profiles_only else ['single_voice','overlap']):
    rng=np.random.default_rng(20261022);negative=[v for v in rows if not v['label']]
    size=a.continuous_minutes*60*16000;pieces=[];length=0
    while length<size:
        row=negative[int(rng.integers(len(negative)))];sound=audio[row['pcm']].astype(float)
        active=np.flatnonzero(abs(sound)>300)
        if len(active):sound=sound[max(0,active[0]-320):min(len(sound),active[-1]+321)]
        sound*=1500/max(1,np.sqrt(np.mean(sound**2)));gap=np.zeros(int(rng.uniform(.02,.22)*16000))
        pieces.extend([sound,gap]);length+=len(sound)+len(gap)
    sound=np.concatenate(pieces)[:size]
    if kind=='overlap':sound=sound+.7*np.roll(sound,23471)
    sound=np.pad(np.clip(sound,-32768,32767).astype('<i2'),(16000,32000))
    result={name:len(measure(name,sound,(496,512,1023))) for name in libs}
    report['continuous'][kind]=dict(seconds=len(sound)/16000,events=result);save();print('CONTINUOUS',kind,result,flush=True)
rng=np.random.default_rng(20261021);differences=[]
for row in ([] if a.profiles_only else rows):
    sound=audio_profile(audio[row['pcm']],'clean',rng)
    fixed=len(measure('candidate',sound,(320,)));mixed=len(measure('candidate',sound,(1,7,319,320,511,1023)));large=len(measure('candidate',sound,(4096,)))
    if fixed!=mixed or fixed!=large:differences.append(dict(pcm=row['pcm'],counts=[fixed,mixed,large]))
report['chunk_invariance']=dict(cases=0 if a.profiles_only else len(rows),mismatches=differences)
report['profiles_only']=a.profiles_only
report['source_sha256']={name:{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (h/name).glob('*') if p.is_file()} for name in libs}
report['corpus_sha256']=hashlib.sha256((corpus/'corpus.json').read_bytes()).hexdigest()
groups=[*report['profiles'].values(),*report['augmented'].values()]
report['no_recall_regression']=all(v['candidate']['detected']>=v['reference']['detected'] for v in groups)
report['no_false_increase']=all(v['candidate']['false']<=v['reference']['false'] for v in groups)
report['continuous_no_false_increase']=all(v['events']['candidate']<=v['events']['reference'] for v in report['continuous'].values())
report['passed']=report['no_recall_regression'] and report['no_false_increase'] and report['continuous_no_false_increase'] and not differences
save();print('CANDIDATE_COMPARISON_COMPLETE',report['passed'],flush=True)
if not report['passed']:raise SystemExit('Candidate regressed; keep firmware unchanged')
