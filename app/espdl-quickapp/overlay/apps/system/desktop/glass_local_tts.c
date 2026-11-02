/* SPDX-License-Identifier: Apache-2.0 */
/* Baker V2: native 24kHz acoustic network + 208-channel HiFi-GAN.
 * Time-major PyTorch reference tensors are represented as [channel,time].
 * No retraining, normalization, denoising or post-hoc waveform stretching. */
#include "glass_local_tts.h"
#if defined(GLV_TTS_FMA) && !defined(GLV_TTS_PARALLEL)
#error FMA diagnostic requires the bounded parallel operator dispatcher
#endif
#ifdef GLV_TTS_PIE
#include "local_tts_pie.h"
#ifdef GLV_TTS_INT8
#include "local_tts_int8_policy.h"
#endif
#endif
#ifdef GLV_TTS_PARALLEL
#include "local_tts_parallel.h"
#endif
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define CHANNELS 144u
#define MAX_PHONES 256u
#define MAX_FRAMES 2048u
#define CONV_TILE 256u
#define VOCODER_HALO 20u
struct matrix {float *v;unsigned c,t;};
struct glv_tts {struct glv_memory *memory;struct glv_model model;int error;};
static void fail(struct glv_tts *s,int error){if(!s->error)s->error=error;}
static struct matrix make(struct glv_tts *s,unsigned c,unsigned t)
{
  struct matrix x={0};if(s->error)return x;
  if(!c||!t||c>1024||t>MAX_FRAMES*300u||(size_t)c*t>SIZE_MAX/4){fail(s,-E2BIG);return x;}
  x.v=glv_alloc(s->memory,(size_t)c*t*4);if(!x.v){fail(s,-ENOMEM);return x;}x.c=c;x.t=t;return x;
}
static void drop(struct matrix *x){glv_free(x->v);memset(x,0,sizeof(*x));}
static struct matrix clone(struct glv_tts *s,struct matrix x)
{struct matrix y=make(s,x.c,x.t);if(y.v)memcpy(y.v,x.v,(size_t)x.c*x.t*4);return y;}
static float *vector(struct glv_tts *s,const char *name,unsigned n)
{
  if(s->error)return NULL;float *v=glv_alloc(s->memory,n*4u);
  if(!v){fail(s,-ENOMEM);return NULL;}int ret=glv_vector(&s->model,name,v,n);
  if(ret){glv_free(v);fail(s,ret);return NULL;}return v;
}
static float *weights(struct glv_tts *s,const char *name,unsigned rows,unsigned cols)
{
  if(s->error)return NULL;const struct glv_tensor *w=glv_tensor(&s->model,name);
  if(!w||w->ndim!=2||w->shape[0]!=rows||w->shape[1]!=cols){fail(s,-EBADMSG);return NULL;}
  float *data=glv_alloc(s->memory,(size_t)rows*cols*4);if(!data){fail(s,-ENOMEM);return NULL;}
  for(unsigned r=0;r<rows;r++){int ret=glv_tensor_row(&s->model,w,r,data+(size_t)r*cols,cols);
    if(ret){glv_free(data);fail(s,ret);return NULL;}}
  return data;
}
static void name_of(char *out,size_t cap,const char *base,const char *suffix)
{snprintf(out,cap,"%s.%s",base,suffix);}
static float gelu(float x){return x*.5f*(1+erff(x*.7071067811865475f));}
static void activation(struct matrix x,float negative)
{for(size_t i=0;i<(size_t)x.c*x.t;i++)if(x.v[i]<0)x.v[i]*=negative;}
/* Independent time positions can overlap the RV32F pipeline. Each position
 * still receives exactly one multiply then add for each original i/k pair. */
static inline void accumulate(float *restrict dest,const float *restrict src,float weight,unsigned n)
{
  for(;n>=4;n-=4,dest+=4,src+=4){
    float a=src[0]*weight,b=src[1]*weight,c=src[2]*weight,d=src[3]*weight;
    float x=dest[0]+a,y=dest[1]+b,z=dest[2]+c,w=dest[3]+d;
    dest[0]=x;dest[1]=y;dest[2]=z;dest[3]=w;
  }
  for(unsigned i=0;i<n;i++)dest[i]+=src[i]*weight;
}
#ifdef GLV_TTS_PARALLEL
struct conv_task {
  struct matrix x,y;const float *weights,*bias,*activated;
  unsigned ic,kernel,dilation,padding;bool depth;
};
static int conv_task_body(void *argument,unsigned first,unsigned last,struct glv_tts_lane *lane)
{
  const struct conv_task *task=argument;
  for(unsigned o=first;o<last;o++){
    const float *row=task->weights+(size_t)o*task->ic*task->kernel;
    float *dest=task->y.v+(size_t)o*task->y.t;
    for(unsigned at=0;at<task->y.t;at+=CONV_TILE){
      int ret=glv_tts_parallel_cancel(lane);if(ret)return ret;
      unsigned n=task->y.t-at;if(n>CONV_TILE)n=CONV_TILE;float sums[CONV_TILE];
      for(unsigned t=0;t<n;t++)sums[t]=task->bias?task->bias[o]:0;
      for(unsigned i=0;i<task->ic;i++){
        const float *src=(task->activated?task->activated:task->x.v)+(size_t)(task->depth?o:i)*task->x.t;
        for(unsigned k=0;k<task->kernel;k++){
          int shift=(int)(k*task->dilation)-(int)task->padding;
          int begin=shift<0?-shift:0,end=(int)task->x.t-shift;
          if(begin<(int)at)begin=at;if(end>(int)(at+n))end=at+n;
          if(begin<end)accumulate(sums+begin-at,src+begin+shift,row[i*task->kernel+k],(unsigned)(end-begin));
        }
      }
      memcpy(dest+at,sums,n*sizeof(float));
    }
  }
  return 0;
}
/* Return 1 only after the cached operation completed. Optional allocation
 * failure leaves the original row-streaming path available; I/O is not replayed. */
