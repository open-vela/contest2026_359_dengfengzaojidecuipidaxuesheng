/* SPDX-License-Identifier: Apache-2.0 */
/* Weight values/scales are unchanged. Two S16 activation pieces retain the
 * residual of the first quantization; sensitive outputs use original FP32. */
#include "local_asr_accel.h"
#include <errno.h>
#include <float.h>
#include <math.h>
#include <string.h>
#ifdef __NuttX__
#ifndef GLV_TTS_PIE
#error ASR PIE requires the qualified S16 dot/context kernel
#endif
extern int32_t glv_tts_dot_pie(const int16_t *,const int16_t *,unsigned);
extern int glv_tts_pie_context_probe(const int16_t *,const int16_t *,int32_t,unsigned);
#endif
static int32_t dot32(const int16_t *w,const int16_t *x)
{
#ifdef __NuttX__
 /* Vector loads require 16-byte alignment. Keep a scalar safety net
  * even though activation/code allocations are explicitly aligned. */
 if(((uintptr_t)w|(uintptr_t)x)&15u){int32_t n=0;for(unsigned i=0;i<32;i++)n+=(int32_t)w[i]*x[i];return n;}
 unsigned saved;__asm__ volatile("csrrw %0,0x7f2,%1":"=r"(saved):"r"(1u):"memory");
 int32_t n=glv_tts_dot_pie(w,x,32);__asm__ volatile("csrw 0x7f2,%0"::"r"(saved):"memory");return n;
#else
 int32_t n=0;for(unsigned i=0;i<32;i++)n+=(int32_t)w[i]*x[i];return n;
#endif
}
static bool pie_selftest(void)
{
 _Alignas(16) int16_t w[32],x[32];unsigned seed=79;
 for(unsigned run=0;run<20;run++){
  int32_t expected=0;
  for(unsigned i=0;i<32;i++){
   seed=seed*1664525u+1013904223u;w[i]=(int)(seed%256u)-128;
   seed=seed*1664525u+1013904223u;x[i]=(int)(seed%65535u)-32767;
   if(run<4){w[i]=run&1?127:-128;x[i]=run&2?32767:-32767;}
   expected+=(int32_t)w[i]*x[i];
  }
  if(dot32(w,x)!=expected)return false;
 }
#ifdef __NuttX__
 int32_t expected=0;for(unsigned i=0;i<8;i++)expected+=(int32_t)w[i]*x[i];
 unsigned saved;__asm__ volatile("csrrw %0,0x7f2,%1":"=r"(saved):"r"(1u):"memory");
 int ret=glv_tts_pie_context_probe(w,x,expected*2,16);
 __asm__ volatile("csrw 0x7f2,%0"::"r"(saved):"memory");if(ret)return false;
#endif
 return true;
}
struct quantized {
 void *storage;int16_t *high,*low;float *s0,*s1,*error;bool *valid;
 unsigned padded,groups;
};
static int16_t quantize(float x,float scale)
{if(!glv_finite(x)||!glv_finite(scale)||scale<=0)return 0;float v=x/scale;if(!glv_finite(v))return 0;unsigned n=(unsigned)(fabsf(v)+.5f);if(n>32767)n=32767;return v<0?-(int)n:(int)n;}
static struct quantized quantized_input(struct glv_memory *m,const float *x,unsigned rows,unsigned cols)
{
 struct quantized q={0};q.padded=(cols+31u)&~31u;q.groups=q.padded/32;
 if(!q.padded||rows>65536u/q.padded)return q;
 size_t elements=(size_t)rows*q.padded,groups=(size_t)rows*q.groups;
 size_t bytes=elements*4u+groups*12u+rows;q.storage=glv_alloc(m,bytes+15u);if(!q.storage)return q;
 q.high=(int16_t *)(((uintptr_t)q.storage+15u)&~(uintptr_t)15u);q.low=q.high+elements;q.s0=(float *)(q.low+elements);q.s1=q.s0+groups;q.error=q.s1+groups;q.valid=(bool *)(q.error+groups);
 for(unsigned r=0;r<rows;r++){
  q.valid[r]=true;
  for(unsigned g=0;g<q.groups;g++){
   float maximum=0,residual[32],rest=0;unsigned begin=g*32;
   for(unsigned i=0;i<32;i++)if(begin+i<cols){float v=x[(size_t)r*cols+begin+i];if(!glv_finite(v)){q.valid[r]=false;v=0;}if(fabsf(v)>maximum)maximum=fabsf(v);}
   size_t at=(size_t)r*q.padded+begin,sg=(size_t)r*q.groups+g;q.s0[sg]=maximum/32767.f;
   if(maximum&&!q.s0[sg])q.valid[r]=false;
   for(unsigned i=0;i<32;i++){
    float v=begin+i<cols?x[(size_t)r*cols+begin+i]:0;
    if(!glv_finite(v))v=0;q.high[at+i]=quantize(v,q.s0[sg]);residual[i]=v-(float)q.high[at+i]*q.s0[sg];if(!glv_finite(residual[i])){q.valid[r]=false;residual[i]=0;}if(fabsf(residual[i])>rest)rest=fabsf(residual[i]);
   }
   q.s1[sg]=rest/32767.f;if(rest&&!q.s1[sg])q.valid[r]=false;
   for(unsigned i=0;i<32;i++){
    q.low[at+i]=quantize(residual[i],q.s1[sg]);
    float v=begin+i<cols?x[(size_t)r*cols+begin+i]:0;
    float reconstructed=(float)q.high[at+i]*q.s0[sg]+(float)q.low[at+i]*q.s1[sg];
    float error=fabsf(v-reconstructed);if(error>q.error[sg])q.error[sg]=error;
   }
  }
 }
 return q;
}
struct lane_stats {uint64_t dots,fallbacks,selftests,selftest_failures,us;};
struct matrix_task {
 const float *x,*weights,*scale,*l1;const int16_t *codes;const bool *packed;
 float *y;unsigned rows,k,n,base;struct quantized q;bool pie;
 struct lane_stats stats[2];
};
static int matrix_body_inner(void *opaque,unsigned first,unsigned last,struct glv_asr_lane *lane)
{
 struct matrix_task *t=opaque;struct lane_stats *stats=t->stats+lane->index;
 bool pie=t->pie;
 if(pie&&!*lane->pie_state){*lane->pie_state=pie_selftest()?1:-1;stats->selftests++;if(*lane->pie_state<0)stats->selftest_failures++;}
 pie=pie&&*lane->pie_state>0;
 for(unsigned o=first;o<last;o++){
  int ret=glv_asr_lane_cancel(lane);if(ret)return ret;
  const float *w=t->weights+(size_t)o*t->k;
  for(unsigned r=0;r<t->rows;r++){
   bool fast=pie&&t->packed[o]&&t->q.valid[r];float sum=0,bound=0;
   if(fast){
    const int16_t *code=t->codes+(size_t)o*t->q.padded,*a=t->q.high+(size_t)r*t->q.padded,*b=t->q.low+(size_t)r*t->q.padded;
    for(unsigned g=0;g<t->q.groups;g++){
     size_t sg=(size_t)r*t->q.groups+g,wg=(size_t)o*t->q.groups+g;
     int32_t hi=dot32(code+g*32,a+g*32),lo=dot32(code+g*32,b+g*32);
     sum+=((float)hi*t->q.s0[sg]+(float)lo*t->q.s1[sg])*t->scale[wg];
     bound+=t->q.error[sg]*t->l1[wg];stats->dots+=2;
    }
    /* Guard the activation representation loss, especially near cancellation.
     * FP reduction reordering is separately covered by operator/end-to-end QA. */
    if(!glv_finite(sum)||!glv_finite(bound)||bound>2e-6f+2e-5f*fabsf(sum))fast=false;
   }
   if(!fast){
    if(pie&&t->packed[o])stats->fallbacks++;
    sum=0;const float *src=t->x+(size_t)r*t->k;
    for(unsigned i=0;i<t->k;i++)sum+=src[i]*w[i];
   }
   t->y[(size_t)r*t->n+t->base+o]=sum;
   if((r&31u)==31u&&(ret=glv_asr_lane_cancel(lane)))return ret;
  }
 }
 return 0;
}
static int matrix_body(void *opaque,unsigned first,unsigned last,struct glv_asr_lane *lane)
{uint64_t began=glv_now_us();int ret=matrix_body_inner(opaque,first,last,lane);((struct matrix_task *)opaque)->stats[lane->index].us+=glv_now_us()-began;return ret;}
static void collect(struct glv_memory *m,const struct lane_stats s[2])
{for(unsigned i=0;i<2;i++){m->profile.asr_math_us+=s[i].us;m->profile.asr_pie_dots+=s[i].dots;m->profile.asr_pie_fallbacks+=s[i].fallbacks;m->profile.asr_pie_selftests+=s[i].selftests;m->profile.asr_pie_selftest_failures+=s[i].selftest_failures;}}
int glv_asr_accel_matmul(struct glv_memory *m,const float *x,float *y,unsigned rows,unsigned k,unsigned n,const struct glv_asr_rows *reader)
{
 if(k>32768u||(!reader->prepare&&(uint64_t)rows*k*n<262144u))return 1;
 struct quantized q={0};bool pie=!m->asr_no_pie&&reader->prepare&&k>=32;
 if(pie){uint64_t began=glv_now_us();q=quantized_input(m,x,rows,k);m->profile.asr_quant_us+=glv_now_us()-began;pie=q.storage!=NULL;}
 size_t row_bytes=(size_t)k*4u+(pie?(size_t)q.padded*2u+q.groups*8u:0);
 unsigned tile=row_bytes?32768u/row_bytes:0;if(tile>64)tile=64;if(tile>n)tile=n;
 float *weights=NULL;for(;tile;tile/=2){weights=glv_alloc(m,(size_t)tile*k*4u);if(weights)break;}
 if(!weights){glv_free(q.storage);return 1;}
 void *packed_storage=pie?glv_alloc(m,(size_t)tile*q.padded*2u+(size_t)tile*q.groups*8u+15u):NULL;
 if(pie&&!packed_storage){glv_free(q.storage);memset(&q,0,sizeof(q));pie=false;}
 int16_t *codes=packed_storage?(int16_t *)(((uintptr_t)packed_storage+15u)&~(uintptr_t)15u):NULL;float *scale=pie?(float *)(codes+(size_t)tile*q.padded):NULL,*l1=pie?scale+(size_t)tile*q.groups:NULL;
 int ret=0;bool packed[64];
 for(unsigned base=0;base<n&&!ret;base+=tile){
  unsigned count=n-base;if(count>tile)count=tile;
  for(unsigned o=0;o<count&&!ret;o++){
   if((ret=glv_cancel(m)))break;
   if(pie){int fmt=reader->prepare(reader->opaque,base+o,weights+(size_t)o*k,codes+(size_t)o*q.padded,scale+(size_t)o*q.groups,l1+(size_t)o*q.groups,k);if(fmt<0)ret=fmt;packed[o]=fmt>0;}
   else {ret=reader->read(reader->opaque,base+o,weights+(size_t)o*k,k);packed[o]=false;}
  }
  if(ret)break;
  struct matrix_task task={.x=x,.weights=weights,.scale=scale,.l1=l1,.codes=codes,.packed=packed,.y=y,.rows=rows,.k=k,.n=n,.base=base,.q=q,.pie=pie};
  ret=glv_asr_parallel_run(m,matrix_body,&task,count,(uint64_t)rows*k*count);collect(m,task.stats);
 }
 glv_free(packed_storage);glv_free(weights);glv_free(q.storage);return ret;
}
struct conv_task {
 const float *x,*bias,*weights;float *y;const struct glv_asr_conv *c;
 unsigned base,ic,k,width,height;size_t plane,output_plane;
};
/* With zero padding every sampled coordinate is in bounds by the validated
 * output-size formula. Hoist coordinate/bound checks out of each MAC, keeping
 * exactly the reference input-channel/kernel order and separate FP32 mul/add. */
