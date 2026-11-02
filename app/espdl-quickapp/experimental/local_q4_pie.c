/* SPDX-License-Identifier: Apache-2.0 */
#include "local_q4_pie.h"
#include "local_q8_pie.h"
#include <errno.h>
#include <math.h>
#include <string.h>
#include <sched.h>

int glv_q4_quantize32(const float *x,int16_t *q,float *scale,unsigned bits)
{
  if(!x||!q||!scale||(bits!=8&&bits!=16))return -EINVAL;
  float maximum=0;
  for(unsigned i=0;i<32;i++){
    if(!glv_finite(x[i]))return -EDOM;
    float a=fabsf(x[i]);if(a>maximum)maximum=a;
  }
  if(maximum==0){memset(q,0,32*sizeof(*q));*scale=0;return 0;}
  int limit=bits==16?32767:127;
  *scale=maximum/(float)limit;
  if(!glv_finite(*scale)||*scale<=0)return -ERANGE;
  for(unsigned i=0;i<32;i++){
    /* Divide before multiply to avoid overflow for finite extreme inputs.
     * Explicit ties-away rounding is identical on host and RV32F. */
    float v=(x[i]/maximum)*(float)limit;
    int n=(int)(v>=0?v+.5f:v-.5f);
    if(n>limit)n=limit;
    if(n< -limit)n= -limit;
    q[i]=(int16_t)n;
  }
  return 0;
}

int32_t glv_q4_dot32_ref(const int16_t *w,const int16_t *x)
{
  int32_t sum=0;
  for(unsigned i=0;i<32;i++)sum+=(int32_t)w[i]*x[i];
  return sum;
}

int glv_q4_selftest(struct glv_memory *memory)
{
#ifdef __NuttX__
  _Alignas(16) int16_t w[32],x[32];unsigned seed=7,old;int ret=0;
  void *batch_allocation=glv_alloc(memory,44u*64u*32u*sizeof(int16_t)+15);
  int32_t *dots=glv_alloc(memory,44u*64u*sizeof(int32_t));
  if(!batch_allocation||!dots){glv_free(dots);glv_free(batch_allocation);return -ENOMEM;}
  int16_t *batch_x=(int16_t *)(((uintptr_t)batch_allocation+15)&~(uintptr_t)15);
  _Alignas(16) int16_t batch_w[1408];
  __asm__ volatile("csrrw %0,0x7f2,%1" : "=r"(old) : "r"(1u) : "memory");
  for(unsigned run=0;run<65536;run++){
    if(!(run%256)){if((ret=glv_cancel(memory)))break;sched_yield();}
    for(unsigned i=0;i<32;i++){
      seed=seed*1664525u+1013904223u;w[i]=(int)((seed>>16)%16)-8;
      seed=seed*1664525u+1013904223u;x[i]=(int)((seed>>1)%65535)-32767;
      if(run<4){w[i]=run&1?7:-8;x[i]=run&2?32767:-32767;}
    }
    if(glv_q4_dot32_pie(w,x)!=glv_q4_dot32_ref(w,x)){ret=-EILSEQ;break;}
  }
  static const unsigned group_cases[]={1,2,16,44},batch_cases[]={1,2,3,8,31,32,63,64};
  for(unsigned gi=0;!ret&&gi<4;gi++)for(unsigned bi=0;!ret&&bi<8;bi++){
    if((ret=glv_cancel(memory)))break;sched_yield();
    unsigned groups=group_cases[gi],batch=batch_cases[bi];
    for(unsigned i=0;i<groups*32;i++){
      seed=seed*1664525u+1013904223u;batch_w[i]=(int)((seed>>16)%16)-8;
    }
    for(unsigned i=0;i<groups*batch*32;i++){
      seed=seed*1664525u+1013904223u;batch_x[i]=(int)((seed>>1)%65535)-32767;
    }
    if(!gi){for(unsigned i=0;i<32;i++)batch_w[i]=-8;
      for(unsigned i=0;i<batch*32;i++)batch_x[i]=bi&1?32767:-32767;}
    glv_q4_dot_batch_pie(batch_w,batch_x,dots,groups,batch);
    for(unsigned g=0;g<groups&&!ret;g++)for(unsigned b=0;b<batch;b++)
      if(dots[g*batch+b]!=glv_q4_dot32_ref(batch_w+g*32,batch_x+(g*batch+b)*32)){ret=-EILSEQ;break;}
  }
  __asm__ volatile("csrw 0x7f2,%0" :: "r"(old) : "memory");
  glv_free(dots);glv_free(batch_allocation);
  return ret?ret:glv_q8_selftest(memory);
#else
  (void)memory;return -ENOTSUP;
#endif
}