static int conv_try_parallel(struct glv_tts *s,struct matrix x,struct matrix y,const struct glv_tensor *w,
                             const float *bias,const float *activated,unsigned ic,unsigned kernel,unsigned dilation,unsigned padding,bool depth)
{
  uint64_t work=(uint64_t)y.c*ic*kernel*y.t;
  if(s->memory->tts_serial_fp||y.c<2||work<262144)return 0;
  float *plane=glv_alloc(s->memory,(size_t)y.c*ic*kernel*4u);
  if(!plane){glv_tts_parallel_fallback(s->memory,GLV_PAR_CACHE,ENOMEM);return 0;}
  int ret=0;
  for(unsigned o=0;o<y.c&&!ret;o++)ret=glv_tensor_row(&s->model,w,o,plane+(size_t)o*ic*kernel,ic*kernel);
  if(!ret){
    struct conv_task task={x,y,plane,bias,activated,ic,kernel,dilation,padding,depth};
    ret=glv_tts_parallel_run(s->memory,conv_task_body,&task,y.c,work);
  }
  glv_free(plane);if(ret){fail(s,ret);return ret;}return 1;
}
#endif
static struct matrix conv_impl(struct glv_tts *s,struct matrix x,const char *base,unsigned dilation,unsigned padding,bool leaky)
{
  struct matrix y={0};char name[128];float *row=NULL,*bias=NULL,*activated=NULL;bool full_activation=false;
  if(s->error||!x.v)return y;name_of(name,sizeof(name),base,"weight");const struct glv_tensor *w=glv_tensor(&s->model,name);
  if(!w||w->ndim!=3){fail(s,-EBADMSG);return y;}
  unsigned oc=w->shape[0],ic=w->shape[1],kernel=w->shape[2];bool depth=ic==1&&oc==x.c;
  if((!depth&&ic!=x.c)||!kernel||x.t+2*padding<dilation*(kernel-1)+1){fail(s,-EBADMSG);return y;}
  unsigned length=x.t+2*padding-dilation*(kernel-1);y=make(s,oc,length);if(!y.v)return y;
  row=glv_alloc(s->memory,(size_t)ic*kernel*4);if(!row){fail(s,-ENOMEM);goto done;}
  name_of(name,sizeof(name),base,"bias");bias=vector(s,name,oc);if(!bias)goto done;
#ifdef GLV_TTS_PIE
  if(!strncmp(base,"vocoder.",8)&&!depth){
    int ret=glv_tts_pie_conv(&s->model,w,x.v,x.c,x.t,y.v,oc,length,dilation,padding,bias,leaky,false);
    if(ret)fail(s,ret);goto done;
  }
#endif
  if(leaky){
    /* Optional reuse only: all mandatory allocations are already owned.
     * If the extra plane cannot fit, keep the bounded one-row reference path. */
    activated=glv_alloc(s->memory,(size_t)x.c*x.t*4u);full_activation=activated!=NULL;
    if(full_activation){
      for(unsigned i=0;i<x.c&&!s->error;i++){
        for(unsigned t=0;t<x.t;t++){float v=x.v[(size_t)i*x.t+t];activated[(size_t)i*x.t+t]=v<0?v*.1f:v;}
        int ret=glv_cancel(s->memory);if(ret)fail(s,ret);
      }
    }else {activated=glv_alloc(s->memory,x.t*4u);if(!activated){fail(s,-ENOMEM);goto done;}}
  }
#ifdef GLV_TTS_PARALLEL
  if(!s->error&&(!leaky||full_activation)){
    int handled=conv_try_parallel(s,x,y,w,bias,full_activation?activated:NULL,ic,kernel,dilation,padding,depth);
    if(handled)goto done;
  }
#endif
  for(unsigned o=0;o<oc&&!s->error;o++){
    int ret=glv_tensor_row(&s->model,w,o,row,ic*kernel);if(ret){fail(s,ret);break;}
    float *dest=y.v+(size_t)o*length;
    /* Keep each output's original i/k summation order while retaining a small
     * output window in cache instead of repeatedly writing a full PSRAM row. */
    for(unsigned at=0;at<length&&!s->error;at+=CONV_TILE){
      unsigned n=length-at;if(n>CONV_TILE)n=CONV_TILE;float sums[CONV_TILE];
      for(unsigned t=0;t<n;t++)sums[t]=bias[o];
      for(unsigned i=0;i<ic;i++){
        const float *src=(full_activation?activated:x.v)+(size_t)(depth?o:i)*x.t;
        if(leaky&&!full_activation){
          int first=(int)at-(int)padding,last=(int)(at+n)+(int)((kernel-1)*dilation)-(int)padding;
          if(first<0)first=0;if(last>(int)x.t)last=x.t;
          for(int t=first;t<last;t++)activated[t]=src[t]<0?src[t]*.1f:src[t];src=activated;
        }
        for(unsigned k=0;k<kernel;k++){
          int shift=(int)(k*dilation)-(int)padding;
          int begin=shift<0?-shift:0,end=(int)x.t-shift;
          if(begin<(int)at)begin=at;if(end>(int)(at+n))end=at+n;
          float value=row[i*kernel+k];
          if(begin<end)accumulate(sums+begin-at,src+begin+shift,value,(unsigned)(end-begin));
        }
      }
      memcpy(dest+at,sums,n*sizeof(float));
      ret=glv_cancel(s->memory);if(ret)fail(s,ret);
    }
  }
done:glv_free(activated);glv_free(row);glv_free(bias);if(s->error)drop(&y);return y;
}
static struct matrix conv(struct glv_tts *s,struct matrix x,const char *base,unsigned dilation,unsigned padding)
{return conv_impl(s,x,base,dilation,padding,false);}
/* Each tile keeps the original convolution sum separate from its residual;
 * the residual is still added only after the complete i/k accumulation. */
