#!/usr/bin/env python3
"""Strict paired production wake replay and independent native CPU benchmarks.

Synthetic development evidence only: no device, microphone, network or training.
The host trace observes the original extractor and matcher once per evaluation.
Benchmarks use separate, uninstrumented DSOs and exclude loading and file I/O.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time

import numpy as np
from wake_augmentation import PROFILES, audio_profile
from wake_streaming import stage_sources


PROBE_SOURCE = r'''
#include "glass_custom_wake.h"
#include "glass_wake_features.h"
#include <stdint.h>
#include <string.h>
#define TRACE_CAPACITY 32
struct runtime_trace {
  int32_t extract_result;
  uint32_t samples;
  float feature[GLASS_WAKE_INPUTS],ratio,positive_distance;
  uint32_t matched;
};
struct runtime_snapshot {
  int32_t result;
  uint32_t evaluations,score_bits,trace_count;
};
static struct runtime_trace traces[TRACE_CAPACITY];
static unsigned trace_count;
static int runtime_extract(const int16_t *pcm,size_t samples,float *out)
{
  int ret=glass_wake_extract(pcm,samples,out);
  if(trace_count<TRACE_CAPACITY){
    struct runtime_trace *trace=&traces[trace_count];
    memset(trace,0,sizeof(*trace));trace->extract_result=ret;trace->samples=samples;
    if(!ret)memcpy(trace->feature,out,sizeof(trace->feature));
  }
  trace_count++;return ret;
}
static float runtime_match(const float *weights,const float *feature,float *distance)
{
  float ret=glass_wake_ratio_distance(weights,feature,distance);
  if(trace_count&&trace_count<=TRACE_CAPACITY){
    struct runtime_trace *trace=&traces[trace_count-1];
    trace->ratio=ret;trace->positive_distance=*distance;trace->matched=1;
  }
  return ret;
}
#define glass_wake_extract runtime_extract
#define glass_wake_ratio_distance runtime_match
#include "glass_custom_wake.c"
#undef glass_wake_extract
#undef glass_wake_ratio_distance
void runtime_observe(struct glass_custom_wake *state,const int16_t *pcm,
                     size_t samples,struct runtime_snapshot *snapshot)
{
  trace_count=0;snapshot->result=glass_custom_wake_feed(state,pcm,samples);
  snapshot->evaluations=glass_custom_wake_evaluations(state);
  float score=glass_custom_wake_score(state);
  memcpy(&snapshot->score_bits,&score,sizeof(score));snapshot->trace_count=trace_count;
}
const void *runtime_trace_data(void){return traces;}
unsigned runtime_trace_size(void){return sizeof(struct runtime_trace);}
'''


class Snapshot(ctypes.Structure):
    _fields_ = [('result', ctypes.c_int32), ('evaluations', ctypes.c_uint32),
                ('score_bits', ctypes.c_uint32), ('trace_count', ctypes.c_uint32)]


class Trace(ctypes.Structure):
    _fields_ = [('extract_result', ctypes.c_int32), ('samples', ctypes.c_uint32),
                ('feature', ctypes.c_float * 416), ('ratio', ctypes.c_float),
                ('positive_distance', ctypes.c_float), ('matched', ctypes.c_uint32)]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compile_runtime(cc, desktop, source, output):
    sources = stage_sources(source, output)
    flags = [cc, '-std=gnu11', '-shared', '-fPIC', '-O3',
             '-I' + str(output), '-I' + str(desktop)]
    production = output / 'runtime.so'
    subprocess.run([*flags, *map(str, sources), '-lm', '-o', str(production)], check=True)
    wrapper = output / 'runtime_probe.c'
    wrapper.write_text(PROBE_SOURCE, encoding='utf-8')
    probe = output / 'probe.so'
    subprocess.run([*flags, str(wrapper), *map(str, sources[1:]),
                    '-lm', '-o', str(probe)], check=True)
    lib = ctypes.CDLL(str(probe))
    lib.glass_custom_wake_create.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
    lib.glass_custom_wake_create.restype = ctypes.c_int
    lib.glass_custom_wake_destroy.argtypes = [ctypes.c_void_p]
    lib.glass_custom_wake_destroy.restype = None
    lib.runtime_observe.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16),
                                   ctypes.c_size_t, ctypes.POINTER(Snapshot)]
    lib.runtime_observe.restype = None
    lib.runtime_trace_data.restype = ctypes.c_void_p
    lib.runtime_trace_size.restype = ctypes.c_uint
    if lib.runtime_trace_size() != ctypes.sizeof(Trace):
        raise RuntimeError('Host trace layout mismatch')
    return lib, production


def trace_difference(left, right):
    for index in range(len(left) // ctypes.sizeof(Trace)):
        start = index * ctypes.sizeof(Trace)
        a = Trace.from_buffer_copy(left[start:start + ctypes.sizeof(Trace)])
        b = Trace.from_buffer_copy(right[start:start + ctypes.sizeof(Trace)])
        for name, _ in Trace._fields_:
            offset = getattr(Trace, name).offset
            size = 416 * 4 if name == 'feature' else 4
            if left[start + offset:start + offset + size] != right[start + offset:start + offset + size]:
                result = {'evaluation_in_feed': index, 'field': name}
                if name == 'feature':
                    result['coefficient'] = next(i for i in range(416)
                        if left[start + offset + i * 4:start + offset + i * 4 + 4]
                        != right[start + offset + i * 4:start + offset + i * 4 + 4])
                else:
                    result.update(reference=getattr(a, name), candidate=getattr(b, name))
                return result
    return {'field': 'trace_bytes'}


def paired_clip(libraries, runtime, sound, chunks, counters):
    states = {}
    try:
        for name, lib in libraries.items():
            state = ctypes.c_void_p()
            ret = lib.glass_custom_wake_create(os.fsencode(runtime), ctypes.byref(state))
            if ret:
                raise RuntimeError(f'{name} model authentication failed: {ret}')
            states[name] = state
        cursor = step = events = 0
        while cursor < len(sound):
            count = min(chunks[step % len(chunks)], len(sound) - cursor)
            pcm = sound[cursor:cursor + count]
            snapshots, traces = {}, {}
            for name, lib in libraries.items():
                value = Snapshot()
                lib.runtime_observe(states[name], pcm.ctypes.data_as(ctypes.POINTER(ctypes.c_int16)),
                                    count, ctypes.byref(value))
                if value.result < 0 or value.trace_count > 32:
                    raise RuntimeError(f'{name} invalid feed result or trace overflow')
                snapshots[name] = value
                traces[name] = ctypes.string_at(lib.runtime_trace_data(), value.trace_count * ctypes.sizeof(Trace))
            counters['feed_pairs'] += 1
            if bytes(snapshots['reference']) != bytes(snapshots['candidate']):
                return events, {'sample_offset': cursor, 'feed_samples': count,
                    'field': 'feed_snapshot', 'snapshots': {name: {field: getattr(v, field)
                        for field, _ in Snapshot._fields_} for name, v in snapshots.items()}}
            if traces['reference'] != traces['candidate']:
                return events, {'sample_offset': cursor, 'feed_samples': count,
                                **trace_difference(traces['reference'], traces['candidate'])}
            counters['extract_pairs'] += snapshots['reference'].trace_count
            for offset in range(0, len(traces['reference']), ctypes.sizeof(Trace)):
                value = Trace.from_buffer_copy(traces['reference'][offset:offset + ctypes.sizeof(Trace)])
                counters['matcher_pairs'] += bool(value.matched)
            events += snapshots['reference'].result
            cursor += count
            step += 1
        return events, None
    finally:
        for name, state in states.items():
            libraries[name].glass_custom_wake_destroy(state)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['reference', 'corpus', 'model', 'output']:
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--benchmark-repetitions', type=int, default=11)
    parser.add_argument('--benchmark-target-ms', type=float, default=50)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--strict-only', action='store_true')
    mode.add_argument('--benchmark-only', action='store_true')
    args = parser.parse_args()
    if args.benchmark_repetitions < 3 or args.benchmark_repetitions > 101 or not args.benchmark_repetitions % 2:
        parser.error('benchmark repetitions must be odd and 3..101')
    if not 5 <= args.benchmark_target_ms <= 1000:
        parser.error('benchmark target must be 5..1000 ms')
    root = Path(__file__).resolve().parents[1]
    desktop = root / 'app/espdl-quickapp/overlay/apps/system/desktop'
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    corpus = args.corpus.resolve()
    model = args.model.resolve()
    if model.is_dir():
        model = model / 'nihao_openvela.gkw'
    runtime = output / 'runtime'; (runtime / 'custom').mkdir(parents=True, exist_ok=True)
    shutil.copyfile(model, runtime / 'custom/nihao_openvela.gkw')
    manifest = json.loads((corpus / 'corpus.json').read_text(encoding='utf-8'))
    if manifest.get('sample_rate') != 16000 or not manifest.get('synthetic_only'):
        raise ValueError('Expected the fixed 16 kHz synthetic development corpus')
    rows = [row for row in manifest['records'] if row['split'] in ['validation', 'test']]
    if not rows or not any(row['label'] for row in rows) or not any(not row['label'] for row in rows):
        raise ValueError('Both development classes are required')
    audio = {}
    for row in rows:
        data = (corpus / row['pcm']).read_bytes()
        if len(data) % 2 or hashlib.sha256(data).hexdigest() != row['sha256']:
            raise ValueError('Corrupt corpus PCM: ' + row['pcm'])
        audio[row['pcm']] = np.frombuffer(data, dtype='<i2').copy()
    report = {'synthetic_only': True, 'physical_room_tested': False,
              'human_false_alarms_per_hour': None, 'model_sha256': sha(model),
              'corpus_sha256': sha(corpus / 'corpus.json'), 'seed': 20261021,
              'runner_sha256': sha(Path(__file__)),
              'benchmark_source_sha256': sha(root / 'app/espdl-quickapp/tests/wake_runtime_bench.c'),
              'augmentation_sha256': sha(Path(__file__).with_name('wake_augmentation.py')),
              'host': platform.platform(), 'compiler': subprocess.check_output([args.cc, '--version'], text=True).splitlines()[0],
              'floating_point_policy': 'same compiler -O3; no fast-math; bitwise comparison',
              'profiles': {}, 'differences': [],
              'counters': {'feed_pairs': 0, 'extract_pairs': 0, 'matcher_pairs': 0},
              'limitations': ['Synthetic development corpus has prior development use.',
                  'Host CPU timings do not measure ESP32-P4 CPU utilization or physical wake distance.',
                  'Identical event endpoints are compared within each identical feed chunk pattern.']}

    def save():
        (output / 'runtime.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')

    libraries, production = {}, {}
    for name, source in [('reference', args.reference.resolve()), ('candidate', desktop)]:
        libraries[name], production[name] = compile_runtime(args.cc, desktop, source, output / name)
    report['source_sha256'] = {name: {path.name: sha(path) for path in (output / name).iterdir()
        if path.suffix in ('.c', '.h')} for name in libraries}
    report['model_headers_identical'] = (output / 'reference/glass_wake_model.h').read_bytes() == (output / 'candidate/glass_wake_model.h').read_bytes()
    if not report['model_headers_identical']:
        raise ValueError('Reference and candidate authentication headers differ')
    started = time.monotonic()
    if not args.benchmark_only:
        rng = np.random.default_rng(report['seed'])
        patterns = {'320': (320,), 'mixed': (1, 7, 319, 320, 511, 1023), '4096': (4096,)}
        for profile in [*PROFILES, 'far_quiet']:
            stats = {name: {'clips': 0, 'positive': 0, 'detected': 0, 'negative': 0, 'false': 0}
                     for name in patterns}
            for row in rows:
                sound = audio_profile(audio[row['pcm']], 'quiet' if profile == 'far_quiet' else profile, rng)
                if profile == 'far_quiet':
                    sound = np.round(sound * .2).astype('<i2')
                for pattern, chunks in patterns.items():
                    events, mismatch = paired_clip(libraries, runtime, sound, chunks, report['counters'])
                    stats[pattern]['clips'] += 1
                    positive = bool(row['label'])
                    stats[pattern]['positive' if positive else 'negative'] += 1
                    stats[pattern]['detected' if positive else 'false'] += int(events == 1 if positive else events > 0)
                    if mismatch:
                        report['differences'].append({'profile': profile, 'pcm': row['pcm'],
                            'chunk_pattern': pattern, **mismatch})
                        report['strict_passed'] = False; save()
                        raise SystemExit('Strict paired wake mismatch: ' + json.dumps(report['differences'][-1]))
            report['profiles'][profile] = stats; save()
            print('WAKE_RUNTIME_PAIRED', profile, len(rows), 'clips x', len(patterns), 'patterns; bitwise identical', flush=True)
        report['strict_passed'] = True
    if not args.strict_only:
        # Balance positive and negative examples from each held-out development split.
        selected = []
        for split in ['validation', 'test']:
            for label in [1, 0]:
                selected.extend([row for row in rows if row['split'] == split and row['label'] == label
                    and 400 <= len(audio[row['pcm']]) <= 64000][:4])
        if len(selected) != 16:
            raise ValueError('Need 16 balanced benchmark clips of at most four seconds')
        paths = output / 'benchmark-pcm.list'
        paths.write_text(''.join(str(corpus / row['pcm']) + '\n' for row in selected), encoding='utf-8')
        report['benchmark_clips'] = [{'pcm': row['pcm'], 'split': row['split'], 'label': row['label'],
                                     'sha256': row['sha256']} for row in selected]
        executable = output / 'wake-runtime-bench'
        subprocess.run([args.cc, '-std=gnu11', '-O3', '-Wall', '-Wextra', '-Werror',
            '-I' + str(desktop), str(root / 'app/espdl-quickapp/tests/wake_runtime_bench.c'),
            '-ldl', '-lm', '-o', str(executable)], check=True)
        report['benchmark'] = {}
        for name, library in production.items():
            result = subprocess.check_output([str(executable), str(library), str(runtime), str(paths),
                str(args.benchmark_repetitions), str(args.benchmark_target_ms)], text=True)
            report['benchmark'][name] = json.loads(result)
        report['benchmark_comparison'] = {}
        for operation in ['idle_feed_320_samples', 'extract', 'matcher']:
            before = report['benchmark']['reference'][operation]['median_us']
            after = report['benchmark']['candidate'][operation]['median_us']
            report['benchmark_comparison'][operation] = {'reference_median_us': before,
                'candidate_median_us': after, 'speedup': before / after,
                'reduction_percent': 100 * (1 - after / before)}
        print('WAKE_RUNTIME_BENCHMARK', json.dumps(report['benchmark_comparison']), flush=True)
    report['elapsed_wall_seconds'] = time.monotonic() - started
    report['passed'] = report.get('strict_passed', args.benchmark_only)
    save()
    print('WAKE_RUNTIME_OK: synthetic host evidence; physical acceptance pending', flush=True)


if __name__ == '__main__':
    main()
