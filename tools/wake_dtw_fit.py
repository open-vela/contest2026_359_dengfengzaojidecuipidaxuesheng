"""Fixed-budget training-only cover selection using the runtime's exact DTW."""
import ctypes
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import numpy as np
from train_custom_wake import is_human


FILES = ['training-features.npy', 'training-labels.npy', 'training-examples.jsonl']


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_cached(cache, out, rows, multiplier, corpus_sha256, extractor_sha256):
    training = json.loads((cache / 'training.json').read_text(encoding='utf-8'))
    previous = json.loads((cache / 'training-expansion.json').read_text(encoding='utf-8'))
    if training['corpus_sha256'] != corpus_sha256 or training['source_sha256']['glass_wake_features.c'] != extractor_sha256:
        raise ValueError('Training feature cache requires the same corpus and feature extractor')
    if previous['multiplier'] != multiplier:
        raise ValueError('Feature cache augmentation multiplier differs')
    for name in FILES:
        if digest(cache / name) != previous['sha256'][name]:
            raise ValueError('Corrupt training feature cache: ' + name)
        if (cache / name).resolve() != (out / name).resolve():
            shutil.copyfile(cache / name, out / name)
    features = np.load(out / FILES[0], allow_pickle=False)
    labels = np.load(out / FILES[1], allow_pickle=False)
    metadata = [json.loads(line) for line in (out / FILES[2]).read_text(encoding='utf-8').splitlines()]
    train = {i: row for i, row in enumerate(rows) if row['split'] == 'train'}
    if features.dtype != np.float32 or features.shape != (len(train) * 6 * multiplier, 416) or labels.shape != (len(features),):
        raise ValueError('Feature cache shape mismatch')
    if len(metadata) != len(features) or not np.all(np.isfinite(features)):
        raise ValueError('Feature cache metadata/nonfinite mismatch')
    for index, item in enumerate(metadata):
        row = train.get(item['source_index'])
        if row is None or row['pcm'] != item['source_pcm'] or row['speaker'] != item['speaker'] or row['label'] != item['label'] or labels[index] != item['label']:
            raise ValueError('Feature cache row identity mismatch')
    print('WAKE_TRAIN_CACHE_VERIFIED', len(features), flush=True)
    return features, labels, metadata, previous


