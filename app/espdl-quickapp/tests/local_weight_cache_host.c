/* SPDX-License-Identifier: Apache-2.0 */
/* Real model/SHA/row decoder with deterministic read faults; no board I/O. */
#include "glass_local_model.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
static bool stop,short_once;
static unsigned reads;
static bool cancelled(void *unused){(void)unused;return stop;}
size_t __real_fread(void *,size_t,size_t,FILE *);
size_t __wrap_fread(void *p,size_t s,size_t n,FILE *f)
{
  reads++;
  if(short_once){short_once=false;assert(s==1&&n>1);return __real_fread(p,s,n-1,f);}
  return __real_fread(p,s,n,f);
}
int main(int argc,char **argv)
{
  assert(argc==2);
  for(unsigned cycle=0;cycle<5;cycle++){
    struct glv_memory memory={.limit=GLV_MEMORY_LIMIT,.cancelled=cancelled};
    struct glv_model model;assert(!glv_model_open(&model,argv[1],&memory,false));
    size_t opened=memory.used;unsigned before=reads;
    assert(!model.cache&&!memory.profile.weight_cache_hits);
    memory.limit=opened+65536;
    assert(glv_model_cache_tensor(&model,"sample.weight")==-ENOMEM);
    assert(memory.used==opened&&reads==before);
    memory.limit=GLV_MEMORY_LIMIT;
    assert(!glv_model_cache_tensor(&model,"sample.weight"));
    assert(!glv_model_cache_tensor(&model,"norm.weight"));
    assert(!glv_model_cache_tensor(&model,"bad.weight"));
    size_t cached=memory.used;assert(cached>opened&&reads==before);
    assert(!glv_model_cache_tensor(&model,"sample.weight")&&memory.used==cached);
    float values[32];const struct glv_tensor *sample=glv_tensor(&model,"sample.weight");
    assert(!glv_tensor_row(&model,sample,0,values,32));
    for(unsigned i=0;i<32;i++)assert(values[i]==1);
    assert(!glv_tensor_row(&model,sample,3,values,32));
    size_t cursor=model.cursor;before=reads;
    assert(!glv_tensor_row(&model,sample,0,values,32));
    assert(reads==before&&model.cursor==cursor&&memory.profile.weight_cache_hits==1);
    assert(!glv_vector(&model,"norm.weight",values,32));
    for(unsigned i=0;i<32;i++)assert(values[i]==(float)(i+1));
    cursor=model.cursor;before=reads;
    assert(!glv_vector(&model,"norm.weight",values,32)&&reads==before&&model.cursor==cursor);
    stop=true;
    assert(glv_vector(&model,"norm.weight",values,32)==-ECANCELED&&reads==before);
    stop=false;
    short_once=true;
    assert(glv_tensor_row(&model,sample,1,values,32)==-EIO);
    assert(model.cursor==SIZE_MAX);
    before=reads;
    assert(!glv_tensor_row(&model,sample,1,values,32)&&reads>before);
    for(unsigned i=0;i<32;i++)assert(values[i]==2);
    before=reads;
    assert(!glv_tensor_row(&model,sample,1,values,32)&&reads==before);
    assert(glv_vector(&model,"bad.weight",values,32)==-EBADMSG);
    before=reads;
    assert(glv_vector(&model,"bad.weight",values,32)==-EBADMSG&&reads==before);
    assert(glv_tensor_row(&model,sample,4,values,32)==-EINVAL);
    glv_model_cache_clear(&model);
    assert(memory.used==opened&&!model.cache&&!model.cache_last_tensor&&!model.cache_bytes);
    before=reads;
    assert(!glv_tensor_row(&model,sample,0,values,32)&&reads>before);
    assert(!glv_model_cache_tensor(&model,"sample.weight"));
    glv_model_close(&model);assert(!memory.used);
  }
  puts("WEIGHT_CACHE_LIFECYCLE_OK cycles=5");return 0;
}
