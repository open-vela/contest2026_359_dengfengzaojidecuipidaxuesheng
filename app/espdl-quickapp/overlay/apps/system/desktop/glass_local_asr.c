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
static int transcribe(const int16_t *pcm,size_t samples,struct glv_memory *memory,char *text,size_t cap,
                       int (*token)(void *,unsigned,unsigned),void *opaque,void (*progress)(void *,unsigned),unsigned clip)
{
 int ret=0;
 struct glv_asr *asr=NULL;
 if(progress)progress(opaque,1);ret=glv_asr_open(&asr,GLV_ASR_RELEASE_ROOT,memory);
 if(!ret){if(progress)progress(opaque,2);ret=glv_asr_transcribe(asr,pcm,samples,text,cap,token,opaque,NULL);}
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
