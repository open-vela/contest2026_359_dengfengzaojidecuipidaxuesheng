/* SPDX-License-Identifier: Apache-2.0 */
/* W8A8 evaluation backend. Separate from the accepted W8A16 implementation.
 * Weights remain in the original SD Q8 format. Do not promote before audio
 * quality, native selftest/context, cancellation and timing acceptance. */
#include "local_tts_int8.h"
#include "local_tts_int8_policy.h"
#ifndef __NuttX__
#include <fenv.h>
#endif
#ifdef GLV_TTS_PARALLEL
#include "local_tts_parallel.h"
#ifndef __NuttX__
#include <fenv.h>
#endif
#endif
#include <errno.h>
#include <math.h>
#include <string.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#if defined(GLV_TTS_PARALLEL) && (!defined(CONFIG_ARCH_RISCV_INTXCPT_EXTENSIONS) || CONFIG_ARCH_RISCV_INTXCPT_EXTREGS < 74)
#error "Parallel PIE requires the reviewed P4 extension save/restore context"
#endif
#endif
#define PATCHES 8u
#define DOT_BLOCK 256u
#define FULL_ROW_MAX 2288u
_Static_assert(FULL_ROW_MAX*128u*128u<INT32_MAX,"Full S8 row fits signed32");
/* RV32F GCC lowers builtin isfinite to a comparison surrounded by two FFLAGS
 * CSR accesses. Inspect the IEEE exponent instead; preserve all FP flags. */
static inline bool finite_bits(float value)
{uint32_t bits;memcpy(&bits,&value,sizeof(bits));return (bits&0x7f800000u)!=0x7f800000u;}
#ifdef __NuttX__
/* Signed-8 PIE kernel. Experimental: activation quantization changes audio. */
extern int32_t glv_tts_dot_int8_pie(const int8_t *,const int8_t *,unsigned);
extern void glv_tts_dot_batch_int8_pie(const int8_t *,const int8_t *,unsigned,unsigned,unsigned,int32_t *);
extern void glv_tts_dot_batch_full_int8_pie(const int8_t *,const int8_t *,unsigned,unsigned,unsigned,int32_t *);
#endif
static int8_t quant(float v,float maximum,int limit)
{
  if(!maximum)return 0;
  float scaled=(v/maximum)*(float)limit;
  int q=(int)(scaled>=0?scaled+.5f:scaled-.5f);
  if(q>limit)q=limit;if(q< -limit)q= -limit;return (int8_t)q;
}
/* A reciprocal replaces per-coefficient division away from rounding borders.
 * For |v|<=maximum and limit<=127, the two rounded paths differ by <1/64
 * even with directed IEEE rounding. Keep a 1/32-wide guard at each half integer
 * and execute the original division there. Nonfinite reciprocals also fall back. */
static inline int8_t quant_recip(float v,float maximum,float factor,int limit)
{
  if(!factor)return quant(v,maximum,limit);
  float scaled=v*factor;int q=(int)(scaled>=0?scaled+.5f:scaled-.5f);
  if(fabsf(scaled-(float)q)>.46875f)return quant(v,maximum,limit);
  if(q>limit)q=limit;if(q< -limit)q= -limit;return (int8_t)q;
}
/* The affine input grid includes real zero. Conversion remains bounded to
 * [-128,127]; a guarded reciprocal preserves the defined division at ties. */
static inline int8_t affine_quant(float v,float scale,float factor,int zero)
{
 float scaled=factor?v*factor:v/scale;
 int q=(int)(scaled>=0?scaled+.5f:scaled-.5f);
 if(factor&&fabsf(scaled-(float)q)>.46875f){scaled=v/scale;q=(int)(scaled>=0?scaled+.5f:scaled-.5f);}
 q+=zero;if(q< -128)q=-128;if(q>127)q=127;return (int8_t)q;
}
/* Preserve the original reciprocal guard; normal model inputs have
 * |v/scale| <=255 (the affine range includes zero). Outside that contract keep
 * the original path. Only the fast mantissa route removes the float/int/float
 * round trip; half-integer guard endpoints stay strict and exact. */
static __attribute__((noinline)) int8_t affine_quant_slow(float v,float scale,float factor,int zero)
{return affine_quant(v,scale,factor,zero);}
static inline int8_t affine_quant_div(float v,float scale,int zero)
{
  float value=v/scale;int q=(int)(value>=0?value+.5f:value-.5f);
  q+=zero;if(q< -128)q=-128;if(q>127)q=127;return (int8_t)q;
}
static __attribute__((always_inline)) inline int8_t affine_quant_bits(float v,float scale,float factor,int zero)
{
  if(!factor)return affine_quant_div(v,scale,zero);
  float value=v*factor;uint32_t bits;memcpy(&bits,&value,4);
  uint32_t mag=bits&0x7fffffffu,q=0;
  if(mag>=0x44000000u)return affine_quant_slow(v,scale,factor,zero);
  if(mag<0x3f000000u){
    if(mag>0x3ef00000u)return affine_quant_div(v,scale,zero);
  }else{
    unsigned shift=150u-(mag>>23); /* exponent[-1,8] -> fractional bits[24,15] */
    uint32_t mant=(mag&0x7fffffu)|0x800000u;
    uint32_t fraction=mant&((1u<<shift)-1u);
    if(fraction>(15u<<(shift-5))&&fraction<(17u<<(shift-5)))
      return affine_quant_div(v,scale,zero);
    q=(mant+(1u<<(shift-1)))>>shift;
  }
  int result=(bits>>31)?-(int)q:(int)q;result+=zero;
  if(result< -128)result=-128;if(result>127)result=127;return (int8_t)result;
}
/* Independent range chains and constant leaky branches. These preserve the
 * original finite checks, include real zero, and never read beyond a row. */
