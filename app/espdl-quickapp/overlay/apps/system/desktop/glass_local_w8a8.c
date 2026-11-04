/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_w8a8.h"
#include <errno.h>
#include <math.h>
#include <string.h>
#ifdef __NuttX__
extern float glv_llm_dot_groups_pie(const int8_t *,const int8_t *,const float *,unsigned,float);
extern int32_t glv_tts_dot_int8_pie(const int8_t *,const int8_t *,unsigned);
extern int glv_tts_int8_context_probe(const int8_t *,const int8_t *,int32_t,unsigned);
#endif
static int32_t scalar_dot(const int8_t *w,const int8_t *x)
{int32_t sum=0;for(unsigned i=0;i<32;i++)sum+=(int32_t)w[i]*x[i];return sum;}
static int dot_test(void)
{
#ifdef __NuttX__
  _Alignas(16) int8_t w[32],x[32];unsigned seed=71;
  for(unsigned run=0;run<32;run++){
    for(unsigned i=0;i<32;i++){seed=seed*1664525u+1013904223u;w[i]=(int)(seed%255)-127;seed=seed*1664525u+1013904223u;x[i]=(int)(seed%255)-127;}
    if(run<4)for(unsigned i=0;i<32;i++){w[i]=run&1?127:-127;x[i]=run&2?127:-127;}
    int32_t expected=scalar_dot(w,x);
    if(glv_tts_dot_int8_pie(w,x,32)!=expected)return -EILSEQ;
  }
  _Alignas(16) int8_t wg[576],xg[576];float scales[18];
  const unsigned groups[]={1,7,18};
  for(unsigned test=0;test<3;test++){
    for(unsigned i=0;i<576;i++){seed=seed*1664525u+1013904223u;wg[i]=(int)(seed%255)-127;seed=seed*1664525u+1013904223u;xg[i]=(int)(seed%255)-127;}
    float expected=0,ws=.00390625f;
    for(unsigned g=0;g<groups[test];g++){scales[g]=(g+1)*.000137f;int32_t n=scalar_dot(wg+g*32,xg+g*32);expected+=((float)n*scales[g])*ws;}
    float actual=glv_llm_dot_groups_pie(wg,xg,scales,groups[test],ws);
    if(memcmp(&actual,&expected,4))return -EILSEQ;
  }
  /* The context probe consumes ONE 16-byte vector twice. Its last
   * argument is the number of scheduler-yield rounds, not vector length.
   * The arithmetic dot test above separately checks all 32 coefficients. */
  int32_t expected16=0;
  for(unsigned i=0;i<16;i++)expected16+=(int32_t)w[i]*x[i];
  if(glv_tts_int8_context_probe(w,x,2*expected16,16))return -EILSEQ;
#endif
  return 0;
}
/* rdcycle is the same counter already used by the ESP-DL port. Each
 * measured span is bounded below one 32-bit wrap; totals accumulate in64. */