static int conv_unpadded(struct conv_task *t,unsigned first,unsigned last,struct glv_asr_lane *lane)
{
 const struct glv_asr_conv *c=t->c;unsigned oc=c->outputs/c->groups;
 bool pointwise=c->kernel_h==1&&c->kernel_w==1&&c->stride_h==1&&c->stride_w==1;
 for(unsigned row=first;row<last;row++){
  unsigned o=t->base+row;const float *weight=t->weights+(size_t)row*t->k;
  const float *input=t->x+(size_t)(o/oc)*t->ic*t->plane;
  for(size_t at=0;at<t->output_plane;at+=128){
   int ret=glv_asr_lane_cancel(lane);if(ret)return ret;
   unsigned count=t->output_plane-at;if(count>128)count=128;
   float sums[128];size_t offsets[128];
   for(unsigned p=0;p<count;p++)sums[p]=t->bias?t->bias[o]:0;
   if(pointwise){
    for(unsigned i=0;i<t->ic;i++){
     const float *src=input+(size_t)i*t->plane+at;float value=weight[i];
     for(unsigned p=0;p<count;p++)sums[p]+=src[p]*value;
    }
   }else{
    for(unsigned p=0;p<count;p++){size_t pos=at+p;
     offsets[p]=(pos/t->width*c->stride_h)*c->width+(pos%t->width*c->stride_w);
    }
    for(unsigned i=0;i<t->ic;i++)for(unsigned h=0;h<c->kernel_h;h++)for(unsigned w=0;w<c->kernel_w;w++){
     float value=weight[((size_t)i*c->kernel_h+h)*c->kernel_w+w];
     const float *src=input+(size_t)i*t->plane+(size_t)h*c->dilation_h*c->width+w*c->dilation_w;
     for(unsigned p=0;p<count;p++)sums[p]+=src[offsets[p]]*value;
    }
   }
   memcpy(t->y+(size_t)o*t->output_plane+at,sums,(size_t)count*sizeof(float));
  }
 }
 return 0;
}
static int conv_body(void *opaque,unsigned first,unsigned last,struct glv_asr_lane *lane)
{
 struct conv_task *t=opaque;const struct glv_asr_conv *c=t->c;unsigned oc=c->outputs/c->groups;
 if(!c->top&&!c->left&&!c->bottom&&!c->right)return conv_unpadded(t,first,last,lane);
 for(unsigned row=first;row<last;row++){
  unsigned o=t->base+row;const float *weight=t->weights+(size_t)row*t->k;
  for(size_t at=0;at<t->output_plane;at+=128){
   int ret=glv_asr_lane_cancel(lane);if(ret)return ret;
   unsigned n=t->output_plane-at;if(n>128)n=128;float sums[128];int ys[128],xs[128];
   for(unsigned i=0;i<n;i++){size_t pos=at+i;sums[i]=t->bias?t->bias[o]:0;ys[i]=(int)(pos/t->width*c->stride_h)-(int)c->top;xs[i]=(int)(pos%t->width*c->stride_w)-(int)c->left;}
   /* Exactly the original FP32 reduction for Conv; parallelize channels only. */
   for(unsigned i=0;i<t->ic;i++)for(unsigned h=0;h<c->kernel_h;h++)for(unsigned w=0;w<c->kernel_w;w++){
    float value=weight[((size_t)i*c->kernel_h+h)*c->kernel_w+w];const float *src=t->x+((size_t)(o/oc)*t->ic+i)*t->plane;
    for(unsigned p=0;p<n;p++){int yy=ys[p]+(int)(h*c->dilation_h),xx=xs[p]+(int)(w*c->dilation_w);if(yy>=0&&(unsigned)yy<c->height&&xx>=0&&(unsigned)xx<c->width)sums[p]+=src[(size_t)yy*c->width+xx]*value;}
   }
   for(unsigned p=0;p<n;p++)t->y[(size_t)o*t->output_plane+at+p]=sums[p];
  }
 }
 return 0;
}
int glv_asr_accel_conv(struct glv_memory *m,const float *x,const float *bias,float *y,const struct glv_asr_conv *c,const struct glv_asr_rows *reader,unsigned k,unsigned height,unsigned width)
{
 if((uint64_t)k*height*width*c->outputs<262144u)return 1;
 unsigned tile=32768u/((size_t)k*4u);if(tile>64)tile=64;if(tile>c->outputs)tile=c->outputs;
 float *weights=NULL;for(;tile;tile/=2){weights=glv_alloc(m,(size_t)tile*k*4u);if(weights)break;}if(!weights)return 1;
 int ret=0;
 for(unsigned base=0;base<c->outputs&&!ret;base+=tile){
  unsigned count=c->outputs-base;if(count>tile)count=tile;
  for(unsigned o=0;o<count&&!ret;o++)ret=reader->read(reader->opaque,base+o,weights+(size_t)o*k,k);
  if(ret)break;
  struct conv_task t={x,bias,weights,y,c,base,c->channels/c->groups,k,width,height,(size_t)c->height*c->width,(size_t)height*width};
  ret=glv_asr_parallel_run(m,conv_body,&t,count,(uint64_t)k*height*width*count);
 }
 glv_free(weights);return ret;
}