static __attribute__((always_inline)) inline int range_row4(const float *in,unsigned time,bool leaky,float *lo,float *hi)
{
  float l0=*lo,l1=0,l2=0,l3=0,h0=*hi,h1=0,h2=0,h3=0;unsigned t=0;
  for(;t+4<=time;t+=4){
    float a=in[t],b=in[t+1],c=in[t+2],v=in[t+3];
    if(!finite_bits(a)||!finite_bits(b)||!finite_bits(c)||!finite_bits(v))return -EDOM;
    if(leaky){if(a<0)a*=.1f;if(b<0)b*=.1f;if(c<0)c*=.1f;if(v<0)v*=.1f;}
    if(a<l0)l0=a;if(b<l1)l1=b;if(c<l2)l2=c;if(v<l3)l3=v;
    if(a>h0)h0=a;if(b>h1)h1=b;if(c>h2)h2=c;if(v>h3)h3=v;
  }
  for(;t<time;t++){
    float v=in[t];if(!finite_bits(v))return -EDOM;if(leaky&&v<0)v*=.1f;
    if(v<l0)l0=v;if(v>h0)h0=v;
  }
  if(l1<l0)l0=l1;if(l2<l0)l0=l2;if(l3<l0)l0=l3;
  if(h1>h0)h0=h1;if(h2>h0)h0=h2;if(h3>h0)h0=h3;
  *lo=l0;*hi=h0;return 0;
}
static __attribute__((always_inline)) inline void quant_row4(const float *in,int8_t *out,unsigned time,
                                                           float scale,float factor,int zero,bool leaky)
{
  unsigned t=0;
  for(;t+4<=time;t+=4){
    float a=in[t],b=in[t+1],c=in[t+2],v=in[t+3];
    if(leaky){if(a<0)a*=.1f;if(b<0)b*=.1f;if(c<0)c*=.1f;if(v<0)v*=.1f;}
    out[t]=affine_quant_bits(a,scale,factor,zero);out[t+1]=affine_quant_bits(b,scale,factor,zero);
    out[t+2]=affine_quant_bits(c,scale,factor,zero);out[t+3]=affine_quant_bits(v,scale,factor,zero);
  }
  for(;t<time;t++){float v=in[t];if(leaky&&v<0)v*=.1f;out[t]=affine_quant_bits(v,scale,factor,zero);}
}
/* Kept separate so the target audit can check that the common input path has
 * no int-to-float conversion; direct division still handles guarded borders. */
