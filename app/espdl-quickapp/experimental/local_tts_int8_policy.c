/* SPDX-License-Identifier: Apache-2.0 */
#include "local_tts_int8_policy.h"
#include "glass_local_assets.h"
#include <errno.h>
#include <string.h>
int glv_tts_int8_policy_load(struct glv_model *model)
{
 if(!model||!model->memory)return -EINVAL;
 struct glv_memory *m=model->memory;if(m->tts_int8_policy)return -EBUSY;
 void *buffer=NULL;size_t bytes=0;int ret=glv_asset_read_small(GLV_TTS_INT8_POLICY_ROOT,"policy.bin",8192,m,&buffer,&bytes);
 if(ret)return ret;const unsigned char *p=buffer;ret=-EBADMSG;
 if(bytes<60||memcmp(p,"GLV8P001",8)||glv_u32(p+8)!=1||glv_u32(p+12)!=GLV_ABI||
    memcmp(p+16,m->tts_base_digest,32)||glv_u32(p+48)!=1||glv_u32(p+56))goto done;
 unsigned count=glv_u32(p+52);if(!count||count>74||bytes!=60u+count*80u)goto done;
 for(unsigned i=0;i<count;i++){
  const char *name=(const char *)p+60u+i*80u;
  if(!memchr(name,0,80)||strncmp(name,"vocoder.",8)||!strncmp(name,"vocoder.upsamples.",18)||
     (i&&strcmp(name-80,name)>=0))goto done;
  const struct glv_tensor *w=glv_tensor(model,name);
  if(!w||w->type!=2||w->ndim!=3||!w->shape[0]||w->shape[0]>208||!w->shape[1]||w->shape[1]>208||!w->shape[2]||w->shape[2]>11)goto done;
 }
 m->tts_int8_policy=buffer;return 0;
done:glv_free(buffer);return ret;
}
void glv_tts_int8_policy_close(struct glv_memory *m)
{if(m){glv_free(m->tts_int8_policy);m->tts_int8_policy=NULL;m->tts_int8_owner=NULL;memset(m->tts_base_digest,0,32);}}
int glv_tts_int8_precision(const struct glv_model *model,const struct glv_tensor *w)
{
 if(!model||!model->memory||!w)return -EINVAL;
 const unsigned char *p=model->memory->tts_int8_policy;if(!p)return -ENOENT;
 unsigned lo=0,hi=glv_u32(p+52);
 while(lo<hi){unsigned mid=(lo+hi)/2;int c=strcmp(w->name,(const char *)p+60u+mid*80u);if(!c)return 16;if(c>0)lo=mid+1;else hi=mid;}
 return 8;
}
