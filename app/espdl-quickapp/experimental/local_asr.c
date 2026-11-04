/* SPDX-License-Identifier: Apache-2.0 */
#include "local_asr.h"
#ifdef GLV_ASR_ACCEL_ENABLED
#include "local_asr_parallel.h"
#endif
#include <errno.h>
#include <string.h>
#define STATES 36u
#define VOCAB 5537u
#define TEXT 4096u
struct glv_asr {
 struct glv_memory *memory;
 struct glv_asr_graph *encoder,*decoder,*joiner;
 struct glv_asr_fbank *fbank;
 struct glv_asr_io input[STATES],output[STATES];
 float decoded[320],logits[VOCAB];int64_t context[2];
 unsigned char *symbols;size_t symbol_bytes;
 uint32_t offsets[VOCAB+1];
 char text[TEXT];size_t used;unsigned filled;bool busy;
 struct glv_asr_stats stats;
 int (*token)(void *,unsigned,unsigned);void *opaque;
 bool streaming;size_t stream_samples;
 int (*partial)(void *,const char *,unsigned,unsigned);void *partial_opaque;
};
void glv_asr_close(struct glv_asr *a)
{
 if(!a)return;
#ifdef GLV_ASR_ACCEL_ENABLED
 glv_asr_parallel_close(a->memory);
#endif
 glv_asr_graph_close(a->encoder);glv_asr_graph_close(a->decoder);glv_asr_graph_close(a->joiner);glv_asr_fbank_destroy(a->fbank);
 for(unsigned i=0;i<STATES;i++){glv_free(a->input[i].data);glv_free(a->output[i].data);}
 glv_free(a->symbols);glv_free(a);
}
void glv_asr_io_statistics(const struct glv_asr *a,struct glv_asr_io_stats *stats)
{
 if(!stats)return;memset(stats,0,sizeof(*stats));if(!a)return;
 struct glv_asr_graph *graphs[]={a->encoder,a->decoder,a->joiner};
 for(unsigned i=0;i<3;i++){struct glv_asr_io_stats one;glv_asr_graph_io_statistics(graphs[i],&one);
   stats->bytes+=one.bytes;stats->reads+=one.reads;stats->seeks+=one.seeks;}
}
size_t glv_asr_cached_bytes(const struct glv_asr *a)
{return a?glv_asr_graph_cached_bytes(a->encoder)+glv_asr_graph_cached_bytes(a->decoder)+glv_asr_graph_cached_bytes(a->joiner):0;}
int glv_asr_make_resident(struct glv_asr *a)
{
 if(!a||a->busy||a->streaming)return -EINVAL;
 struct glv_asr_graph *graphs[]={a->encoder,a->decoder,a->joiner};
 for(unsigned i=0;i<3;i++){int ret=glv_asr_graph_make_resident(graphs[i]);if(ret)return ret;}
 return 0;
}
int glv_asr_prepare_partial(struct glv_asr *a)
{
 if(!a||a->busy||a->streaming)return -EINVAL;
 /* Mandatory complete residency: all three verified raw Q4/Q8 files are
  * cached and their SD streams closed before any frame is evaluated. */
 int ret=glv_asr_make_resident(a);
 printf("ASR_FULL_RAM budget=%lu used=%lu cached=%lu graphs=3 ready=%u\n",(unsigned long)a->memory->limit,(unsigned long)a->memory->used,(unsigned long)glv_asr_cached_bytes(a),!ret);
 return ret;
}
static int token_load(struct glv_asr *a,const char *root)
{
 char path[320];if(snprintf(path,sizeof(path),"%s/tokens.bin",root)>=(int)sizeof(path))return -ENAMETOOLONG;
 FILE *f=NULL;size_t size=0;int ret=glv_open_asset(path,&f,&size);if(ret)return ret;
 unsigned char h[48];ret=-EBADMSG;if(size<48||(ret=glv_read_exact(f,h,sizeof(h))))goto done;
 uint32_t bytes=glv_u32(h+12);
 if(memcmp(h,"GLT1",4)||glv_u32(h+4)!=1||glv_u32(h+8)!=VOCAB||bytes<(VOCAB+1)*4||bytes>1024u*1024u||size!=48u+bytes){ret=-EBADMSG;goto done;}
 unsigned char digest[32];ret=glv_sha_file(f,bytes,digest,a->memory);if(ret)goto done;
 if(memcmp(digest,h+16,32)||fseek(f,48,SEEK_SET)){ret=-EBADMSG;goto done;}
 a->symbol_bytes=bytes-(VOCAB+1)*4;
 for(unsigned i=0;i<=VOCAB;i++){
   unsigned char b[4];if((ret=glv_read_exact(f,b,4)))goto done;a->offsets[i]=glv_u32(b);
   if((i&&a->offsets[i]<=a->offsets[i-1])||a->offsets[i]>a->symbol_bytes||(!i&&a->offsets[i])){ret=-EBADMSG;goto done;}
 }
 if(a->offsets[VOCAB]!=a->symbol_bytes){ret=-EBADMSG;goto done;}
 a->symbols=glv_alloc(a->memory,a->symbol_bytes);if(!a->symbols){ret=-ENOMEM;goto done;}
 if((ret=glv_read_exact(f,a->symbols,a->symbol_bytes)))goto done;
 for(unsigned i=0;i<VOCAB;i++){
   const unsigned char *p=a->symbols+a->offsets[i],*end=a->symbols+a->offsets[i+1];
   while(p<end){uint32_t cp;if(glv_utf8_read(&p,end,&cp)||!cp||cp==0x7f||cp<0x20){ret=-EBADMSG;goto done;}}
 }
done:fclose(f);return ret;
}
static int check_io(struct glv_asr_graph *g,bool out,unsigned index,unsigned type,size_t count)
{
 struct glv_asr_shape s;unsigned t;size_t bytes;int ret=glv_asr_graph_io_info(g,out,index,&s,&t,&bytes);
 if(ret)return ret;return t==type&&bytes==count*(type==7?8u:4u)?0:-EBADMSG;
}
int glv_asr_open(struct glv_asr **out,const char *root,struct glv_memory *memory)
{
 if(!out||!root||!memory)return -EINVAL;*out=NULL;int ret=glv_cancel(memory);if(ret)return ret;
 struct glv_asr *a=glv_alloc(memory,sizeof(*a));if(!a)return -ENOMEM;a->memory=memory;
 char path[320];const char *stems[]={"encoder-epoch-99-avg-1","decoder-epoch-99-avg-1","joiner-epoch-99-avg-1"};
 struct glv_asr_graph **graphs[]={&a->encoder,&a->decoder,&a->joiner};
 for(unsigned i=0;i<3;i++){
   if(snprintf(path,sizeof(path),"%s/%s",root,stems[i])>=(int)sizeof(path)){ret=-ENAMETOOLONG;goto done;}
   if((ret=glv_asr_graph_open(graphs[i],path,memory)))goto done;
 }
 if(glv_asr_graph_inputs(a->encoder)!=STATES||glv_asr_graph_outputs(a->encoder)!=STATES||
    glv_asr_graph_inputs(a->decoder)!=1||glv_asr_graph_outputs(a->decoder)!=1||
    glv_asr_graph_inputs(a->joiner)!=2||glv_asr_graph_outputs(a->joiner)!=1){ret=-EBADMSG;goto done;}
 if((ret=check_io(a->encoder,false,0,1,39u*80))||(ret=check_io(a->encoder,true,0,1,8u*320))||
    (ret=check_io(a->decoder,false,0,7,2))||(ret=check_io(a->decoder,true,0,1,320))||
    (ret=check_io(a->joiner,false,0,1,320))||(ret=check_io(a->joiner,false,1,1,320))||
    (ret=check_io(a->joiner,true,0,1,VOCAB)))goto done;
 /* The joiner reuses a ~1.92 MiB packed matrix for each output frame. Cache
  * only this graph when the current budget leaves >=5 MiB before its
  * allocation; all encoder weights remain streamed. The same glv_memory
  * allocator continues to enforce the real P4 system reserve at every step. */
 if(memory->used<=memory->limit&&memory->limit-memory->used>=5u*1024u*1024u)
   if((ret=glv_asr_graph_cache(a->joiner,2u*1024u*1024u)))goto done;
 for(unsigned i=0;i<STATES;i++){
   struct glv_asr_shape si,so;unsigned ti,to;
   if((ret=glv_asr_graph_io_info(a->encoder,false,i,&si,&ti,&a->input[i].bytes))||
      (ret=glv_asr_graph_io_info(a->encoder,true,i,&so,&to,&a->output[i].bytes)))goto done;
   if(i&&(ti!=to||si.rank!=so.rank||memcmp(si.dims,so.dims,si.rank*4))){ret=-EBADMSG;goto done;}
   a->input[i].data=glv_alloc(memory,a->input[i].bytes);
   if(!a->input[i].data){ret=-ENOMEM;goto done;}
 }
 if((ret=token_load(a,root))||(ret=glv_asr_fbank_create(&a->fbank,memory)))goto done;
done:if(ret){glv_asr_close(a);return ret;}*out=a;return 0;
}
static int decoder(struct glv_asr *a)
{
 struct glv_asr_io input={a->context,sizeof(a->context)},output={a->decoded,sizeof(a->decoded)};
 uint64_t began=glv_now_us();int ret=glv_asr_graph_run(a->decoder,&input,1,&output,1);a->memory->profile.asr_decoder_us+=glv_now_us()-began;return ret;
}
static int chunk(struct glv_asr *a)
{
 glv_free(a->output[0].data);a->output[0].data=NULL;
 uint64_t began=glv_now_us();int ret=glv_asr_graph_exchange(a->encoder,a->input,STATES,a->output,STATES);a->memory->profile.asr_encoder_us+=glv_now_us()-began;if(ret)return ret;
 for(unsigned i=1;i<STATES;i++){a->input[i].data=a->output[i].data;a->output[i].data=NULL;}
 for(unsigned frame=0;frame<8;frame++){
   struct glv_asr_io input[2]={{(float *)a->output[0].data+frame*320,320u*4},{a->decoded,sizeof(a->decoded)}};
   struct glv_asr_io output={a->logits,sizeof(a->logits)};
   began=glv_now_us();ret=glv_asr_graph_run(a->joiner,input,2,&output,1);a->memory->profile.asr_joiner_us+=glv_now_us()-began;if(ret)return ret;
   unsigned best=0;for(unsigned i=0;i<VOCAB;i++){
     if(!glv_finite(a->logits[i]))return -EBADMSG;
     if(a->logits[i]>a->logits[best])best=i;
   }
   /* Match sherpa's max-one-symbol-per-frame greedy search: blank=0,
    * unknown=2, initial context [-1,0], no beam or hidden language model. */
   if(best!=0&&best!=2){
     size_t size=a->offsets[best+1]-a->offsets[best];
     if(a->stats.tokens>=1024||size>=TEXT-a->used)return -E2BIG;
     memcpy(a->text+a->used,a->symbols+a->offsets[best],size);a->used+=size;a->text[a->used]=0;
     unsigned timestamp=a->stats.chunks*8+frame;
     if(a->token&&(ret=a->token(a->opaque,best,timestamp)))return ret;
     a->stats.tokens++;
     if(a->partial&&(ret=a->partial(a->partial_opaque,a->text,a->stats.tokens,timestamp+1)))return ret;
     a->context[0]=a->context[1];a->context[1]=best;
     if((ret=decoder(a)))return ret;
   }
 }
 a->stats.chunks++;
 if(a->partial&&(ret=a->partial(a->partial_opaque,a->text,a->stats.tokens,a->stats.chunks*8)))return ret;
 return glv_cancel(a->memory);
}
static int feature(void *opaque,const float *data,unsigned count)
{
 struct glv_asr *a=opaque;if(count!=80)return -EINVAL;
 if(a->filled==39){
   int ret=chunk(a);if(ret)return ret;
   memmove(a->input[0].data,(float *)a->input[0].data+32u*80,7u*80*4);a->filled=7;
 }
 memcpy((float *)a->input[0].data+a->filled*80,data,80u*4);a->filled++;a->stats.frames++;return 0;
}
int glv_asr_transcribe(struct glv_asr *a,const int16_t *pcm,size_t samples,char *text,size_t capacity,
                       int (*token)(void *,unsigned,unsigned),void *opaque,struct glv_asr_stats *stats)
{
 if(!a||(!pcm&&samples)||!text||!capacity||samples>16000u*60)return -EINVAL;
 if(a->busy)return -EBUSY;a->busy=true;text[0]=0;memset(&a->stats,0,sizeof(a->stats));
 a->streaming=false;a->partial=NULL;a->partial_opaque=NULL;
 a->used=a->filled=0;a->text[0]=0;a->token=token;a->opaque=opaque;a->context[0]=-1;a->context[1]=0;
 for(unsigned i=0;i<STATES;i++){
  glv_free(a->output[i].data);a->output[i].data=NULL;
  if(!a->input[i].data)a->input[i].data=glv_alloc(a->memory,a->input[i].bytes);
  if(!a->input[i].data){glv_asr_stream_abort(a);return -ENOMEM;}
  memset(a->input[i].data,0,a->input[i].bytes);
 }
 int ret=glv_cancel(a->memory);if(!ret)ret=decoder(a);
 if(!ret)ret=glv_asr_fbank_pcm(a->fbank,pcm,samples,12800,feature,a);
 if(!ret)ret=glv_cancel(a->memory);
 if(!ret){if(a->used>=capacity)ret=-ENOSPC;else memcpy(text,a->text,a->used+1);}
 if(stats)*stats=a->stats;a->token=NULL;a->opaque=NULL;a->busy=false;return ret;
}

