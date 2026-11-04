#!/usr/bin/env python3
"""Reproduce the ESP-DSP dependency-scan failure and verify private headers."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
from build_bilibili_v3 import prepare_asr_dsp_vendor

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
repo = Path(__file__).resolve().parents[1]
vendor = repo/'app/espdl-quickapp/experimental/esp_dsp'
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
destination = out/'esp_dsp'
shutil.copytree(vendor, destination, dirs_exist_ok=True)
probe = out/'probe.c'
probe.write_text('#include "esp_dsp/dsps_add.h"\n#include "esp_dsp/dsps_sub.h"\n#include "esp_dsp/dsps_mul.h"\n')
command = ['gcc', '-M', '-MT', 'probe.o', str(probe)]
# With neither a global nor a per-file compatibility include, the old layout
# fails exactly like NuttX's automatic dependency regeneration.
if not (destination/'esp_err.h').exists():
    original = subprocess.run(command, capture_output=True, text=True)
    assert original.returncode and 'esp_err.h' in original.stderr
    (out/'negative-control.log').write_text(original.stderr)
lock = prepare_asr_dsp_vendor(vendor, destination)
for attempt in range(2):
    prepare_asr_dsp_vendor(vendor, destination)
    run = subprocess.run(command, capture_output=True, text=True, check=True)
    assert 'esp_dsp/esp_err.h' in run.stdout and 'esp_dsp/sdkconfig.h' in run.stdout
    (out/f'dependencies-{attempt}.d').write_text(run.stdout)
for op in ('add', 'sub', 'mul'):
    source = out/f'dsps_{op}_f32_ansi.c'
    scan = subprocess.run(['gcc','-M',str(source)],capture_output=True,text=True,check=True)
    assert 'esp_dsp/esp_err.h' in scan.stdout and 'esp_dsp/sdkconfig.h' in scan.stdout
    subprocess.run(['gcc','-std=gnu11','-O3','-fno-fast-math','-ffp-contract=off',
                    '-c',str(source),'-o',str(out/(op+'.o'))],check=True)
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
for row in lock['files']:
    assert sha(vendor/row['path']) == sha(destination/row['path']) == row['sha256']
result = dict(passed=True, dependency_scan_without_private_flags=True,
              repeated_preparation=True, pinned_sources_unchanged=len(lock['files']),
              official_kernels_compiled=3)
(out/'verification.json').write_text(json.dumps(result,indent=2)+'\n')
print('ASR_DSP_DEPENDENCIES_OK:',json.dumps(result))
