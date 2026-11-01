/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_ai_config.h"
#include "glass_chat.h"
#include "glass_voice.h"
#include "qpk_espdl.h"
#ifdef GLASS_LOCAL_VOICE
#include "glass_local_voice.h"
#endif
#ifdef GLASS_LOCAL_SPEECH
#include "glass_speech.h"
#endif
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t gate_lock=PTHREAD_MUTEX_INITIALIZER;
static bool blocked,ui_ack,ui_needed;
static unsigned starting;
static uint64_t clock_ms(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
bool glass_ai_config_active(void)
{pthread_mutex_lock(&gate_lock);bool value=blocked||ui_needed;pthread_mutex_unlock(&gate_lock);return value;}
bool glass_ai_start_enter(void)
{
  pthread_mutex_lock(&gate_lock);bool allow=!blocked&&!ui_needed;if(allow)starting++;
  pthread_mutex_unlock(&gate_lock);return allow;
}
void glass_ai_start_leave(void)
{pthread_mutex_lock(&gate_lock);if(starting)starting--;pthread_mutex_unlock(&gate_lock);}
bool glass_ai_config_ui_pending(void)
{pthread_mutex_lock(&gate_lock);bool value=ui_needed;pthread_mutex_unlock(&gate_lock);return value;}
void glass_ai_config_ui_ack(void)
{pthread_mutex_lock(&gate_lock);ui_needed=false;ui_ack=true;pthread_mutex_unlock(&gate_lock);}
int glass_ai_config_begin(unsigned timeout_ms)
{
  uint64_t began=clock_ms();
  pthread_mutex_lock(&gate_lock);
  if(blocked||ui_needed){pthread_mutex_unlock(&gate_lock);return -EBUSY;}
  blocked=ui_needed=true;ui_ack=false;pthread_mutex_unlock(&gate_lock);
  bool cancelled=false,final_ui=false;
  for(;;){
    pthread_mutex_lock(&gate_lock);bool starters=starting!=0,ui_ready=ui_ack;pthread_mutex_unlock(&gate_lock);
    if(!starters&&!cancelled){
      /* Admission is closed before cancellation; all accepted starters have
       * published their ownership. Never hold config_lock while draining. */
      glass_chat_cancel();glass_voice_cancel();qpk_dl_cancel();
#ifdef GLASS_LOCAL_VOICE
      glass_local_voice_cancel_from(GLV_CANCEL_CONFIG);
#endif
#ifdef GLASS_LOCAL_SPEECH
      glass_speech_stop();
#endif
      cancelled=true;
    }
    if(cancelled&&ui_ready){
      struct qpk_dl_result dl;qpk_dl_status(&dl);
      bool quiet=glass_chat_config_idle()&&glass_voice_config_idle()&&!dl.busy;
#ifdef GLASS_LOCAL_VOICE
      quiet=quiet&&glass_local_voice_config_idle();
#endif
#ifdef GLASS_LOCAL_SPEECH
      struct speech_snapshot speech;glass_speech_get(&speech);quiet=quiet&&!speech.active;
#endif
      if(quiet){
        /* Cancellation can publish one last wake/result event while draining.
         * Retire UI mailboxes again after every producer has stopped. */
        if(!final_ui){
          pthread_mutex_lock(&gate_lock);ui_needed=true;ui_ack=false;pthread_mutex_unlock(&gate_lock);
          final_ui=true;continue;
        }
        printf("[ai-config] stage=quiesced elapsed_ms=%lu\n",(unsigned long)(clock_ms()-began));return 0;
      }
    }
    if(clock_ms()-began>=timeout_ms){
      pthread_mutex_lock(&gate_lock);blocked=false;pthread_mutex_unlock(&gate_lock);
      printf("[ai-config] stage=timeout elapsed_ms=%lu saved=0\n",(unsigned long)(clock_ms()-began));return -ETIMEDOUT;
    }
    usleep(10000);
  }
}
void glass_ai_config_end(void)
{
  /* Each request reloads settings. Refresh only an existing chat service;
   * cancelled conversations, tool actions and uploads are never replayed. */
  glass_chat_config_reload();
  pthread_mutex_lock(&gate_lock);blocked=false;pthread_mutex_unlock(&gate_lock);
  printf("[ai-config] stage=ready\n");
}
