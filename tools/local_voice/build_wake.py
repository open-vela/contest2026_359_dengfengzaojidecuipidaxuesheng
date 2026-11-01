#!/usr/bin/env python3
"""Build the unchanged EI MFCC/EON implementation against host or RV32 tools."""
import argparse
import json
from pathlib import Path
import subprocess
import shlex

SOURCES=[
 'dsp/memory.cpp','dsp/kissfft/kiss_fft.cpp','dsp/kissfft/kiss_fftr.cpp','dsp/dct/fast-dct-fft.cpp',
 'tensorflow/lite/core/api/common.cc','tensorflow/lite/core/api/error_reporter.cc',
 'tensorflow/lite/kernels/kernel_util_lite.cc','tensorflow/lite/kernels/internal/quantization_util.cc',
 'tensorflow/lite/micro/micro_context.cc','tensorflow/lite/micro/micro_utils.cc','tensorflow/lite/micro/micro_log.cc',
 'tensorflow/lite/micro/kernels/kernel_util_micro.cc',
 'tensorflow/lite/micro/micro_allocator.cc','tensorflow/lite/micro/micro_allocation_info.cc',
 'tensorflow/lite/micro/memory_helpers.cc','tensorflow/lite/micro/flatbuffer_utils.cc',
 'tensorflow/lite/micro/single_arena_buffer_allocator.cc',
 'tensorflow/lite/micro/persistent_arena_buffer_allocator.cc',
 'tensorflow/lite/micro/non_persistent_arena_buffer_allocator.cc',
 'tensorflow/lite/micro/memory_planner/greedy_memory_planner.cc',
 'tensorflow/lite/micro/memory_planner/non_persistent_buffer_planner_shim.cc',
 'tensorflow/lite/micro/kernels/conv.cc','tensorflow/lite/micro/kernels/conv_common.cc',
 'tensorflow/lite/micro/kernels/fully_connected.cc','tensorflow/lite/micro/kernels/fully_connected_common.cc',
 'tensorflow/lite/micro/kernels/pooling.cc','tensorflow/lite/micro/kernels/pooling_common.cc',
 'tensorflow/lite/micro/kernels/reshape.cc','tensorflow/lite/micro/kernels/softmax.cc','tensorflow/lite/micro/kernels/softmax_common.cc',
]

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--sdk',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
 p.add_argument('--external',action='store_true');p.add_argument('--wrapper',type=Path,required=True)
 p.add_argument('--compiler',default='g++');p.add_argument('--archive-only',action='store_true')
 p.add_argument('--archiver',default='ar');p.add_argument('--apps',type=Path)
 p.add_argument('--toolchain',type=Path);p.add_argument('--extra-object',action='append',default=[])
 a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
 root=a.sdk/'original';sdk=root/'edge-impulse-sdk';repo=Path(__file__).resolve().parents[2]
 board=[]
 if a.apps:
  if not a.toolchain:p.error('--apps requires --toolchain')
  command=['make','-s','-C',str(a.apps/'system/desktop'),'APPDIR='+str(a.apps),'TOPDIR='+str(a.apps.parent/'nuttx'),
   'CROSSDEV='+str(a.toolchain/'riscv32-esp-elf-'),'--eval=local-flags:;$(info LOCAL_CXX=$(CXX))$(info LOCAL_FLAGS=$(CXXFLAGS))','local-flags']
  r=subprocess.run(command,capture_output=True,text=True,check=True)
  board=shlex.split(next(s.removeprefix('LOCAL_FLAGS=') for s in r.stdout.splitlines() if s.startswith('LOCAL_FLAGS=')))
  a.compiler=str(a.toolchain/'riscv32-esp-elf-g++');a.archiver=str(a.toolchain/'riscv32-esp-elf-ar')
 flags=[*board,'-std=gnu++14','-O2','-g','-ffunction-sections','-fdata-sections','-fno-exceptions','-fno-rtti',
  '-DEIDSP_USE_CMSIS_DSP=0','-DEI_CLASSIFIER_TFLITE_ENABLE_CMSIS_NN=0','-DEI_C_LINKAGE=1',
  '-I'+str(root),'-I'+str(sdk),'-I'+str(sdk/'third_party/flatbuffers/include'),'-I'+str(sdk/'third_party/gemmlowp'),
  '-I'+str(root/'tflite-model'),'-I'+str(repo/'app/espdl-quickapp/overlay/apps/system/desktop')]
 if a.apps:flags+=['-include',str(repo/'app/espdl-quickapp/overlay/apps/system/desktop/glass_local_cxx_math.h')]
 generated=a.sdk/'external/tflite_learn_1153216_4_compiled.cpp' if a.external else root/'tflite-model/tflite_learn_1153216_4_compiled.cpp'
 sources=[sdk/s for s in SOURCES if (sdk/s).exists()]+[generated,a.wrapper]
 missing=[s for s in SOURCES if not (sdk/s).exists()]
 (a.output/'sources.json').write_text(json.dumps(dict(sources=list(map(str,sources)),missing=missing),indent=2))
 objects=[]
 with (a.output/'build.log').open('w') as log:
  for index,source in enumerate(sources):
   obj=a.output/(str(index)+'.o');objects.append(obj)
   result=subprocess.run([a.compiler,*flags,'-c',str(source),'-o',str(obj)],stdout=log,stderr=subprocess.STDOUT)
   if result.returncode:
    log.flush();print((a.output/'build.log').read_text()[-12000:]);raise SystemExit(result.returncode)
  if a.archive_only:
   command=[a.archiver,'rcs',str(a.output/'liblocalwake.a'),*map(str,objects)]
  else:command=[a.compiler,*map(str,objects),*a.extra_object,'-Wl,--gc-sections','-lm','-pthread','-o',str(a.output/'wake-host')]
  result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT)
  if result.returncode:
   log.flush();print((a.output/'build.log').read_text()[-14000:]);raise SystemExit(result.returncode)
 (a.output/'compile.json').write_text(json.dumps(dict(compiler=a.compiler,flags=flags,external=a.external,archive=a.archive_only),indent=2))
 print('WAKE_SDK_BUILD_OK',a.output,flush=True)

if __name__=='__main__':main()