#ifdef __NuttX__
#include <nuttx/config.h>
#define TICKS_PER_US CONFIG_ESPRESSIF_CPU_FREQ_MHZ
static inline uint32_t ticks(void){uint32_t n;__asm__ volatile("rdcycle %0":"=r"(n));return n;}
#else
#define TICKS_PER_US 1u
static inline uint32_t ticks(void){return (uint32_t)glv_now_us();}
#endif
static inline bool finite32(float x)
{uint32_t bits;memcpy(&bits,&x,4);return (bits&0x7f800000u)!=0x7f800000u;}
int glv_w8a8_prepare(struct glv_model *m)
{
 if(!m||!m->memory||!m->data)return -EINVAL;
 if(m->w8a8_validated)return 0;
 uint32_t began=ticks();int ret=0;unsigned matrices=0;
 for(unsigned i=0;i<m->count;i++){
  const struct glv_tensor *t=m->tensors+i;if(t->type!=2)continue;
  unsigned rows=t->shape[0],cols=t->shape[1];
  if(t->ndim!=2||!rows||rows>8192||!cols||cols>576||cols%32||t->stride!=cols+4u||
     t->offset>m->bytes||(size_t)rows*t->stride>m->bytes-t->offset||
     t->bytes!=(size_t)rows*t->stride){ret=-EBADMSG;break;}
  for(unsigned r=0;r<rows;r++){
   if(!(r&15u)&&(ret=glv_cancel(m->memory)))goto done;
   bool planar=glv_model_q8_planar(m,t);
   const unsigned char *base=m->data+t->offset;
   const unsigned char *p=planar?base+(size_t)r*cols:base+(size_t)r*t->stride+4u;
   float scale;memcpy(&scale,planar?base+(size_t)rows*cols+r*4u:p-4u,4);
   if(!finite32(scale)||scale<=0){ret=-EBADMSG;goto done;}
   for(unsigned c=0;c<cols;c++)if(p[c]==128){ret=-EBADMSG;goto done;}
  }matrices++;
 }
 if(!ret&&matrices!=113)ret=-EBADMSG;
 if(!ret&&!m->w8a8_workspace){
  m->w8a8_workspace=glv_alloc(m->memory,GLV_MATBATCH_MAX*(576u+18u*sizeof(float))+15u);
  if(!m->w8a8_workspace)ret=-ENOMEM;
 }
 if(!ret){
  /* Model bytes are already SHA-authenticated and private. Reorder only
   * representation: same signed codes/scales, no new quantization. Reserve
   * the largest tensor once; failure leaves the original reader usable. */
  size_t maximum=0;
  for(unsigned i=0;i<m->count;i++)if(m->tensors[i].type==2&&!glv_model_q8_planar(m,m->tensors+i)&&m->tensors[i].bytes>maximum)maximum=m->tensors[i].bytes;
  unsigned char *temporary=maximum?glv_alloc(m->memory,maximum):NULL;
  if(temporary){
   uint32_t repack=ticks();
   for(unsigned i=0;i<m->count;i++){
    struct glv_tensor *t=m->tensors+i;if(t->type!=2||glv_model_q8_planar(m,t))continue;
    if((ret=glv_cancel(m->memory)))break;
    unsigned rows=t->shape[0],cols=t->shape[1];unsigned char *base=m->data+t->offset;
    for(unsigned row=0;row<rows;row++){
     memcpy(temporary+(size_t)row*cols,base+(size_t)row*t->stride+4u,cols);
     memcpy(temporary+(size_t)rows*cols+row*4u,base+(size_t)row*t->stride,4);
    }
    memcpy(base,temporary,t->bytes);m->w8a8_planar[i/8]|=1u<<(i%8);
   }
   glv_free(temporary);m->memory->profile.llm_repack_ticks+=(uint32_t)(ticks()-repack);
  }
  if(!ret)m->w8a8_validated=true;
 }
done:m->memory->profile.llm_prepare_ticks+=(uint32_t)(ticks()-began);return ret;
}
int glv_w8a8_matbatch_cached(struct glv_model *m,const char *name,const float *x,float *out,unsigned rows,unsigned cols,unsigned batch,bool reuse)
{
 if(!m||!m->memory||!x||!out||!batch||batch>GLV_MATBATCH_MAX||!cols||cols>576||cols%32)return -EINVAL;
 const struct glv_tensor *t=glv_tensor(m,name);
 if(!t||t->type!=2||t->ndim!=2||t->shape[0]!=rows||t->shape[1]!=cols||t->stride!=cols+4)return -EBADMSG;
 int ret=0;if(m->data&&!m->w8a8_validated&&(ret=glv_w8a8_prepare(m)))return ret;
 bool resident=m->data&&m->w8a8_validated;bool planar=resident&&glv_model_q8_planar(m,t);
 if(resident&&(t->offset>m->bytes||(size_t)rows*t->stride>m->bytes-t->offset))return -EBADMSG;
 uint32_t matrix_start=ticks(),section=ticks();unsigned groups=cols/32;
 uint64_t compute_ticks=0,row_ticks=0,fast_rows=0,direct_rows=0,dots=0;
 /* One owner-held workspace replaces two heap admissions per matrix.
  * mallinfo admission walks the PSRAM arena; it is not an arithmetic op. */
 void *raw=m->w8a8_workspace?m->w8a8_workspace:glv_alloc(m->memory,(size_t)batch*cols+15);
 int8_t *codes=(int8_t *)(((uintptr_t)raw+15)&~(uintptr_t)15);
 float *scales=m->w8a8_workspace?(float *)(codes+GLV_MATBATCH_MAX*576u):glv_alloc(m->memory,(size_t)batch*groups*sizeof(float));
 _Alignas(16) int8_t weights[576];unsigned char packed[580];
 if(!raw||!scales){ret=-ENOMEM;goto done;}
 if(reuse&&m->w8a8_workspace&&m->w8a8_input==x&&m->w8a8_columns==cols&&m->w8a8_batch==batch){
  m->memory->profile.llm_quant_reuses++;
 }else{
 m->w8a8_input=NULL;
 for(unsigned b=0;b<batch;b++)for(unsigned g=0;g<groups;g++){
  const float *input=x+(size_t)b*cols+g*32;float maximum=0;
  for(unsigned i=0;i<32;i++){if(!finite32(input[i])){ret=-ERANGE;goto done;}float a=fabsf(input[i]);if(a>maximum)maximum=a;}
  scales[b*groups+g]=maximum/127.f;
  for(unsigned i=0;i<32;i++){
   float v=maximum?input[i]/maximum*127.f:0;
   /* Finite magnitude <=127: unsigned conversion truncates exactly
    * like floorf; keep the same division/addition before the conversion. */
   unsigned q=(unsigned)(fabsf(v)+.5f);if(q>127)q=127;
   codes[(size_t)b*cols+g*32+i]=(int8_t)(v<0?-(int)q:(int)q);
  }
 }
 m->w8a8_input=x;m->w8a8_columns=cols;m->w8a8_batch=batch;
 }
 m->memory->profile.llm_quant_ticks+=(uint32_t)(ticks()-section);
#ifdef __NuttX__
 unsigned saved;__asm__ volatile("csrrw %0,0x7f2,%1":"=r"(saved):"r"(1u):"memory");
 if(!m->memory->profile.llm_w8a8_selftests){ret=dot_test();if(!ret)m->memory->profile.llm_w8a8_selftests++;}
 __asm__ volatile("csrw 0x7f2,%0"::"r"(saved):"memory");
 if(ret)goto done;
#else
 if(!m->memory->profile.llm_w8a8_selftests){ret=dot_test();if(ret)goto done;m->memory->profile.llm_w8a8_selftests++;}
#endif
 m->memory->profile.llm_w8a8_calls++;
 for(unsigned r=0;r<rows;r++){
  /* A checkpoint before each bounded tile (<=8*64*576 MACs), plus one
   * after it completes. Storage epoch and cancellation are both checked;
   * publication performs the original full check again. No action retries. */
  if(!(r&7u)){
   section=ticks();ret=glv_cancel(m->memory);
   m->memory->profile.llm_check_ticks+=(uint32_t)(ticks()-section);m->memory->profile.llm_checkpoints++;
   if(ret)break;
  }
  section=ticks();float ws;const int8_t *row_weights=weights;
  if(planar){
   const unsigned char *p=m->data+t->offset;
   ws=*(const float *)(p+(size_t)rows*cols+r*4u);
   row_weights=(const int8_t *)p+(size_t)r*cols;fast_rows++;direct_rows++;
   if(batch>1){memcpy(weights,row_weights,cols);row_weights=weights;}
  }else if(resident){
   const unsigned char *p=m->data+t->offset+(size_t)r*t->stride;
   memcpy(&ws,p,4);memcpy(weights,p+4,cols);
   fast_rows++;
  }else{
   if((ret=glv_tensor_q8_packed(m,t,r,packed,sizeof(packed),cols)))break;
   memcpy(&ws,packed,4);memcpy(weights,packed+4,cols);
   for(unsigned i=0;i<cols;i++)if(weights[i]==-128){ret=-EBADMSG;break;}
   if(ret)break;
  }
  row_ticks+=(uint32_t)(ticks()-section);section=ticks();
#ifdef __NuttX__
  __asm__ volatile("csrrw %0,0x7f2,%1":"=r"(saved):"r"(1u):"memory");
#endif
  for(unsigned b=0;b<batch;b++){
   float sum=0;
#ifdef __NuttX__
   sum=glv_llm_dot_groups_pie(row_weights,codes+(size_t)b*cols,scales+(size_t)b*groups,groups,ws);
#else
   for(unsigned g=0;g<groups;g++){
    const int8_t *w=row_weights+g*32,*a=codes+(size_t)b*cols+g*32;
#ifdef __NuttX__
    int32_t integer=glv_tts_dot_int8_pie(w,a,32);
#else
    int32_t integer=scalar_dot(w,a);
#endif
    sum+=((float)integer*scales[b*groups+g])*ws;
   }
#endif
   out[(size_t)b*rows+r]=sum;if(!finite32(sum))ret=-ERANGE;
  }
#ifdef __NuttX__
  __asm__ volatile("csrw 0x7f2,%0"::"r"(saved):"memory");
#endif
  compute_ticks+=(uint32_t)(ticks()-section);
  dots+=(uint64_t)batch*groups;if(ret)break;
 }
 if(!ret){section=ticks();ret=glv_cancel(m->memory);m->memory->profile.llm_check_ticks+=(uint32_t)(ticks()-section);m->memory->profile.llm_checkpoints++;}
done:
 m->memory->profile.llm_compute_ticks+=compute_ticks;
 m->memory->profile.llm_row_ticks+=row_ticks;m->memory->profile.llm_fast_rows+=fast_rows;
 m->memory->profile.weight_q8_bulk_rows+=fast_rows;m->memory->profile.llm_planar_rows+=direct_rows;
 m->memory->profile.llm_w8a8_dots+=dots;
 m->memory->profile.matmul_us+=compute_ticks/TICKS_PER_US;
 if(!m->w8a8_workspace){glv_free(scales);glv_free(raw);}m->memory->profile.llm_matrix_ticks+=(uint32_t)(ticks()-matrix_start);return ret;
}

int glv_w8a8_matbatch(struct glv_model *m,const char *name,const float *x,float *out,unsigned rows,unsigned cols,unsigned batch)
{return glv_w8a8_matbatch_cached(m,name,x,out,rows,cols,batch,false);}
