/* SPDX-License-Identifier: Apache-2.0 */
/* EdgeIdiot 9M9: 16 layers, MQA 7/1, 224 hidden, 576 FFN; interleaved RoPE. */
#include "glass_local_llm.h"
#include "glass_local_w8a8.h"
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define HIDDEN 224u
#define KV 32u
#define FFN 576u
#define VOCAB 8192u
#define LAYERS 16u
static uint32_t llm_ticks(void)
{
#ifdef __NuttX__
 uint32_t n;__asm__ volatile("rdcycle %0":"=r"(n));return n;
#else
 return (uint32_t)glv_now_us();
#endif
}

struct glv_llm {
  struct glv_memory *memory;struct glv_model model;struct glv_tokenizer tokenizer;
  char root[384];
  float *keys,*values,*scratch,*logits;unsigned position,capacity;
  bool cache_tried,head_cache_tried,projection_cache_tried;
  size_t projection_cache_bytes;
  int processed[GLV_MAX_CONTEXT];
  /* Draft candidates only. Every proposed token is verified against the
   * full model's current greedy logits before publication. */
  int draft[96];unsigned draft_count;
};
static void discard_optional_cache(struct glv_llm *s)
{
  glv_model_block_release(&s->model);
  glv_model_cache_clear(&s->model);
  s->cache_tried=s->head_cache_tried=s->projection_cache_tried=false;
  s->projection_cache_bytes=0;
}
/* Retry arithmetic only after reclaiming optional raw rows. Matrix calls
 * own and release their temporary allocations on ENOMEM. No tool action,
 * media error or cancellation is retried; inputs and outputs do not alias.
 */