struct integer_task {
 const int16_t *x,*w;int32_t *y;unsigned rows,padded,cols,base;bool pie;
 struct lane_stats stats[2];
};
/* The qualified shared PIE kernel accepts at most256 items per call. Keep
 * that boundary, alignment and the original TTS assembly unchanged. INT8
 * zero-point differences are in [-255,255], so each partial fits INT32;
 * the admitted <=32768-item whole dot is bounded by32768*255*255. */
static int32_t integer_dot_block(const int16_t *w,const int16_t *x,unsigned n)
{
#ifdef __NuttX__
 if(!(((uintptr_t)w|(uintptr_t)x)&15u)){
  unsigned saved;__asm__ volatile("csrrw %0,0x7f2,%1":"=r"(saved):"r"(1u):"memory");
  int32_t value=glv_tts_dot_pie(w,x,n);
  __asm__ volatile("csrw 0x7f2,%0"::"r"(saved):"memory");return value;
 }
#endif
 int32_t value=0;for(unsigned i=0;i<n;i++)value+=(int32_t)w[i]*x[i];return value;
}
static bool integer_block_selftest(void)
{
 _Alignas(16) int16_t w[256],x[256];unsigned seed=125;
 for(unsigned run=0;run<8;run++){
  for(unsigned i=0;i<256;i++){
   seed=seed*1664525u+1013904223u;w[i]=run<4?(run&1?255:-255):(int)(seed%511u)-255;
   seed=seed*1664525u+1013904223u;x[i]=run<4?(run&2?255:-255):(int)(seed%511u)-255;
  }
  for(unsigned n=32;n<=256;n*=2){int32_t expected=0;
   for(unsigned i=0;i<n;i++)expected+=(int32_t)w[i]*x[i];
   if(integer_dot_block(w,x,n)!=expected)return false;
  }
 }
 return true;
}
static int integer_body(void *opaque,unsigned first,unsigned last,struct glv_asr_lane *lane)
{
 struct integer_task *t=opaque;struct lane_stats *s=&t->stats[lane->index];bool pie=t->pie;
 if(pie&&*lane->pie_state>=0&&*lane->pie_state<2){
  bool qualified=*lane->pie_state>0||pie_selftest();
  *lane->pie_state=qualified&&integer_block_selftest()?2:-1;
  s->selftests++;if(*lane->pie_state<0)s->selftest_failures++;
 }
 pie=pie&&*lane->pie_state>0;uint64_t began=glv_now_us();
 for(unsigned o=first;o<last;o++){
  int ret=glv_asr_lane_cancel(lane);if(ret)return ret;
  const int16_t *w=t->w+(size_t)o*t->padded;
  for(unsigned r=0;r<t->rows;r++){
   const int16_t *x=t->x+(size_t)r*t->padded;int32_t sum=0;
   if(pie){for(unsigned k=0;k<t->padded;k+=256){unsigned n=t->padded-k;if(n>256)n=256;
     sum+=integer_dot_block(w+k,x+k,n);s->dots+=n/32u;}}
   else for(unsigned k=0;k<t->padded;k++)sum+=(int32_t)w[k]*x[k];
   t->y[(size_t)r*t->cols+t->base+o]=sum;
   if((r&31u)==31u&&(ret=glv_asr_lane_cancel(lane)))return ret;
  }
 }
 s->us+=glv_now_us()-began;return 0;
}
int glv_asr_accel_integer(struct glv_memory *m,const uint8_t *x,int32_t *y,unsigned rows,unsigned k,unsigned cols,
                         unsigned az,int bz,const struct glv_asr_integer_rows *reader)
{
 if(!m||!x||!y||!reader||!reader->read||!rows||!k||k>32768u||!cols||az>255||bz< -128||bz>127)return -EINVAL;
 if(m->asr_reference)return 1;
 unsigned padded=(k+31u)&~31u,tile=32768u/(padded*2u);if(tile>32)tile=32;if(tile>cols)tile=cols;if(!tile)tile=1;
 if((size_t)rows>SIZE_MAX/padded-tile)return -EOVERFLOW;
 size_t elements=((size_t)rows+tile)*padded;
 if(elements>(SIZE_MAX-31u-(size_t)k)/2u)return -EOVERFLOW;
 unsigned char *storage=glv_alloc(m,elements*2u+31u+k);if(!storage)return 1;
 int16_t *a=(int16_t *)(((uintptr_t)storage+15u)&~(uintptr_t)15u),*w=a+(size_t)rows*padded;
 int8_t *raw=(int8_t *)(w+(size_t)tile*padded);int ret=0;
 for(unsigned r=0;r<rows;r++){
  if((ret=glv_cancel(m)))break;
  for(unsigned j=0;j<padded;j++)a[(size_t)r*padded+j]=j<k?(int)x[(size_t)r*k+j]-(int)az:0;
 }
 for(unsigned base=0;base<cols&&!ret;base+=tile){
  unsigned count=cols-base;if(count>tile)count=tile;
  for(unsigned o=0;o<count&&!ret;o++){
   ret=reader->read(reader->opaque,base+o,raw,k);if(ret)break;
   for(unsigned j=0;j<padded;j++)w[(size_t)o*padded+j]=j<k?(int)raw[j]-bz:0;
  }
  if(ret)break;
  struct integer_task task={.x=a,.w=w,.y=y,.rows=rows,.padded=padded,.cols=cols,.base=base,.pie=!m->asr_no_pie};
  ret=glv_asr_parallel_run(m,integer_body,&task,count,(uint64_t)rows*k*count);
  for(unsigned i=0;i<2;i++){
   m->profile.asr_pie_dots+=task.stats[i].dots;m->profile.asr_pie_selftests+=task.stats[i].selftests;
   m->profile.asr_pie_selftest_failures+=task.stats[i].selftest_failures;m->profile.asr_math_us+=task.stats[i].us;
  }
 }
 glv_free(storage);return ret;
}

