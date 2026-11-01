/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_platform.h"
#include "glass_music_pcm.h"
#include "glass_speech.h"
#include "glass_status.h"
#include <errno.h>
#include <stdlib.h>
#include <malloc.h>
int glv_platform_budget(size_t *budget,size_t *largest)
{
  glass_speech_clear_cache();
  /* On this board the user malloc arena is PSRAM; the internal kernel arena
   * is separate. Never count kernel/DMA memory as inference capacity. */
  struct mallinfo mi=mallinfo();size_t free_bytes=mi.fordblks;
  *largest=mi.mxordblk;*budget=0;
  if(free_bytes<=4u*1024u*1024u||*largest<3500u*1024u)return -ENOMEM;
  *budget=free_bytes-4u*1024u*1024u;if(*budget>GLV_MEMORY_LIMIT)*budget=GLV_MEMORY_LIMIT;
  return *budget<4608u*1024u?-ENOMEM:0;
}
int glv_platform_audio_open(void **out,bool (*cancelled)(void *),void *opaque)
{
  *out=NULL;struct music_pcm *pcm=calloc(1,sizeof(*pcm));if(!pcm)return -ENOMEM;
  int ret=music_pcm_foreground_open(pcm,24000,glass_speech_volume(),3000,cancelled,opaque);
  if(ret){free(pcm);return ret;}*out=pcm;return 0;
}
int glv_platform_audio_write(void *pcm,const int16_t *samples,unsigned frames)
{return music_pcm_write(pcm,samples,frames,1);}
int glv_platform_audio_finish(void *pcm,bool drain)
{if(!pcm)return 0;int ret=drain?music_pcm_drain(pcm):0;music_pcm_close(pcm);free(pcm);return ret;}
void glv_platform_wake_yield(void){glass_speech_yield_background();}
bool glv_platform_storage_ready(void){return glass_status_storage_ready();}
extern int mmcsd_local_session_stamp(uint32_t *) __attribute__((weak));
int glv_platform_storage_stamp(struct glv_storage_stamp *out)
{
  if(!mmcsd_local_session_stamp)return -ENOTSUP;
  uint32_t before=glass_status_storage_epoch();
  if(!glass_status_storage_ready())return -ENODEV;
  int ret=mmcsd_local_session_stamp(&out->writes);if(ret)return ret;
  out->media=glass_status_storage_epoch();
  return before==out->media&&glass_status_storage_ready()?0:-ENODEV;
}
int glv_platform_session_budget(size_t retained,size_t *budget,size_t *largest)
{
  glass_speech_clear_cache();struct mallinfo mi=mallinfo();
  size_t free_bytes=mi.fordblks;*largest=mi.mxordblk;*budget=0;
  if(free_bytes<=4u*1024u*1024u||*largest<65536u)return -ENOMEM;
  size_t available=free_bytes-4u*1024u*1024u;
  *budget=retained>GLV_MEMORY_LIMIT||available>GLV_MEMORY_LIMIT-retained?GLV_MEMORY_LIMIT:retained+available;
  return 0;
}