static void conv_add(struct glv_tts *s,struct matrix x,const char *base,unsigned padding,struct matrix target)
{
  char name[128];float *row=NULL,*bias=NULL,*out=NULL;if(s->error)return;
  name_of(name,sizeof(name),base,"weight");const struct glv_tensor *w=glv_tensor(&s->model,name);
  if(!w||w->ndim!=3||w->shape[0]!=target.c||w->shape[1]!=x.c||x.t+2*padding+1!=target.t+w->shape[2]){fail(s,-EBADMSG);return;}
  unsigned kernel=w->shape[2];row=glv_alloc(s->memory,(size_t)x.c*kernel*4);out=glv_alloc(s->memory,CONV_TILE*4u);
  if(!row||!out){fail(s,-ENOMEM);goto done;}
  name_of(name,sizeof(name),base,"bias");bias=vector(s,name,target.c);if(!bias)goto done;
#ifdef GLV_TTS_PIE
  if(!strncmp(base,"vocoder.",8)){
    int ret=glv_tts_pie_conv(&s->model,w,x.v,x.c,x.t,target.v,target.c,target.t,1,padding,bias,false,true);
    if(ret)fail(s,ret);goto done;
  }
#endif
  for(unsigned o=0;o<target.c&&!s->error;o++){
    int ret=glv_tensor_row(&s->model,w,o,row,x.c*kernel);if(ret){fail(s,ret);break;}
    for(unsigned at=0;at<target.t&&!s->error;at+=CONV_TILE){
      unsigned n=target.t-at;if(n>CONV_TILE)n=CONV_TILE;
      for(unsigned t=0;t<n;t++)out[t]=bias[o];
      for(unsigned i=0;i<x.c;i++)for(unsigned k=0;k<kernel;k++){
        int shift=(int)k-(int)padding,begin=shift<0?-shift:0,end=(int)x.t-shift;
        if(begin<(int)at)begin=at;if(end>(int)(at+n))end=at+n;
        const float *src=x.v+(size_t)i*x.t;float v=row[i*kernel+k];
        if(begin<end)accumulate(out+begin-at,src+begin+shift,v,(unsigned)(end-begin));
      }
      for(unsigned t=0;t<n;t++)target.v[(size_t)o*target.t+at+t]+=out[t];
      ret=glv_cancel(s->memory);if(ret)fail(s,ret);
    }
  }
done:glv_free(row);glv_free(out);glv_free(bias);
}
static struct matrix linear(struct glv_tts *s,struct matrix x,const char *weight_name,const char *bias_name,unsigned oc)
{
  struct matrix y=make(s,oc,x.t);float *row=NULL,*bias=NULL;if(!y.v)return y;
  const struct glv_tensor *w=glv_tensor(&s->model,weight_name);
  if(!w||w->ndim!=2||w->shape[0]!=oc||w->shape[1]!=x.c){fail(s,-EBADMSG);goto done;}
  row=glv_alloc(s->memory,x.c*4u);if(!row){fail(s,-ENOMEM);goto done;}
  if(bias_name){bias=vector(s,bias_name,oc);if(!bias)goto done;}
#ifdef GLV_TTS_PARALLEL
  {int handled=conv_try_parallel(s,x,y,w,bias,NULL,x.c,1,1,0,false);if(handled)goto done;}
#endif
  for(unsigned o=0;o<oc&&!s->error;o++){
    int ret=glv_tensor_row(&s->model,w,o,row,x.c);if(ret){fail(s,ret);break;}
    float *dest=y.v+(size_t)o*x.t;for(unsigned t=0;t<x.t;t++)dest[t]=bias?bias[o]:0;
    for(unsigned i=0;i<x.c;i++)for(unsigned t=0;t<x.t;t++)dest[t]+=row[i]*x.v[(size_t)i*x.t+t];
  }
done:glv_free(row);glv_free(bias);if(s->error)drop(&y);return y;
}
static void norm(struct glv_tts *s,struct matrix x,const char *base,bool group)
{
  if(s->error)return;char name[128];name_of(name,sizeof(name),base,"weight");float *scale=vector(s,name,x.c);
  name_of(name,sizeof(name),base,"bias");float *bias=vector(s,name,x.c);
  if(!scale||!bias)goto done;
  if(group){
    size_t n=(size_t)x.c*x.t;float mean=0,var=0;
    for(size_t i=0;i<n;i++)mean+=x.v[i];mean/=n;
    for(size_t i=0;i<n;i++){float d=x.v[i]-mean;var+=d*d;}float factor=1/sqrtf(var/n+1e-5f);
    for(unsigned c=0;c<x.c;c++)for(unsigned t=0;t<x.t;t++)x.v[(size_t)c*x.t+t]=(x.v[(size_t)c*x.t+t]-mean)*factor*scale[c]+bias[c];
  }else for(unsigned t=0;t<x.t;t++){
    float mean=0,var=0;for(unsigned c=0;c<x.c;c++)mean+=x.v[(size_t)c*x.t+t];mean/=x.c;
    for(unsigned c=0;c<x.c;c++){float d=x.v[(size_t)c*x.t+t]-mean;var+=d*d;}float factor=1/sqrtf(var/x.c+1e-5f);
    for(unsigned c=0;c<x.c;c++)x.v[(size_t)c*x.t+t]=(x.v[(size_t)c*x.t+t]-mean)*factor*scale[c]+bias[c];
  }
done:glv_free(scale);glv_free(bias);
}
#ifdef GLV_TTS_PARALLEL
struct block_task {
  struct matrix x,h;const float *a,*b,*ab,*bb,*scale;const int *mask;
};
static int block_task_body(void *argument,unsigned first,unsigned last,struct glv_tts_lane *lane)
{
  const struct block_task *task=argument;struct matrix x=task->x,h=task->h;
  if(x.c>CHANNELS)return -E2BIG;
  float hidden[CHANNELS*4];
  for(unsigned t=first;t<last;t++){
    if(t==first||(t&7)==0){int ret=glv_tts_parallel_cancel(lane);if(ret)return ret;}
    for(unsigned o=0;o<x.c*4;o++){
      float sum=task->ab[o];
      for(unsigned c=0;c<x.c;c++)sum+=task->a[(size_t)o*x.c+c]*h.v[(size_t)c*x.t+t];
      hidden[o]=gelu(sum);
    }
    for(unsigned o=0;o<x.c;o++){
      float sum=task->bb[o];for(unsigned c=0;c<x.c*4;c++)sum+=task->b[(size_t)o*x.c*4+c]*hidden[c];
      x.v[(size_t)o*x.t+t]=task->mask&&!task->mask[t]?0:x.v[(size_t)o*x.t+t]+task->scale[o]*sum;
    }
  }
  return 0;
}
#endif
static void block(struct glv_tts *s,struct matrix x,const char *base,const int *mask)
{
  struct matrix h={0};char name[128];float *a=NULL,*b=NULL,*ab=NULL,*bb=NULL,*scale=NULL,*hidden=NULL;
  name_of(name,sizeof(name),base,"dw");h=conv(s,x,name,1,3);if(!h.v)return;
  name_of(name,sizeof(name),base,"norm");norm(s,h,name,false);
  name_of(name,sizeof(name),base,"a.weight");a=weights(s,name,x.c*4,x.c);
  name_of(name,sizeof(name),base,"a.bias");ab=vector(s,name,x.c*4);
  name_of(name,sizeof(name),base,"b.weight");b=weights(s,name,x.c,x.c*4);
  name_of(name,sizeof(name),base,"b.bias");bb=vector(s,name,x.c);
  name_of(name,sizeof(name),base,"scale");scale=vector(s,name,x.c);
  hidden=glv_alloc(s->memory,x.c*4*4u);if(!hidden)fail(s,-ENOMEM);
#ifdef GLV_TTS_PARALLEL
  if(!s->error&&!s->memory->tts_serial_fp){
    /* Inputs/weights are already owned. Time columns are independent after
     * depthwise convolution/norm; each lane gets its own hidden scratch. */
    struct block_task task={x,h,a,b,ab,bb,scale,mask};
    unsigned before=s->memory->profile.tts_parallel_jobs;
    int ret=glv_tts_parallel_run(s->memory,block_task_body,&task,x.t,(uint64_t)x.t*x.c*x.c*8u);
    s->memory->profile.tts_parallel_mlp_jobs+=s->memory->profile.tts_parallel_jobs-before;
    if(ret)fail(s,ret);goto block_done;
  }
#endif
  for(unsigned t=0;t<x.t&&!s->error;t++){
    if((t&15)==0){int ret=glv_cancel(s->memory);if(ret){fail(s,ret);break;}}
    for(unsigned o=0;o<x.c*4;o++){
      float sum=ab[o];for(unsigned c=0;c<x.c;c++)sum+=a[(size_t)o*x.c+c]*h.v[(size_t)c*x.t+t];hidden[o]=gelu(sum);
    }
    for(unsigned o=0;o<x.c;o++){
      float sum=bb[o];for(unsigned c=0;c<x.c*4;c++)sum+=b[(size_t)o*x.c*4+c]*hidden[c];
      x.v[(size_t)o*x.t+t]=mask&&!mask[t]?0:x.v[(size_t)o*x.t+t]+scale[o]*sum;
    }
  }
#ifdef GLV_TTS_PARALLEL
block_done:
#endif
  drop(&h);glv_free(a);glv_free(b);glv_free(ab);glv_free(bb);glv_free(scale);glv_free(hidden);
}
static void attention(struct glv_tts *s,struct matrix x,const int *ids)
{
  struct matrix normed=clone(s,x),qkv={0},out={0},projected={0};float *scores=NULL;
  norm(s,normed,"acoustic.attnorm",false);
  if(s->error)goto done;
  qkv=linear(s,normed,"acoustic.att.in_proj_weight","acoustic.att.in_proj_bias",x.c*3);drop(&normed);
  out=make(s,x.c,x.t);scores=glv_alloc(s->memory,x.t*4u);if(!scores)fail(s,-ENOMEM);
  for(unsigned head=0;head<4&&!s->error;head++)for(unsigned t=0;t<x.t;t++){
    float largest=-FLT_MAX;unsigned width=x.c/4;
    for(unsigned k=0;k<x.t;k++){
      float sum=0;for(unsigned c=0;c<width;c++)sum+=qkv.v[(head*width+c)*x.t+t]*qkv.v[(x.c+head*width+c)*x.t+k];
      scores[k]=ids[k]?sum/sqrtf((float)width):-INFINITY;if(scores[k]>largest)largest=scores[k];
    }
    float total=0;for(unsigned k=0;k<x.t;k++){scores[k]=expf(scores[k]-largest);total+=scores[k];}
    if(!glv_finite(total)||total<=0){fail(s,-ERANGE);break;}
    for(unsigned c=0;c<width;c++){
      float sum=0;for(unsigned k=0;k<x.t;k++)sum+=scores[k]/total*qkv.v[(2*x.c+head*width+c)*x.t+k];out.v[(head*width+c)*x.t+t]=sum;
    }
  }
  if(!s->error)projected=linear(s,out,"acoustic.att.out_proj.weight","acoustic.att.out_proj.bias",x.c);
  if(!s->error)for(unsigned c=0;c<x.c;c++)for(unsigned t=0;t<x.t;t++)x.v[(size_t)c*x.t+t]=ids[t]?x.v[(size_t)c*x.t+t]+projected.v[(size_t)c*x.t+t]:0;
done:drop(&normed);drop(&qkv);drop(&out);drop(&projected);glv_free(scores);
}
static struct matrix predict(struct glv_tts *s,struct matrix x,const char *base)
{
  struct matrix a={0},b={0};char name[128];name_of(name,sizeof(name),base,"net.0");a=conv(s,x,name,1,1);
  if(s->error)goto done;activation(a,0);name_of(name,sizeof(name),base,"net.2");norm(s,a,name,true);
  name_of(name,sizeof(name),base,"net.4");b=conv(s,a,name,1,1);drop(&a);
  if(s->error)goto done;activation(b,0);name_of(name,sizeof(name),base,"net.6");norm(s,b,name,true);
  name_of(name,sizeof(name),base,"net.7");a=conv(s,b,name,1,0);
done:drop(&b);if(s->error)drop(&a);return a;
}
int glv_tts_open(const char *root,struct glv_memory *memory,struct glv_tts **out)
{
  *out=NULL;
#ifdef GLV_TTS_INT8
  if(memory->tts_int8_policy)return -EBUSY;
#endif
  struct glv_tts *s=glv_alloc(memory,sizeof(*s));if(!s)return -ENOMEM;s->memory=memory;
  /* Low-free-memory sessions stream the same Q8 rows from SD. No different
   * model, quantization, or numerical path is selected. */
  bool resident=memory->limit>=10u*1024u*1024u;
  int ret=glv_model_open(&s->model,root,memory,resident);if(ret){glv_tts_close(s);return ret;}
  s->model.bulk_q8=memory->tts_bulk_rows;
  const struct glv_tensor *w=glv_tensor(&s->model,"acoustic.embed.weight");
  const struct glv_tensor *v=glv_tensor(&s->model,"vocoder.input_conv.weight");
  if(!w||w->ndim!=2||w->shape[0]!=261||w->shape[1]!=CHANNELS||!v||v->shape[0]!=208){glv_tts_close(s);return -EBADMSG;}
#ifdef GLV_TTS_INT8
  if(memory->tts_int8){ret=glv_tts_int8_policy_load(&s->model);if(ret){glv_tts_close(s);return ret;}memory->tts_int8_owner=s;}
#endif
  *out=s;return 0;
}
void glv_tts_close(struct glv_tts *s)
{if(s){
#ifdef GLV_TTS_INT8
  if(s->memory->tts_int8_owner==s)glv_tts_int8_policy_close(s->memory);
#endif
  glv_model_close(&s->model);glv_free(s);}}
