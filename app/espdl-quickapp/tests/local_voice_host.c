/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_llm.h"
#include "glass_local_tts.h"
#include "glass_local_frontend.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static int audio_sink(void *opaque,const float *audio,unsigned n,unsigned rate)
{assert(rate==24000);return fwrite(audio,sizeof(float),n,opaque)==n?0:-EIO;}
int main(int argc,char **argv)
{
  assert(argc>=3);struct glv_memory memory={.limit=GLV_MEMORY_LIMIT};int ret=0;
  assert(glv_finite(0)&&glv_finite(1)&&!glv_finite(INFINITY)&&!glv_finite(-INFINITY)&&!glv_finite(NAN));
  const char *limit=getenv("GLV_TEST_MEMORY_LIMIT");if(limit)memory.limit=strtoul(limit,NULL,10);
  memory.tts_parallel_input=getenv("GLV_TEST_TTS_INPUT")!=NULL;
  memory.tts_parallel_pie=getenv("GLV_TEST_TTS_PIE_PAR")!=NULL;
  memory.tts_weight_lut=getenv("GLV_TEST_TTS_WEIGHT_LUT")!=NULL;
  memory.tts_coarse_profile=getenv("GLV_TEST_TTS_COARSE")!=NULL;
  memory.tts_window_plan=getenv("GLV_TEST_TTS_WINDOWS")!=NULL;
  memory.tts_bulk_rows=getenv("GLV_TEST_TTS_BULK")!=NULL;
  memory.tts_fma=getenv("GLV_TEST_TTS_FMA")!=NULL;
  memory.tts_single_dot=getenv("GLV_TEST_TTS_SINGLE")!=NULL;memory.tts_serial_fp=getenv("GLV_TEST_TTS_SERIAL")!=NULL;
  if(!strcmp(argv[1],"tokenize")){
    struct glv_tokenizer tokenizer;ret=glv_tokenizer_open(&tokenizer,argv[2],&memory);if(ret)goto done;
    for(int i=3;i<argc;i++){int ids[512];int n=glv_encode(&tokenizer,argv[i],ids,512);
      printf("%d:",n);for(int j=0;j<n;j++)printf(" %d",ids[j]);puts("");}
    glv_tokenizer_close(&tokenizer);
  }else if(!strcmp(argv[1],"frontend")){
    for(int i=3;i<argc;i++){
      struct glv_phone_plan *plan=NULL;ret=glv_frontend_plan(argv[2],argv[i],&memory,&plan);
      if(ret)goto done;assert(plan->count==1);
      struct glv_phone_part *part=&plan->parts[0];printf("%u:",part->count);
      for(unsigned j=0;j<part->count;j++)printf(" %d",part->ids[j]);puts("");glv_free(plan);
    }
  }else if(!strcmp(argv[1],"synthesize")){
    assert(argc==5);struct glv_phone_plan *plan=NULL;struct glv_tts *tts=NULL;
    ret=glv_frontend_plan(argv[2],argv[3],&memory,&plan);if(ret)goto done;
    char path[512];snprintf(path,sizeof(path),"%s/tts-v2",argv[2]);ret=glv_tts_open(path,&memory,&tts);
    FILE *f=fopen(argv[4],"wb");assert(f);
    for(unsigned i=0;i<plan->count&&!ret;i++){
      if(i){float zero[240]={0};for(unsigned n=0;n<3456&&!ret;n+=240)ret=audio_sink(f,zero,3456-n>240?240:3456-n,24000);}
      if(!ret)ret=glv_tts_phones(tts,plan->parts[i].ids,plan->parts[i].kind,plan->parts[i].count,i==0,audio_sink,f);
    }
    fclose(f);glv_tts_close(tts);glv_free(plan);
  }else if(!strcmp(argv[1],"logits")||!strcmp(argv[1],"prefill")||!strcmp(argv[1],"generate")){
    assert(argc>=6);struct glv_llm *llm=NULL;ret=glv_llm_open(argv[2],&memory,&llm);if(ret)goto done;
    FILE *f=fopen(argv[4],"wb");assert(f);
    if(!strcmp(argv[1],"logits")||!strcmp(argv[1],"prefill")){
      int ids[512];int n=glv_llm_encode(llm,argv[3],ids,512);assert(n>0);
      float *logits=glv_alloc(&memory,8192*4u);assert(logits);
      if(!strcmp(argv[1],"prefill")){
        unsigned prefix=(unsigned)atoi(argv[5]);if(prefix){assert(prefix<=(unsigned)n);n=(int)prefix;}
        ret=glv_llm_prefill(llm,ids,(unsigned)n,logits);
        if(!ret)assert(fwrite(logits,4,8192,f)==8192);
      }else for(int i=0;i<n&&!ret;i++){
        ret=glv_llm_forward(llm,ids[i],logits);
        if(!ret)assert(fwrite(logits,4,8192,f)==8192);
      }
      glv_free(logits);
    }else{
      char reply[4096];ret=glv_llm_generate(llm,argv[3],(unsigned)atoi(argv[5]),reply,sizeof(reply),NULL,NULL);
      if(ret>=0){assert(fwrite(reply,1,ret,f)==(size_t)ret);ret=0;}
    }
    fclose(f);glv_llm_close(llm);
  }else if(!strcmp(argv[1],"mel")){
    assert(argc==5);struct glv_tts *tts=NULL;ret=glv_tts_open(argv[2],&memory,&tts);if(ret)goto done;
    FILE *f=fopen(argv[3],"rb");assert(f);uint32_t pairs[512];size_t bytes=fread(pairs,1,sizeof(pairs),f);fclose(f);
    assert(bytes&&bytes%8==0);unsigned count=bytes/8;int ids[256];unsigned char kind[256];
    for(unsigned i=0;i<count;i++){ids[i]=pairs[i*2];kind[i]=pairs[i*2+1];}
    float *mel=NULL;unsigned frames=0;ret=glv_tts_mel(tts,ids,kind,count,&mel,&frames);
    if(!ret){f=fopen(argv[4],"wb");assert(f);assert(fwrite(&frames,4,1,f)==1);assert(fwrite(mel,4,(size_t)frames*80,f)==(size_t)frames*80);fclose(f);}
    glv_free(mel);glv_tts_close(tts);
  }else if(!strcmp(argv[1],"vocode")){
    assert(argc==5);struct glv_tts *tts=NULL;ret=glv_tts_open(argv[2],&memory,&tts);if(ret)goto done;
    FILE *f=fopen(argv[3],"rb");assert(f);uint32_t frames=0;assert(fread(&frames,4,1,f)==1&&frames&&frames<=2048);
    float *mel=glv_alloc(&memory,(size_t)frames*80*4);assert(mel);assert(fread(mel,4,(size_t)frames*80,f)==(size_t)frames*80);fclose(f);
    f=fopen(argv[4],"wb");assert(f);ret=glv_tts_vocode(tts,mel,frames,audio_sink,f);fclose(f);
    glv_free(mel);glv_tts_close(tts);
  }else {ret=-EINVAL;}
done:
  fprintf(stderr,"LOCAL_VOICE_HOST result=%d peak=%zu remaining=%zu tts_chunks=%u context_frames=%u output_frames=%u vocoder_ms=%llu mel_ms=%llu memory_fallbacks=%u fma_calls=%u fma_rejected=%u\n",
    ret,memory.peak,memory.used,memory.profile.tts_chunks,memory.profile.tts_context_frames,memory.profile.tts_output_frames,
    (unsigned long long)(memory.profile.tts_vocoder_us/1000),(unsigned long long)(memory.profile.tts_mel_us/1000),memory.profile.tts_memory_fallbacks,memory.profile.tts_fma_upsample_calls,memory.profile.tts_fma_rejections);
  fprintf(stderr,"WINDOW_PROFILE planned=%u checks=%u skips=%u attempts=%u failed_frames=%u failed_us=%llu\n",memory.tts_window_plan,memory.profile.tts_window_checks,memory.profile.tts_window_skips,memory.profile.tts_window_attempts,memory.profile.tts_window_failed_frames,(unsigned long long)memory.profile.tts_window_failed_us);
  fprintf(stderr,"INPUT_PROFILE enabled=%u max_jobs=%u quant_jobs=%u rounding_fallbacks=%u\n",memory.tts_parallel_input,memory.profile.tts_input_max_jobs,memory.profile.tts_input_quant_jobs,memory.profile.tts_input_rounding_fallbacks);
  assert(memory.used==0);return ret?1:0;
}
