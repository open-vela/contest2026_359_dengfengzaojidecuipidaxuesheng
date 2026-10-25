#!/usr/bin/env python3
"""Train a small MFCC/DTW wake model and calibrate against confusable speech.

Template bank is fitted on training voices only. Threshold uses validation;
held-out test metrics are synthetic development evidence, not human FAR/hour.
"""
import argparse,ctypes,hashlib,json,re,struct,subprocess
from pathlib import Path
import numpy as np
from wake_streaming import calibration_detector
from wake_augmentation import audio_profile, PROFILES

COMMAND_NEGATIVES=['打开 OpenVela','关闭 OpenVela']

def production_negative_ceiling(scores):
    """Reserve 0.8% with strict event comparison, even for small holdouts.

    An interpolated quantile between the third/fourth of 270 negatives can
    admit three false events (1.11%). Use the exact allowed count instead.
    """
    ordered=np.sort(np.asarray(scores,dtype=np.float64))
    if not len(ordered) or not np.all(np.isfinite(ordered)):
        raise ValueError('Calibration requires finite negative observations')
    allowed=int(len(ordered)*.008)
    return min(.995,float(ordered[min(allowed,len(ordered)-1)])*.995)

def is_human(row):
    return row.get('source_type')=='human' or row.get('human_recording') is True or row.get('source','').lower().startswith('human')