/* Diagnostic on a worker owned heap. Padding keeps the deliberately offset
 * raw vector loads within this private allocation. Never used for inference. */
int glv_asr_accel_probe(struct glv_memory *m)
{
 unsigned char *raw=glv_alloc(m,512);if(!raw)return -ENOMEM;
 int16_t *w=(int16_t *)(((uintptr_t)raw+31u)&~(uintptr_t)15u),*x=w+96;
 unsigned mismatches=0,guard_failures=0;int32_t expected=0;
 for(unsigned off=0;off<=8;off+=8){
  int16_t *a=(int16_t *)((char *)w+off),*b=(int16_t *)((char *)x+off);
  expected=0;for(unsigned i=0;i<40;i++){a[i]=(int16_t)((int)i-17);b[i]=(int16_t)(i*71-1000);if(i<32)expected+=(int32_t)a[i]*b[i];}
  int32_t guarded=dot32(a,b),native=guarded;
#ifdef __NuttX__
  unsigned saved;__asm__ volatile("csrrw %0,0x7f2,%1":"=r"(saved):"r"(1u):"memory");
  native=glv_tts_dot_pie(a,b,32);__asm__ volatile("csrw 0x7f2,%0"::"r"(saved):"memory");
#endif
  if(native!=expected)mismatches++;if(guarded!=expected)guard_failures++;
  printf("ASR_ALIGN_PROBE offset=%u address_mod16=%u expected=%ld raw=%ld guarded=%ld\n",off,(unsigned)((uintptr_t)a&15u),(long)expected,(long)native,(long)guarded);
 }
 printf("ASR_ALIGN_RESULT raw_mismatches=%u guarded_failures=%u heap_mod16=%u\n",mismatches,guard_failures,(unsigned)((uintptr_t)raw&15u));
 bool batch=integer_block_selftest();printf("ASR_INT8_BLOCK_PROBE passed=%u max_items=256\n",batch);
 glv_free(raw);return guard_failures||!batch?-EBADMSG:0;
}
