/* SPDX-License-Identifier: Apache-2.0 */
#include "local_q4_pie.h"
#include <assert.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool cancelled(void *p){return *(bool *)p;}
int main(void)
{
  float input[32]={0},scale=1;int16_t q[32];
  assert(!glv_q4_quantize32(input,q,&scale,16)&&scale==0);
  for(unsigned i=0;i<32;i++)assert(q[i]==0);
  input[0]=FLT_MAX;input[1]=-FLT_MAX;
  assert(!glv_q4_quantize32(input,q,&scale,16)&&q[0]==32767&&q[1]==-32767);
  input[0]=NAN;assert(glv_q4_quantize32(input,q,&scale,16)==-EDOM);
  input[0]=INFINITY;assert(glv_q4_quantize32(input,q,&scale,16)==-EDOM);
  memset(input,0,sizeof(input));input[0]=FLT_TRUE_MIN;
  assert(glv_q4_quantize32(input,q,&scale,16)==-ERANGE);
  assert(glv_q4_quantize32(input,q,&scale,4)==-EINVAL);
  int16_t weights[32];for(unsigned i=0;i<32;i++){weights[i]=-8;q[i]=-32767;}
  assert(glv_q4_dot32_ref(weights,q)==8388352);
  for(unsigned i=0;i<32;i++)q[i]=32767;
  assert(glv_q4_dot32_ref(weights,q)==-8388352);
  unsigned seed=7;
  for(unsigned run=0;run<1000;run++){
    int64_t expected=0;
    for(unsigned i=0;i<32;i++){
      seed=seed*1664525u+1013904223u;weights[i]=(seed>>16)%16-8;
      seed=seed*1664525u+1013904223u;q[i]=(int)((seed>>1)%65535)-32767;
      expected+=(int64_t)weights[i]*q[i];
    }
    assert(glv_q4_dot32_ref(weights,q)==expected);
  }
  bool stop=false;struct glv_memory memory={.limit=GLV_MEMORY_LIMIT,.cancelled=cancelled,.opaque=&stop};
  struct glv_tensor tensor={.name="weight",.type=1,.ndim=2,.shape={3,64},.stride=40,.bytes=120};
  unsigned char storage[121]={0};unsigned char *data=storage+1; /* deliberately unaligned */
  for(unsigned b=0;b<6;b++){float s=.125f;memcpy(data+b*20,&s,4);memset(data+b*20+4,0x71,16);}
  struct glv_model model={.memory=&memory,.tensors=&tensor,.count=1,.bytes=120,.data=data};
  float x[32*64],y[32*3];for(unsigned i=0;i<32*64;i++)x[i]=(i%3==0)?1:-1;
  for(unsigned batch=1;batch<=32;batch++){
    assert(!glv_q4_matbatch(&model,"weight",x,y,3,64,batch,16));
    for(unsigned b=0;b<batch;b++)for(unsigned r=0;r<3;r++){
      float expected=0;for(unsigned c=0;c<64;c++)expected+=(c%2?7:1)*.125f*x[b*64+c];
      assert(fabsf(y[b*3+r]-expected)<.00001f);
    }
    assert(memory.used==0);
  }
  stop=true;assert(glv_q4_matbatch(&model,"weight",x,y,3,64,32,16)==-ECANCELED);stop=false;
  size_t limit=memory.limit;memory.limit=200;
  assert(glv_q4_matbatch(&model,"weight",x,y,3,64,32,16)==-ENOMEM);assert(!memory.used);memory.limit=limit;
  float invalid=NAN;memcpy(data,&invalid,4);
  assert(glv_q4_matbatch(&model,"weight",x,y,3,64,1,16)==-EBADMSG);
  invalid=.125f;memcpy(data,&invalid,4);
  model.bytes=119;assert(glv_q4_matbatch(&model,"weight",x,y,3,64,1,16)==-EBADMSG);model.bytes=120;
  model.data=NULL;model.file=tmpfile();assert(model.file);
  assert(fwrite(data,1,119,model.file)==119);rewind(model.file);
  assert(glv_q4_matbatch(&model,"weight",x,y,3,64,1,16)==-EIO);fclose(model.file);
  assert(!memory.used);
  puts("Q4_UNIT_PASS zero/extreme/nonfinite/underflow/1000-dots/batches1-32/unaligned/cancel/OOM/corruption/truncation/leaks");
  return 0;
}