def validate_splits(all_rows,allow_human_speaker_overlap=False):
    """Reject speaker/take leakage before reading audio or fitting prototypes."""
    speakers={split:{r['speaker'] for r in all_rows if r['split']==split} for split in ['train','validation','test']}
    for row in all_rows:
        if row['split'] in speakers:
            if type(row.get('label')) is not int or row['label'] not in (0,1):
                raise ValueError('Only explicitly labeled integer 0/1 rows may enter train/validation/test')
            if not isinstance(row.get('pcm'),str) or not row['pcm'] or '\0' in row['pcm']:
                raise ValueError('Training rows require a PCM path')
            if not isinstance(row.get('sha256'),str) or not re.fullmatch('[0-9a-fA-F]{64}',row['sha256']):
                raise ValueError('Training rows require a full PCM SHA256')
    synthetic={split:{r['speaker'] for r in all_rows if r['split']==split and not is_human(r)} for split in speakers}
    if synthetic['train']&synthetic['validation'] or synthetic['train']&synthetic['test'] or synthetic['validation']&synthetic['test']:
        raise ValueError('TTS speakers must remain independent')
    overlap=(speakers['train']&speakers['validation'])|(speakers['train']&speakers['test'])|(speakers['validation']&speakers['test'])
    if overlap and not allow_human_speaker_overlap:raise ValueError('Overlapping human speakers require explicit --allow-human-speaker-overlap')
    if any(not is_human(row) for row in all_rows if row['speaker'] in overlap):raise ValueError('Only explicitly identified human speakers may overlap')
    recordings={}
    audio_splits={}
    for row in all_rows:
        if row.get('sha256'):
            audio_splits.setdefault(row['sha256'],set()).add(row['split'])
        if is_human(row):
            if row['split'] in speakers and row['speaker'].lower().startswith(('unknown','unassigned','unlabeled','pending')):
                raise ValueError('Unknown human speakers cannot enter train/validation/test')
            recording=row.get('recording_id')
            if not recording:raise ValueError('Human rows require recording_id to isolate recording takes before augmentation')
            recordings.setdefault(recording,set()).add(row['split'])
    if any(len(splits)>1 for splits in recordings.values()):raise ValueError('One human recording take cannot cross splits, even through different segments')
    if any(len(splits)>1 for splits in audio_splits.values()):raise ValueError('Identical PCM cannot cross data splits')
    return speakers,recordings,overlap


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--corpus',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--room-augmentation',action='store_true',help='Fit the same-size template bank to noise, echo and playback profiles')
    p.add_argument('--training-multiplier',type=int,default=1,help='Train-only acoustic expansion; 20 uses 120 variants per training clip')
    p.add_argument('--defer-test',action='store_true',help='Fit/calibrate using train/validation only; reserve every test voice and PCM for final external evaluation')
    p.add_argument('--allow-human-speaker-overlap',action='store_true',help='Explicit personal adaptation: a human speaker may cross splits only through different recording_id values')
    p.add_argument('--prototype-fitting',choices=['trust-blend','dtw-cover'],default='trust-blend')
    p.add_argument('--reuse-training-features',type=Path,help='Verified feature cache from the same corpus/extractor; dtw-cover only')
    p.add_argument('--template-bank',type=Path,help='Isolated 54/144 train-only fitted bank directory; still requires the unchanged validation gate')
    p.add_argument('--production-calibration',action='store_true',help='Calibrate the actual joint distance/ratio/early-event policy; static full-clip raw ratios remain diagnostic only')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    if not 1<=a.training_multiplier<=20:p.error('training multiplier must be 1..20')
    if a.reuse_training_features and a.prototype_fitting!='dtw-cover':p.error('Feature cache reuse currently requires dtw-cover')
    desktop=Path(__file__).resolve().parents[1]/'app/espdl-quickapp/overlay/apps/system/desktop'
    shared=a.output/'wake-features.so'
    subprocess.run(['gcc','-shared','-fPIC','-O3',str(desktop/'glass_wake_features.c'),'-lm','-o',str(shared)],check=True)
    lib=ctypes.CDLL(str(shared));fp=ctypes.POINTER(ctypes.c_float);sp=ctypes.POINTER(ctypes.c_int16)
    lib.glass_wake_extract.argtypes=[sp,ctypes.c_size_t,fp];lib.glass_wake_extract.restype=ctypes.c_int
    lib.glass_wake_dtw.argtypes=[fp,fp];lib.glass_wake_dtw.restype=ctypes.c_float
    lib.glass_wake_ratio.argtypes=[fp,fp];lib.glass_wake_ratio.restype=ctypes.c_float
    manifest=json.loads((a.corpus/'corpus.json').read_text(encoding='utf-8'));all_rows=manifest['records'];rng=np.random.default_rng(20261016)
    speakers,recordings,overlap=validate_splits(all_rows,a.allow_human_speaker_overlap)
    rows=[r for r in all_rows if r['split'] in (['train','validation'] if a.defer_test else ['train','validation','test'])]
    unique=[];training_hashes={};duplicates=[]
    for row in rows:
        if row['split']=='train':
            original=training_hashes.get(row['sha256'])
            if original:
                if original['label']!=row['label']:raise ValueError('Identical training PCM has conflicting labels')
                duplicates.append({'pcm':row['pcm'],'canonical_pcm':original['pcm'],'sha256':row['sha256']});continue
            training_hashes[row['sha256']]=row
        unique.append(row)
    rows=unique
    def extract(sound):
        sound=np.clip(sound[:64000],-32768,32767).astype(np.int16);feature=np.zeros(416,dtype=np.float32)
        ret=lib.glass_wake_extract(sound.ctypes.data_as(sp),len(sound),feature.ctypes.data_as(fp))
        if ret:raise ValueError('Invalid/non-speech training input')
        return feature
    base=[];audio=[]
    for row in rows:
        raw=(a.corpus/row['pcm']).read_bytes();assert hashlib.sha256(raw).hexdigest()==row['sha256']
        sound=np.frombuffer(raw,dtype='<i2').astype(np.float32);audio.append(sound);base.append(extract(sound))
    base=np.array(base,dtype=np.float32)
    # The deployment bank stays 18/48 clean anchors times three time scales,
    # independent of the number of training speakers and raw recordings.
    voices=sorted(speakers['train'])
    if len(voices)>18:raise ValueError('At most 18 training speakers fit the fixed clean-anchor budget')
    positive=[];buckets={}
    for number,voice in enumerate(voices):
        candidates=[i for i,r in enumerate(rows) if r['split']=='train' and r['label']==1 and r['speaker']==voice]
        texts=sorted({rows[i]['text'] for i in candidates})
        if not texts:raise ValueError('Each training speaker needs positive examples: '+voice)
        rotated=texts[number%len(texts):]+texts[:number%len(texts)]
        buckets[voice]=sorted(candidates,key=lambda i:(rows[i].get('rate','+0%')!='+0%',rotated.index(rows[i]['text']),rows[i]['pcm']))
    while len(positive)<18:
        changed=False
        for voice in voices:
            if buckets[voice] and len(positive)<18:positive.append(buckets[voice].pop(0));changed=True
        if not changed:raise ValueError('Need at least 18 distinct positive training clips')
    negative=[];buckets={}
    for voice in voices:
        candidates=[i for i,r in enumerate(rows) if r['split']=='train' and r['label']==0 and r['speaker']==voice]
        other=[i for i in positive if rows[i]['speaker']!=voice] or positive
        def hardness(index):
            distances=[float(lib.glass_wake_dtw(base[index].ctypes.data_as(fp),base[j].ctypes.data_as(fp))) for j in other]
            return sum(sorted(distances)[:3])/min(3,len(distances))
        anchors=[next((i for i in candidates if rows[i]['text']==text),None) for text in COMMAND_NEGATIVES]
        anchors=[i for i in anchors if i is not None]
        buckets[voice]=anchors+sorted((i for i in candidates if i not in anchors),key=hardness)
    while len(negative)<48:
        changed=False
        for voice in voices:
            if buckets[voice] and len(negative)<48:negative.append(buckets[voice].pop(0));changed=True
        if not changed:raise ValueError('Need at least 48 distinct negative training clips')
    ids=positive+negative;bank=[];bank_meta=[]
    bank_rng=np.random.default_rng(20261018)
    for index in ids:
        for rate in [1,.85,1.15]:
            sound=audio[index]
            shifted=sound if rate==1 else np.interp(np.arange(0,len(sound),rate),np.arange(len(sound)),sound)
            profile='clean'
            if a.room_augmentation and rate!=1:
                profile=['noise_20db','echo','playback'][int(bank_rng.integers(3))]
                shifted=audio_profile(shifted,profile,bank_rng)
            bank.append(extract(shifted));bank_meta.append({**rows[index],'template_resample_rate':rate,'acoustic_profile':profile})
    templates=np.ascontiguousarray(bank,dtype=np.float32);tp=templates.ctypes.data_as(fp)
    expansion=None
    if a.template_bank:
        provenance=json.loads((a.template_bank/'template-provenance.json').read_text(encoding='utf-8'))
        supplied=np.load(a.template_bank/'templates.npy',allow_pickle=False)
        if supplied.dtype!=np.float32 or supplied.shape!=(198,416) or not np.all(np.isfinite(supplied)):
            raise ValueError('External template bank must be finite float32 198x416')
        bank_meta=provenance['templates']
        if len(bank_meta)!=198 or provenance.get('fit_splits')!=['train'] or provenance.get('validation_test_used_for_fitting') is not False:
            raise ValueError('External bank requires explicit train-only provenance')
        by_digest={r['sha256']:r for r in rows if r['split']=='train'}
        for i,item in enumerate(bank_meta):
            source=by_digest.get(item.get('sha256'))
            if source is None or item.get('split')!='train' or source['speaker']!=item.get('speaker') or source['label']!=item.get('label') or item['label']!=(1 if i<54 else 0):
                raise ValueError('External bank template is not a matching training source')
        if hashlib.sha256((a.template_bank/'templates.npy').read_bytes()).hexdigest()!=provenance['templates_sha256']:
            raise ValueError('External template bank digest mismatch')
        if provenance['corpus_sha256']!=hashlib.sha256((a.corpus/'corpus.json').read_bytes()).hexdigest():
            raise ValueError('External bank corpus mismatch')
        templates=np.ascontiguousarray(supplied);tp=templates.ctypes.data_as(fp)
        expansion=provenance
    elif a.training_multiplier>1:
        from wake_training_expansion import fit_expanded
        templates,bank_meta,expansion=fit_expanded(rows,audio,base,ids,extract,a.training_multiplier,a.output,templates,bank_meta,
            fit_mode=a.prototype_fitting,reuse=a.reuse_training_features,
            corpus_sha256=hashlib.sha256((a.corpus/'corpus.json').read_bytes()).hexdigest(),
            extractor_sha256=hashlib.sha256((desktop/'glass_wake_features.c').read_bytes()).hexdigest())
        tp=templates.ctypes.data_as(fp)
    # Diagnostic arrays are safe to inspect even when the validation gate fails.
    # Deployment GKW/header export still happens only after the gate below.
    np.save(a.output/'templates.npy',templates)
    stream=calibration_detector(desktop,a.output/'calibration',templates,joint_distance=a.production_calibration)
    streaming_validation=[]
    validation_scenarios={profile:[] for profile in PROFILES} if a.room_augmentation else {}
    validation_rng=np.random.default_rng(20261019)
    evaluated_splits=['train','validation'] if a.defer_test else ['train','validation','test']
    results={name:[] for name in evaluated_splits}
    for index,row in enumerate(rows):
        count=6 if row['split']=='train' else 10
        for variant in range(count):
            sound=audio[index].copy()
            if variant:
                rate=rng.uniform(.85,1.15);sound=np.interp(np.arange(0,len(sound),rate),np.arange(len(sound)),sound)
                delay=int(rng.uniform(.008,.045)*16000)
                if len(sound)>delay:sound[delay:]+=rng.uniform(0,.3)*sound[:-delay].copy()
                sound*=rng.uniform(.25,1.6);rms=np.sqrt(np.mean(sound**2)+1)
                sound+=rng.normal(0,rms*10**(-rng.uniform(12,35)/20),len(sound))
            feature=base[index] if not variant else extract(sound)
            ratio=float(lib.glass_wake_ratio(tp,feature.ctypes.data_as(fp)))
            results[row['split']].append((row['label'],ratio,row['speaker'],row['text'],variant))
            if row['split']=='validation':
                pcm=np.concatenate([np.clip(sound,-32768,32767).astype(np.int16),np.zeros(16000,dtype=np.int16)])
                streaming_validation.append((row['label'],stream(pcm),row['speaker'],row['text'],variant))
        if row['split']=='validation' and a.room_augmentation:
            for profile in PROFILES:
                pcm=audio_profile(audio[index],profile,validation_rng)
                validation_scenarios[profile].append((row['label'],stream(pcm),row['speaker'],row['text'],0))
        if index%50==0:print('WAKE_FIT',index,flush=True)
    val=results['validation'];positives=np.array([r[1] for r in val if r[0]]);negatives=np.array([r[1] for r in val if not r[0]])
    stream_pos=np.array([r[1] for r in streaming_validation if r[0]])
    stream_neg=np.array([r[1] for r in streaming_validation if not r[0]])
    # Reserve a margin beneath the documented 1% development false-positive
    # budget, with the stricter offline/streaming validation bound prevailing.
    ceiling=production_negative_ceiling(stream_neg) if a.production_calibration else min(float(np.quantile(negatives,.008)),float(np.quantile(stream_neg,.008)))*.995
    if a.room_augmentation:
        ceiling=min(ceiling,*[(production_negative_ceiling([r[1] for r in rr if not r[0]]) if a.production_calibration else float(np.quantile([r[1] for r in rr if not r[0]],.008))*.995) for rr in validation_scenarios.values()])
    if a.production_calibration:ceiling=min(.995,ceiling)
    desired=float(np.quantile(stream_pos,.98)) if a.production_calibration else max(float(np.quantile(positives,.98)),float(np.quantile(stream_pos,.98)))
    threshold=(ceiling+desired)/2 if desired<ceiling else ceiling
    # The authenticated model stores float32. Report/count with that same value.
    if a.production_calibration:threshold=float(np.float32(threshold))
    if not .5<threshold<1:raise RuntimeError('Calibration outside supported model range')
    def summarize(rr):
        pos=[r[1] for r in rr if r[0]];neg=[r[1] for r in rr if not r[0]]
        clean=[r for r in rr if r[0] and not r[4]]
        return {'samples':len(rr),'positive':len(pos),'negative':len(neg),
          'true_positive':sum(x<threshold for x in pos),'false_positive':sum(x<threshold for x in neg),
          'recall':sum(x<threshold for x in pos)/len(pos) if pos else None,'false_positive_fraction':sum(x<threshold for x in neg)/len(neg) if neg else None,
          'min_negative_ratio':min(neg) if neg else None,'max_positive_ratio':max(pos) if pos else None,
          'clean_recall':sum(r[1]<threshold for r in clean)/len(clean) if clean else None}
    def metrics(name):
        return summarize(streaming_validation if name=='streaming_validation' else validation_scenarios[name] if name in validation_scenarios else results[name])
    locales={r['speaker']:r.get('locale') or ('-'.join(r['speaker'].split('-')[:2]) if not is_human(r) else 'human-local') for r in rows}
    report={'wake_phrase':manifest['wake_phrase'],'architecture':'MFCC13x32; 54 positive + 144 hard-negative templates; banded DTW over first 8 coefficients; three-nearest ratio',
      'synthetic_only':not any(is_human(r) for r in rows),'human_wake_tested':False,'human_false_alarms_per_hour':None,
      'split_policy':manifest['split_policy'],'speakers':{k:sorted(v) for k,v in speakers.items()},'ratio_threshold':threshold,
      'seed':20261016,'metrics':{k:metrics(k) for k in [*evaluated_splits,'streaming_validation']},'templates':bank_meta,
      'per_speaker_metrics':{split:{voice:summarize([x for x in rr if x[2]==voice]) for voice in sorted({x[2] for x in rr})} for split,rr in results.items()},
      'per_locale_metrics':{split:{locale:summarize([x for x in rr if locales[x[2]]==locale]) for locale in sorted({locales[x[2]] for x in rr})} for split,rr in results.items()},
      'human_recording_takes':len(recordings),'human_speaker_overlap_explicit':a.allow_human_speaker_overlap,
      'human_recordings_replayed':any(is_human(r) for r in rows if r['split']!='train'),
      'human_clips_by_split':{split:sum(is_human(r) and r['split']==split for r in all_rows) for split in speakers},
      'human_recording_takes_by_split':{split:len({r['recording_id'] for r in all_rows if r['split']==split and is_human(r)}) for split in speakers},
      'raw_training_clips':sum(r['split']=='train' for r in all_rows),
      'effective_unique_training_clips':sum(r['split']=='train' for r in rows),
      'duplicate_training_aliases_excluded':duplicates,
      'overlapping_human_speakers':sorted(overlap),'test_evaluated':not a.defer_test,
      'holdout_test_pending':a.defer_test,'gate_scope':'validation_only' if a.defer_test else 'validation_and_test',
      'deployment_approved':False,'public_streaming_with_absolute_distance_gate_required':True,
      'room_augmentation':a.room_augmentation,'training_expansion':expansion,'validation_scenarios':{k:metrics(k) for k in validation_scenarios},
      'calibration_policy':'Training-only templates; validation of joint production distance/ratio/early policy; raw full-clip ratio is diagnostic only' if a.production_calibration else 'Training-only templates; offline and production-VAD validation threshold; test is synthetic development evidence, not an untouched final evaluation',
      'production_joint_calibration':a.production_calibration,'raw_feature_metrics_diagnostic_only':a.production_calibration,
      'gate_metrics':['streaming_validation',*list(validation_scenarios)] if a.production_calibration else ['validation','streaming_validation',*list(validation_scenarios)],
      'corpus_sha256':hashlib.sha256((a.corpus/'corpus.json').read_bytes()).hexdigest(),
      'source_sha256':{name:hashlib.sha256(path.read_bytes()).hexdigest() for name,path in [('train_custom_wake.py',Path(__file__)),('wake_streaming.py',Path(__file__).with_name('wake_streaming.py')),('wake_augmentation.py',Path(__file__).with_name('wake_augmentation.py')),('glass_custom_wake.c',desktop/'glass_custom_wake.c'),('glass_custom_wake.h',desktop/'glass_custom_wake.h'),('glass_wake_features.c',desktop/'glass_wake_features.c'),('glass_wake_features.h',desktop/'glass_wake_features.h')]}}
    passed=all(report['metrics'][k]['recall']>=.85 and report['metrics'][k]['clean_recall']>=.85 and report['metrics'][k]['false_positive_fraction']<=.01 for k in (['streaming_validation'] if a.production_calibration else ['validation','streaming_validation'])+([] if a.defer_test else ['test']))
    if a.room_augmentation:
        passed=passed and all(r['recall']>=.85 and r['false_positive_fraction']<=.01 for r in report['validation_scenarios'].values())
    report['synthetic_gate_passed']=passed
    (a.output/'training.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    (a.output/'scores.json').write_text(json.dumps(results,ensure_ascii=False)+'\n',encoding='utf-8')
    (a.output/'streaming-validation.json').write_text(json.dumps(streaming_validation,ensure_ascii=False)+'\n',encoding='utf-8')
    (a.output/'validation-scenarios.json').write_text(json.dumps(validation_scenarios,ensure_ascii=False)+'\n',encoding='utf-8')
    print(json.dumps(report['metrics'],indent=2),flush=True)
    if a.room_augmentation:print('ROOM_VALIDATION',json.dumps(report['validation_scenarios'],indent=2),flush=True)
    if not passed:raise RuntimeError('Synthetic gate failed; model NOT exported')
    header=struct.pack('<4sIIIIfII',b'GKW3',416,54,144,16000,threshold,64000,1)
    data=header+templates.astype('<f4').tobytes();digest=hashlib.sha256(data).digest();(a.output/'nihao_openvela.gkw').write_bytes(data)
    report['model_bytes']=len(data);report['model_sha256']=digest.hex()
    (a.output/'training.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    auth='/* Generated by train_custom_wake.py. SPDX-License-Identifier: Apache-2.0 */\n#pragma once\n'
    auth+='#define GLASS_WAKE_PHRASE "你好 openvela"\n#define GLASS_WAKE_MODEL_BYTES '+str(len(data))+'\n'
    auth+='static const unsigned char glass_wake_model_sha[32]={'+','.join('0x%02x'%x for x in digest)+'};\n'
    (a.output/'glass_wake_model.h').write_text(auth,encoding='utf-8')
    print('CUSTOM_WAKE_TRAINED',digest.hex(),len(data),flush=True)

if __name__=='__main__':main()
