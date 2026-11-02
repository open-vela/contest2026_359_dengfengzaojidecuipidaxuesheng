/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_ai_config.h"
#include "glass_portal.h"
#include "glass_chat.h"
#include "glass_voice.h"
#include "glass_speech.h"
#include "qpk_espdl.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static atomic_bool quiet=true,auto_ack=true,ui_exit;
static atomic_uint cancels,writes,reloads;
static bool write_fail;
#ifdef GLASS_LOCAL_VOICE
#include "glass_local_voice.h"
static unsigned local_cancels;
void glass_local_voice_cancel_from(enum glv_cancel_source source){assert(source==GLV_CANCEL_CONFIG);assert(glass_ai_config_active());local_cancels++;}
bool glass_local_voice_config_idle(void){return atomic_load(&quiet);}
#endif
static char saved[8193]="{\"model\":\"before\",\"api_key\":\"fixture-secret\"}";
/* The ASR public state is idle as soon as cancellation is requested. Its
 * transport deliberately remains busy until the test releases quiet. */
void glass_chat_cancel(void)
{
  assert(glass_ai_config_active());assert(!glass_ai_start_enter());
  cJSON *o=portal_config_get("ai");assert(o);cJSON_Delete(o); /* no config_lock deadlock */
  atomic_fetch_add(&cancels,1);
}
bool glass_chat_config_idle(void){return atomic_load(&quiet);}
void glass_chat_config_reload(void){assert(glass_ai_config_active());atomic_fetch_add(&reloads,1);}
void glass_voice_cancel(void){}
bool glass_voice_config_idle(void){return atomic_load(&quiet);}
void glass_voice_get(struct glass_voice_snapshot *s){memset(s,0,sizeof(*s));}
void glass_speech_stop(void){}
void glass_speech_get(struct speech_snapshot *s){memset(s,0,sizeof(*s));s->active=!atomic_load(&quiet);}
void qpk_dl_cancel(void){}
void qpk_dl_status(struct qpk_dl_result *s){memset(s,0,sizeof(*s));s->busy=!atomic_load(&quiet);}
int qpk_storage_read(const char *root,const char *package,const char *key,char *out,size_t cap,size_t *length)
{
  (void)root;assert(!strcmp(package,"portal"));if(strcmp(key,"ai"))return -ENOENT;
  assert(strlen(saved)<cap);strcpy(out,saved);*length=strlen(saved);return 0;
}
int qpk_storage_write(const char *root,const char *package,const char *key,const char *data,size_t length)
{
  (void)root;assert(!strcmp(package,"portal")&&!strcmp(key,"ai"));
  assert(glass_ai_config_active()&&atomic_load(&quiet));
  if(write_fail)return -ENOSPC;
  assert(length<sizeof(saved));memcpy(saved,data,length);saved[length]=0;atomic_fetch_add(&writes,1);return 0;
}
static void *ui_worker(void *arg)
{
  (void)arg;while(!atomic_load(&ui_exit)){
    if(atomic_load(&auto_ack)&&glass_ai_config_ui_pending())glass_ai_config_ui_ack();
    usleep(1000);
  }return NULL;
}
static int patch(const char *json)
{cJSON *o=cJSON_Parse(json);assert(o);int ret=portal_config_save("ai",o);cJSON_Delete(o);return ret;}
static void *save_worker(void *result)
{*(int *)result=patch("{\"model\":\"after\"}");return NULL;}
static void wait_blocked(void)
{for(unsigned i=0;i<1000&&!glass_ai_config_active();i++)usleep(1000);assert(glass_ai_config_active());}
int main(void)
{
  pthread_t ui,writer;assert(!pthread_create(&ui,NULL,ui_worker,NULL));
  /* A starter admitted just before maintenance must finish publishing its
   * ownership before cancellation or configuration writes can proceed. */
  assert(glass_ai_start_enter());quiet=false;
  int result=-1;assert(!pthread_create(&writer,NULL,save_worker,&result));wait_blocked();
  usleep(20000);assert(!atomic_load(&cancels)&&!atomic_load(&writes));
  assert(!glass_ai_start_enter()&&glass_ai_config_begin(20)==-EBUSY);
  glass_ai_start_leave();
  for(unsigned i=0;i<1000&&!atomic_load(&cancels);i++)usleep(1000);
  assert(atomic_load(&cancels)==1);usleep(20000);assert(!atomic_load(&writes));
  quiet=true;assert(!pthread_join(writer,NULL)&&!result);
  assert(atomic_load(&writes)==1&&atomic_load(&reloads)==1&&!glass_ai_config_active());
#ifdef GLASS_LOCAL_VOICE
  assert(local_cancels==1);
#endif
  assert(glass_ai_start_enter());glass_ai_start_leave();
  assert(strstr(saved,"after")&&strstr(saved,"fixture-secret"));
  assert(!patch("{\"model\":\"after\"}")&&atomic_load(&writes)==1);
  write_fail=true;assert(patch("{\"model\":\"failed\"}")==-ENOSPC);write_fail=false;
  assert(!glass_ai_config_active()&&strstr(saved,"after")&&!strstr(saved,"failed"));
  assert(patch("{\"unknown\":1}")==-EINVAL&&!glass_ai_config_active());
  unsigned before=atomic_load(&writes),refreshes=atomic_load(&reloads);
  quiet=false;assert(glass_ai_config_begin(40)==-ETIMEDOUT);
  assert(!glass_ai_config_active()&&atomic_load(&writes)==before&&atomic_load(&reloads)==refreshes);
  quiet=true;auto_ack=false;
  assert(glass_ai_config_begin(40)==-ETIMEDOUT&&glass_ai_config_active());
  assert(!glass_ai_start_enter()); /* failed UI barrier still retires old UI first */
  glass_ai_config_ui_ack();assert(!glass_ai_config_active());auto_ack=true;
  assert(!patch("{}")&&atomic_load(&writes)==before);
  atomic_store(&ui_exit,true);assert(!pthread_join(ui,NULL));
  puts("AI_CONFIG_OK: admission race; real cleanup barrier; UI ack; write ordering; no-op; errors/timeouts restore readiness without replay; retained secrets");return 0;
}
