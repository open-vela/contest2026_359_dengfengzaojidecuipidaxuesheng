/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_asr.h"
#include "glass_local_assets.h"
#include "local_asr.h"
#include <errno.h>
#include <string.h>
static unsigned u16(const unsigned char *b){return b[0]|(unsigned)b[1]<<8;}
static int wave(struct glv_memory *m,unsigned clip,int16_t **pcm,size_t *samples)
{
 *pcm=NULL;*samples=0;char name[32],path[320];snprintf(name,sizeof(name),"checks/%u.wav",clip);
 int ret=glv_asset_check(GLV_ASR_RELEASE_ROOT,name,m);if(ret)return ret;
 snprintf(path,sizeof(path),"%s/%s",GLV_ASR_RELEASE_ROOT,name);FILE *f=NULL;size_t size;
 if((ret=glv_open_asset(path,&f,&size)))return ret;
 unsigned char h[16];ret=-EBADMSG;
 if(size<44||size>16000u*60u*2u+4096||glv_read_exact(f,h,12)||memcmp(h,"RIFF",4)||memcmp(h+8,"WAVE",4)||glv_u32(h+4)!=size-8)goto done;
 size_t at=12,data_offset=0,data_bytes=0;bool format=false;
 while(at<size){
   if((ret=glv_cancel(m)))goto done;ret=-EBADMSG;
   if(size-at<8||glv_read_exact(f,h,8))goto done;
   uint32_t bytes=glv_u32(h+4);at+=8;
   if(bytes>size-at||((bytes&1u)&&bytes==size-at))goto done;
   if(!memcmp(h,"fmt ",4)){
     if(format||bytes<16||glv_read_exact(f,h,16)||u16(h)!=1||u16(h+2)!=1||
        glv_u32(h+4)!=16000||glv_u32(h+8)!=32000||u16(h+12)!=2||u16(h+14)!=16)goto done;
     format=true;
   }else if(!memcmp(h,"data",4)){
     if(data_offset||!bytes||(bytes&1u)||bytes>16000u*60u*2u)goto done;
     data_offset=at;data_bytes=bytes;
   }
   at+=bytes+(bytes&1u);if(fseek(f,(long)at,SEEK_SET)){ret=-EIO;goto done;}
 }
 if(!format||!data_offset||at!=size)goto done;
 *pcm=glv_alloc(m,data_bytes);if(!*pcm){ret=-ENOMEM;goto done;}
 if(fseek(f,(long)data_offset,SEEK_SET)){ret=-EIO;goto done;}
 for(size_t offset=0;offset<data_bytes;){
   if((ret=glv_cancel(m)))goto done;size_t n=data_bytes-offset;if(n>16384)n=16384;
   if((ret=glv_read_exact(f,(char *)*pcm+offset,n)))goto done;offset+=n;
 }
 *samples=data_bytes/2;ret=0;
done:fclose(f);if(ret){glv_free(*pcm);*pcm=NULL;*samples=0;}return ret;
}
int glv_asr_assets_check(struct glv_memory *memory)
{
 if(!memory)return -EINVAL;
 static const char *assets[]={"encoder-epoch-99-avg-1.plan","encoder-epoch-99-avg-1.data",
   "decoder-epoch-99-avg-1.plan","decoder-epoch-99-avg-1.data","joiner-epoch-99-avg-1.plan",
   "joiner-epoch-99-avg-1.data","tokens.bin"};
 for(unsigned i=0;i<7;i++){int ret=glv_asset_check(GLV_ASR_RELEASE_ROOT,assets[i],memory);if(ret)return ret;}
 return 0;
}
#ifdef GLV_ASR_STREAMING
struct stream_probe {int (*token)(void *,unsigned,unsigned);void *opaque;unsigned tokens;uint64_t began,first;};
static int probe_partial(void *opaque,const char *text,unsigned tokens,unsigned frames)
{
 struct stream_probe *p=opaque;(void)text;
 if(tokens>p->tokens){if(!p->first)p->first=glv_now_us()-p->began;
  p->tokens=tokens;if(p->token)return p->token(p->opaque,tokens,frames);}
 return 0;
}
#endif
static int transcribe(const int16_t *pcm,size_t samples,struct glv_memory *memory,char *text,size_t cap,
                       int (*token)(void *,unsigned,unsigned),void *opaque,void (*progress)(void *,unsigned),unsigned clip)
{
 int ret=0;
 struct glv_asr *asr=NULL;
 if(progress)progress(opaque,1);uint64_t load_start=glv_now_us();ret=glv_asr_open(&asr,GLV_ASR_RELEASE_ROOT,memory);
#ifdef GLV_ASR_STREAMING
 if(!ret)ret=glv_asr_prepare_partial(asr);
 uint64_t load_us=glv_now_us()-load_start,compute_us=0,tail_us=0;
 struct stream_probe probe={.token=token,.opaque=opaque};
 struct glv_asr_io_stats before={0},after={0};
 if(!ret){
  if(progress)progress(opaque,2);glv_asr_io_statistics(asr,&before);probe.began=glv_now_us();
  ret=glv_asr_stream_begin(asr,probe_partial,&probe);
  for(size_t at=0;at<samples&&!ret;){size_t n=samples-at;if(n>1600)n=1600;ret=glv_asr_stream_accept(asr,pcm+at,n);at+=n;}
  uint64_t tail_start=glv_now_us();
  if(!ret)ret=glv_asr_stream_finish(asr,text,cap,NULL);
  tail_us=glv_now_us()-tail_start;compute_us=glv_now_us()-probe.began;glv_asr_io_statistics(asr,&after);
 }
 printf("LOCAL_ASR_STREAM_BENCH sample=%u samples=%lu load_us=%llu compute_us=%llu tail_us=%llu first_us=%llu resident=%lu inference_reads=%llu peak=%lu budget=%lu result=%d\n",
 clip,(unsigned long)samples,(unsigned long long)load_us,(unsigned long long)compute_us,(unsigned long long)tail_us,(unsigned long long)probe.first,
 (unsigned long)glv_asr_cached_bytes(asr),(unsigned long long)(after.reads-before.reads),(unsigned long)memory->peak,(unsigned long)memory->limit,ret);
#else
 if(!ret){if(progress)progress(opaque,2);ret=glv_asr_transcribe(asr,pcm,samples,text,cap,token,opaque,NULL);}
#endif
 printf("LOCAL_ASR_ACCEL pie_dots=%llu pie_fallbacks=%llu selftests=%llu selftest_failures=%llu parallel_jobs=%llu workers=%llu parallel_fallbacks=%llu cpu0=%llu cpu1=%llu overlap_us=%llu block_loads=%llu block_hits=%llu block_bytes=%llu block_peak=%llu\n",
 (unsigned long long)memory->profile.asr_pie_dots,(unsigned long long)memory->profile.asr_pie_fallbacks,(unsigned long long)memory->profile.asr_pie_selftests,(unsigned long long)memory->profile.asr_pie_selftest_failures,
 (unsigned long long)memory->profile.asr_parallel_jobs,(unsigned long long)memory->profile.asr_parallel_workers,(unsigned long long)memory->profile.asr_parallel_fallbacks,(unsigned long long)memory->profile.asr_parallel_cpu0,(unsigned long long)memory->profile.asr_parallel_cpu1,(unsigned long long)memory->profile.asr_parallel_overlap_us,
 (unsigned long long)memory->profile.asr_block_loads,(unsigned long long)memory->profile.asr_block_hits,(unsigned long long)memory->profile.asr_block_bytes,(unsigned long long)memory->profile.asr_block_peak_bytes);
 printf("ASR_HOT peak=%llu hits=%llu evictions=%llu\n",(unsigned long long)memory->profile.asr_hot_peak_bytes,(unsigned long long)memory->profile.asr_hot_hits,(unsigned long long)memory->profile.asr_hot_evictions);
 printf("ASR_DSP calls=%llu elements=%llu selftests=%llu fallbacks=%llu\n",(unsigned long long)memory->profile.asr_dsp_calls,(unsigned long long)memory->profile.asr_dsp_elements,(unsigned long long)memory->profile.asr_dsp_selftests,(unsigned long long)memory->profile.asr_dsp_fallbacks);
 printf("ASR_TIMING io=%llu rows_including_io=%llu quant=%llu lane_math=%llu owner_wait=%llu encoder=%llu decoder=%llu joiner=%llu frontend=%llu\n",
 (unsigned long long)memory->profile.asr_io_us,(unsigned long long)memory->profile.asr_rows_us,(unsigned long long)memory->profile.asr_quant_us,(unsigned long long)memory->profile.asr_math_us,(unsigned long long)memory->profile.asr_wait_us,
 (unsigned long long)memory->profile.asr_encoder_us,(unsigned long long)memory->profile.asr_decoder_us,(unsigned long long)memory->profile.asr_joiner_us,(unsigned long long)memory->profile.asr_front_us);
 for(unsigned op=1;op<28;op++)if(memory->profile.asr_op_us[op])printf("ASR_OP op=%u us=%llu\n",op,(unsigned long long)memory->profile.asr_op_us[op]);
 if(asr){struct glv_asr_io_stats io;glv_asr_io_statistics(asr,&io);
   printf("LOCAL_ASR_IO sample=%u bytes=%llu reads=%llu seeks=%llu packed_cache=%lu result=%d\n",clip,
     (unsigned long long)io.bytes,(unsigned long long)io.reads,(unsigned long long)io.seeks,(unsigned long)glv_asr_cached_bytes(asr),ret);}
 glv_asr_close(asr);if(ret)text[0]=0;return ret;
}
int glv_asr_transcribe_pcm(const int16_t *pcm,size_t samples,struct glv_memory *memory,char *text,size_t cap,
                            int (*token)(void *,unsigned,unsigned),void *opaque,void (*progress)(void *,unsigned))
{
 if(!memory||!pcm||!samples||!text||!cap)return -EINVAL;text[0]=0;
 int ret=glv_asr_assets_check(memory);if(ret)return ret;
 return transcribe(pcm,samples,memory,text,cap,token,opaque,progress,2);
}
int glv_asr_test_file(unsigned clip,struct glv_memory *memory,char *text,size_t cap,
                      int (*token)(void *,unsigned,unsigned),void *opaque,void (*progress)(void *,unsigned))
{
 if(clip>1||!memory||!text||!cap)return -EINVAL;text[0]=0;
 int ret=glv_asr_assets_check(memory);if(ret)return ret;
 int16_t *pcm=NULL;size_t samples=0;ret=wave(memory,clip,&pcm,&samples);if(ret)return ret;
 ret=transcribe(pcm,samples,memory,text,cap,token,opaque,progress,clip);glv_free(pcm);return ret;
}