void glv_asr_stream_abort(struct glv_asr *a)
{if(!a)return;a->token=NULL;a->opaque=NULL;a->partial=NULL;a->partial_opaque=NULL;a->streaming=false;a->busy=false;}
int glv_asr_stream_begin(struct glv_asr *a,int (*partial)(void *,const char *,unsigned,unsigned),void *opaque)
{
 if(!a||!partial)return -EINVAL;if(a->busy)return -EBUSY;
 a->busy=a->streaming=true;a->stream_samples=0;a->used=a->filled=0;a->text[0]=0;
 memset(&a->stats,0,sizeof(a->stats));a->token=NULL;a->opaque=NULL;a->partial=partial;a->partial_opaque=opaque;a->context[0]=-1;a->context[1]=0;
 for(unsigned i=0;i<STATES;i++){
  glv_free(a->output[i].data);a->output[i].data=NULL;
  if(!a->input[i].data)a->input[i].data=glv_alloc(a->memory,a->input[i].bytes);
  if(!a->input[i].data){glv_asr_stream_abort(a);return -ENOMEM;}
  memset(a->input[i].data,0,a->input[i].bytes);
 }
 int ret=glv_cancel(a->memory);if(!ret)ret=decoder(a);if(!ret)ret=glv_asr_fbank_stream_begin(a->fbank);
 if(ret)glv_asr_stream_abort(a);return ret;
}
int glv_asr_stream_accept(struct glv_asr *a,const int16_t *pcm,size_t count)
{
 if(!a||!a->streaming||(!pcm&&count)||count>16000u*600u-a->stream_samples)return -EINVAL;
 a->stream_samples+=count;return glv_asr_fbank_stream_accept(a->fbank,pcm,count,feature,a);
}
int glv_asr_stream_finish(struct glv_asr *a,char *text,size_t capacity,struct glv_asr_stats *stats)
{
 if(!a||!a->streaming||!text||!capacity)return -EINVAL;text[0]=0;
 int ret=glv_cancel(a->memory);if(!ret)ret=glv_asr_fbank_stream_finish(a->fbank,12800,feature,a);
 if(!ret){if(a->used>=capacity)ret=-ENOSPC;else memcpy(text,a->text,a->used+1);}
 if(stats)*stats=a->stats;glv_asr_stream_abort(a);return ret;
}
