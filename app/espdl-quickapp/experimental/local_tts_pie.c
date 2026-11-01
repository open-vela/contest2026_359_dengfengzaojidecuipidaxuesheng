/* SPDX-License-Identifier: Apache-2.0 */
#include "local_tts_pie.h"
#ifdef GLV_TTS_INT8
#include "local_tts_int8.h"
#include "local_tts_int8_policy.h"
#endif
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
/* RV32F GCC lowers builtin isfinite to a comparison surrounded by two FFLAGS
 * CSR accesses. Inspect the IEEE exponent instead; preserve all FP flags. */
static inline bool finite_bits(float value)
{uint32_t bits;memcpy(&bits,&value,sizeof(bits));return (bits&0x7f800000u)!=0x7f800000u;}
#ifdef __NuttX__
/* Reuse the already deployed signed-16 PIE dot kernel, not a float C fallback. */
extern int32_t glv_tts_dot_pie(const int16_t *,const int16_t *,unsigned);
extern void glv_tts_dot_batch_pie(const int16_t *,const int16_t *,unsigned,unsigned,unsigned,int32_t *);
#endif
static int16_t quant(float v,float maximum,int limit)
{
  if(!maximum)return 0;
  float scaled=(v/maximum)*(float)limit;
  int q=(int)(scaled>=0?scaled+.5f:scaled-.5f);
  if(q>limit)q=limit;if(q< -limit)q= -limit;return (int16_t)q;
}
/* A reciprocal replaces per-coefficient division away from rounding borders.
 * For |v|<=maximum and limit<=32767, the two rounded paths differ by <1/64
 * even with directed IEEE rounding. Keep a 1/32-wide guard at each half integer
 * and execute the original division there. Nonfinite reciprocals also fall back. */
static inline int16_t quant_recip(float v,float maximum,float factor,int limit)
{
  if(!factor)return quant(v,maximum,limit);
  float scaled=v*factor;int q=(int)(scaled>=0?scaled+.5f:scaled-.5f);
  if(fabsf(scaled-(float)q)>.46875f)return quant(v,maximum,limit);
  if(q>limit)q=limit;if(q< -limit)q= -limit;return (int16_t)q;
}
/* Exact reciprocal-guard quantization for the stable signed16 input path.
 * Keep the old division formula at half-integer boundaries. Valid inputs have
 * |v|<=maximum; the broad65536 guard keeps unexpected inputs on the old path. */
