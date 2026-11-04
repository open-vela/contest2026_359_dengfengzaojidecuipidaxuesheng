/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_platform.h"
#include "glass_music_pcm.h"
#include "glass_speech.h"
#include "glass_status.h"
#include <errno.h>
#include "glass_material.h"
#include <stdlib.h>
#include <malloc.h>
extern bool glass_pinyin_reclaim_idle(void) __attribute__((weak));
extern int glass_ui_memory_begin(unsigned) __attribute__((weak));
extern void glass_ui_memory_end(void) __attribute__((weak));
void glv_platform_memory_begin(void)
{gmat_pressure(true);if(glass_ui_memory_begin)(void)glass_ui_memory_begin(150);}
void glv_platform_memory_end(void)
{if(glass_ui_memory_end)glass_ui_memory_end();else gmat_pressure(false);}
static void reclaim_optional(void)
{glass_speech_clear_cache();if(glass_pinyin_reclaim_idle)glass_pinyin_reclaim_idle();}
int glv_platform_memory_pressure(void)
{
  struct mallinfo mi=mallinfo();size_t free_bytes=mi.fordblks,largest=mi.mxordblk;
  if(free_bytes<GLV_SYSTEM_RESERVE||largest<65536u)return 2;
  return free_bytes<GLV_SYSTEM_RESERVE+256u*1024u||largest<262144u?1:0;
}
int glv_platform_budget(size_t *budget,size_t *largest)
{
  reclaim_optional();
  /* On this board the user malloc arena is PSRAM; the internal kernel arena
   * is separate. Never count kernel/DMA memory as inference capacity. */
  struct mallinfo mi=mallinfo();size_t free_bytes=mi.fordblks;
  *largest=mi.mxordblk;*budget=0;
  if(free_bytes<=GLV_SYSTEM_RESERVE||*largest<3500u*1024u)return -ENOMEM;
  *budget=free_bytes-GLV_SYSTEM_RESERVE;if(*budget>GLV_MEMORY_LIMIT)*budget=GLV_MEMORY_LIMIT;
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
  reclaim_optional();struct mallinfo mi=mallinfo();
  size_t free_bytes=mi.fordblks;*largest=mi.mxordblk;*budget=0;
  if(free_bytes<=GLV_SYSTEM_RESERVE||*largest<65536u)return -ENOMEM;
  size_t available=free_bytes-GLV_SYSTEM_RESERVE;
  *budget=retained>GLV_MEMORY_LIMIT||available>GLV_MEMORY_LIMIT-retained?GLV_MEMORY_LIMIT:retained+available;
  return 0;
}
