/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_capture.h"
#include <errno.h>
#include <string.h>

int glv_capture_collect(struct glv_memory *memory,const struct glv_capture_source *source,
                          const struct glv_capture_signals *signals,const struct glv_capture_options *settings,int16_t **out,size_t *samples)
{
  if(!out||!samples)return -EINVAL;*out=NULL;*samples=0;
  if(!memory||!source||!source->open||!source->read||!source->close||!source->milliseconds)return -EINVAL;
  struct glv_capture_options defaults={60000,8000,1500,true};if(!settings)settings=&defaults;
  if(settings->max_ms<500||settings->max_ms>60000||!settings->no_speech_ms||settings->no_speech_ms>settings->max_ms||
     !settings->end_silence_ms||settings->end_silence_ms>settings->max_ms)return -EINVAL;
  int ret=glv_cancel(memory);if(ret)return ret;
  size_t capacity=(size_t)settings->max_ms*(GLV_CAPTURE_RATE/1000);
  int16_t *pcm=glv_alloc(memory,capacity*sizeof(*pcm));if(!pcm)return -ENOMEM;
  /* open may fail after partial reservation; close is required on that path. */
  ret=source->open(source->opaque);size_t used=0,voiced=0,quiet=0;
  uint64_t began=source->milliseconds(source->opaque);bool finished=false;
  if(!ret&&signals&&signals->level)signals->level(signals->opaque,0,0);
  while(!ret){
    if((ret=glv_cancel(memory)))break;
    if(signals&&signals->finish&&signals->finish(signals->opaque)){finished=true;break;}
    uint64_t now=source->milliseconds(source->opaque);
    if(now<began){ret=-EIO;break;}
    uint64_t elapsed=now-began,sample_ms=(uint64_t)used*1000/GLV_CAPTURE_RATE;
    if(elapsed<sample_ms)elapsed=sample_ms;
    if(elapsed>=settings->max_ms){ret=-E2BIG;break;}
    if(settings->automatic_endpoint&&voiced<800&&elapsed>=settings->no_speech_ms){ret=-ENODATA;break;}
    int16_t block[GLV_CAPTURE_BLOCK];int count=source->read(source->opaque,block);
    if(count<0){ret=count;break;}if(!count)continue;
    if((unsigned)count>GLV_CAPTURE_BLOCK){ret=-EIO;break;}
    if((size_t)count>capacity-used){ret=-E2BIG;break;}
    unsigned peak=0,sum=0;
    for(unsigned i=0;i<(unsigned)count;i++){
      int value=block[i];unsigned magnitude=value<0?(unsigned)-value:(unsigned)value;
      sum+=magnitude;if(magnitude>peak)peak=magnitude;
    }
    memcpy(pcm+used,block,(size_t)count*sizeof(*pcm));used+=(unsigned)count;
    if(peak>=640&&sum/(unsigned)count>=160){voiced+=(unsigned)count;quiet=0;}
    else quiet+=(unsigned)count;
    if(signals&&signals->level)signals->level(signals->opaque,peak,(unsigned)((uint64_t)used*1000/GLV_CAPTURE_RATE));
    if(settings->automatic_endpoint&&voiced>=800&&quiet>=(size_t)settings->end_silence_ms*(GLV_CAPTURE_RATE/1000)){
      finished=true;break;
    }
  }
  int closed=source->close(source->opaque);if(!ret)ret=closed;
  if(!ret)ret=glv_cancel(memory);
  if(!ret&&(!finished||used<GLV_CAPTURE_RATE/10))ret=-ENODATA;
  if(!ret&&used<capacity){
    int16_t *compact=glv_alloc(memory,used*sizeof(*pcm));
    if(!compact)ret=-ENOMEM;else{memcpy(compact,pcm,used*sizeof(*pcm));glv_free(pcm);pcm=compact;}
  }
  if(ret){glv_free(pcm);return ret;}*out=pcm;*samples=used;return 0;
}