int glv_q4_matbatch(struct glv_model *m,const char *name,const float *x,float *out,
                    unsigned rows,unsigned cols,unsigned batch,unsigned bits)
{
  if(!m||!name||!m->memory||!x||!out||!rows||rows>8192||!batch||batch>GLV_MATBATCH_MAX||!cols||cols>1408||cols%32||
     (bits!=8&&bits!=16))return -EINVAL;
  const struct glv_tensor *t=glv_tensor(m,name);
  if(!t||t->type!=1||t->ndim!=2||t->shape[0]!=rows||t->shape[1]!=cols||
     t->stride!=cols/32*20||(!m->data&&!m->file))return -EINVAL;
  unsigned groups=cols/32;
  uint64_t began=glv_now_us(),io=m->memory->profile.weight_read_us+m->memory->profile.weight_seek_us;
  uint64_t decoded=m->memory->profile.weight_decode_us;
  /* Groups-major blocks let one native call retain four weight vectors across
   * every prompt position. The full model stays packed on storage. */
  void *allocation=glv_alloc(m->memory,(size_t)batch*cols*sizeof(int16_t)+15);
  int16_t *qx=(int16_t *)(((uintptr_t)allocation+15)&~(uintptr_t)15);
  float *scales=glv_alloc(m->memory,(size_t)batch*groups*sizeof(float));
  unsigned char *row=glv_alloc(m->memory,t->stride);
  void *weight_allocation=glv_alloc(m->memory,(size_t)cols*sizeof(int16_t)+15);
  int16_t *qw=(int16_t *)(((uintptr_t)weight_allocation+15)&~(uintptr_t)15);
  float *weight_scales=glv_alloc(m->memory,groups*sizeof(float));
  int32_t *dots=glv_alloc(m->memory,(size_t)batch*groups*sizeof(int32_t));
  float sums[GLV_MATBATCH_MAX];
  int ret=0;
  if(!allocation||!scales||!row||!weight_allocation||!weight_scales||!dots){ret=-ENOMEM;goto done;}
  for(unsigned b=0;b<batch;b++)for(unsigned g=0;g<groups;g++){
    ret=glv_q4_quantize32(x+b*cols+g*32,qx+(g*batch+b)*32,scales+g*batch+b,bits);
    if(ret)goto done;
  }
  for(unsigned b=0;b<batch;b++)memset(out+b*rows,0,rows*sizeof(float));
  m->memory->q4_matrix_calls++;
#ifdef __NuttX__
  unsigned pie_state;
  __asm__ volatile("csrrw %0,0x7f2,%1" : "=r"(pie_state) : "r"(1u) : "memory");
#endif
  for(unsigned r=0;r<rows;r++){
    ret=glv_tensor_q4_packed(m,t,r,row,t->stride);if(ret)break;
    uint64_t unpack=glv_now_us();
    for(unsigned g=0;g<groups;g++){
      const unsigned char *block=row+g*20;
      float ws;memcpy(&ws,block,4);
      if(!glv_finite(ws)||ws<=0){ret=-EBADMSG;break;}
      weight_scales[g]=ws;
      for(unsigned i=0;i<32;i++){
        int v=(block[4+i/2]>>((i%2)*4))&15;
        qw[g*32+i]=(int16_t)(v>=8?v-16:v);
      }
    }
    m->memory->profile.weight_decode_us+=glv_now_us()-unpack;
    if(ret)break;
#ifdef __NuttX__
    glv_q4_dot_batch_pie(qw,qx,dots,groups,batch);
#else
    for(unsigned g=0;g<groups;g++)for(unsigned b=0;b<batch;b++)
      dots[g*batch+b]=glv_q4_dot32_ref(qw+g*32,qx+(g*batch+b)*32);
#endif
    m->memory->q4_dot_calls+=groups*batch;
    memset(sums,0,batch*sizeof(float));
    /* Per-output FP32 reduction retains ascending group order, with separate
     * multiplications. Contiguous sums avoid strided PSRAM writes per group. */
    for(unsigned g=0;g<groups;g++)for(unsigned b=0;b<batch;b++)
      sums[b]+=((float)dots[g*batch+b]*weight_scales[g])*scales[g*batch+b];
    for(unsigned b=0;b<batch;b++)if(!glv_finite(sums[b])){ret=-ERANGE;break;}
    if(ret)break;
    for(unsigned b=0;b<batch;b++)out[b*rows+r]=sums[b];
  }
#ifdef __NuttX__
  __asm__ volatile("csrw 0x7f2,%0" :: "r"(pie_state) : "memory");
#endif
done:
  glv_free(dots);glv_free(weight_scales);glv_free(weight_allocation);
  glv_free(row);glv_free(scales);glv_free(allocation);
  m->memory->profile.matmul_us+=glv_now_us()-began-
    (m->memory->profile.weight_read_us+m->memory->profile.weight_seek_us-io)-
    (m->memory->profile.weight_decode_us-decoded);
  return ret;
}
