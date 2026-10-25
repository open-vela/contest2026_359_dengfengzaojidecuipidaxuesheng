"""Host-only calibration using the production VAD and feature extractor.

The calibration wrapper disables wake events so cooldown cannot hide negative
utterances. It is never linked into the firmware. Final tests use the public
detector API with the exported model and its generated authentication header.
"""
import ctypes
import shutil
import subprocess
from pathlib import Path


def stage_sources(desktop, destination, model_header=None):
    destination.mkdir(parents=True, exist_ok=True)
    names = ['glass_custom_wake.c', 'glass_custom_wake.h',
             'glass_wake_features.c', 'glass_wake_features.h',
             'qpk_espdl_sha256.c', 'glass_wake_model.h']
    for name in names:
        source = model_header if name == 'glass_wake_model.h' and model_header else desktop / name
        shutil.copyfile(source, destination / name)
    return [destination / name for name in
            ['glass_custom_wake.c', 'glass_wake_features.c', 'qpk_espdl_sha256.c']]


def calibration_detector(desktop, destination, templates, joint_distance=False):
    stage_sources(desktop, destination)
    wrapper = destination / 'calibration.c'
    legacy = '''/* Host diagnostics only. Never ship this wrapper. */
#include "glass_custom_wake.c"
float wake_calibrate(float *weights, const int16_t *pcm, size_t count)
{
  struct glass_custom_wake *s=calloc(1,sizeof(*s));
  if(!s)return -1;
  s->weights=weights;s->noise=50;s->initial_floor=1e20f;s->warmup=4;s->threshold=0;
  float minimum=1000;
  for(size_t offset=0;offset<count;){
    size_t n=count-offset;if(n>320)n=320;
    unsigned before=s->evaluations;
    int ret=glass_custom_wake_feed(s,pcm+offset,n);offset+=n;
    if(ret<0){free(s);return -1;}
    if(s->evaluations!=before){
      float ratio=logf(1/s->score-1)/20;
      if(ratio<minimum)minimum=ratio;
    }
  }
  free(s);return minimum;
}
'''
    joint = '''/* Host diagnostics only. Never ship this wrapper.
 * Observe the real classifier without letting cooldown hide later windows. */
#include "glass_wake_features.h"
static float observed_ratio,observed_distance;
static float observe_ratio_distance(const float *,const float *,float *);
#define glass_wake_ratio_distance observe_ratio_distance
#include "glass_custom_wake.c"
#undef glass_wake_ratio_distance
static float observe_ratio_distance(const float *weights,const float *feature,
                                    float *distance)
{
  float ratio=glass_wake_ratio_distance(weights,feature,distance);
  observed_ratio=ratio;observed_distance=*distance;return ratio;
}
float wake_calibrate(float *weights,const int16_t *pcm,size_t count)
{
  struct glass_custom_wake *s=calloc(1,sizeof(*s));
  if(!s)return -1;
  s->weights=weights;s->noise=50;s->initial_floor=1e20f;s->warmup=4;s->threshold=0;
  float minimum=1e20f;
  for(size_t offset=0;offset<count;){
    size_t n=count-offset;if(n>320)n=320;
    unsigned before=s->evaluations;
    int ret=glass_custom_wake_feed(s,pcm+offset,n);offset+=n;
    if(ret<0){free(s);return -1;}
    if(s->evaluations!=before&&observed_distance<=GLASS_WAKE_MAX_POSITIVE_DISTANCE){
      /* At threshold zero no event fires. Speaking remains true only for
       * the early evaluation; the final evaluation has reset the segment. */
      float required=observed_ratio+(s->speaking?.03f:0);
      if(required<minimum)minimum=required;
    }
  }
  free(s);return minimum;
}
'''
    wrapper.write_text(joint if joint_distance else legacy, encoding='utf-8')
    library = destination / 'calibration.so'
    subprocess.run(['gcc', '-shared', '-fPIC', '-O3', '-I'+str(desktop), str(wrapper),
                    str(destination / 'glass_wake_features.c'),
                    str(destination / 'qpk_espdl_sha256.c'), '-lm', '-o', str(library)], check=True)
    lib = ctypes.CDLL(str(library))
    fp, sp = ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_int16)
    lib.wake_calibrate.argtypes = [fp, sp, ctypes.c_size_t]
    lib.wake_calibrate.restype = ctypes.c_float
    # Capture both objects so neither the library nor the template array expires.
    def evaluate(pcm):
        result = float(lib.wake_calibrate(templates.ctypes.data_as(fp), pcm.ctypes.data_as(sp), len(pcm)))
        if result < 0:
            raise RuntimeError('Production streaming calibration failed')
        return result
    evaluate.joint_distance = joint_distance
    evaluate.no_eligible_window = 1e20 if joint_distance else 1000
    return evaluate
