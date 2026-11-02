/* SPDX-License-Identifier: Apache-2.0 */
#include "local_q8_pie.h"
#include "local_q4_pie.h"
#include <errno.h>
#include <math.h>
#include <sched.h>
#include <string.h>

int glv_q8_selftest(struct glv_memory *m)
{
#ifdef __NuttX__
  /* Wider than Q4: worst group sum is 32*128*32767 < 2^27. */
  void *xa=glv_alloc(m,44u*64u*32u*2u+15u);
  int32_t *dots=glv_alloc(m,44u*64u*4u);
  if(!xa||!dots){glv_free(dots);glv_free(xa);return -ENOMEM;}
  int16_t *x=(int16_t *)(((uintptr_t)xa+15u)&~(uintptr_t)15u);
  _Alignas(16) int16_t w[1408];unsigned seed=511,old;int ret=0;
  __asm__ volatile("csrrw %0,0x7f2,%1":"=r"(old):"r"(1u):"memory");
  static const unsigned gs[]={1,2,16,44},bs[]={1,2,3,8,31,32,63,64};
  for(unsigned run=0;!ret&&run<256;run++){
    if((ret=glv_cancel(m)))break;sched_yield();
    unsigned groups=gs[run%4],batch=bs[(run/4)%8];
    for(unsigned i=0;i<groups*32;i++){seed=seed*1664525u+1013904223u;w[i]=(int)((seed>>16)&255)-128;}
    for(unsigned i=0;i<groups*batch*32;i++){seed=seed*1664525u+1013904223u;x[i]=(int)((seed>>1)%65535)-32767;}
    if(run<64){
      for(unsigned i=0;i<groups*32;i++)w[i]=(run&32)?127:-128;
      for(unsigned i=0;i<groups*batch*32;i++)x[i]=(run&16)?32767:-32767;
    }
    glv_q4_dot_batch_pie(w,x,dots,groups,batch);
    for(unsigned g=0;g<groups&&!ret;g++)for(unsigned b=0;b<batch;b++)
      if(dots[g*batch+b]!=glv_q4_dot32_ref(w+g*32,x+(g*batch+b)*32)){ret=-EILSEQ;break;}
  }
  __asm__ volatile("csrw 0x7f2,%0"::"r"(old):"memory");
  glv_free(dots);glv_free(xa);return ret;
#else
  (void)m;return -ENOTSUP;
#endif
}

int glv_q8_matbatch(struct glv_model *m,const char *name,const float *x,float *out,
                    unsigned rows,unsigned cols,unsigned batch)
{
  if(!m||!m->memory||!name||!x||!out||!rows||rows>8192||!cols||cols>1408||cols%32||
     !batch||batch>GLV_MATBATCH_MAX)return -EINVAL;
  const struct glv_tensor *t=glv_tensor(m,name);
  if(!t||t->type!=2||t->ndim!=2||t->shape[0]!=rows||t->shape[1]!=cols||t->stride!=cols+4u)return -EBADMSG;
  unsigned groups=cols/32u;
  uint64_t began=glv_now_us(),io=m->memory->profile.weight_read_us+m->memory->profile.weight_seek_us;
  uint64_t decoded=m->memory->profile.weight_decode_us;
  void *xa=glv_alloc(m->memory,(size_t)batch*cols*2u+15u);
  void *wa=glv_alloc(m->memory,(size_t)cols*2u+15u);
  unsigned char *row=glv_alloc(m->memory,t->stride);
  float *scales=glv_alloc(m->memory,(size_t)groups*batch*4u);
  int32_t *dots=glv_alloc(m->memory,(size_t)groups*batch*4u);
  int16_t *qx=(int16_t *)(((uintptr_t)xa+15u)&~(uintptr_t)15u);
  int16_t *qw=(int16_t *)(((uintptr_t)wa+15u)&~(uintptr_t)15u);
  float sums[GLV_MATBATCH_MAX];int ret=0;
  if(!xa||!wa||!row||!scales||!dots){ret=-ENOMEM;goto done;}
  for(unsigned b=0;b<batch;b++)for(unsigned g=0;g<groups;g++){
    ret=glv_q4_quantize32(x+(size_t)b*cols+g*32u,qx+(g*batch+b)*32u,scales+g*batch+b,16);
    if(ret)goto done;
  }
  m->memory->profile.llm_q8_pie_calls++;
#ifdef __NuttX__
  unsigned old;__asm__ volatile("csrrw %0,0x7f2,%1":"=r"(old):"r"(1u):"memory");
#endif
  for(unsigned r=0;r<rows;r++){
    if((ret=glv_cancel(m->memory)))break;
    ret=glv_tensor_q8_packed(m,t,r,row,t->stride,cols);if(ret)break;
    uint64_t unpack=glv_now_us();float ws;memcpy(&ws,row,4);
    if(!glv_finite(ws)||ws<=0){ret=-EBADMSG;break;}
    for(unsigned i=0;i<cols;i++)qw[i]=(int8_t)row[4u+i];
    m->memory->profile.weight_decode_us+=glv_now_us()-unpack;
#ifdef __NuttX__
    glv_q4_dot_batch_pie(qw,qx,dots,groups,batch);
#else
    for(unsigned g=0;g<groups;g++)for(unsigned b=0;b<batch;b++)
      dots[g*batch+b]=glv_q4_dot32_ref(qw+g*32u,qx+(g*batch+b)*32u);
#endif
    m->memory->profile.llm_q8_pie_dots+=(uint64_t)groups*batch;
    memset(sums,0,batch*4u);
    for(unsigned g=0;g<groups;g++)for(unsigned b=0;b<batch;b++)
      sums[b]+=((float)dots[g*batch+b]*ws)*scales[g*batch+b];
    for(unsigned b=0;b<batch;b++)if(!glv_finite(sums[b])){ret=-ERANGE;break;}
    if(ret)break;
    for(unsigned b=0;b<batch;b++)out[(size_t)b*rows+r]=sums[b];
  }
#ifdef __NuttX__
  __asm__ volatile("csrw 0x7f2,%0"::"r"(old):"memory");
#endif
done:
  glv_free(dots);glv_free(scales);glv_free(row);glv_free(wa);glv_free(xa);
  m->memory->profile.matmul_us+=glv_now_us()-began-
    (m->memory->profile.weight_read_us+m->memory->profile.weight_seek_us-io)-
    (m->memory->profile.weight_decode_us-decoded);
  return ret;
}