int glv_tts_mel(struct glv_tts *s,const int *ids,const unsigned char *kind,unsigned n,float **mel,unsigned *frames)
{
  *mel=NULL;*frames=0;if(!s||!n||n>MAX_PHONES||!ids||!kind)return -EINVAL;s->error=0;
  struct matrix x=make(s,CHANNELS,n),duration={0},prosody={0},projected={0},expanded={0},low={0},out={0},detail={0},detail_out={0};
  float row[CHANNELS];unsigned durations[MAX_PHONES],total=0;char name[128];
  const struct glv_tensor *embedding=glv_tensor(&s->model,"acoustic.embed.weight");
  for(unsigned t=0;t<n&&!s->error;t++){
    if(ids[t]<0||ids[t]>=261){fail(s,-EINVAL);break;}
    int ret=glv_tensor_row(&s->model,embedding,ids[t],row,CHANNELS);if(ret){fail(s,ret);break;}
    for(unsigned c=0;c<CHANNELS;c++){
      float angle=t*expf((float)(c/2*2)*(-logf(10000.f)/CHANNELS));
      x.v[c*n+t]=ids[t]?row[c]+(c%2?cosf(angle):sinf(angle)):0;
    }
  }
  for(unsigned i=0;i<4&&!s->error;i++){snprintf(name,sizeof(name),"acoustic.enc.%u",i);block(s,x,name,ids);}
  if(!s->error)attention(s,x,ids);
  if(!s->error)duration=predict(s,x,"acoustic.duration");
  if(!s->error)prosody=predict(s,x,"acoustic.prosody");
  for(unsigned i=0;i<n&&!s->error;i++){
    float value=fmaxf(0,fminf(4.5f,duration.v[i]));float factor=kind[i]==2?1.2f:kind[i]==1?1.05f:1.f;
    value=(expf(value)-1)*factor/.9f;
    /* PyTorch round is ties-to-even. The prepared NuttX libm has no
     * nearbyintf; keep the rule explicit instead of changing it to roundf. */
    float whole=floorf(value),fraction=value-whole;
    if(fraction>.5f||(fraction==.5f&&((unsigned)whole&1)))whole++;
    value=fmaxf(1,fminf(80,whole));durations[i]=ids[i]?(unsigned)value:0;
    total+=durations[i];
  }
  if(!total||total>MAX_FRAMES)fail(s,-E2BIG);drop(&duration);
  if(!s->error)projected=linear(s,prosody,"acoustic.proj.weight","acoustic.proj.bias",CHANNELS);drop(&prosody);
  if(!s->error)expanded=make(s,CHANNELS,total);
  if(!s->error)for(unsigned c=0;c<CHANNELS;c++){
    unsigned at=0;for(unsigned t=0;t<n;t++)for(unsigned d=0;d<durations[t];d++)expanded.v[(size_t)c*total+at++]=x.v[c*n+t]+projected.v[c*n+t];
  }
  drop(&x);drop(&projected);
  if(!s->error)block(s,expanded,"acoustic.dec.0",NULL);
  if(!s->error)low=make(s,CHANNELS,(total+1)/2);
  if(!s->error)for(unsigned c=0;c<CHANNELS;c++)for(unsigned t=0;t<low.t;t++)
    low.v[(size_t)c*low.t+t]=t*2+1<total?(expanded.v[(size_t)c*total+t*2]+expanded.v[(size_t)c*total+t*2+1])*.5f:expanded.v[(size_t)c*total+t*2];
  for(unsigned i=1;i<4&&!s->error;i++){snprintf(name,sizeof(name),"acoustic.dec.%u",i);block(s,low,name,NULL);}
  float *pyramid=vector(s,"acoustic.pyramid_gate",1);
  if(!s->error){float gate=1/(1+expf(-*pyramid));
    for(unsigned c=0;c<CHANNELS;c++)for(unsigned t=0;t<total;t++){
      float pos=(t+.5f)*.5f-.5f;int left=(int)floorf(pos);float fraction=pos-left;
      int a=left<0?0:left>=(int)low.t?(int)low.t-1:left;
      int b=left+1<0?0:left+1>=(int)low.t?(int)low.t-1:left+1;
      expanded.v[(size_t)c*total+t]+=gate*(low.v[(size_t)c*low.t+a]*(1-fraction)+low.v[(size_t)c*low.t+b]*fraction);
    }
  }
  glv_free(pyramid);drop(&low);
  if(!s->error)block(s,expanded,"acoustic.dec.4",NULL);
  if(!s->error)out=linear(s,expanded,"acoustic.mel.weight","acoustic.mel.bias",80);
  if(!s->error)detail=conv(s,expanded,"acoustic.detail.0",1,2);drop(&expanded);
  if(!s->error){for(size_t i=0;i<(size_t)detail.c*detail.t;i++)detail.v[i]=gelu(detail.v[i]);detail_out=conv(s,detail,"acoustic.detail.2",1,0);}
  drop(&detail);float *detail_gate=vector(s,"acoustic.detail_gate",1);
  if(!s->error){float gate=1/(1+expf(-*detail_gate));for(size_t i=0;i<(size_t)80*total;i++){
      out.v[i]+=gate*detail_out.v[i];if(!glv_finite(out.v[i])){fail(s,-ERANGE);break;}}}
  glv_free(detail_gate);drop(&detail_out);
  if(s->error){drop(&out);return s->error;}
  *mel=out.v;*frames=total;return 0;
}
#ifdef GLV_TTS_PARALLEL
struct upsample_task {struct matrix x,y;const float *packed,*bias;unsigned step,padding;};
static int upsample_task_body(void *argument,unsigned first_output,unsigned last_output,struct glv_tts_lane *lane)
{
  const struct upsample_task *task=argument;struct matrix x=task->x,y=task->y;
  unsigned step=task->step,padding=task->padding;const float *packed=task->packed,*bias=task->bias;
    for(unsigned o=first_output;o<last_output;o++)for(unsigned p=0;p<step;p++){
      unsigned first=(p+padding)%step,shift=(p+padding-first)/step;
      const float *weight=packed+((size_t)o*step+p)*x.c*2;
      float *dest=y.v+(size_t)o*y.t+p;
      for(unsigned at=0;at<x.t;at+=CONV_TILE){
        unsigned stop=at+CONV_TILE;if(stop>x.t)stop=x.t;
        for(unsigned q=at;q<stop;){
          if(q>0&&q+4<x.t&&q+4<=stop){
            float a=bias[o],b=a,c=a,d=a;
            for(unsigned i=0;i<x.c;i++){
              const float *in=x.v+(size_t)i*x.t+q+shift;float v=weight[i*2],u=weight[i*2+1];
              a+=in[0]*v;b+=in[1]*v;c+=in[2]*v;d+=in[3]*v;
              a+=in[-1]*u;b+=in[0]*u;c+=in[1]*u;d+=in[2]*u;
            }
            dest[(size_t)q*step]=a;dest[(size_t)(q+1)*step]=b;
            dest[(size_t)(q+2)*step]=c;dest[(size_t)(q+3)*step]=d;q+=4;
          }else{
            float sum=bias[o];int t=(int)(q+shift);
            for(unsigned i=0;i<x.c;i++){
              const float *in=x.v+(size_t)i*x.t;
              if((unsigned)t<x.t)sum+=in[t]*weight[i*2];
              if((unsigned)(t-1)<x.t)sum+=in[t-1]*weight[i*2+1];
            }
            dest[(size_t)q*step]=sum;q++;
          }
        }
        int ret=glv_tts_parallel_cancel(lane);if(ret)return ret;
      }
    }
  return 0;
}
/* Two output channels reuse the same five input values. Keep each output's
 * i/tap order and separate multiply/add rounding; tails use the old kernel. */
