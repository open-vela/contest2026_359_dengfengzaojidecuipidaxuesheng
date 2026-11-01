#!/usr/bin/env python3
from pathlib import Path
import argparse,json,subprocess
def main():
 p=argparse.ArgumentParser();p.add_argument('--sdk',type=Path,required=True);p.add_argument('--objects',type=Path,required=True)
 p.add_argument('--release',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 repo=Path(__file__).resolve().parents[2];desktop=repo/'app/espdl-quickapp/overlay/apps/system/desktop';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
 sdk=a.sdk.resolve()/'original';base=['-O2','-g','-fsanitize=address,undefined','-fno-pie','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-I'+str(desktop)]
 objects=[]
 for name in ('model','sha','assets'):
  obj=out/(name+'.o');subprocess.run(['gcc','-std=gnu11',*base,'-c',str(desktop/('glass_local_'+name+'.c')),'-o',str(obj)],check=True);objects.append(obj)
 obj=out/'adapter.o'
 subprocess.run(['g++','-std=gnu++14',*base,'-fno-exceptions','-fno-rtti','-DGLV_WAKE_TEST','-DEIDSP_USE_CMSIS_DSP=0','-DEI_CLASSIFIER_TFLITE_ENABLE_CMSIS_NN=0','-DEI_C_LINKAGE=1',
  '-DGLV_RELEASE_ROOT="'+str(a.release.resolve())+'"','-I'+str(sdk),'-I'+str(sdk/'edge-impulse-sdk'),
  '-I'+str(sdk/'edge-impulse-sdk/third_party/flatbuffers/include'),'-I'+str(sdk/'edge-impulse-sdk/third_party/gemmlowp'),
  '-c',str(desktop/'glass_local_wake.cpp'),'-o',str(obj)],check=True)
 objects.append(obj)
 # Existing original/external equivalence objects; exclude the old main wrapper.
 sources=json.loads((a.objects/'sources.json').read_text())['sources']
 objects += [a.objects/(str(i)+'.o') for i in range(len(sources)-1)]
 exe=out/'wake-adapter'
 subprocess.run(['g++','-std=gnu++14',*base,'-DGLV_WAKE_TEST','-no-pie',str(repo/'app/espdl-quickapp/tests/local_wake_host.cpp'),*map(str,objects),'-Wl,--gc-sections','-lm','-pthread','-o',str(exe)],check=True)
 r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=120);(out/'run.log').write_text(r.stdout+r.stderr);print((r.stdout+r.stderr)[-4000:]);r.check_returncode()
 (out/'VERIFICATION.json').write_text(json.dumps(dict(passed=True,cleanup_cycles=20,weight_start_skews=16,scope='host forced 0..15-byte weight-start skew; SDK output equivalence separate; real-person accuracy not measured'),indent=2)+'\n')
if __name__=='__main__':main()