static __attribute__((noinline)) void quant_input4_plain(const float *x,int8_t *y,unsigned n,float scale,float factor,int zero)
{quant_row4(x,y,n,scale,factor,zero,false);}
static __attribute__((noinline)) void quant_input4_leaky(const float *x,int8_t *y,unsigned n,float scale,float factor,int zero)
{quant_row4(x,y,n,scale,factor,zero,true);}
struct weight_entry {int8_t value;uint16_t generation;};
_Static_assert(sizeof(struct weight_entry)==4,"weight memo entry layout");
static bool weight_rounding_supported(void)
{
#ifdef __NuttX__
  unsigned mode;__asm__ volatile("csrr %0,frm" : "=r"(mode));return mode==0;
#else
  return fegetround()==FE_TONEAREST;
#endif
}
static inline int8_t weight_quant(struct weight_entry *table,unsigned generation,
                                   unsigned char code,float scale,float peak,float factor,unsigned *hits,unsigned *misses)
{
  struct weight_entry *e=&table[code];
  if(e->generation==generation){(*hits)++;return e->value;}
  float value=scale*(int8_t)code;int8_t converted=quant_recip(value,peak,factor,127);
  e->value=converted;e->generation=(uint16_t)generation;(*misses)++;return converted;
}
#ifdef GLV_TTS_PARALLEL
struct input_task {
  const float *source;int8_t *dest;float *peaks;
  unsigned time;bool leaky;float maximum,factor;bool affine;float scale;int zero;float lows[2],highs[2];bool fast;
};
static bool input_rounding_supported(void)
{
#ifdef __NuttX__
  unsigned mode;__asm__ volatile("csrr %0,frm" : "=r"(mode));return mode==0;
#else
  return fegetround()==FE_TONEAREST;
#endif
}
static int input_peak_body(void *argument,unsigned first,unsigned last,struct glv_tts_lane *lane)
{
  struct input_task *task=argument;
  if(task->affine){
    float lo=0,hi=0;
    for(unsigned c=first;c<last;c++){
      int ret=glv_tts_parallel_cancel(lane);if(ret)return ret;
      if(task->fast){
        const float *row=task->source+(size_t)c*task->time;
        ret=task->leaky?range_row4(row,task->time,true,&lo,&hi):range_row4(row,task->time,false,&lo,&hi);
        if(ret)return ret;
      }else for(unsigned t=0;t<task->time;t++){
        float v=task->source[(size_t)c*task->time+t];if(!finite_bits(v))return -EDOM;
        if(task->leaky&&v<0)v*=.1f;if(v<lo)lo=v;if(v>hi)hi=v;
      }
    }
    unsigned index=first?1:0;task->lows[index]=lo;task->highs[index]=hi;return 0;
  }
  for(unsigned c=first;c<last;c++){
    int ret=glv_tts_parallel_cancel(lane);if(ret)return ret;
    float peak=0,m1=0,m2=0,m3=0;unsigned t=0;
    for(;t+4<=task->time;t+=4){
      const float *in=task->source+(size_t)c*task->time+t;float a=in[0],b=in[1],d=in[2],e=in[3];
      if(!finite_bits(a)||!finite_bits(b)||!finite_bits(d)||!finite_bits(e))return -EDOM;
      if(task->leaky){if(a<0)a*=.1f;if(b<0)b*=.1f;if(d<0)d*=.1f;if(e<0)e*=.1f;}
      a=fabsf(a);b=fabsf(b);d=fabsf(d);e=fabsf(e);
      if(a>peak)peak=a;if(b>m1)m1=b;if(d>m2)m2=d;if(e>m3)m3=e;
    }
    for(;t<task->time;t++){
      float v=task->source[(size_t)c*task->time+t];if(!finite_bits(v))return -EDOM;
      if(task->leaky&&v<0)v*=.1f;float a=fabsf(v);if(a>peak)peak=a;
    }
    if(m1>peak)peak=m1;if(m2>peak)peak=m2;if(m3>peak)peak=m3;task->peaks[c]=peak;
  }
  return 0;
}
static int input_quant_body(void *argument,unsigned first,unsigned last,struct glv_tts_lane *lane)
{
  struct input_task *task=argument;
  for(unsigned c=first;c<last;c++){
    int ret=glv_tts_parallel_cancel(lane);if(ret)return ret;
    if(task->fast&&task->affine){
      const float *row=task->source+(size_t)c*task->time;int8_t *out=task->dest+(size_t)c*task->time;
      if(task->leaky)quant_input4_leaky(row,out,task->time,task->scale,task->factor,task->zero);
      else quant_input4_plain(row,out,task->time,task->scale,task->factor,task->zero);
    }else for(unsigned t=0;t<task->time;t++){
      float v=task->source[(size_t)c*task->time+t];if(task->leaky&&v<0)v*=.1f;
      task->dest[(size_t)c*task->time+t]=task->affine?affine_quant(v,task->scale,task->factor,task->zero):quant_recip(v,task->maximum,task->factor,127);
    }
  }
  return 0;
}
#endif
static int32_t dot(const int8_t *w,const int8_t *x,unsigned n)
{
  /* |w|,|x| <= 128, n <= FULL_ROW_MAX: |sum| <= 37486592.
   * Both pointers are 16-byte aligned and n is a multiple of sixteen. */
#ifdef __NuttX__
  return glv_tts_dot_int8_pie(w,x,n);
#else
  int32_t sum=0;for(unsigned i=0;i<n;i++)sum+=(int32_t)w[i]*x[i];return sum;
#endif
}
static void batch(const int8_t *w,const int8_t *x,unsigned stride,unsigned rows,unsigned patches,int32_t *out)
{
#ifdef __NuttX__
  glv_tts_dot_batch_int8_pie(w,x,stride,rows,patches,out);
#else
  unsigned blocks=(stride+DOT_BLOCK-1)/DOT_BLOCK;
  for(unsigned r=0;r<rows;r++)for(unsigned p=0;p<patches;p++)for(unsigned b=0;b<blocks;b++){
    unsigned at=b*DOT_BLOCK,n=stride-at;if(n>DOT_BLOCK)n=DOT_BLOCK;
    *out++=dot(w+(size_t)r*stride+at,x+(size_t)p*stride+at,n);
  }
#endif
}
/* Full-row signed8 products remain well inside INT32. Output stays in FP32;
 * only reduction/conversion frequency changes. The old256 path is preserved. */
