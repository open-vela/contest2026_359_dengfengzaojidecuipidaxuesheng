/* Real service, asset verification and control parser; deterministic engines/HA. */
#include "glass_local_voice.h"
#include "glass_local_assets.h"
#include "glass_local_platform.h"
#include "glass_local_llm.h"
#include "glass_local_frontend.h"
#include "glass_local_tts.h"
#include "glass_portal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
static atomic_bool hold_generation,entered;
static unsigned live,maximum_live,chat_calls,tool_calls,writes,reads;
static bool lamp[2];
struct glv_llm {struct glv_memory *memory;bool control;};
bool glv_platform_storage_ready(void){return true;}
int glv_platform_storage_stamp(struct glv_storage_stamp *s){s->writes=0;s->media=1;return 0;}
bool glass_ai_start_enter(void){return true;}
void glass_ai_start_leave(void){}
bool glass_ai_config_active(void){return false;}
int glass_ai_config_begin(unsigned ms){(void)ms;return 0;}
void glass_ai_config_end(void){}
void glv_platform_wake_yield(void){}
int glv_platform_budget(size_t *b,size_t *largest){*b=GLV_MEMORY_LIMIT;*largest=8u*1024u*1024u;return 0;}
int glv_platform_audio_open(void **p,bool (*cancel)(void *),void *arg){(void)p;(void)cancel;(void)arg;assert(0);return -ENOSYS;}
int glv_platform_audio_write(void *p,const int16_t *v,unsigned n){(void)p;(void)v;(void)n;assert(0);return -ENOSYS;}
int glv_platform_audio_finish(void *p,bool drain){(void)drain;assert(!p);return 0;}
int glv_llm_open(const char *root,struct glv_memory *m,struct glv_llm **out)
{
 bool control=!strcmp(root,GLV_RELEASE_ROOT "/edgeidiot");assert(control||!strcmp(root,GLV_CHAT_ROOT "/edgeidiot"));
 *out=glv_alloc(m,sizeof(**out));assert(*out);(*out)->memory=m;(*out)->control=control;
 live++;if(live>maximum_live)maximum_live=live;assert(live==1);return 0;
}
void glv_llm_close(struct glv_llm *s){if(s){assert(live==1);live--;glv_free(s);}}
size_t glv_llm_trim_kv(struct glv_llm *s){(void)s;return 0;}
size_t glv_llm_resident_bytes(const struct glv_llm *s){return s?sizeof(*s):0;}
size_t glv_llm_kv_bytes(const struct glv_llm *s){(void)s;return 0;}
int glv_llm_encode(struct glv_llm *s,const char *text,int *ids,unsigned cap){(void)s;assert(text&&cap);ids[0]=1;return 1;}
int glv_llm_generate(struct glv_llm *s,const char *prompt,unsigned max,char *answer,size_t cap,glv_text_sink sink,void *arg)
{
 assert(!s->control&&max==96);chat_calls++;assert(!strstr(prompt,"执行回执"));
 if(strstr(prompt,"还记得"))assert(strstr(prompt,"聊天记忆"));
 snprintf(answer,cap,"普通聊天回答");assert(!sink(arg,answer,8));return 8;
}
int glv_llm_generate_tool(struct glv_llm *s,const char *prompt,unsigned max,char *answer,size_t cap,glv_text_sink sink,void *arg)
{
 assert(s->control&&max==64);tool_calls++;
 if(atomic_load(&hold_generation)){
  atomic_store(&entered,true);while(!glv_cancel(s->memory))usleep(1000);return -ECANCELED;
 }
 /* No extra model review or opposite action is allowed after completion. */
 assert(!strstr(prompt,"只读核对"));
 snprintf(answer,cap,"Tool: living_lights.set_power(on=true)");
 int ret=sink(arg,answer,12);return ret?ret:12;
}
int glv_frontend_plan(const char *r,const char *t,struct glv_memory *m,struct glv_phone_plan **p){(void)r;(void)t;(void)m;(void)p;assert(0);return -ENOSYS;}
int glv_tts_open(const char *r,struct glv_memory *m,struct glv_tts **s){(void)r;(void)m;(void)s;assert(0);return -ENOSYS;}
void glv_tts_close(struct glv_tts *s){assert(!s);}
int glv_tts_phones(struct glv_tts *s,const int *i,const unsigned char *k,unsigned n,bool lead,glv_audio_sink sink,void *a){(void)s;(void)i;(void)k;(void)n;(void)lead;(void)sink;(void)a;assert(0);return -ENOSYS;}
cJSON *portal_config_get(const char *section)
{
 assert(!strcmp(section,"localcontrol"));
 const char *catalog="{\"resources\":[{\"id\":\"lamp_desk\",\"name\":\"书桌台灯\",\"entity_id\":\"light.desk\"},{\"id\":\"lamp_bed\",\"name\":\"床头灯\",\"entity_id\":\"light.bed\"},{\"id\":\"living_lights\",\"name\":\"客厅全部灯光\",\"members\":[\"lamp_desk\",\"lamp_bed\"]}]}";
 cJSON *o=cJSON_CreateObject();cJSON_AddStringToObject(o,"catalog",catalog);return o;
}
uint32_t glass_board_home_revision(void *p){(void)p;return 1;}
char *glass_board_home_local_checked(void *p,const char *method,const char *entity,bool on,uint32_t rev,atomic_bool *cancel)
{
 (void)p;assert(rev==1&&!atomic_load(cancel));unsigned id=!strcmp(entity,"light.desk")?0:1;
 assert(id==0||!strcmp(entity,"light.bed"));
 if(!strcmp(method,"home_power")){lamp[id]=on;writes++;}else {assert(!strcmp(method,"home_state"));reads++;}
 cJSON *o=cJSON_CreateObject();cJSON_AddBoolToObject(o,"ok",true);cJSON_AddBoolToObject(o,"verified",true);
 cJSON_AddStringToObject(o,"entity",entity);cJSON_AddStringToObject(o,"state",lamp[id]?"on":"off");
 char *r=cJSON_PrintUnformatted(o);cJSON_Delete(o);return r;
}
static struct glv_snapshot idle(void)
{struct glv_snapshot s;for(unsigned i=0;i<30000;i++){glass_local_voice_get(&s,NULL,0);if(!s.busy&&!s.config_pending)return s;usleep(1000);}assert(!"worker timeout");return s;}
int main(void)
{
 assert(!glass_local_voice_start());struct glv_snapshot s=idle();assert(s.thinking_rounds==5);
 assert(glass_local_voice_thinking(3)==-EINVAL&&glass_local_voice_thinking(7)==-EINVAL);
 assert(!glass_local_voice_send("聊天记忆",GLV_DIALOGUE,NULL));s=idle();assert(!s.error&&s.model_rounds==1&&!s.resident_control_model&&chat_calls==1&&!writes);
 char reply[4096];
 for(unsigned rounds=4;rounds<=6;rounds++){
  assert(!glass_local_voice_thinking(rounds));assert(!glass_local_voice_select(GLV_CONTROL));
  lamp[0]=lamp[1]=false;unsigned before=writes,before_calls=tool_calls;
  assert(!glass_local_voice_send("打开客厅全部灯光",GLV_CONTROL,NULL));s=idle();
  glass_local_voice_get(NULL,reply,sizeof(reply));
  assert(!s.error&&s.model_rounds==1&&s.review_rounds==0&&s.thinking_rounds==rounds&&s.tool_rounds==1&&s.resident_control_model);
  assert(tool_calls-before_calls==1&&lamp[0]&&lamp[1]&&writes-before==2&&strstr(reply,"执行回执")&&!strstr(reply,"on=false"));
  printf("DUAL_ROUNDS level=%u model=%u review=%u writes=%u\n",rounds,s.model_rounds,s.review_rounds,writes-before);
 }
 assert(!glass_local_voice_new());idle();assert(!glass_local_voice_select(GLV_DIALOGUE));
 glass_local_voice_get(NULL,reply,sizeof(reply));assert(!strcmp(reply,"普通聊天回答"));
 unsigned before=writes;assert(!glass_local_voice_send("还记得聊天记忆吗",GLV_DIALOGUE,NULL));s=idle();assert(!s.error&&s.model_rounds==1&&writes==before&&!s.resident_control_model);
 assert(!glass_local_voice_select(GLV_CONTROL));atomic_store(&hold_generation,true);
 lamp[0]=lamp[1]=false;assert(!glass_local_voice_send("打开客厅全部灯光",GLV_CONTROL,NULL));
 for(unsigned i=0;i<10000&&!atomic_load(&entered);i++)usleep(1000);assert(atomic_load(&entered));
 assert(glass_local_voice_select(GLV_DIALOGUE)==-EBUSY&&glass_local_voice_thinking(4)==-EBUSY);
 before=writes;glass_local_voice_cancel();s=idle();assert(s.error==-ECANCELED&&s.model_rounds<6&&writes==before);
 assert(maximum_live==1);puts("DUAL_AI_HOST_OK actual-worker/assets/control-parser; fake-generators/HA; default5; maximum Low4 Medium5 High6; early-completion1; single-resident; no-extra-inference; separate-chat; cancellation");
 return 0;
}