static int upsample_task_body_pair(void *argument,unsigned first_output,unsigned last_output,struct glv_tts_lane *lane)
{
  const struct upsample_task *task=argument;struct matrix x=task->x,y=task->y;
  unsigned step=task->step,padding=task->padding,o=first_output;
  for(;o+1<last_output;o+=2)for(unsigned p=0;p<step;p++){
    unsigned first=(p+padding)%step,shift=(p+padding-first)/step;
    const float *w0=task->packed+((size_t)o*step+p)*x.c*2;
    const float *w1=w0+(size_t)step*x.c*2;
    float *d0=y.v+(size_t)o*y.t+p,*d1=d0+y.t;
    for(unsigned at=0;at<x.t;at+=CONV_TILE){
      unsigned stop=at+CONV_TILE;if(stop>x.t)stop=x.t;
      for(unsigned q=at;q<stop;){
        if(q>0&&q+4<x.t&&q+4<=stop){
          float a=task->bias[o],b=a,c=a,d=a;
          float e=task->bias[o+1],f=e,g=e,h=e;
          for(unsigned i=0;i<x.c;i++){
            const float *in=x.v+(size_t)i*x.t+q+shift;
            float x0=in[-1],x1=in[0],x2=in[1],x3=in[2],x4=in[3];
            float v=w0[i*2],u=w0[i*2+1],v1=w1[i*2],u1=w1[i*2+1];
            a+=x1*v;b+=x2*v;c+=x3*v;d+=x4*v;
            e+=x1*v1;f+=x2*v1;g+=x3*v1;h+=x4*v1;
            a+=x0*u;b+=x1*u;c+=x2*u;d+=x3*u;
            e+=x0*u1;f+=x1*u1;g+=x2*u1;h+=x3*u1;
          }
          d0[(size_t)q*step]=a;d0[(size_t)(q+1)*step]=b;d0[(size_t)(q+2)*step]=c;d0[(size_t)(q+3)*step]=d;
          d1[(size_t)q*step]=e;d1[(size_t)(q+1)*step]=f;d1[(size_t)(q+2)*step]=g;d1[(size_t)(q+3)*step]=h;q+=4;
        }else{
          float a=task->bias[o],b=task->bias[o+1];int t=(int)(q+shift);
          for(unsigned i=0;i<x.c;i++){
            const float *in=x.v+(size_t)i*x.t;
            if((unsigned)t<x.t){float v=in[t];a+=v*w0[i*2];b+=v*w1[i*2];}
            if((unsigned)(t-1)<x.t){float v=in[t-1];a+=v*w0[i*2+1];b+=v*w1[i*2+1];}
          }
          d0[(size_t)q*step]=a;d1[(size_t)q*step]=b;q++;
        }
      }
      int ret=glv_tts_parallel_cancel(lane);if(ret)return ret;
    }
  }
  return o<last_output?upsample_task_body(argument,o,last_output,lane):0;
}
#ifdef GLV_TTS_FMA
/* Explicit one-rounding variant, isolated for default-off A/B and ELF review. */
static int upsample_task_body_fma(void *argument,unsigned first_output,unsigned last_output,struct glv_tts_lane *lane)
{
  const struct upsample_task *task=argument;struct matrix x=task->x,y=task->y;
  unsigned step=task->step,padding=task->padding;const float *packed=task->packed,*bias=task->bias;
    for(unsigned o=first_output;o<last_output;o++)for(unsigned p=0;p<step;p++){
      unsigned first=(p+padding)%step,shift=(p+padding-first)/step;
      const float *weight=packed+((size_t)o*step+p)*x.c*2;
      float *dest=y.v+(size_t)o*y.t+p;
      for(unsigned at=0;at<x.t;at+=CONV_TILE){
        unsigned stop=at+CONV_TILE;if(stop>x.t)stop=x.t;
        for(unsigned q=at;q<stop;){
          if(q>0&&q+4<x.t&&q+4<=stop){
            float a=bias[o],b=a,c=a,d=a;
            for(unsigned i=0;i<x.c;i++){
              const float *in=x.v+(size_t)i*x.t+q+shift;float v=weight[i*2],u=weight[i*2+1];
              a=__builtin_fmaf(in[0],v,a);b=__builtin_fmaf(in[1],v,b);c=__builtin_fmaf(in[2],v,c);d=__builtin_fmaf(in[3],v,d);
              a=__builtin_fmaf(in[-1],u,a);b=__builtin_fmaf(in[0],u,b);c=__builtin_fmaf(in[1],u,c);d=__builtin_fmaf(in[2],u,d);
            }
            dest[(size_t)q*step]=a;dest[(size_t)(q+1)*step]=b;
            dest[(size_t)(q+2)*step]=c;dest[(size_t)(q+3)*step]=d;q+=4;
          }else{
            float sum=bias[o];int t=(int)(q+shift);
            for(unsigned i=0;i<x.c;i++){
              const float *in=x.v+(size_t)i*x.t;
              if((unsigned)t<x.t)sum=__builtin_fmaf(in[t],weight[i*2],sum);
              if((unsigned)(t-1)<x.t)sum=__builtin_fmaf(in[t-1],weight[i*2+1],sum);
            }
            dest[(size_t)q*step]=sum;q++;
          }
        }
        int ret=glv_tts_parallel_cancel(lane);if(ret)return ret;
      }
    }
  return 0;
}
#endif