static int llm_matvec(struct glv_llm *s,const char *name,const float *x,float *out,unsigned rows,unsigned cols)
{uint32_t now=llm_ticks();glv_model_block_select(&s->model,name);int ret=glv_w8a8_matbatch_cached(&s->model,name,x,out,rows,cols,1,strstr(name,".k_proj.")||strstr(name,".v_proj.")||strstr(name,".up_proj."));glv_model_block_select(&s->model,NULL);if(rows==VOCAB)s->memory->profile.llm_head_ticks+=(uint32_t)(llm_ticks()-now);return ret;}
static int llm_matbatch(struct glv_llm *s,const char *name,const float *x,float *out,unsigned rows,unsigned cols,unsigned batch)
{glv_model_block_select(&s->model,name);int ret=glv_w8a8_matbatch_cached(&s->model,name,x,out,rows,cols,batch,strstr(name,".k_proj.")||strstr(name,".v_proj.")||strstr(name,".up_proj."));glv_model_block_select(&s->model,NULL);return ret;}
static void progress(struct glv_llm *s,enum glv_llm_progress stage,unsigned done,unsigned total)
{if(s->memory->llm_progress)s->memory->llm_progress(s->memory->opaque,stage,done,total);}
void glv_llm_close(struct glv_llm *s)
{
  if(!s)return;
  glv_model_close(&s->model);glv_tokenizer_close(&s->tokenizer);
  glv_free(s->keys);glv_free(s->values);glv_free(s->scratch);glv_free(s->logits);glv_free(s);
}
int glv_llm_open(const char *root,struct glv_memory *memory,struct glv_llm **out)
{
  *out=NULL;if(strlen(root)>=384)return -ENAMETOOLONG;struct glv_llm *s=glv_alloc(memory,sizeof(*s));if(!s)return -ENOMEM;s->memory=memory;strcpy(s->root,root);
  int ret=glv_model_open(&s->model,root,memory,true);if(ret)goto fail;
  /* Q8 complete rows share the existing bounded, authenticated reader. */
  s->model.bulk_q8=true;
  ret=glv_w8a8_prepare(&s->model);if(ret)goto fail;
  const struct glv_tensor *embedding=glv_tensor(&s->model,"model.embed_tokens.weight");
  if(!embedding||embedding->ndim!=2||embedding->shape[0]!=VOCAB||embedding->shape[1]!=HIDDEN){ret=-EBADMSG;goto fail;}
  char path[384];if(snprintf(path,sizeof(path),"%s/tokenizer.bin",root)>=(int)sizeof(path)){ret=-ENAMETOOLONG;goto fail;}
  ret=glv_tokenizer_open(&s->tokenizer,path,memory);if(ret)goto fail;
  s->scratch=glv_alloc(memory,(HIDDEN*7+KV*2+FFN*2+7*GLV_MAX_CONTEXT)*sizeof(float));
  s->logits=glv_alloc(memory,VOCAB*sizeof(float));
  if(!s->scratch||!s->logits){ret=-ENOMEM;goto fail;}
  *out=s;return 0;
fail:glv_llm_close(s);return ret;
}
size_t glv_llm_resident_bytes(const struct glv_llm *s)
{return s&&s->model.data?s->model.bytes:0;}
size_t glv_llm_kv_bytes(const struct glv_llm *s)
{return s?(size_t)2*LAYERS*s->capacity*KV*sizeof(float):0;}
size_t glv_llm_trim_kv(struct glv_llm *s)
{
  if(!s)return 0;size_t before=s->memory->used;
  glv_free(s->keys);glv_free(s->values);s->keys=s->values=NULL;
  s->position=s->capacity=0;memset(s->processed,0,sizeof(s->processed));
  discard_optional_cache(s);s->memory->profile.llm_kv_bytes=0;
  return before-s->memory->used;
}
static int rms_core(struct glv_llm *s,const char *name,const float *x,float *out,float *weight)
{
  int ret=glv_vector(&s->model,name,weight,HIDDEN);if(ret)return ret;
  float sum=0;for(unsigned i=0;i<HIDDEN;i++)sum+=x[i]*x[i];
  float scale=1/sqrtf(sum/HIDDEN+1e-5f);if(!glv_finite(scale))return -ERANGE;
  for(unsigned i=0;i<HIDDEN;i++)out[i]=x[i]*scale*weight[i];return 0;
}
static int rms(struct glv_llm *s,const char *name,const float *x,float *out,float *weight)
{uint32_t now=llm_ticks();int ret=rms_core(s,name,x,out,weight);s->memory->profile.llm_norm_ticks+=(uint32_t)(llm_ticks()-now);return ret;}
static void rope_coefficients(float *out,unsigned position)
{
  for(unsigned i=0;i<16;i++){float angle=position/powf(10000.f,(float)(i*2)/32.f);out[i*2]=cosf(angle);out[i*2+1]=sinf(angle);}
}
static void rope(float *v,unsigned heads,const float *coefficients)
{
  for(unsigned head=0;head<heads;head++)for(unsigned i=0;i<16;i++){
    float c=coefficients[i*2],s=coefficients[i*2+1];float a=v[head*32+i*2],b=v[head*32+i*2+1];
    v[head*32+i*2]=a*c-b*s;v[head*32+i*2+1]=a*s+b*c;
  }
}
static int reserve_kv(struct glv_llm *s,unsigned tokens)
{
  if(!tokens||tokens>GLV_MAX_CONTEXT||s->position>tokens)return -EINVAL;
  if(s->capacity>=tokens)return 0;
  /* Grow one KV plane at a time, copying each layer with its old stride.
   * Optional raw caches may be evicted to admit mandatory KV storage. */
  unsigned old=s->capacity;size_t bytes=(size_t)LAYERS*tokens*KV*sizeof(float);
  float *keys=glv_alloc(s->memory,bytes);
  if(!keys){discard_optional_cache(s);keys=glv_alloc(s->memory,bytes);}
  if(!keys)return -ENOMEM;
  for(unsigned l=0;l<LAYERS;l++)if(s->position)
    memcpy(keys+(size_t)l*tokens*KV,s->keys+(size_t)l*old*KV,(size_t)s->position*KV*sizeof(float));
  glv_free(s->keys);s->keys=keys;
  float *values=glv_alloc(s->memory,bytes);
  if(!values){discard_optional_cache(s);values=glv_alloc(s->memory,bytes);}
  if(!values){
    /* A failed partial relocation is never reused with mixed layer strides. */
    glv_free(s->keys);glv_free(s->values);s->keys=s->values=NULL;s->capacity=s->position=0;return -ENOMEM;
  }
  for(unsigned l=0;l<LAYERS;l++)if(s->position)
    memcpy(values+(size_t)l*tokens*KV,s->values+(size_t)l*old*KV,(size_t)s->position*KV*sizeof(float));
  glv_free(s->values);s->values=values;s->capacity=tokens;s->memory->profile.llm_kv_bytes=glv_llm_kv_bytes(s);return 0;
}
static void prepare_cache(struct glv_llm *s,bool decode)
{
  if(s->model.data)return;
#ifndef GLV_DISABLE_LLM_BLOCK_CACHE
  /* Mandatory KV and prefill work are allocated first. Recycle one packed
   * matrix/tile buffer; retain existing useful row caches and system reserve. */
  if(!s->model.block_data){
    for(size_t bytes=192u*1024u;bytes>=24u*1024u;bytes/=2){
      int ret=glv_model_block_reserve(&s->model,bytes);if(!ret)break;
    }
  }
#endif
#ifndef GLV_DISABLE_LLM_WEIGHT_CACHE
  /* Mandatory KV and prefill work are already allocated. These raw caches
   * preserve FP32 math, remain optional, and die with this request's model. */
  char name[128];
  if(!s->cache_tried){s->cache_tried=true;
  for(unsigned layer=0;layer<LAYERS;layer++){
    snprintf(name,sizeof(name),"model.layers.%u.input_layernorm.weight",layer);
    (void)glv_model_cache_tensor(&s->model,name);
    snprintf(name,sizeof(name),"model.layers.%u.post_attention_layernorm.weight",layer);
    (void)glv_model_cache_tensor(&s->model,name);
  }
  (void)glv_model_cache_tensor(&s->model,"model.norm.weight");
  }
  /* Prime raw rows during their first prefill read, after mandatory
   * workspace is allocated. Failed optional admission retries in decode. */
  if(!s->head_cache_tried){
  const char *head=glv_tensor(&s->model,"lm_head.weight")?"lm_head.weight":"model.embed_tokens.weight";
  int admitted=glv_model_cache_tensor(&s->model,head);
  s->head_cache_tried=!admitted||decode;
  }
  /* Preserve head priority if it could not fit beside prefill workspace. */
  if(!s->head_cache_tried&&!decode)return;
  /* Keep the projection cap across both admission phases. Decode can
   * top up after prefill scratch is released; existing tensors charge zero.
   * Mandatory KV growth and later prefills can still evict all raw caches.
   */
  if(!s->projection_cache_tried){
    static const char *const suffixes[]={"mlp.gate_proj.weight","mlp.up_proj.weight",
      "mlp.down_proj.weight","self_attn.q_proj.weight","self_attn.o_proj.weight",
      "self_attn.k_proj.weight","self_attn.v_proj.weight"};
    for(unsigned layer=0;layer<LAYERS;layer++)for(unsigned kind=0;kind<7;kind++){
      snprintf(name,sizeof(name),"model.layers.%u.%s",layer,suffixes[kind]);
      const struct glv_tensor *t=glv_tensor(&s->model,name);
      if(!t||t->ndim!=2)continue;
      /* Leave margin for the cache node and bitmap inside this 4 MiB cap. */
      size_t extra=s->projection_cache_bytes;
      size_t charge=glv_alloc_charge(t->bytes+(t->shape[0]+7u)/8u+128u);
      if(extra>4u*1024u*1024u||charge>4u*1024u*1024u-extra)continue;
      size_t before=s->model.cache_bytes;
      (void)glv_model_cache_tensor(&s->model,name);
      s->projection_cache_bytes+=s->model.cache_bytes-before;
    }
    s->projection_cache_tried=decode;
  }
#else
  (void)s;(void)decode;
#endif
}
static int attention(struct glv_llm *s,const float *q,const float *keys,const float *values,unsigned count,float *scores,float *context)
{
 uint32_t began=llm_ticks();
 for(unsigned p=0;p<count;p++){
  if(!(p&63u)){int ret=glv_cancel(s->memory);if(ret)return ret;}
  const float *k=keys+(size_t)p*KV;
  float a0=0,a1=0,a2=0,a3=0,a4=0,a5=0,a6=0;
  for(unsigned i=0;i<32;i++){
   float v=k[i];a0+=q[i]*v;a1+=q[32+i]*v;a2+=q[64+i]*v;
   a3+=q[96+i]*v;a4+=q[128+i]*v;a5+=q[160+i]*v;a6+=q[192+i]*v;
  }
  scores[p]=a0*.1767766952966369f;scores[GLV_MAX_CONTEXT+p]=a1*.1767766952966369f;
  scores[2*GLV_MAX_CONTEXT+p]=a2*.1767766952966369f;scores[3*GLV_MAX_CONTEXT+p]=a3*.1767766952966369f;
  scores[4*GLV_MAX_CONTEXT+p]=a4*.1767766952966369f;scores[5*GLV_MAX_CONTEXT+p]=a5*.1767766952966369f;
  scores[6*GLV_MAX_CONTEXT+p]=a6*.1767766952966369f;
 }
 s->memory->profile.llm_attn_qk_ticks+=(uint32_t)(llm_ticks()-began);
 for(unsigned head=0;head<7;head++){
  float *w=scores+head*GLV_MAX_CONTEXT;float largest=-FLT_MAX;
  began=llm_ticks();for(unsigned p=0;p<count;p++)if(w[p]>largest)largest=w[p];
  float total=0;for(unsigned p=0;p<count;p++){w[p]=expf(w[p]-largest);total+=w[p];}
  if(!glv_finite(total)||total<=0)return -ERANGE;
  for(unsigned p=0;p<count;p++)w[p]/=total;
  s->memory->profile.llm_attn_softmax_ticks+=(uint32_t)(llm_ticks()-began);
  began=llm_ticks();
  /* Four independent register accumulators hide FPU latency without
   * changing the ascending-position reduction of any output lane. */
  for(unsigned i=0;i<32;i+=4){
   float a=0,b=0,c=0,d=0;
   for(unsigned p=0;p<count;p++){
    const float *v=values+(size_t)p*KV+i;float weight=w[p];
    a+=weight*v[0];b+=weight*v[1];c+=weight*v[2];d+=weight*v[3];
   }
   float *out=context+head*32+i;out[0]=a;out[1]=b;out[2]=c;out[3]=d;
  }
  s->memory->profile.llm_attn_value_ticks+=(uint32_t)(llm_ticks()-began);
 }
 return 0;
}
int glv_llm_forward(struct glv_llm *s,int token,float *logits)
{
  if(!s||token<0||(unsigned)token>=VOCAB||s->position>=GLV_MAX_CONTEXT)return -EINVAL;
  if(!s->capacity){int ret=reserve_kv(s,GLV_MAX_CONTEXT);if(ret)return ret;}
  if(s->position>=s->capacity)return -E2BIG;
  prepare_cache(s,true);
  float *x=s->scratch,*xb=x+HIDDEN,*q=xb+HIDDEN,*context=q+HIDDEN;
  float *dense=context+HIDDEN,*weight=dense+HIDDEN,*extra=weight+HIDDEN;
  float *key=extra+HIDDEN,*value=key+KV,*gate=value+KV,*up=gate+FFN,*scores=up+FFN;
  int ret=glv_tensor_row(&s->model,glv_tensor(&s->model,"model.embed_tokens.weight"),token,x,HIDDEN);
  if(ret)return ret;char name[128];unsigned position=s->position;
  /* extra is an existing unused 512-float scratch region. */
  rope_coefficients(extra,position);
  for(unsigned layer=0;layer<LAYERS;layer++){
    if((ret=glv_cancel(s->memory)))return ret;
#define PATH(suffix) snprintf(name,sizeof(name),"model.layers.%u." suffix,layer)
    PATH("input_layernorm.weight");if((ret=rms(s,name,x,xb,weight)))return ret;
    PATH("self_attn.q_proj.weight");if((ret=llm_matvec(s,name,xb,q,HIDDEN,HIDDEN)))return ret;
    PATH("self_attn.k_proj.weight");if((ret=llm_matvec(s,name,xb,key,KV,HIDDEN)))return ret;
    PATH("self_attn.v_proj.weight");if((ret=llm_matvec(s,name,xb,value,KV,HIDDEN)))return ret;
    rope(q,7,extra);rope(key,1,extra);
    uint32_t attn_began=llm_ticks();
    size_t base=(size_t)layer*s->capacity*KV;
    memcpy(s->keys+base+position*KV,key,KV*4u);memcpy(s->values+base+position*KV,value,KV*4u);
    ret=attention(s,q,s->keys+base,s->values+base,position+1,scores,context);if(ret)return ret;
    s->memory->profile.llm_attention_ticks+=(uint32_t)(llm_ticks()-attn_began);
    PATH("self_attn.o_proj.weight");if((ret=llm_matvec(s,name,context,dense,HIDDEN,HIDDEN)))return ret;
    for(unsigned i=0;i<HIDDEN;i++)x[i]+=dense[i];
    PATH("post_attention_layernorm.weight");if((ret=rms(s,name,x,xb,weight)))return ret;
    PATH("mlp.gate_proj.weight");if((ret=llm_matvec(s,name,xb,gate,FFN,HIDDEN)))return ret;
    PATH("mlp.up_proj.weight");if((ret=llm_matvec(s,name,xb,up,FFN,HIDDEN)))return ret;
    uint32_t activation_began=llm_ticks();
    for(unsigned i=0;i<FFN;i++)gate[i]=gate[i]/(1+expf(-gate[i]))*up[i];
    s->memory->profile.llm_activation_ticks+=(uint32_t)(llm_ticks()-activation_began);
    PATH("mlp.down_proj.weight");if((ret=llm_matvec(s,name,gate,dense,HIDDEN,FFN)))return ret;
    for(unsigned i=0;i<HIDDEN;i++)x[i]+=dense[i];
#undef PATH
  }
  if(logits){
    if((ret=rms(s,"model.norm.weight",x,xb,weight)))return ret;
    const char *head=glv_tensor(&s->model,"lm_head.weight")?"lm_head.weight":"model.embed_tokens.weight";
    if((ret=llm_matvec(s,head,xb,logits,VOCAB,HIDDEN)))return ret;
  }
  s->processed[s->position++]=token;return 0;
}
int glv_llm_encode(struct glv_llm *s,const char *text,int *ids,unsigned count)
{return glv_encode(&s->tokenizer,text,ids,count);}
static float *prefill_workspace(struct glv_llm *s,unsigned width)
{
  void *q4_reserve=NULL;
  if(s->memory->q4_bits==16){
    q4_reserve=glv_alloc(s->memory,width*(FFN*2+FFN/32*8+4)+FFN*2+FFN/32*24+256);
    if(!q4_reserve)return NULL;
  }
  float *work=glv_alloc(s->memory,(width*(HIDDEN*5+KV*2+FFN*2+64)+HIDDEN+7*GLV_MAX_CONTEXT)*sizeof(float));
  glv_free(q4_reserve);return work;
}
static int prefill_impl(struct glv_llm *s,const int *ids,unsigned count,float *logits,bool every)
{
  if(!s||!ids||!count||count>GLV_MAX_CONTEXT-s->position)return -EINVAL;
  for(unsigned i=0;i<count;i++)if(ids[i]<0||(unsigned)ids[i]>=VOCAB)return -EINVAL;
  int ret=0;if(!s->capacity&&(ret=reserve_kv(s,GLV_MAX_CONTEXT)))return ret;
  unsigned start=s->position;
  if(count>s->capacity-start)return -E2BIG;
  if(!every)progress(s,GLV_LLM_PREFILL,0,count*LAYERS);
  /* Up to 64 positions reuse each SD projection (about 1.4 MiB). Reserve
   * room for the largest decoded row before choosing the batch, so a tight
   * budget can shrink it instead of exhausting memory at the first matmul.
   * Scalar dots keep ascending-column FP32 accumulation; attention
   * still uses only the causal prefix, including within the current batch. */
  float *row_reserve=glv_alloc(s->memory,FFN*sizeof(float));
  if(!row_reserve&&(s->model.cache_bytes||s->model.block_data)){discard_optional_cache(s);row_reserve=glv_alloc(s->memory,FFN*sizeof(float));}
  if(!row_reserve)return -ENOMEM;
  /* Power-of-two capacities retain the old eight-position fallback even
   * for a partial prompt such as 31 tokens (32 -> 16 -> 8, not 31 -> 15 -> 7). */
  unsigned width=GLV_MATBATCH_MAX;
  while(width>1&&width/2>=count)width/=2;
  float *work=NULL;
  for(;width;width/=2){
    if((ret=glv_cancel(s->memory)))break;
    /* Optional decode rows must not force later prefills to use a smaller
     * batch. Evict them and retry this width before reducing the workspace.
     */
    work=prefill_workspace(s,width);
    if(!work&&(s->model.cache_bytes||s->model.block_data)){discard_optional_cache(s);work=prefill_workspace(s,width);}
    if(work)break;
  }
  glv_free(row_reserve);
  if(!work)return ret?ret:-ENOMEM;
  /* Packed batch kernels can need about 280 KiB beyond this workspace.
   * Keep 1 MiB uncommitted during cache admission, also allowing ordinary
   * GUI/network allocations to grow without consuming the system reserve.
   * Decode can top up after this workspace is released.
   */
  void *cache_guard=s->model.data?NULL:glv_alloc(s->memory,1024u*1024u);
  if(cache_guard){prepare_cache(s,false);glv_free(cache_guard);}
  float *x=work,*xb=x+width*HIDDEN,*q=xb+width*HIDDEN,*context=q+width*HIDDEN;
  float *dense=context+width*HIDDEN,*key=dense+width*HIDDEN,*value=key+width*KV;
  float *gate=value+width*KV,*up=gate+width*FFN,*weight=up+width*FFN,*scores=weight+HIDDEN;
  float *coefficients=scores+7*GLV_MAX_CONTEXT;
  char name[128];unsigned last=0;
  for(unsigned at=0;at<count&&!ret;){
    unsigned batch=count-at;if(batch>width)batch=width;last=batch-1;
    for(unsigned b=0;b<batch&&!ret;b++)ret=glv_tensor_row(&s->model,glv_tensor(&s->model,"model.embed_tokens.weight"),ids[at+b],x+b*HIDDEN,HIDDEN);
    for(unsigned b=0;b<batch&&!ret;b++)rope_coefficients(coefficients+b*64,start+at+b);
    for(unsigned layer=0;layer<LAYERS&&!ret;layer++){
#define PATH(suffix) snprintf(name,sizeof(name),"model.layers.%u." suffix,layer)
#define BATCH(suffix,input,output,rows,cols) do {PATH(suffix);ret=llm_matbatch(s,name,input,output,rows,cols,batch);if(ret)goto done;} while(0)
      PATH("input_layernorm.weight");for(unsigned b=0;b<batch;b++)if((ret=rms(s,name,x+b*HIDDEN,xb+b*HIDDEN,weight)))goto done;
      BATCH("self_attn.q_proj.weight",xb,q,HIDDEN,HIDDEN);
      BATCH("self_attn.k_proj.weight",xb,key,KV,HIDDEN);
      BATCH("self_attn.v_proj.weight",xb,value,KV,HIDDEN);
      uint32_t attn_began=llm_ticks();
      size_t base=(size_t)layer*s->capacity*KV;
      for(unsigned b=0;b<batch;b++){
        unsigned position=start+at+b;rope(q+b*HIDDEN,7,coefficients+b*64);rope(key+b*KV,1,coefficients+b*64);
        memcpy(s->keys+base+position*KV,key+b*KV,KV*4u);memcpy(s->values+base+position*KV,value+b*KV,KV*4u);
        ret=attention(s,q+b*HIDDEN,s->keys+base,s->values+base,position+1,scores,context+b*HIDDEN);if(ret)goto done;
      }
      s->memory->profile.llm_attention_ticks+=(uint32_t)(llm_ticks()-attn_began);
      BATCH("self_attn.o_proj.weight",context,dense,HIDDEN,HIDDEN);
      for(unsigned i=0;i<batch*HIDDEN;i++)x[i]+=dense[i];
      PATH("post_attention_layernorm.weight");for(unsigned b=0;b<batch;b++)if((ret=rms(s,name,x+b*HIDDEN,xb+b*HIDDEN,weight)))goto done;
      BATCH("mlp.gate_proj.weight",xb,gate,FFN,HIDDEN);
      BATCH("mlp.up_proj.weight",xb,up,FFN,HIDDEN);
      uint32_t activation_began=llm_ticks();
      for(unsigned i=0;i<batch*FFN;i++)gate[i]=gate[i]/(1+expf(-gate[i]))*up[i];
      s->memory->profile.llm_activation_ticks+=(uint32_t)(llm_ticks()-activation_began);
      BATCH("mlp.down_proj.weight",gate,dense,HIDDEN,FFN);
      for(unsigned i=0;i<batch*HIDDEN;i++)x[i]+=dense[i];
#undef BATCH
#undef PATH
      if(!every)progress(s,GLV_LLM_PREFILL,at*LAYERS+(layer+1)*batch,count*LAYERS);
    }
    if(!ret&&logits&&every){
      for(unsigned b=0;b<batch&&!ret;b++)ret=rms(s,"model.norm.weight",x+b*HIDDEN,xb+b*HIDDEN,weight);
      const char *head=glv_tensor(&s->model,"lm_head.weight")?"lm_head.weight":"model.embed_tokens.weight";
      if(!ret)ret=llm_matbatch(s,head,xb,logits+(size_t)at*VOCAB,VOCAB,HIDDEN,batch);
    }
    if(!ret){memcpy(s->processed+start+at,ids+at,batch*sizeof(*ids));at+=batch;s->position=start+at;}
  }
  if(!ret&&logits&&!every){
    ret=rms(s,"model.norm.weight",x+last*HIDDEN,xb,weight);
    const char *head=glv_tensor(&s->model,"lm_head.weight")?"lm_head.weight":"model.embed_tokens.weight";
    if(!ret)ret=llm_matvec(s,head,xb,logits,VOCAB,HIDDEN);
  }
done:glv_free(work);return ret;
}
int glv_llm_prefill(struct glv_llm *s,const int *ids,unsigned count,float *logits)
{return prefill_impl(s,ids,count,logits,false);}
static int greedy(const float *logits)
{int next=0;for(unsigned i=1;i<VOCAB;i++)if(logits[i]>logits[next])next=(int)i;return next;}
static bool eos(int token){return token==4||token==0||token==2||token==3;}
#define SPEC_WIDTH 4u
static unsigned draft_block(struct glv_llm *s,const int *prompt,unsigned count,const int *generated,unsigned used,unsigned maximum,int *block,bool *previous)
{
 block[0]=generated[used-1];if(maximum<2)return 1;
 /* Longest suffix match only nominates candidates, never grants validity.
  * Both sources are inert token arrays: previous generated text or prompt.
  * No resource selection, permissions, tool actions or retries occur here. */
 for(unsigned n=used<8?used:8;n;n--){
  for(unsigned source=0;source<2;source++){
   const int *data=source?prompt:s->draft;unsigned length=source?count:s->draft_count;
   for(unsigned at=0;at+n<length;at++){
    if(memcmp(data+at,generated+used-n,n*sizeof(*data)))continue;
    unsigned k=1;while(k<SPEC_WIDTH&&k<maximum&&at+n+k-1<length&&!eos(data[at+n+k-1])){block[k]=data[at+n+k-1];k++;}
    if(k>1){*previous=!source;return k;}
   }
  }
 }
 return 1;
}
static int generate(struct glv_llm *s,const char *prompt,unsigned maximum,char *reply,size_t capacity,glv_text_sink sink,void *opaque,bool tool)
{
  if(!s||!maximum||maximum>96||capacity<1)return -EINVAL;
  int ids[GLV_MAX_CONTEXT],generated[96];reply[0]=0;
  progress(s,GLV_LLM_TOKENIZING,0,0);
  int count=glv_encode(&s->tokenizer,prompt,ids,GLV_MAX_CONTEXT-maximum);if(count<=0)return count?count:-EINVAL;
  unsigned reuse=0;
  if(s->memory->reuse_kv){
    while(reuse<s->position&&reuse+1<(unsigned)count&&s->processed[reuse]==ids[reuse])reuse++;
  }
  /* Compare actual encoded IDs, including boundary merges and sanitized
   * history. Recompute at least the last prompt position for clean logits;
   * last turn's logits may contain repetition penalties. */
  s->position=reuse;s->memory->profile.kv_reused_tokens=reuse;
  s->memory->profile.prefill_tokens=(unsigned)count-reuse;
  int ret=reserve_kv(s,(unsigned)count+maximum);
  if(ret==-ENOMEM){
    /* Reclaim old token state before growing a KV plane. This is before
     * any new prefill/output, so no tool or speech action is replayed. */
    glv_llm_trim_kv(s);reuse=0;s->memory->profile.kv_reused_tokens=0;
    s->memory->profile.prefill_tokens=(unsigned)count;s->memory->profile.llm_kv_retries++;
    ret=reserve_kv(s,(unsigned)count+maximum);
  }
  if(ret==-ENOMEM&&s->model.data){
    /* Resident weight bytes are optional; complete FP32 KV is mandatory.
     * Reopen the already lease-bound file through the authenticated row
     * reader, before prefill or publication. No device action is retried,
     * no input is shortened, and the W8A8 integer dot/scales stay identical. */
    glv_llm_trim_kv(s);glv_model_close(&s->model);
    ret=glv_model_open(&s->model,s->root,s->memory,false);
    if(!ret){
      s->model.bulk_q8=true;s->memory->profile.llm_resident_bytes=0;
      reuse=0;s->memory->profile.kv_reused_tokens=0;s->memory->profile.prefill_tokens=(unsigned)count;
      ret=reserve_kv(s,(unsigned)count+maximum);
      if(!ret)printf("LOCAL_LLM_STREAM_FALLBACK tokens=%u reason=memory_only\n",(unsigned)count+maximum);
    }
  }
  if(ret)return ret;
  if(s->model.cache_bytes>s->memory->profile.weight_cache_peak_bytes)
    s->memory->profile.weight_cache_peak_bytes=s->model.cache_bytes;
  if((ret=glv_llm_prefill(s,ids+reuse,(unsigned)count-reuse,s->logits)))return ret;
  progress(s,GLV_LLM_DECODING,0,maximum);
  unsigned used=0;float *verify=NULL;bool tried_verify=false;
  while(used<maximum){
    if((ret=glv_cancel(s->memory)))goto decode_done;
    /* Same prompt-plus-generation last-128 repetition window/penalty and no-repeat 4-gram
     * policy as the supplied Python chat_engine.py. */
    unsigned combined=(unsigned)count+used,begin=combined>128?combined-128:0;
    for(unsigned i=begin;!tool&&i<combined;i++){
      int token=i<(unsigned)count?ids[i]:generated[i-(unsigned)count];bool seen=false;
      for(unsigned j=begin;j<i;j++)if((j<(unsigned)count?ids[j]:generated[j-(unsigned)count])==token){seen=true;break;}
      if(!seen){float *v=s->logits+token;*v=*v>0?*v/1.15f:*v*1.15f;}
    }
    if(!tool&&used>=3)for(unsigned i=0;i+3<used;i++)
      if(generated[i]==generated[used-3]&&generated[i+1]==generated[used-2]&&generated[i+2]==generated[used-1])
        s->logits[generated[i+3]]=-INFINITY;
    int next=greedy(s->logits);
    if(eos(next))break;
    generated[used++]=next;ret=glv_decode(&s->tokenizer,generated,used,reply,capacity);if(ret<0)goto decode_done;
    if(sink&&(ret=sink(opaque,reply,used)))goto decode_done;
    progress(s,GLV_LLM_DECODING,used,maximum);
    if(used<maximum){
      int block[SPEC_WIDTH];bool previous=false;
      unsigned width=tool?draft_block(s,ids,(unsigned)count,generated,used,maximum-used,block,&previous):1;
      if(width>1&&!tried_verify){tried_verify=true;verify=glv_alloc(s->memory,SPEC_WIDTH*VOCAB*sizeof(float));}
      if(width>1&&verify){
        unsigned base=s->position;
        ret=prefill_impl(s,block,width,verify,true);
        if(ret==-ENOMEM){
          /* Numerical precompute only; nothing from this block published.
           * Drop speculative positions before the original single step. */
          s->position=base;s->memory->profile.llm_spec_fallbacks++;
          glv_free(verify);verify=NULL;
          ret=glv_llm_forward(s,next,s->logits);
          if(ret)goto decode_done;
        }else if(ret)goto decode_done;
        else{
          unsigned accepted=1;
          for(;accepted<width;accepted++)if(greedy(verify+(size_t)(accepted-1)*VOCAB)!=block[accepted])break;
          s->position=base+accepted;memcpy(s->logits,verify+(size_t)(accepted-1)*VOCAB,VOCAB*sizeof(float));
          s->memory->profile.llm_spec_blocks++;s->memory->profile.llm_spec_drafted+=width-1;s->memory->profile.llm_spec_accepted+=accepted-1;
          if(previous)s->memory->profile.llm_spec_previous++;else s->memory->profile.llm_spec_prompt++;
          for(unsigned i=1;i<accepted;i++){
            if((ret=glv_cancel(s->memory)))goto decode_done;
            generated[used++]=block[i];ret=glv_decode(&s->tokenizer,generated,used,reply,capacity);if(ret<0)goto decode_done;
            if(sink&&(ret=sink(opaque,reply,used)))goto decode_done;
            progress(s,GLV_LLM_DECODING,used,maximum);
          }
        }
      }else if((ret=glv_llm_forward(s,next,s->logits)))goto decode_done;
    }
  }
  ret=glv_decode(&s->tokenizer,generated,used,reply,capacity);
decode_done:
  glv_free(verify);
  if(ret>=0&&tool){memcpy(s->draft,generated,used*sizeof(*generated));s->draft_count=used;}
  else if(ret<0)s->draft_count=0;
  return ret;
}
int glv_llm_generate(struct glv_llm *s,const char *prompt,unsigned maximum,char *reply,size_t capacity,glv_text_sink sink,void *opaque)
{int ret=generate(s,prompt,maximum,reply,capacity,sink,opaque,false);if(s)glv_model_block_release(&s->model);return ret;}
int glv_llm_generate_tool(struct glv_llm *s,const char *prompt,unsigned maximum,char *reply,size_t capacity,glv_text_sink sink,void *opaque)
{int ret=generate(s,prompt,maximum,reply,capacity,sink,opaque,true);if(s)glv_model_block_release(&s->model);return ret;}
