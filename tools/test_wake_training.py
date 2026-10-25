#!/usr/bin/env python3
"""Host checks for speaker/take isolation and the fixed-budget augmentation path."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from train_custom_wake import is_human, validate_splits, production_negative_ceiling
from wake_training_expansion import fit_expanded


def rejected(rows, allow=False):
    try:
        validate_splits(rows, allow)
    except ValueError:
        return
    raise AssertionError('Leaking/unknown speaker manifest was accepted')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(); args.output.mkdir(parents=True, exist_ok=True)
    # Keep the reserved negative-event count below 0.8%, including the small
    # 270-clip profile holdouts where interpolated quantiles can admit 1.11%.
    for count in [1,99,100,124,125,250,270,1000,2700]:
        scores=np.linspace(.6,1.2,count)
        cutoff=float(np.float32(production_negative_ceiling(scores)))
        assert np.count_nonzero(scores<cutoff)<=int(count*.008)
    gap=np.array([.4,.41,.42]+[1.] * 267)
    assert np.count_nonzero(gap<(float(np.quantile(gap,.008))*.995))==3
    assert np.count_nonzero(gap<production_negative_ceiling(gap))==2
    assert production_negative_ceiling(np.full(270,1e20))==.995
    for invalid in [[],[float('nan')],[float('inf')]]:
        try:production_negative_ceiling(invalid)
        except ValueError:pass
        else:raise AssertionError('Invalid calibration scores accepted')
    def identity(name):return {'label':1,'pcm':name+'.pcm','sha256':hashlib.sha256(name.encode()).hexdigest()}
    tts = lambda split: {**identity('tts-'+split),'split': split, 'speaker': 'tts-voice', 'source_type': 'synthetic'}
    human = lambda split, take, speaker='person-01': {'split': split, 'speaker': speaker,
        'source_type': 'human', 'recording_id': take,**identity(take)}
    rejected([tts('train'), tts('test')])
    rejected([tts('train'), tts('test')], True)
    rejected([human('train', 'take-1'), human('validation', 'take-2')])
    rejected([human('train', 'same-take'), human('test', 'same-take')], True)
    rejected([human('train', 'take-1', 'unknown')])
    for label in [None,-1,2,'1',True]:rejected([{**tts('train'),'label':label}])
    rejected([{**tts('train'),'sha256':'invalid'}])
    rejected([{**tts('train'),'pcm':''}])
    rejected([tts('train'),{**tts('test'),'speaker':'different-tts','sha256':tts('train')['sha256']}])
    _, takes, overlap = validate_splits([human('train', 'take-1'), human('test', 'take-2')], True)
    assert len(takes) == 2 and overlap == {'person-01'}
    assert is_human(human('train', 'take-1')) and not is_human(tts('train'))
    rows, audio = [], []
    at = np.arange(1200) / 16000
    for label, count in [(1, 18), (0, 48)]:
        for i in range(count):
            rows.append({'split': 'train', 'speaker': 'tts-' + str(i % 2),
                'label': label, 'text':f'wording-{i%3}', 'pcm': f'fixture-{label}-{i}.pcm', 'source_type': 'synthetic'})
            audio.append((1000 * np.sin(2 * np.pi * (150 + i * 11) * at)).astype(np.float32))
    # Non-training audio is intentionally invalid; neither augmentation nor
    # competition may touch it even if it has a negative label.
    for split in ['validation', 'test']:
        rows.append({'split': split, 'speaker': split + '-only', 'label': 0,
                     'pcm': split + '-must-not-be-used.pcm', 'source_type': 'synthetic'})
        audio.append(np.array([np.nan], dtype=np.float32))
    def extract(sound):
        assert len(sound) >= 400 and np.all(np.isfinite(sound))
        return np.interp(np.linspace(0, len(sound) - 1, 416), np.arange(len(sound)), sound).astype(np.float32) / 1000
    base = np.array([extract(sound) if i < 66 else np.zeros(416, dtype=np.float32)
                     for i, sound in enumerate(audio)])
    anchors = list(range(66)); initial = np.repeat(base[:66], 3, axis=0)
    initial_meta = [rows[i] for i in anchors for _ in range(3)]
    bank, _, report = fit_expanded(rows, audio, base, anchors, extract, 20,
                                   args.output, initial, initial_meta)
    assert bank.shape == (198, 416) and np.all(np.isfinite(bank))
    examples = [json.loads(line) for line in (args.output / 'training-examples.jsonl').read_text().splitlines()]
    assert len(examples) == 66 * 120 == report['training_examples']
    assert all(row['source_index'] < 66 for row in examples)
    competition = [row for row in examples if row['noise_kind'] == 'speech']
    assert competition and all(row['background_split'] == 'train'
        and row['background_speaker'] != row['speaker']
        and row['background_pcm'].startswith('fixture-0-') for row in competition)
    assert report['template_budget'] == {'positive': 54, 'negative': 144}
    dtw_out=args.output/'dtw';dtw_out.mkdir(exist_ok=True)
    dtw_bank,dtw_meta,dtw_report=fit_expanded(rows,audio,base,anchors,extract,20,dtw_out,initial,initial_meta,fit_mode='dtw-cover')
    assert dtw_bank.shape==(198,416) and len(dtw_meta)==198
    assert dtw_report['fits'][0]['fixed_clean_anchors']==6
    assert all(item['augmentation']['source_index']<66 for item in dtw_meta)
    result = {'passed': True, 'speaker_take_leakage_rejected': True,
              'unknown_human_speaker_rejected': True, 'tts_overlap_always_rejected': True,
              'unknown_labels_and_bad_pcm_identity_rejected':True,'dtw_cover_fixed_budget_checked':True,
              'small_holdout_false_positive_reserve_checked':True,
              'nontraining_audio_never_read_by_augmentation': True,
              'competition_examples': len(competition), 'training_examples': len(examples),
              'template_shape': list(bank.shape), 'math_accuracy_tested': False}
    (args.output / 'verification.json').write_text(json.dumps(result, indent=2) + '\n')
    print('WAKE_TRAINING_PIPELINE_OK', json.dumps(result), flush=True)


if __name__ == '__main__':
    main()