static void batch_full(const int8_t *w,const int8_t *x,unsigned stride,unsigned rows,unsigned patches,int32_t *out)
{
#ifdef __NuttX__
  glv_tts_dot_batch_full_int8_pie(w,x,stride,rows,patches,out);
#else
  for(unsigned r=0;r<rows;r++)for(unsigned p=0;p<patches;p++)
    *out++=dot(w+(size_t)r*stride,x+(size_t)p*stride,stride);
#endif
}
static __attribute__((noinline)) int selftest_full(struct glv_memory *memory)
{
  static const _Alignas(16) int8_t w[2*FULL_ROW_MAX]={[0 ... FULL_ROW_MAX-1]=127,[FULL_ROW_MAX ... 2*FULL_ROW_MAX-1]=-128};
  static const _Alignas(16) int8_t x[3*FULL_ROW_MAX]={[0 ... FULL_ROW_MAX-1]=-128,[FULL_ROW_MAX ... 2*FULL_ROW_MAX-1]=127,[2*FULL_ROW_MAX ... 3*FULL_ROW_MAX-1]=-97};
  int32_t out[6];const unsigned sizes[]={16,240,256,272,528,1152,FULL_ROW_MAX};
  for(unsigned k=0;k<sizeof(sizes)/sizeof(sizes[0]);k++){
    if(glv_cancel(memory))return -ECANCELED;
    unsigned stride=sizes[k];batch_full(w,x,stride,2,3,out);
    for(unsigned r=0;r<2;r++)for(unsigned p=0;p<3;p++){
      int64_t sum=0;for(unsigned i=0;i<stride;i++)sum+=(int32_t)w[r*stride+i]*x[p*stride+i];
      if(out[r*3+p]!=sum||dot(w+r*stride,x+p*stride,stride)!=sum)return -EILSEQ;
    }
  }
  memory->profile.tts_int8_reduce_selftests++;return 0;
}
static __attribute__((noinline)) int selftest(struct glv_memory *memory)
{
  _Alignas(16) int8_t w[2*528],x[3*528];int32_t results[18];
  unsigned seed=7;
  for(unsigned run=0;run<8;run++){
    for(unsigned i=0;i<2*528;i++){seed=seed*1664525u+1013904223u;w[i]=run<4?((run&1)?127:-127):(int)((seed>>16)%255)-127;}
    for(unsigned i=0;i<3*528;i++){seed=seed*1664525u+1013904223u;x[i]=run<4?((run&2)?127:-128):(int)((seed>>1)%256)-128;}
    for(unsigned n=16;n<=256;n+=16){
      int32_t expected=0;for(unsigned i=0;i<n;i++)expected+=(int32_t)w[i]*x[i];
      if(dot(w,x,n)!=expected)return -EILSEQ;
    }
    const unsigned strides[]={16,32,48,64,240,256,272,512,528};
    for(unsigned test=0;test<sizeof(strides)/sizeof(strides[0]);test++){
    unsigned stride=strides[test],blocks=(stride+255)/256;
    batch(w,x,stride,2,3,results);
    for(unsigned r=0;r<2;r++)for(unsigned p=0;p<3;p++)for(unsigned b=0;b<blocks;b++){
      unsigned at=b*256,n=stride-at;if(n>256)n=256;
      int32_t reference=0;for(unsigned i=0;i<n;i++)reference+=(int32_t)w[r*stride+at+i]*x[p*stride+at+i];
      if(results[(r*3+p)*blocks+b]!=reference)return -EILSEQ;
    }
    }
  }
  memory->profile.tts_pie_selftests++;memory->profile.tts_int8_selftests++;return 0;
}
struct conv_stat {uint64_t pack_us,compute_us,output_us,dot_calls;unsigned batches,context_checks;};
struct conv_task {
  struct glv_memory *memory;
  const int8_t *input,*weights;const float *scales,*bias;float *out;
  int8_t *patch[2];int32_t *results[2];struct conv_stat stat[2];
  unsigned channels,time,outputs,length,kernel,stride,count,blocks,dilation,padding;
  float xs;bool add,probe,reduce;int zero;
};
#ifndef GLV_TTS_PARALLEL
struct glv_tts_lane {struct glv_memory *memory;};
#endif
static int conv_cancel(struct glv_tts_lane *lane)
{
#ifdef GLV_TTS_PARALLEL
  return glv_tts_parallel_cancel(lane);
#else
  return glv_cancel(lane->memory);
#endif
}
static int context_test(unsigned seed)
{
  _Alignas(16) int8_t w[16],x[16];int32_t expected=0;
  for(unsigned i=0;i<16;i++){w[i]=(int8_t)(i+seed);x[i]=(int8_t)(17-(int)i);expected+=(int32_t)w[i]*x[i];}
#ifdef __NuttX__
  extern int glv_tts_int8_context_probe(const int8_t *,const int8_t *,int32_t,unsigned);
  return glv_tts_int8_context_probe(w,x,2*expected,16)?-EILSEQ:0;
#else
  /* Host integer reference only; actual PIE context acceptance is native. */
  return dot(w,x,16)==expected?0:-EILSEQ;
#endif
}
static int conv_body(void *argument,unsigned first,unsigned last,struct glv_tts_lane *lane)
{
  struct conv_task *task=argument;struct glv_memory *memory=task->memory;
  unsigned index=first?1:0;struct conv_stat local={0},*stat=&local;
  const int8_t *input=task->input,*weights=task->weights;const float *scales=task->scales,*bias=task->bias;
  int8_t *patch=task->patch[index];int32_t *results=task->results[index];float *out=task->out;
  unsigned channels=task->channels,time=task->time,outputs=task->outputs,length=task->length;
  unsigned kernel=task->kernel,stride=task->stride,count=task->count,blocks=task->blocks,dilation=task->dilation,padding=task->padding;
  float xs=task->xs;bool add=task->add;int ret=0;uint64_t phase_began=0;
#ifdef __NuttX__
  unsigned saved;__asm__ volatile("csrrw %0,0x7f2,%1" : "=r"(saved) : "r"(1u) : "memory");
#endif
  if(task->probe){if((ret=conv_cancel(lane)))goto done;if((ret=context_test(index?29:11)))goto done;stat->context_checks++;}
  for(unsigned tile=first;tile<last;tile++){
    if((ret=conv_cancel(lane)))goto done;
    unsigned at=tile*PATCHES;
    unsigned n=length-at;if(n>PATCHES)n=PATCHES;
    if(!memory->tts_coarse_profile)phase_began=glv_now_us();
    for(unsigned c=0;c<channels;c++)for(unsigned k=0;k<kernel;k++){
      int pos=(int)at+(int)(k*dilation)-(int)padding;
      unsigned column=c*kernel+k;
      if(n==8&&time>=8&&pos>=0&&(unsigned)pos<=time-8){
        const int8_t *in=input+(size_t)c*time+(unsigned)pos;int8_t *dest=patch+column;
        dest[0]=in[0];dest[stride]=in[1];dest[2*stride]=in[2];dest[3*stride]=in[3];
        dest[4*stride]=in[4];dest[5*stride]=in[5];dest[6*stride]=in[6];dest[7*stride]=in[7];
      }else for(unsigned p=0;p<n;p++){
        int at_input=pos+(int)p;
        patch[(size_t)p*stride+column]=(unsigned)at_input<time?input[(size_t)c*time+(unsigned)at_input]:(int8_t)task->zero;
      }
    }
    for(unsigned p=0;p<n;p++)memset(patch+(size_t)p*stride+count,0,(stride-count));
    if(!memory->tts_coarse_profile){stat->pack_us+=glv_now_us()-phase_began;phase_began=glv_now_us();}
    if(!memory->tts_single_dot){
      if(task->reduce)batch_full(weights,patch,stride,outputs,n,results);else batch(weights,patch,stride,outputs,n,results);
      stat->batches++;
    }
    else for(unsigned o=0;o<outputs;o++)for(unsigned p=0;p<n;p++)for(unsigned b=0;b<blocks;b++){
      unsigned block=task->reduce?stride:DOT_BLOCK,k=b*block,ncol=stride-k;if(ncol>block)ncol=block;
      results[(o*n+p)*blocks+b]=dot(weights+(size_t)o*stride+k,patch+(size_t)p*stride+k,ncol);
    }
    stat->dot_calls+=(uint64_t)outputs*n*blocks;
    if(!memory->tts_coarse_profile){stat->compute_us+=glv_now_us()-phase_began;phase_began=glv_now_us();}
    for(unsigned o=0;o<outputs;o++){
    unsigned p=0;
    /* Independent positions hide FP conversion/multiply latency; every position
     * still accumulates the same 256-term blocks in the original order. */
    for(;p+4<=n;p+=4){
      float a=0,b=0,c=0,d=0,scale=scales[o];
      const int32_t *value=results+(o*n+p)*blocks;
      for(unsigned k=0;k<blocks;k++){
        a+=((float)value[k]*scale)*xs;b+=((float)value[blocks+k]*scale)*xs;
        c+=((float)value[2*blocks+k]*scale)*xs;d+=((float)value[3*blocks+k]*scale)*xs;
      }
      a+=bias[o];b+=bias[o];c+=bias[o];d+=bias[o];
      if(!finite_bits(a)||!finite_bits(b)||!finite_bits(c)||!finite_bits(d)){ret=-ERANGE;goto done;}
      float *dest=out+(size_t)o*length+at+p;
      if(add){dest[0]+=a;dest[1]+=b;dest[2]+=c;dest[3]+=d;}
      else{dest[0]=a;dest[1]=b;dest[2]=c;dest[3]=d;}
    }
    for(;p<n;p++){
      float sum=0;
      for(unsigned b=0;b<blocks;b++){
        int32_t value=results[(o*n+p)*blocks+b];
        sum+=((float)value*scales[o])*xs;
      }
      sum+=bias[o];if(!finite_bits(sum)){ret=-ERANGE;goto done;}
      if(add)out[(size_t)o*length+at+p]+=sum;else out[(size_t)o*length+at+p]=sum;
    }
    }
    if(!memory->tts_coarse_profile)stat->output_us+=glv_now_us()-phase_began;
  }
done:
#ifdef __NuttX__
  __asm__ volatile("csrw 0x7f2,%0" :: "r"(saved) : "memory");
#endif
  task->stat[index]=local;
  return ret;
}
int glv_tts_int8_conv(struct glv_model *model,const struct glv_tensor *w,const float *x,
                     unsigned channels,unsigned time,float *out,unsigned outputs,
                     unsigned length,unsigned dilation,unsigned padding,const float *bias,
                     bool leaky,bool add)
{
  if(!model||!model->memory||!w||!x||!out||!bias||!channels||!time||!length||
     !dilation||dilation>64||padding>640||!outputs||outputs>208||
     w->type!=2||w->ndim!=3||w->shape[0]!=outputs||w->shape[1]!=channels||
     !w->shape[2]||channels>208||w->shape[2]>11||time>2048u*300u)return -EINVAL;
  unsigned kernel=w->shape[2],count=channels*kernel,stride=(count+15u)&~15u,blocks=(stride+DOT_BLOCK-1)/DOT_BLOCK;
  if((size_t)time+2u*padding<dilation*(kernel-1)+1u||
     length!=(size_t)time+2u*padding-dilation*(kernel-1))return -EINVAL;
  struct glv_memory *memory=model->memory;int ret=0;uint64_t began=glv_now_us();
  bool reduce=memory->tts_int8_reduce;float input_factor=0;
  if(stride>FULL_ROW_MAX)return -E2BIG;
  if(reduce)blocks=1;
  void *input_raw=glv_alloc(memory,(size_t)channels*time+15);
  void *weight_raw=glv_alloc(memory,(size_t)outputs*stride+15);
  void *patch_raw=glv_alloc(memory,(size_t)PATCHES*stride+15);
  float *scales=glv_alloc(memory,outputs*4u),*row=glv_alloc(memory,count*4u);
  int32_t *results=glv_alloc(memory,(size_t)outputs*PATCHES*blocks*4u);
  int8_t *input=(int8_t *)(((uintptr_t)input_raw+15)&~(uintptr_t)15);
  int8_t *weights=(int8_t *)(((uintptr_t)weight_raw+15)&~(uintptr_t)15);
  int8_t *patch=(int8_t *)(((uintptr_t)patch_raw+15)&~(uintptr_t)15);
#ifdef __NuttX__
  unsigned saved=0;bool pie_active=false;
#endif
  if(!input_raw||!weight_raw||!patch_raw||!scales||!row||!results){ret=-ENOMEM;goto done;}
  uint64_t phase_began=glv_now_us();
  bool affine=glv_tts_int8_affine(memory);float lo=0,hi=0;int zero=0;
  float maximum=0;
#ifdef GLV_TTS_PARALLEL
  float channel_peaks[208];
  struct input_task input_task={.source=x,.dest=input,.peaks=channel_peaks,.time=time,.leaky=leaky,.affine=affine,.fast=memory->tts_int8_reduce};
  uint64_t input_work=(uint64_t)channels*time;
  bool parallel_input=memory->tts_parallel_input&&!memory->tts_serial_fp&&input_work>=65536u;
  if(parallel_input&&!input_rounding_supported()){parallel_input=false;memory->profile.tts_input_rounding_fallbacks++;}
  if(parallel_input){
    unsigned before=memory->profile.tts_parallel_jobs;
    ret=glv_tts_parallel_run(memory,input_peak_body,&input_task,channels,input_work*8u);
    memory->profile.tts_input_max_jobs+=memory->profile.tts_parallel_jobs-before;
    if(ret)goto done;
    if(affine){lo=fminf(input_task.lows[0],input_task.lows[1]);hi=fmaxf(input_task.highs[0],input_task.highs[1]);}
    else for(unsigned c=0;c<channels;c++)if(channel_peaks[c]>maximum)maximum=channel_peaks[c];
  }else
#endif
  {
    if(affine){
      for(unsigned c=0;c<channels;c++){
        if((ret=glv_cancel(memory)))goto done;
        if(memory->tts_int8_reduce){
          const float *in=x+(size_t)c*time;
          ret=leaky?range_row4(in,time,true,&lo,&hi):range_row4(in,time,false,&lo,&hi);
          if(ret)goto done;
        }else for(unsigned t=0;t<time;t++){
          float v=x[(size_t)c*time+t];if(!finite_bits(v)){ret=-EDOM;goto done;}
          if(leaky&&v<0)v*=.1f;if(v<lo)lo=v;if(v>hi)hi=v;
        }
      }
    }else {
    float m1=0,m2=0,m3=0;
  for(unsigned c=0;c<channels;c++){
    if((ret=glv_cancel(memory)))goto done;
    unsigned t=0;
    for(;t+4<=time;t+=4){
      const float *in=x+(size_t)c*time+t;float a=in[0],b=in[1],d=in[2],e=in[3];
      if(!finite_bits(a)||!finite_bits(b)||!finite_bits(d)||!finite_bits(e)){ret=-EDOM;goto done;}
      if(leaky){if(a<0)a*=.1f;if(b<0)b*=.1f;if(d<0)d*=.1f;if(e<0)e*=.1f;}
      a=fabsf(a);b=fabsf(b);d=fabsf(d);e=fabsf(e);
      if(a>maximum)maximum=a;if(b>m1)m1=b;if(d>m2)m2=d;if(e>m3)m3=e;
    }
    for(;t<time;t++){
      float v=x[(size_t)c*time+t];if(!finite_bits(v)){ret=-EDOM;goto done;}
      if(leaky&&v<0)v*=.1f;float a=fabsf(v);if(a>maximum)maximum=a;
    }
  }
  if(m1>maximum)maximum=m1;if(m2>maximum)maximum=m2;if(m3>maximum)maximum=m3;
  }
  }
  float xs=affine?(hi>lo?(hi-lo)/255.f:1.f):(maximum?maximum/127.f:1.f);
  if(!finite_bits(xs)||xs<=0){ret=-ERANGE;goto done;}
  if(affine&&hi>lo){float q=-lo/xs;zero=(int)(q+.5f)-128;if(zero< -128)zero=-128;if(zero>127)zero=127;}
  input_factor=affine?1.f/xs:(maximum?127.f/maximum:0);
  if(!finite_bits(input_factor)||input_factor<=0)input_factor=0;
#ifdef GLV_TTS_PARALLEL
  if(parallel_input){
    input_task.maximum=maximum;input_task.factor=input_factor;input_task.scale=xs;input_task.zero=zero;
    unsigned before=memory->profile.tts_parallel_jobs;
    ret=glv_tts_parallel_run(memory,input_quant_body,&input_task,channels,input_work*8u);
    memory->profile.tts_input_quant_jobs+=memory->profile.tts_parallel_jobs-before;
    if(ret)goto done;
  }else
#endif
  {
  for(unsigned c=0;c<channels;c++){
    if((ret=glv_cancel(memory)))goto done;
    if(memory->tts_int8_reduce&&affine){
      const float *in=x+(size_t)c*time;int8_t *out=input+(size_t)c*time;
      if(leaky)quant_input4_leaky(in,out,time,xs,input_factor,zero);else quant_input4_plain(in,out,time,xs,input_factor,zero);
    }else for(unsigned t=0;t<time;t++){
      float v=x[(size_t)c*time+t];if(leaky&&v<0)v*=.1f;
      input[(size_t)c*time+t]=affine?affine_quant(v,xs,input_factor,zero):quant_recip(v,maximum,input_factor,127);
    }
  }
  }
  memory->profile.tts_pie_input_us+=glv_now_us()-phase_began;phase_began=glv_now_us();
  struct weight_entry *weight_table=NULL;unsigned weight_hits=0,weight_misses=0;
  /* The integer output workspace is dead until convolution dispatch. Reuse
   * its first 1024 bytes without increasing the mandatory allocation floor. */
  if(memory->tts_weight_lut&&count>=256&&(size_t)outputs*PATCHES*blocks*4u>=256*sizeof(struct weight_entry)){
    if(weight_rounding_supported()){weight_table=(struct weight_entry *)(void *)results;memset(weight_table,0,256*sizeof(*weight_table));}
    else memory->profile.tts_weight_lut_rounding_fallbacks++;
  }
  for(unsigned o=0;o<outputs;o++){
    float peak=0,encoded_scale=0;unsigned char *packed=(unsigned char *)(void *)row;
    if(weight_table){
      if((ret=glv_tensor_q8_packed(model,w,o,packed,(size_t)count*4,count)))goto done;
      memcpy(&encoded_scale,packed,4);unsigned maximum_code=0;
      for(unsigned i=0;i<count;i++){
        int q=(int8_t)packed[4u+i];unsigned magnitude=q<0?-q:q;
        if(magnitude>maximum_code)maximum_code=magnitude;
      }
      /* Positive scale, integer codes and RNE make abs(scale*q) monotone in
       * abs(q). This is exactly the old decoded-row peak, including -128. */
      peak=encoded_scale*(float)maximum_code;
      if(!finite_bits(peak)){ret=-EBADMSG;goto done;}
    }else{
      if((ret=glv_tensor_row(model,w,o,row,count)))goto done;
      for(unsigned i=0;i<count;i++){float a=fabsf(row[i]);if(a>peak)peak=a;}
    }
    scales[o]=peak?peak/127.f:1.f;
    if(!finite_bits(scales[o])||scales[o]<=0){ret=-ERANGE;goto done;}
    float factor=peak?127.f/peak:0;if(!finite_bits(factor)||factor<=0)factor=0;
    if(weight_table&&peak){
      memory->profile.tts_weight_lut_rows++;
      for(unsigned i=0;i<count;i++)weights[(size_t)o*stride+i]=weight_quant(weight_table,o+1,packed[4u+i],encoded_scale,peak,factor,&weight_hits,&weight_misses);
    }else if(weight_table)memset(weights+(size_t)o*stride,0,count);
    else for(unsigned i=0;i<count;i++)weights[(size_t)o*stride+i]=quant_recip(row[i],peak,factor,127);
    memset(weights+(size_t)o*stride+count,0,(stride-count));
  }
  memory->profile.tts_weight_lut_hits+=weight_hits;memory->profile.tts_weight_lut_misses+=weight_misses;
  memory->profile.tts_pie_weights_us+=glv_now_us()-phase_began;
#ifdef __NuttX__
  __asm__ volatile("csrrw %0,0x7f2,%1" : "=r"(saved) : "r"(1u) : "memory");pie_active=true;
#endif
  if(!memory->profile.tts_int8_selftests&&(ret=selftest(memory)))goto done;
  if(reduce&&!memory->profile.tts_int8_reduce_selftests&&(ret=selftest_full(memory)))goto done;
  memory->profile.tts_pie_calls++;
#ifdef __NuttX__
  __asm__ volatile("csrw 0x7f2,%0" :: "r"(saved) : "memory");pie_active=false;
#endif
  float adjusted_bias[208];
  if(affine)for(unsigned o=0;o<outputs;o++){
    int32_t sum=0;for(unsigned i=0;i<count;i++)sum+=weights[(size_t)o*stride+i];
    adjusted_bias[o]=bias[o]-((float)(sum*zero)*scales[o])*xs;
  }
  struct conv_task task={.zero=zero,.memory=memory,.input=input,.weights=weights,.scales=scales,.bias=affine?adjusted_bias:bias,.out=out,
    .patch={patch,NULL},.results={results,NULL},.channels=channels,.time=time,.outputs=outputs,.length=length,
    .reduce=reduce,.kernel=kernel,.stride=stride,.count=count,.blocks=blocks,.dilation=dilation,.padding=padding,.xs=xs,.add=add};
  unsigned tiles=(length+PATCHES-1)/PATCHES;
#ifdef GLV_TTS_PARALLEL
  void *second_patch=NULL;int32_t *second_results=NULL;
  uint64_t work=(uint64_t)outputs*length*stride;
  if(memory->tts_parallel_pie&&!memory->tts_serial_fp&&!memory->tts_single_dot&&tiles>=2&&work>=262144){
    memory->profile.tts_pie_parallel_eligible++;
    if(!input_rounding_supported())memory->profile.tts_pie_parallel_rounding_fallbacks++;
    else {
      second_patch=glv_alloc(memory,(size_t)PATCHES*stride+15);
      if(second_patch)second_results=glv_alloc(memory,(size_t)outputs*PATCHES*blocks*4);
      if(second_patch&&second_results){
        task.patch[1]=(int8_t *)(((uintptr_t)second_patch+15)&~(uintptr_t)15);task.results[1]=second_results;task.probe=true;
      }else memory->profile.tts_pie_parallel_scratch_fallbacks++;
    }
  }
  if(task.probe){
    unsigned before=memory->profile.tts_parallel_jobs;
    ret=glv_tts_parallel_run(memory,conv_body,&task,tiles,work);
    memory->profile.tts_pie_parallel_jobs+=memory->profile.tts_parallel_jobs-before;
  }else
#endif
  {struct glv_tts_lane serial={.memory=memory};ret=conv_body(&task,0,tiles,&serial);}
#ifdef GLV_TTS_PARALLEL
  glv_free(second_results);glv_free(second_patch);
#endif
  /* Each lane owns its counters. Merge only after both ranges have retired. */
  for(unsigned i=0;i<2;i++){
    struct conv_stat *v=&task.stat[i];memory->profile.tts_pie_pack_us+=v->pack_us;
    memory->profile.tts_pie_compute_us+=v->compute_us;memory->profile.tts_pie_output_us+=v->output_us;
    memory->profile.tts_pie_dot_calls+=v->dot_calls;memory->profile.tts_pie_batches+=v->batches;
    memory->profile.tts_pie_context_checks+=v->context_checks;
  }

done:
#ifdef __NuttX__
  if(pie_active)__asm__ volatile("csrw 0x7f2,%0" :: "r"(saved) : "memory");
#endif
  glv_free(results);glv_free(row);glv_free(scales);glv_free(patch_raw);glv_free(weight_raw);glv_free(input_raw);
  if(!ret){
    memory->profile.tts_int8_calls++;
    if(reduce){memory->profile.tts_int8_reduce_calls++;
      if(affine)memory->profile.tts_int8_fast_input_calls++;
      if(affine&&input_factor>0)memory->profile.tts_int8_input_conversions_saved+=2u*(uint64_t)channels*time;
      memory->profile.tts_int8_conversions_saved+=(uint64_t)outputs*length*((stride+DOT_BLOCK-1)/DOT_BLOCK-1);
    }
  }
  memory->profile.tts_pie_us+=glv_now_us()-began;
  return ret;
}