#endif
static struct matrix upsample(struct glv_tts *s,struct matrix x,unsigned stage)
{
  const unsigned stride[]={5,5,4,3};unsigned step=stride[stage];char name[128];
  snprintf(name,sizeof(name),"vocoder.upsamples.%u.1.weight",stage);const struct glv_tensor *w=glv_tensor(&s->model,name);
  struct matrix y={0};float *bias=NULL,*row=NULL,*packed=NULL;
  if(s->error)return y;
  if(!w||w->ndim!=3||w->shape[0]!=x.c||w->shape[2]!=2*step){fail(s,-EBADMSG);return y;}
  unsigned oc=w->shape[1],kernel=w->shape[2],padding=(step+1)/2;
  if(s->memory->tts_upsample_pair&&s->memory->tts_fma){fail(s,-EINVAL);return y;}
  y=make(s,oc,x.t*step);if(!y.v)return y;
  snprintf(name,sizeof(name),"vocoder.upsamples.%u.1.bias",stage);bias=vector(s,name,oc);
  row=glv_alloc(s->memory,oc*kernel*4u);if(!row)fail(s,-ENOMEM);if(s->error)goto done;
  activation(x,.1f);
  #ifdef GLV_TTS_PARALLEL
  uint64_t pack_began=glv_now_us();
  #endif
  /* Each output phase uses exactly two ascending kernel taps per input
   * channel. Pack those taps once, then retain four output sums in registers.
   * The original i/k order and individual FP32 rounding are unchanged. */
  packed=glv_alloc(s->memory,(size_t)x.c*oc*kernel*sizeof(float));
  if(packed){
    for(unsigned i=0;i<x.c;i++){
      int ret=glv_tensor_row(&s->model,w,i,row,oc*kernel);if(ret){fail(s,ret);goto done;}
      for(unsigned o=0;o<oc;o++)for(unsigned p=0;p<step;p++){
        unsigned first=(p+padding)%step;size_t at=((size_t)o*step+p)*x.c*2+i*2;
        packed[at]=row[o*kernel+first];packed[at+1]=row[o*kernel+first+step];
      }
    }
#ifdef GLV_TTS_PARALLEL
    s->memory->profile.tts_upsample_pack_us+=glv_now_us()-pack_began;
    struct upsample_task task={x,y,packed,bias,step,padding};
    glv_tts_parallel_body operation=upsample_task_body;
    if(s->memory->tts_upsample_pair){operation=upsample_task_body_pair;s->memory->profile.tts_upsample_pair_calls++;}
#ifdef GLV_TTS_FMA
    if(s->memory->tts_fma){operation=upsample_task_body_fma;s->memory->profile.tts_fma_upsample_calls++;}
#endif
    uint64_t compute_began=glv_now_us();
    int ret=glv_tts_parallel_run(s->memory,operation,&task,oc,(uint64_t)oc*step*x.c*x.t*2);
    s->memory->profile.tts_upsample_compute_us+=glv_now_us()-compute_began;
    if(ret)fail(s,ret);goto done;
#endif
    for(unsigned o=0;o<oc;o++)for(unsigned p=0;p<step;p++){
      unsigned first=(p+padding)%step,shift=(p+padding-first)/step;
      const float *weight=packed+((size_t)o*step+p)*x.c*2;
      float *dest=y.v+(size_t)o*y.t+p;
      for(unsigned at=0;at<x.t;at+=CONV_TILE){
        unsigned stop=at+CONV_TILE;if(stop>x.t)stop=x.t;
        for(unsigned q=at;q<stop;){
          if(q>0&&q+4<x.t&&q+4<=stop){
            float a=bias[o],b=a,c=a,d=a;
            for(unsigned i=0;i<x.c;i++){
              const float *in=x.v+(size_t)i*x.t+q+shift;float v=weight[i*2],u=weight[i*2+1];
              a+=in[0]*v;b+=in[1]*v;c+=in[2]*v;d+=in[3]*v;
              a+=in[-1]*u;b+=in[0]*u;c+=in[1]*u;d+=in[2]*u;
            }
            dest[(size_t)q*step]=a;dest[(size_t)(q+1)*step]=b;
            dest[(size_t)(q+2)*step]=c;dest[(size_t)(q+3)*step]=d;q+=4;
          }else{
            float sum=bias[o];int t=(int)(q+shift);
            for(unsigned i=0;i<x.c;i++){
              const float *in=x.v+(size_t)i*x.t;
              if((unsigned)t<x.t)sum+=in[t]*weight[i*2];
              if((unsigned)(t-1)<x.t)sum+=in[t-1]*weight[i*2+1];
            }
            dest[(size_t)q*step]=sum;q++;
          }
        }
        int ret=glv_cancel(s->memory);if(ret){fail(s,ret);goto done;}
      }
    }
    goto done;
  }
  if(s->memory->tts_upsample_pair)s->memory->profile.tts_upsample_pair_fallbacks++;
#ifdef GLV_TTS_FMA
  /* Do not silently mix rounding modes after a failed optional packing buffer. */
  if(s->memory->tts_fma){s->memory->profile.tts_fma_rejections++;fail(s,-ENOMEM);goto done;}
#endif
  for(unsigned o=0;o<oc;o++)for(unsigned t=0;t<y.t;t++)y.v[(size_t)o*y.t+t]=bias[o];
  for(unsigned i=0;i<x.c&&!s->error;i++){
    int ret=glv_tensor_row(&s->model,w,i,row,oc*kernel);if(ret){fail(s,ret);break;}
    for(unsigned o=0;o<oc;o++)for(unsigned k=0;k<kernel;k++){
      float v=row[o*kernel+k];float *out=y.v+(size_t)o*y.t;const float *in=x.v+(size_t)i*x.t;
      for(unsigned t=0;t<x.t;t++){int pos=(int)(t*step+k)-(int)padding;if(pos>=0&&(unsigned)pos<y.t)out[pos]+=in[t]*v;}
    }
  }
done:glv_free(packed);glv_free(bias);glv_free(row);if(s->error)drop(&y);return y;
}
static void residual(struct glv_tts *s,struct matrix x,unsigned index,unsigned kernel)
{
  const unsigned dilation[]={1,3,5};char name[128];
  for(unsigned i=0;i<3&&!s->error;i++){
    struct matrix b={0};snprintf(name,sizeof(name),"vocoder.blocks.%u.convs1.%u.1",index,i);
    b=conv_impl(s,x,name,dilation[i],dilation[i]*(kernel-1)/2,true);
    if(!s->error){activation(b,.1f);snprintf(name,sizeof(name),"vocoder.blocks.%u.convs2.%u.1",index,i);conv_add(s,b,name,(kernel-1)/2,x);}
    drop(&b);
  }
}
static struct matrix vocoder(struct glv_tts *s,struct matrix mel)
{
  struct matrix x=conv(s,mel,"vocoder.input_conv",1,3);
  for(unsigned stage=0;stage<4&&!s->error;stage++){
    uint64_t began=glv_now_us();struct matrix base=upsample(s,x,stage);
    uint64_t upsample_spent=glv_now_us()-began;s->memory->profile.tts_upsample_us+=upsample_spent;
    s->memory->profile.tts_upsample_stage_us[stage]+=upsample_spent;drop(&x);
    struct matrix sum=make(s,base.c,base.t);
    const unsigned kernels[]={3,7,11};
    for(unsigned j=0;j<3&&!s->error;j++){
      struct matrix branch=clone(s,base);residual(s,branch,stage*3+j,kernels[j]);
      if(!s->error)for(size_t at=0;at<(size_t)sum.c*sum.t;at++)sum.v[at]+=branch.v[at];drop(&branch);
    }
    drop(&base);if(!s->error)for(size_t at=0;at<(size_t)sum.c*sum.t;at++)sum.v[at]/=3;x=sum;
  }
  struct matrix out={0};if(!s->error){activation(x,.01f);out=conv(s,x,"vocoder.output_conv.1",1,3);}drop(&x);
  if(!s->error)for(unsigned i=0;i<out.t;i++){out.v[i]=tanhf(out.v[i]);if(!glv_finite(out.v[i])){fail(s,-ERANGE);break;}}
  if(s->error)drop(&out);return out;
}
#ifdef GLV_TTS_PIE
/* A necessary live-allocation floor, not a promise that malloc will succeed.
 * At the final k11 residual add, chunk/base/sum/branch/b are all owned, plus
 * conv_add's row/tile/bias and the six PIE temporaries. Smaller budgets cannot
 * reach this point, even if optional caches and helpers are unavailable.
 * Keep the old estimate as well; never enlarge or alter a fitting window. */