def fit_dtw_cover(rows, base, anchor_ids, features, labels, metadata, multiplier, out, cache_report=None):
    desktop = Path(__file__).resolve().parents[1] / 'app/espdl-quickapp/overlay/apps/system/desktop'
    wrapper = out / 'dtw_fit.c'
    wrapper.write_text('''/* Host fitting only. Exact public DTW, no deployment code. */
#include "glass_wake_features.h"
#include <stddef.h>
void wake_dtw_update(const float *data,size_t count,const float *prototype,float *minimum)
{
  for(size_t i=0;i<count;i++){
    float distance=glass_wake_dtw(data+i*GLASS_WAKE_INPUTS,prototype);
    if(distance<minimum[i])minimum[i]=distance;
  }
}
''', encoding='utf-8')
    library = out / 'dtw-fit.so'
    subprocess.run(['gcc', '-shared', '-fPIC', '-O3', '-I' + str(desktop), str(wrapper),
                    str(desktop / 'glass_wake_features.c'), '-lm', '-o', str(library)], check=True)
    lib = ctypes.CDLL(str(library)); fp = ctypes.POINTER(ctypes.c_float)
    lib.wake_dtw_update.argtypes = [fp, ctypes.c_size_t, fp, fp]
    lib.wake_dtw_update.restype = None
    clean = {item['source_index']: index for index, item in enumerate(metadata) if item['variant'] == 0}
    positive_groups = {}
    for index, row in enumerate(rows):
        if row['split'] == 'train' and row['label'] == 1:
            positive_groups.setdefault((row['speaker'], row['text']), []).append(index)
    positive_anchors = [min(indices, key=lambda i: (rows[i].get('rate', '+0%') != '+0%',
                         abs(int(rows[i].get('rate', '+0%').replace('%', ''))), rows[i]['pcm']))
                        for _, indices in sorted(positive_groups.items())]
    if len(positive_anchors) > 54:
        raise ValueError('Clean speaker/pronunciation groups exceed the 54-positive budget; explicit domain selection is required')
    bank, bank_meta, fits = [], [], []
    for label, budget in [(1, 54), (0, 144)]:
        class_ids = np.flatnonzero(labels == label)
        data = np.ascontiguousarray(features[class_ids], dtype=np.float32)
        locations = {int(global_id): local for local, global_id in enumerate(class_ids)}
        fixed = positive_anchors if label else [i for i in anchor_ids if rows[i]['label'] == 0]
        selected = [locations[clean[index]] for index in fixed]
        if len(set(selected)) != len(selected) or len(selected) > budget:
            raise ValueError('Invalid fixed DTW cover anchors')
        minimum = np.full(len(data), np.inf, dtype=np.float32)
        def update(index):
            lib.wake_dtw_update(data.ctypes.data_as(fp), len(data), data[index].ctypes.data_as(fp), minimum.ctypes.data_as(fp))
        for number, choice in enumerate(selected):
            update(choice)
            if number % 8 == 0:
                print('WAKE_DTW_FIXED', label, number + 1, len(selected), flush=True)
        while len(selected) < budget:
            # Exact DTW farthest cover. Every selected feature is an observed
            # train example, rather than a 5% interpolation between banks.
            choice = int(np.argmax(minimum))
            if choice in selected:
                choice = next(i for i in range(len(data)) if i not in selected)
            selected.append(choice); update(choice)
            if len(selected) % 8 == 0 or len(selected) == budget:
                print('WAKE_DTW_COVER', label, len(selected), budget, 'radius', float(np.max(minimum)), flush=True)
        for number, local in enumerate(selected):
            global_id = int(class_ids[local]); item = metadata[global_id]
            bank.append(data[local]); bank_meta.append({**rows[item['source_index']],
                'prototype_source': 'fixed clean speaker/pronunciation anchor' if number < len(fixed) else 'exact-DTW farthest training cover',
                'training_feature_index': global_id, 'augmentation': item})
        fits.append({'label': label, 'training_examples': len(data), 'budget': budget,
                     'fixed_clean_anchors': len(fixed), 'nearest_dtw_mean': float(np.mean(minimum)),
                     'nearest_dtw_p95': float(np.quantile(minimum, .95)), 'nearest_dtw_max': float(np.max(minimum))})
    result = np.ascontiguousarray(bank, dtype=np.float32)
    if result.shape != (198, 416) or not np.all(np.isfinite(result)):
        raise ValueError('Invalid DTW-cover bank')
    train = [row for row in rows if row['split'] == 'train']
    report = {'multiplier': multiplier, 'base_variants_per_training_clip': 6,
        'variants_per_training_clip': 6 * multiplier, 'base_training_examples': len(train) * 6,
        'training_examples': len(features), 'unique_training_clips': len(train),
        'synthetic_augmented': True, 'new_human_recordings': len({row['recording_id'] for row in train if is_human(row)}),
        'synthetic_sources_only': not any(is_human(row) for row in train),
        'seed': 2026101910, 'fit_splits': ['train'], 'validation_test_used_for_fitting': False,
        'template_budget': {'positive': 54, 'negative': 144}, 'fits': fits,
        'prototype_fitting': 'exact public runtime DTW farthest cover; all clean speaker/pronunciation groups fixed',
        'prototype_trust_region': None, 'feature_cache_reused': cache_report is not None,
        'noise_kind_counts': {kind: sum(item['noise_kind'] == kind for item in metadata) for kind in sorted({item['noise_kind'] for item in metadata})},
        'trainer_sha256': digest(Path(__file__)), 'extractor_sha256': digest(desktop / 'glass_wake_features.c'),
        'sha256': {name: digest(out / name) for name in FILES}}
    (out / 'training-expansion.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('WAKE_DTW_COVER_COMPLETE', len(features), result.shape, flush=True)
    return result, bank_meta, report
