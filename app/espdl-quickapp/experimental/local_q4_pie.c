/* SPDX-License-Identifier: Apache-2.0 */
#include "local_q4_pie.h"
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
  __asm__ volatile("csrw 0x7f2,%0" :: "r"(old) : "memory");
  return ret;
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
  /* Aligned activation blocks and one encoded row; never inflate the whole
   * model. Every allocation uses the existing memory cap/reserve checks. */
  void *allocation=glv_alloc(m->memory,(size_t)batch*cols*sizeof(int16_t)+15);
  int16_t *qx=(int16_t *)(((uintptr_t)allocation+15)&~(uintptr_t)15);
  float *scales=glv_alloc(m->memory,(size_t)batch*groups*sizeof(float));
  unsigned char *row=glv_alloc(m->memory,t->stride);
  int ret=0;
  if(!allocation||!scales||!row){ret=-ENOMEM;goto done;}
  for(unsigned b=0;b<batch;b++)for(unsigned g=0;g<groups;g++){
    ret=glv_q4_quantize32(x+b*cols+g*32,qx+b*cols+g*32,scales+b*groups+g,bits);
    if(ret)goto done;
  }
  for(unsigned b=0;b<batch;b++)memset(out+b*rows,0,rows*sizeof(float));
  m->memory->q4_matrix_calls++;
#ifdef __NuttX__
  unsigned pie_state;
  __asm__ volatile("csrrw %0,0x7f2,%1" : "=r"(pie_state) : "r"(1u) : "memory");
#endif
  for(unsigned r=0;r<rows;r++){
    ret=glv_cancel(m->memory);if(ret)break;
    size_t offset=t->offset+(size_t)r*t->stride;
    if(offset>m->bytes||t->stride>m->bytes-offset){ret=-EBADMSG;break;}
    if(m->data)memcpy(row,m->data+offset,t->stride);
    else {
      if(m->cursor!=offset&&fseek(m->file,(long)offset,SEEK_SET)){ret=-EIO;break;}
      ret=glv_read_exact(m->file,row,t->stride);if(ret)break;
      m->cursor=offset+t->stride;
    }
    for(unsigned g=0;g<groups;g++){
      const unsigned char *block=row+g*20;
      float ws;memcpy(&ws,block,4);
      if(!glv_finite(ws)||ws<=0){ret=-EBADMSG;break;}
      _Alignas(16) int16_t qw[32];
      for(unsigned i=0;i<32;i++){
        int v=(block[4+i/2]>>((i%2)*4))&15;
        qw[i]=(int16_t)(v>=8?v-16:v);
      }
      for(unsigned b=0;b<batch;b++){
#ifdef __NuttX__
        int32_t dot=glv_q4_dot32_pie(qw,qx+b*cols+g*32);
#else
        int32_t dot=glv_q4_dot32_ref(qw,qx+b*cols+g*32);
#endif
        out[b*rows+r]+=((float)dot*ws)*scales[b*groups+g];
        m->memory->q4_dot_calls++;
      }
    }
    if(ret)break;
    for(unsigned b=0;b<batch;b++)if(!glv_finite(out[b*rows+r])){ret=-ERANGE;break;}
    if(ret)break;
  }
#ifdef __NuttX__
  __asm__ volatile("csrw 0x7f2,%0" :: "r"(pie_state) : "memory");
#endif
done:
  glv_free(row);glv_free(scales);glv_free(allocation);return ret;
}