static __attribute__((noinline)) int16_t quant_recip_slow16(float v,float maximum,float factor)
{return quant_recip(v,maximum,factor,32767);}
static __attribute__((always_inline)) inline int16_t quant_bits16(float v,float maximum,float factor)
{
  if(!factor)return quant(v,maximum,32767);
  float value=v*factor;uint32_t bits;memcpy(&bits,&value,4);
  uint32_t mag=bits&0x7fffffffu,q=0;
  if(mag>=0x47800000u)return quant_recip_slow16(v,maximum,factor);
  if(mag<0x3f000000u){
    if(mag>0x3ef00000u)return quant(v,maximum,32767);
  }else{
    unsigned shift=150u-(mag>>23);
    uint32_t mant=(mag&0x7fffffu)|0x800000u,fraction=mant&((1u<<shift)-1u);
    if(fraction>(15u<<(shift-5))&&fraction<(17u<<(shift-5)))return quant(v,maximum,32767);
    q=(mant+(1u<<(shift-1)))>>shift;
  }
  int result=(bits>>31)?-(int)q:(int)q;
  if(result< -32767)result=-32767;if(result>32767)result=32767;return (int16_t)result;
}
static __attribute__((always_inline)) inline void quant16_row(const float *in,int16_t *out,unsigned n,float maximum,float factor,bool leaky)
{
  for(unsigned t=0;t<n;t++){float v=in[t];if(leaky&&v<0)v*=.1f;out[t]=quant_bits16(v,maximum,factor);}
}
static __attribute__((noinline)) void quant_input16_plain(const float *in,int16_t *out,unsigned n,float maximum,float factor)
{quant16_row(in,out,n,maximum,factor,false);}
static __attribute__((noinline)) void quant_input16_leaky(const float *in,int16_t *out,unsigned n,float maximum,float factor)
{quant16_row(in,out,n,maximum,factor,true);}
static __attribute__((noinline)) int quant_fast_selftest(struct glv_memory *m)
{
  static const int values[]={-32767,-32766,-127,-2,-1,0,1,2,127,32766,32767};
  for(unsigned k=0;k<sizeof(values)/sizeof(values[0]);k++)for(unsigned j=0;j<3;j++){
    float v=((float)values[k]+.5f)/32767.f;
    uint32_t bits;memcpy(&bits,&v,4);
    if(j==0)bits+=(bits>>31)?1u:(uint32_t)-1;
    if(j==2)bits+=(bits>>31)?(uint32_t)-1:1u;
    memcpy(&v,&bits,4);
    if(quant_bits16(v,1.f,32767.f)!=quant_recip(v,1.f,32767.f,32767))return -EILSEQ;
  }
  m->profile.tts_quant_fast_selftests++;return glv_cancel(m);
}
struct weight_entry {int16_t value;uint16_t generation;};
_Static_assert(sizeof(struct weight_entry)==4,"weight memo entry layout");
static bool weight_rounding_supported(void)
{
#ifdef __NuttX__
  unsigned mode;__asm__ volatile("csrr %0,frm" : "=r"(mode));return mode==0;
#else
  return fegetround()==FE_TONEAREST;
#endif
}
static inline int16_t weight_quant(struct weight_entry *table,unsigned generation,
                                   unsigned char code,float scale,float peak,float factor,unsigned *hits,unsigned *misses)
{
  struct weight_entry *e=&table[code];
  if(e->generation==generation){(*hits)++;return e->value;}
  float value=scale*(int8_t)code;int16_t converted=quant_recip(value,peak,factor,127);
  e->value=converted;e->generation=(uint16_t)generation;(*misses)++;return converted;
}
#ifdef GLV_TTS_PARALLEL
struct input_task {
  const float *source;int16_t *dest;float *peaks;
  unsigned time;bool leaky;float maximum,factor;bool fast;
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
    if(task->fast){
      const float *row=task->source+(size_t)c*task->time;int16_t *out=task->dest+(size_t)c*task->time;
      if(task->leaky)quant_input16_leaky(row,out,task->time,task->maximum,task->factor);
      else quant_input16_plain(row,out,task->time,task->maximum,task->factor);
    }else for(unsigned t=0;t<task->time;t++){
      float v=task->source[(size_t)c*task->time+t];if(task->leaky&&v<0)v*=.1f;
      task->dest[(size_t)c*task->time+t]=quant_recip(v,task->maximum,task->factor,32767);
    }
  }
  return 0;
}
#endif
static int32_t dot(const int16_t *w,const int16_t *x,unsigned n)
{
  /* |w| <= 127, |x| <= 32767, n <= 256: |sum| <= 1065320704.
   * Both pointers are 16-byte aligned and n is a multiple of eight. */
#ifdef __NuttX__
  return glv_tts_dot_pie(w,x,n);
#else
  int32_t sum=0;for(unsigned i=0;i<n;i++)sum+=(int32_t)w[i]*x[i];return sum;
#endif
}
static void batch(const int16_t *w,const int16_t *x,unsigned stride,unsigned rows,unsigned patches,int32_t *out)
{
#ifdef __NuttX__
  glv_tts_dot_batch_pie(w,x,stride,rows,patches,out);
#else
  unsigned blocks=(stride+DOT_BLOCK-1)/DOT_BLOCK;
  for(unsigned r=0;r<rows;r++)for(unsigned p=0;p<patches;p++)for(unsigned b=0;b<blocks;b++){
    unsigned at=b*DOT_BLOCK,n=stride-at;if(n>DOT_BLOCK)n=DOT_BLOCK;
    *out++=dot(w+(size_t)r*stride+at,x+(size_t)p*stride+at,n);
  }
#endif
}
static __attribute__((noinline)) int selftest(struct glv_memory *memory)
{
  _Alignas(16) int16_t w[2*520],x[3*520];int32_t results[18];
  unsigned seed=7;
  for(unsigned run=0;run<8;run++){
    for(unsigned i=0;i<2*520;i++){seed=seed*1664525u+1013904223u;w[i]=run<4?((run&1)?127:-127):(int)((seed>>16)%255)-127;}
    for(unsigned i=0;i<3*520;i++){seed=seed*1664525u+1013904223u;x[i]=run<4?((run&2)?32767:-32767):(int)((seed>>1)%65535)-32767;}
    for(unsigned n=8;n<=256;n+=8){
      int32_t expected=0;for(unsigned i=0;i<n;i++)expected+=(int32_t)w[i]*x[i];
      if(dot(w,x,n)!=expected)return -EILSEQ;
    }
    const unsigned strides[]={8,24,32,40,248,256,264,512,520};
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
  memory->profile.tts_pie_selftests++;memory->profile.tts_int16_selftests++;return 0;
}
struct conv_stat {uint64_t pack_us,compute_us,output_us,dot_calls;unsigned batches,context_checks;};
struct conv_task {
  struct glv_memory *memory;
  const int16_t *input,*weights;const float *scales,*bias;float *out;
  int16_t *patch[2];int32_t *results[2];struct conv_stat stat[2];
  unsigned channels,time,outputs,length,kernel,stride,count,blocks,dilation,padding;
  float xs;bool add,probe;
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
  _Alignas(16) int16_t w[8],x[8];int32_t expected=0;
  for(unsigned i=0;i<8;i++){w[i]=(int16_t)(i+seed);x[i]=(int16_t)(17-(int)i);expected+=(int32_t)w[i]*x[i];}
#ifdef __NuttX__
  extern int glv_tts_pie_context_probe(const int16_t *,const int16_t *,int32_t,unsigned);
  return glv_tts_pie_context_probe(w,x,2*expected,16)?-EILSEQ:0;
#else
  /* Host integer reference only; actual PIE context acceptance is native. */
  return dot(w,x,8)==expected?0:-EILSEQ;
#endif
}
static int conv_body(void *argument,unsigned first,unsigned last,struct glv_tts_lane *lane)
{
  struct conv_task *task=argument;struct glv_memory *memory=task->memory;
  unsigned index=first?1:0;struct conv_stat local={0},*stat=&local;
  const int16_t *input=task->input,*weights=task->weights;const float *scales=task->scales,*bias=task->bias;
  int16_t *patch=task->patch[index];int32_t *results=task->results[index];float *out=task->out;
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
        const int16_t *in=input+(size_t)c*time+(unsigned)pos;int16_t *dest=patch+column;
        dest[0]=in[0];dest[stride]=in[1];dest[2*stride]=in[2];dest[3*stride]=in[3];
        dest[4*stride]=in[4];dest[5*stride]=in[5];dest[6*stride]=in[6];dest[7*stride]=in[7];
      }else for(unsigned p=0;p<n;p++){
        int at_input=pos+(int)p;
        patch[(size_t)p*stride+column]=(unsigned)at_input<time?input[(size_t)c*time+(unsigned)at_input]:0;
      }
    }
    for(unsigned p=0;p<n;p++)memset(patch+(size_t)p*stride+count,0,(stride-count)*2u);
    if(!memory->tts_coarse_profile){stat->pack_us+=glv_now_us()-phase_began;phase_began=glv_now_us();}
    if(!memory->tts_single_dot){batch(weights,patch,stride,outputs,n,results);stat->batches++;}
    else for(unsigned o=0;o<outputs;o++)for(unsigned p=0;p<n;p++)for(unsigned b=0;b<blocks;b++){
      unsigned k=b*DOT_BLOCK,ncol=stride-k;if(ncol>DOT_BLOCK)ncol=DOT_BLOCK;
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
static __attribute__((noinline)) int glv_tts_pie_conv_w16(struct glv_model *model,const struct glv_tensor *w,const float *x,
                     unsigned channels,unsigned time,float *out,unsigned outputs,
                     unsigned length,unsigned dilation,unsigned padding,const float *bias,
                     bool leaky,bool add)
{

  if(!model||!model->memory||!w||!x||!out||!bias||!channels||!time||!length||
     !dilation||dilation>64||padding>640||!outputs||outputs>208||
     w->type!=2||w->ndim!=3||w->shape[0]!=outputs||w->shape[1]!=channels||
     !w->shape[2]||channels>208||w->shape[2]>11||time>2048u*300u)return -EINVAL;
  unsigned kernel=w->shape[2],count=channels*kernel,stride=(count+7u)&~7u,blocks=(stride+DOT_BLOCK-1)/DOT_BLOCK;
  if((size_t)time+2u*padding<dilation*(kernel-1)+1u||
     length!=(size_t)time+2u*padding-dilation*(kernel-1))return -EINVAL;
  struct glv_memory *memory=model->memory;int ret=0;uint64_t began=glv_now_us();
  if(memory->tts_quant_fast&&!memory->profile.tts_quant_fast_selftests){ret=quant_fast_selftest(memory);if(ret)return ret;}
  void *input_raw=glv_alloc(memory,(size_t)channels*time*2+15);
  void *weight_raw=glv_alloc(memory,(size_t)outputs*stride*2+15);
  void *patch_raw=glv_alloc(memory,(size_t)PATCHES*stride*2+15);
  float *scales=glv_alloc(memory,outputs*4u),*row=glv_alloc(memory,count*4u);
  int32_t *results=glv_alloc(memory,(size_t)outputs*PATCHES*blocks*4u);
  int16_t *input=(int16_t *)(((uintptr_t)input_raw+15)&~(uintptr_t)15);
  int16_t *weights=(int16_t *)(((uintptr_t)weight_raw+15)&~(uintptr_t)15);
  int16_t *patch=(int16_t *)(((uintptr_t)patch_raw+15)&~(uintptr_t)15);
#ifdef __NuttX__
  unsigned saved=0;bool pie_active=false;
#endif
  if(!input_raw||!weight_raw||!patch_raw||!scales||!row||!results){ret=-ENOMEM;goto done;}
  uint64_t phase_began=glv_now_us();
  float maximum=0;
#ifdef GLV_TTS_PARALLEL
  float channel_peaks[208];
  struct input_task input_task={x,input,channel_peaks,time,leaky,0,0,memory->tts_quant_fast};
  uint64_t input_work=(uint64_t)channels*time;
  bool parallel_input=memory->tts_parallel_input&&!memory->tts_serial_fp&&input_work>=65536u;
  if(parallel_input&&!input_rounding_supported()){parallel_input=false;memory->profile.tts_input_rounding_fallbacks++;}
  if(parallel_input){
    unsigned before=memory->profile.tts_parallel_jobs;
    ret=glv_tts_parallel_run(memory,input_peak_body,&input_task,channels,input_work*8u);
    memory->profile.tts_input_max_jobs+=memory->profile.tts_parallel_jobs-before;
    if(ret)goto done;
    for(unsigned c=0;c<channels;c++)if(channel_peaks[c]>maximum)maximum=channel_peaks[c];
  }else
#endif
  {
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
  float xs=maximum?maximum/32767.f:1.f;
  if(!finite_bits(xs)||xs<=0){ret=-ERANGE;goto done;}
  float input_factor=maximum?32767.f/maximum:0;
  if(!finite_bits(input_factor)||input_factor<=0)input_factor=0;
#ifdef GLV_TTS_PARALLEL
  if(parallel_input){
    input_task.maximum=maximum;input_task.factor=input_factor;
    unsigned before=memory->profile.tts_parallel_jobs;
    ret=glv_tts_parallel_run(memory,input_quant_body,&input_task,channels,input_work*8u);
    memory->profile.tts_input_quant_jobs+=memory->profile.tts_parallel_jobs-before;
    if(ret)goto done;
  }else
#endif
  {
  for(unsigned c=0;c<channels;c++){
    if((ret=glv_cancel(memory)))goto done;
    if(memory->tts_quant_fast){
      const float *row=x+(size_t)c*time;int16_t *out=input+(size_t)c*time;
      if(leaky)quant_input16_leaky(row,out,time,maximum,input_factor);else quant_input16_plain(row,out,time,maximum,input_factor);
    }else for(unsigned t=0;t<time;t++){
      float v=x[(size_t)c*time+t];if(leaky&&v<0)v*=.1f;
      input[(size_t)c*time+t]=quant_recip(v,maximum,input_factor,32767);
    }
  }
  }
  if(memory->tts_quant_fast){memory->profile.tts_quant_fast_calls++;if(input_factor>0)memory->profile.tts_quant_fast_input_saved+=2u*(uint64_t)channels*time;}
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
    }else if(weight_table)memset(weights+(size_t)o*stride,0,count*2u);
    else for(unsigned i=0;i<count;i++)weights[(size_t)o*stride+i]=quant_recip(row[i],peak,factor,127);
    memset(weights+(size_t)o*stride+count,0,(stride-count)*2u);
  }
  memory->profile.tts_weight_lut_hits+=weight_hits;memory->profile.tts_weight_lut_misses+=weight_misses;
  memory->profile.tts_pie_weights_us+=glv_now_us()-phase_began;
#ifdef __NuttX__
  __asm__ volatile("csrrw %0,0x7f2,%1" : "=r"(saved) : "r"(1u) : "memory");pie_active=true;
#endif
  if(!memory->profile.tts_int16_selftests&&(ret=selftest(memory)))goto done;
  memory->profile.tts_pie_calls++;
#ifdef __NuttX__
  __asm__ volatile("csrw 0x7f2,%0" :: "r"(saved) : "memory");pie_active=false;
#endif
  struct conv_task task={.memory=memory,.input=input,.weights=weights,.scales=scales,.bias=bias,.out=out,
    .patch={patch,NULL},.results={results,NULL},.channels=channels,.time=time,.outputs=outputs,.length=length,
    .kernel=kernel,.stride=stride,.count=count,.blocks=blocks,.dilation=dilation,.padding=padding,.xs=xs,.add=add};
  unsigned tiles=(length+PATCHES-1)/PATCHES;
#ifdef GLV_TTS_PARALLEL
  void *second_patch=NULL;int32_t *second_results=NULL;
  uint64_t work=(uint64_t)outputs*length*stride;
  if(memory->tts_parallel_pie&&!memory->tts_serial_fp&&!memory->tts_single_dot&&tiles>=2&&work>=262144){
    memory->profile.tts_pie_parallel_eligible++;
    if(!input_rounding_supported())memory->profile.tts_pie_parallel_rounding_fallbacks++;
    else {
      second_patch=glv_alloc(memory,(size_t)PATCHES*stride*2+15);
      if(second_patch)second_results=glv_alloc(memory,(size_t)outputs*PATCHES*blocks*4);
      if(second_patch&&second_results){
        task.patch[1]=(int16_t *)(((uintptr_t)second_patch+15)&~(uintptr_t)15);task.results[1]=second_results;task.probe=true;
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
  memory->profile.tts_pie_us+=glv_now_us()-began;
  return ret;
}

int glv_tts_pie_conv(struct glv_model *model,const struct glv_tensor *w,const float *x,
                     unsigned channels,unsigned time,float *out,unsigned outputs,
                     unsigned length,unsigned dilation,unsigned padding,const float *bias,
                     bool leaky,bool add)
{
#ifdef GLV_TTS_INT8
  if(model&&model->memory&&model->memory->tts_int8){
    int precision=glv_tts_int8_precision(model,w);if(precision<0)return precision;
    if(precision==8)return glv_tts_int8_conv(model,w,x,channels,time,out,outputs,length,dilation,padding,bias,leaky,add);
    int ret=glv_tts_pie_conv_w16(model,w,x,channels,time,out,outputs,length,dilation,padding,bias,leaky,add);
    if(!ret)model->memory->profile.tts_int8_kept_calls++;return ret;
  }
#endif
  return glv_tts_pie_conv_w16(model,w,x,channels,time,out,outputs,length,dilation,padding,bias,leaky,add);
}