static size_t window_minimum(struct glv_tts *s,unsigned frames)
{
  const struct glv_tensor *w=glv_tensor(&s->model,"vocoder.blocks.11.convs2.2.1.weight");
  if(!w||w->type!=2||w->ndim!=3||w->shape[0]!=13||w->shape[1]!=13||w->shape[2]!=11)return 0;
  size_t time=(size_t)frames*300u,plane=13u*time*4u;
  const size_t sizes[]={80u*frames*4u,plane,plane,plane,plane,
    13u*11u*4u,CONV_TILE*4u,13u*4u,
    13u*time*2u+15u,13u*144u*2u+15u,8u*144u*2u+15u,
    13u*4u,13u*11u*4u,13u*8u*4u};
  size_t total=0;for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++)total+=glv_alloc_charge(sizes[i]);
  return total;
}
#endif
int glv_tts_vocode(struct glv_tts *s,const float *mel,unsigned frames,glv_audio_sink sink,void *opaque)
{
  if(!s||!mel||!frames||frames>MAX_FRAMES||!sink)return -EINVAL;s->error=0;
  /* Exact reverse support through output k7, three residual pairs per stage
   * (maximum radius 60), transposed strides 3/4/5/5, and input k7 needs 19
   * mel frames on each side. Keep one extra frame; preserve global boundaries.
   * This network has no time-global normalization in its vocoder. */
  for(unsigned at=0;at<frames&&!s->error;){
    unsigned tile=64,center,left,right;
retry:
    center=frames-at;if(center>tile)center=tile;
    left=at>VOCODER_HALO?at-VOCODER_HALO:0;
    right=at+center+VOCODER_HALO;if(right>frames)right=frames;
    /* Fixed 208-channel graph: the last residual peak is four 13-channel
     * sample-rate planes, one activation row, and the held mel slice. Bound
     * these by 64 KiB per mel frame plus 64 KiB for rows/allocator overhead.
     * Re-evaluate after each sink: buffered earlier PCM also uses this arena. */
    size_t available=s->memory->used<s->memory->limit?s->memory->limit-s->memory->used:0;
    size_t estimate=(size_t)(right-left)*65536u+65536u;
    if(estimate>available&&tile>1){tile/=2;goto retry;}
#ifdef GLV_TTS_PIE
    if(s->memory->tts_window_plan&&tile>1){
      s->memory->profile.tts_window_checks++;
      if(window_minimum(s,right-left)>available){
        s->memory->profile.tts_window_skips++;tile/=2;goto retry;
      }
    }
#endif
    struct matrix chunk=make(s,80,right-left),audio={0};
    if(!chunk.v){
      if(s->error==-ENOMEM&&tile>1){s->error=0;s->memory->profile.tts_memory_fallbacks++;tile/=2;goto retry;}
      break;
    }
    for(unsigned c=0;c<80;c++)memcpy(chunk.v+(size_t)c*chunk.t,mel+(size_t)c*frames+left,chunk.t*4u);
    s->memory->profile.tts_window_attempts++;
    uint64_t began=glv_now_us();audio=vocoder(s,chunk);drop(&chunk);
    uint64_t spent=glv_now_us()-began;s->memory->profile.tts_vocoder_us+=spent;
    if(s->error==-ENOMEM){s->memory->profile.tts_window_failed_us+=spent;s->memory->profile.tts_window_failed_frames+=right-left;}
    if(s->error==-ENOMEM&&tile>1){
      /* No samples from this window have reached the sink. Only allocation
       * failure can retry a smaller center; I/O, cancellation and bad models
       * keep their original errors and are never replayed. */
      drop(&audio);s->error=0;s->memory->profile.tts_memory_fallbacks++;tile/=2;goto retry;
    }
    if(!s->error){unsigned offset=(at-left)*300,n=center*300;
      if(audio.c!=1||offset>audio.t||n>audio.t-offset)fail(s,-EBADMSG);
      else {
        s->memory->profile.tts_chunks++;s->memory->profile.tts_context_frames+=right-left;
        s->memory->profile.tts_output_frames+=center;
        int ret=sink(opaque,audio.v+offset,n,24000);if(ret)fail(s,ret);
      }}
    drop(&audio);int ret=glv_cancel(s->memory);if(ret)fail(s,ret);
    if(!s->error)at+=center;
  }
  return s->error;
}
int glv_tts_phones(struct glv_tts *s,const int *ids,const unsigned char *kind,unsigned count,bool leading,glv_audio_sink sink,void *opaque)
{
  if(!s)return -EINVAL;
  float *mel=NULL;unsigned frames=0;uint64_t began=glv_now_us();
  int ret=glv_tts_mel(s,ids,kind,count,&mel,&frames);
  s->memory->profile.tts_mel_us+=glv_now_us()-began;if(ret)return ret;
  if(leading){float zero[240]={0};for(unsigned at=0;at<3000&&!ret;){unsigned n=3000-at;if(n>240)n=240;
      ret=sink(opaque,zero,n,24000);at+=n;if(!ret)ret=glv_cancel(s->memory);}}
  if(!ret)ret=glv_tts_vocode(s,mel,frames,sink,opaque);glv_free(mel);return ret;
}
