/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include <nuttx/audio/audio.h>
#include "glass_local_capture.h"
#include "glass_music.h"
#include "glass_music_pcm.h"
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define VOICE_RATE GLV_CAPTURE_RATE
#define VOICE_BUFFERS 4
#define VOICE_BYTES 960
#define VOICE_FRAMES GLV_CAPTURE_BLOCK
#define VOICE_SETTLE_FRAMES 1024
static pthread_mutex_t capture_entry=PTHREAD_MUTEX_INITIALIZER;
/* Accessed only by the one thread holding capture_entry. The existing capture
 * helpers invoke this callback synchronously; no DMA callback retains it. */
static struct glv_memory *capture_memory;
static uint64_t capture_ms(void *unused)
{(void)unused;struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static int capture_session(void)
{
  uint64_t until=capture_ms(NULL)+5000;
  for(;;){
    int ret=glv_cancel(capture_memory);if(ret)return ret;
    ret=music_pcm_session_trylock();if(ret!=-EBUSY)return ret;
    if(capture_ms(NULL)>=until)return -ETIMEDOUT;usleep(10000);
  }
}
#define VOICE_CAPTURE_CANCELLED() (glv_cancel(capture_memory)!=0)
#define VOICE_CAPTURE_LOCK() capture_session()
#include "glass_voice_capture.inc"
static int input_open(void *opaque){return capture_open(opaque);}
static int input_read(void *opaque,int16_t *mono){unsigned level=0;return capture_next(opaque,mono,&level);}
static int input_close(void *opaque){capture_close(opaque);return 0;}
int glv_capture_platform(struct glv_memory *memory,const struct glv_capture_signals *signals,
                            const struct glv_capture_options *options,int16_t **pcm,size_t *samples)
{
  if(!memory||!pcm||!samples)return -EINVAL;*pcm=NULL;*samples=0;
  int ret=pthread_mutex_trylock(&capture_entry);if(ret)return ret==EBUSY?-EBUSY:-ret;
  capture_memory=memory;struct voice_capture capture;
  struct glv_capture_source source={input_open,input_read,input_close,capture_ms,&capture};
  ret=glv_capture_collect(memory,&source,signals,options,pcm,samples);
  capture_memory=NULL;pthread_mutex_unlock(&capture_entry);return ret;
}

#ifdef GLV_ASR_STREAMING
int glv_capture_platform_stream(struct glv_memory *memory,const struct glv_capture_signals *signals,const struct glv_capture_options *options,glv_capture_consume consume,void *opaque,glv_capture_stream_progress progress)
{
 if(!memory||!consume)return -EINVAL;
 int ret=pthread_mutex_trylock(&capture_entry);if(ret)return ret==EBUSY?-EBUSY:-ret;
 /* Same native adapter and exclusive session as collect(). Main owns entry;
  * producer alone touches capture, and join retires it before entry unlock. */
 capture_memory=memory;struct voice_capture capture;
 struct glv_capture_source source={input_open,input_read,input_close,capture_ms,&capture};
 ret=glv_capture_stream(memory,&source,signals,options,consume,opaque,progress);
 capture_memory=NULL;pthread_mutex_unlock(&capture_entry);return ret;
}
#endif
