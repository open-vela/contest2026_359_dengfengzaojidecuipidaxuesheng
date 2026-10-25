#include <assert.h>
#include <time.h>
/* SPDX-License-Identifier: Apache-2.0 */
/* Service fixtures only. None of these functions opens a device/network. */
int netlib_get_ipv4addr(const char *name,struct in_addr *out)
{(void)name;out->s_addr=htonl(0xc0a80158);return 0;}
const struct c6_desktop_backend g_c6_desktop_backend={0};
static int review_music_mode,review_chat_mode,review_weather_mode,review_ble_mode;
static int review_grok_mode=-1;
void review_grok_fixture(int mode){review_grok_mode=mode;review_chat_mode=mode?1:0;}
static void review_grok_chat(struct chat_snapshot *out){
  if(review_grok_mode>=0)out->revision=(unsigned)review_grok_mode+100;
  if(review_grok_mode>0&&review_grok_mode<4){out->phase=CHAT_REQUESTING;out->turns[0].state=CHAT_WAITING;strcpy(out->turns[0].progress,review_grok_mode==1?"正在思考":review_grok_mode==2?"正在接收回复":"正在编写应用草稿");if(review_grok_mode!=2)out->turns[0].reply[0]=0;}
  else if(review_grok_mode==5){out->turns[0].state=CHAT_FAILED;out->turns[0].error=CHAT_ERROR_NETWORK;}
  else if(review_grok_mode==6)out->turns[0].state=CHAT_CANCELLED;
  else if(review_grok_mode==7)out->count=0;
}
static unsigned review_music_position=42000,review_music_duration=204000,review_music_revision;
static const char *review_draft_source;
static enum qpk_volume review_draft_volume;
void review_qpk_volume_fixture(enum qpk_volume volume){review_draft_volume=volume;}
static uint32_t review_previewed_revision;
void review_qpk_fixture(const char *source){review_draft_source=source;review_previewed_revision=0;}
void review_music_fixture(int mode){review_music_mode=mode;review_music_position=42000;review_music_duration=204000;review_music_revision++;}
void review_music_progress_fixture(unsigned position,unsigned duration){review_music_position=position;review_music_duration=duration;review_music_revision++;}
void review_chat_fixture(int mode){review_chat_mode=mode;}
/* One shared service fixture for both production chat presentations. */
static bool review_dialogue;
static struct chat_snapshot review_dialogue_state;
static unsigned review_dialogue_sends,review_dialogue_cancels,review_voice_starts;
static struct glass_voice_snapshot review_dialogue_voice;
static struct speech_snapshot review_speech;
static unsigned review_speech_calls;
static enum chat_activity review_dialogue_activity;
static int review_speech_failure;
static char review_spoken[CHAT_REPLY_BYTES+1];
void review_chat_dialog_fixture(bool enabled)
{
  review_dialogue_activity=CHAT_ACTIVITY_THINK;
  review_dialogue=enabled;memset(&review_dialogue_state,0,sizeof(review_dialogue_state));
  review_dialogue_state.loaded=review_dialogue_state.configured=true;review_dialogue_state.phase=CHAT_IDLE;
  review_dialogue_state.revision=1000;strcpy(review_dialogue_state.model,"共享对话验证");
  memset(&review_speech,0,sizeof(review_speech));review_speech_calls=0;review_speech_failure=0;review_spoken[0]=0;
  review_dialogue_sends=review_dialogue_cancels=review_voice_starts=0;memset(&review_dialogue_voice,0,sizeof(review_dialogue_voice));
}
unsigned review_chat_send_count(void){return review_dialogue_sends;}
unsigned review_chat_cancel_count(void){return review_dialogue_cancels;}
unsigned review_voice_start_count(void){return review_voice_starts;}
int glass_chat_send_mode(const char *prompt,enum glass_agent_mode mode)
{
  if(!review_dialogue)return 0;
  if(review_dialogue_state.phase!=CHAT_IDLE)return -EBUSY;
  if(!prompt||!prompt[0]||strlen(prompt)>CHAT_PROMPT_BYTES)return -EINVAL;
  if(review_dialogue_state.count==CHAT_MAX_TURNS){memmove(review_dialogue_state.turns,review_dialogue_state.turns+1,(CHAT_MAX_TURNS-1)*sizeof(struct chat_turn));review_dialogue_state.count--;}
  struct chat_turn *turn=&review_dialogue_state.turns[review_dialogue_state.count++];memset(turn,0,sizeof(*turn));
  turn->mode=mode;turn->id=++review_dialogue_sends;turn->state=CHAT_WAITING;strcpy(turn->prompt,prompt);strcpy(turn->progress,"正在思考");
  review_dialogue_state.active_mode=mode;
  review_dialogue_activity=CHAT_ACTIVITY_THINK;review_dialogue_state.phase=CHAT_REQUESTING;review_dialogue_state.revision++;return 0;
}
int glass_chat_send(const char *prompt){return glass_chat_send_mode(prompt,AGENT_MODE_CREATE);}
int glass_chat_send_bot(const char *prompt,enum glass_agent_mode mode){return glass_chat_send_mode(prompt,mode);}
int glass_chat_send_bot_camera(const char *prompt,enum glass_agent_mode mode){return glass_chat_send_mode(prompt,mode);}
int glass_chat_retry_mode(enum glass_agent_mode mode){(void)mode;return 0;}
int glass_chat_clear_mode(enum glass_agent_mode mode)
{
  unsigned n=0;for(unsigned i=0;i<review_dialogue_state.count;i++)if(review_dialogue_state.turns[i].mode!=mode)
    review_dialogue_state.turns[n++]=review_dialogue_state.turns[i];
  review_dialogue_state.count=n;review_dialogue_state.revision++;return 0;
}
bool glass_chat_get_mode(struct chat_snapshot *out,enum glass_agent_mode mode)
{
  if(out->view_mode!=mode)out->revision=0;
  if(!glass_chat_get(out))return false;
  unsigned n=0;for(unsigned i=0;i<out->count;i++)if(out->turns[i].mode==mode)out->turns[n++]=out->turns[i];
  out->count=n;out->view_mode=mode;return true;
}
void review_chat_reply_fixture(const char *text,bool done)
{
  assert(review_dialogue_state.count);struct chat_turn *turn=&review_dialogue_state.turns[review_dialogue_state.count-1];
  snprintf(turn->reply,sizeof(turn->reply),"%s",text);turn->state=done?CHAT_DONE:CHAT_WAITING;
  snprintf(turn->progress,sizeof(turn->progress),"%s",done?"回复已完成":"正在接收回复");
  review_dialogue_activity=CHAT_ACTIVITY_REPLY;
  review_dialogue_state.phase=done?CHAT_IDLE:CHAT_REQUESTING;review_dialogue_state.revision++;
}
void glass_chat_cancel(void)
{
  if(!review_dialogue)return;review_dialogue_cancels++;review_dialogue_state.phase=CHAT_IDLE;
  if(review_dialogue_state.count)review_dialogue_state.turns[review_dialogue_state.count-1].state=CHAT_CANCELLED;
  review_dialogue_state.revision++;
}
int glass_chat_clear(void)
{
  if(review_dialogue){review_dialogue_state.count=0;review_dialogue_state.phase=CHAT_IDLE;review_dialogue_state.revision++;}return 0;
}
int glass_voice_start(void)
{
  assert(!review_dialogue||!review_speech.active);
  if(review_dialogue){review_voice_starts++;review_dialogue_voice.phase=GLASS_VOICE_LISTENING;review_dialogue_voice.active=true;review_dialogue_voice.revision++;}return 0;
}
void glass_voice_stop(void)
{
  if(review_dialogue){review_dialogue_voice.phase=GLASS_VOICE_FINISHING;review_dialogue_voice.revision++;}
}
void review_voice_text_fixture(const char *text)
{
  snprintf(review_dialogue_voice.text,sizeof(review_dialogue_voice.text),"%s",text);
  review_dialogue_voice.phase=GLASS_VOICE_DONE;review_dialogue_voice.active=false;review_dialogue_voice.revision++;
}
void glass_speech_get(struct speech_snapshot *out){*out=review_speech;}
int glass_speech_say_request(const char *text,uint32_t *request)
{int ret=glass_speech_say(text);if(!ret&&request)*request=review_speech.request;return ret;}
int glass_speech_say(const char *text)
{
  assert(!review_dialogue_voice.active);assert(strlen(text)<=240);for(const unsigned char *p=(const unsigned char *)text;*p;){unsigned n=*p<128?1:*p<224?2:*p<240?3:4;assert(*p<128||*p>=194);for(unsigned j=1;j<n;j++)assert(p[j]&&(p[j]&192)==128);p+=n;}
  if(review_speech_failure)return review_speech_failure;
  assert(!review_speech.active);review_speech_calls++;review_speech.request++;review_speech.active=true;review_speech.phase=SPEECH_SPEAKING;review_speech.error=0;
  assert(strlen(review_spoken)+strlen(text)<sizeof(review_spoken));strcat(review_spoken,text);return 0;
}
void glass_speech_stop(void){review_speech.wake_ready=false;if(review_speech.active)review_speech.phase=SPEECH_STOPPING;}
void glass_speech_stop_request(uint32_t request){if(request&&request==review_speech.request)glass_speech_stop();}
void glass_speech_clear_cache(void){}
const char *glass_speech_backend(void){return "mock";}
int glass_speech_wait_wake(uint32_t *request)
{
  if(review_speech.active)return -EBUSY;
  if(review_speech_failure)return review_speech_failure;
  review_speech.request++;*request=review_speech.request;review_speech.active=true;
  review_speech.phase=SPEECH_WAKE;review_speech.error=0;review_speech.wake_events=0;review_speech.wake_ready=false;return 0;
}
void review_wake_fixture(bool release)
{review_speech.wake_events=1;if(release){review_speech.active=false;review_speech.phase=SPEECH_IDLE;review_speech.wake_ready=true;}}
void review_speech_replace_fixture(void)
{review_speech.request++;review_speech.active=true;review_speech.phase=SPEECH_WAKE;}
int glass_speech_listen(void)
{if(review_speech.active)return -EBUSY;review_speech.request++;review_speech.active=true;review_speech.phase=SPEECH_WAKE;review_speech.error=0;review_speech.command_event=0;return 0;}
void review_local_command_fixture(int command,bool stale)
{
  struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
  review_speech.last_command=command;review_speech.command_event++;review_speech.revision++;
  review_speech.command_ms=(uint32_t)((uint64_t)now.tv_sec*1000+now.tv_nsec/1000000)-(stale?3000:0);
  if(command==8){review_speech.active=false;review_speech.phase=SPEECH_IDLE;}
}
void review_speech_done_fixture(int error){review_speech.active=false;review_speech.error=error;review_speech.phase=error?SPEECH_ERROR:SPEECH_IDLE;}
unsigned review_speech_count(void){return review_speech_calls;}
const char *review_speech_text(void){return review_spoken;}
void review_speech_failure_fixture(int error){review_speech_failure=error;}
void review_chat_phase_fixture(enum chat_phase phase){review_dialogue_state.phase=phase;review_dialogue_state.revision++;}
void review_chat_error_fixture(void){review_dialogue_state.phase=CHAT_IDLE;review_dialogue_state.turns[review_dialogue_state.count-1].state=CHAT_FAILED;review_dialogue_state.turns[review_dialogue_state.count-1].error=CHAT_ERROR_NETWORK;review_dialogue_state.revision++;}
void review_chat_activity_fixture(enum chat_activity activity){review_dialogue_activity=activity;review_dialogue_state.revision++;}
void review_chat_tool_fixture(const char *name,const char *progress)
{
  assert(review_dialogue_state.count);struct chat_turn *turn=&review_dialogue_state.turns[review_dialogue_state.count-1];
  if(turn->tool_count==CHAT_TOOL_TRACE){memmove(turn->tools,turn->tools+1,(CHAT_TOOL_TRACE-1)*sizeof(turn->tools[0]));turn->tool_count--;}
  snprintf(turn->tools[turn->tool_count++],sizeof(turn->tools[0]),"%s",name);turn->tool_total++;
  snprintf(turn->progress,sizeof(turn->progress),"%s",progress);review_chat_activity_fixture(CHAT_ACTIVITY_TOOL);
}
void review_chat_card_fixture(unsigned count)
{
  assert(review_dialogue_state.count);struct chat_turn *turn=&review_dialogue_state.turns[review_dialogue_state.count-1];
  unsigned n=count>CHAT_CARD_COUNT?CHAT_CARD_COUNT:count;turn->card_count=n;
  static const struct chat_card samples[CHAT_CARD_COUNT]={
    {.entity="light.living",.kind="light",.name="客厅主灯",.state="已开启",.level=80,.on=true},
    {.entity="switch.study",.kind="switch",.name="书房插座",.state="已关闭",.level=-1,.on=false},
    {.entity="sensor.temperature",.kind="sensor",.name="客厅温度",.state="23.5 °C",.level=-1,.on=false},
    {.entity="cover.balcony",.kind="cover",.name="阳台窗帘",.state="已打开",.level=60,.on=true}};
  for(unsigned i=0;i<n;i++)turn->cards[i]=samples[i];
  review_dialogue_state.revision++;
}
void review_speech_level_fixture(unsigned level){review_speech.level=level;}
void review_chat_verified_fixture(bool verified)
{review_chat_card_fixture(1);review_dialogue_state.turns[review_dialogue_state.count-1].cards[0].action=true;review_dialogue_state.turns[review_dialogue_state.count-1].cards[0].verified=verified;review_dialogue_state.revision++;}
void review_chat_music_card_fixture(bool playing)
{
  assert(review_dialogue_state.count);
  struct chat_turn *turn=&review_dialogue_state.turns[review_dialogue_state.count-1];
  turn->card_count=1;memset(turn->cards,0,sizeof(turn->cards));
  struct chat_card *c=&turn->cards[0];
  strcpy(c->entity,"music.player");strcpy(c->kind,"music");strcpy(c->name,"海阔天空");
  snprintf(c->state,sizeof(c->state),"%s",playing?"播放中 · Beyond":"已请求播放");
  c->level=playing?39:-1;c->on=playing;review_dialogue_state.revision++;
}
void review_voice_level_fixture(unsigned level){review_dialogue_voice.level=level;}
void review_voice_phase_fixture(enum glass_voice_phase phase){review_dialogue_voice.phase=phase;review_dialogue_voice.revision++;}
void review_voice_error_fixture(void){review_dialogue_voice.active=false;review_dialogue_voice.phase=GLASS_VOICE_ERROR;review_dialogue_voice.error=GLASS_VOICE_ERROR_EMPTY;}
void review_voice_network_error_fixture(void){review_dialogue_voice.active=false;review_dialogue_voice.phase=GLASS_VOICE_ERROR;review_dialogue_voice.error=GLASS_VOICE_ERROR_NETWORK;}
void glass_chat_activity_get(struct chat_activity_snapshot *out){if(review_dialogue){memset(out,0,sizeof(*out));out->loaded=out->configured=true;out->phase=review_dialogue_state.phase;if(review_dialogue_state.count){const struct chat_turn *t=&review_dialogue_state.turns[review_dialogue_state.count-1];out->turn_id=t->id;out->state=t->state;out->activity=review_dialogue_activity;}return;}memset(out,0,sizeof(*out));out->loaded=true;out->configured=true;out->phase=CHAT_IDLE;out->state=CHAT_DONE;if(review_chat_mode)out->turn_id=1;if(review_grok_mode>0&&review_grok_mode<4){out->phase=CHAT_REQUESTING;out->state=CHAT_WAITING;out->activity=review_grok_mode==1?CHAT_ACTIVITY_THINK:review_grok_mode==2?CHAT_ACTIVITY_REPLY:CHAT_ACTIVITY_TOOL;}else if(review_grok_mode==5)out->state=CHAT_FAILED;else if(review_grok_mode==6)out->state=CHAT_CANCELLED;else if(review_grok_mode==7)out->turn_id=0;}
static bool review_weather_custom;
static struct glass_weather_snapshot review_weather_sample;
void review_weather_snapshot_fixture(const struct glass_weather_snapshot *s)
{review_weather_custom=s!=NULL;if(s)review_weather_sample=*s;}
void review_weather_fixture(int mode){review_weather_mode=mode;review_weather_custom=false;}
void review_ble_fixture(int mode){review_ble_mode=mode;}
static void review_music_library(struct music_snapshot *out){memset(out,0,sizeof(*out));out->initialized=true;out->volume=40;out->revision=review_music_revision;if(review_music_mode){out->favorite_count=3;const char *names[]={"午后的小调","城市与晚风","熟悉的旋律"};for(unsigned i=0;i<3;i++){snprintf(out->favorites[i].name,sizeof(out->favorites[i].name),"%s",names[i]);strcpy(out->favorites[i].artist,"审核示例歌手");strcpy(out->favorites[i].album,"离线示例专辑");}out->current=out->result=out->favorites[0];out->has_result=true;out->search_revision=1;out->state=review_music_mode==2?MUSIC_PLAYING:MUSIC_IDLE;out->position_ms=review_music_position;out->duration_ms=review_music_duration;}}
bool glass_chat_get(struct chat_snapshot *out){if(review_dialogue){if(out->revision==review_dialogue_state.revision)return false;*out=review_dialogue_state;return true;}memset(out,0,sizeof(*out));out->loaded=true;out->phase=CHAT_IDLE;out->configured=true;out->revision=review_chat_mode+1;strcpy(out->model,"离线审核 · 服务示例");if(review_chat_mode){out->count=1;out->turns[0].id=1;out->turns[0].state=CHAT_DONE;strcpy(out->turns[0].prompt,"帮我安排今天的学习计划。");strcpy(out->turns[0].reply,"可以先完成一项最重要的任务，再留出时间整理笔记。\n\n上午：专注学习与练习。\n下午：回顾进度，补充问题。\n晚上：整理收获，为明天留下一条清晰的待办。\n\n以上内容仅用于离线页面审核。");out->turns[0].clipped=true;strcpy(out->turns[0].archive,"review-response");}review_grok_chat(out);return true;}
static unsigned review_archive_page,review_archive_revision=1,review_archive_total=2;
void review_chat_pages_fixture(unsigned pages){review_archive_total=pages;review_archive_page=0;}
int glass_chat_page_request(const char *name,unsigned page)
{(void)name;if(page>=review_archive_total)return -EINVAL;review_archive_page=page;review_archive_revision++;return 0;}
bool glass_chat_page_get(struct chat_page *out){memset(out,0,sizeof(*out));out->revision=review_archive_revision;out->page=review_archive_page;out->pages=review_archive_total;strcpy(out->text,"完整回复（离线审核示例）\n\n1. 选择今天最重要的一件事。\n2. 在一个专注时段里完成它。\n3. 安排短休息，再检查进度。\n4. 保存笔记，整理明天的待办。\n\n这个页面沿用原有分页与返回操作。");return true;}
size_t glass_chat_copy_utf8(char *out,size_t cap,const char *text){size_t n=strlen(text);if(n>=cap)n=cap-1;while(n&&(text[n]&0xc0)==0x80)n--;memcpy(out,text,n);out[n]=0;return n;}
void glass_weather_get(struct glass_weather_snapshot *out){if(review_weather_custom){*out=review_weather_sample;return;}memset(out,0,sizeof(*out));out->state=WEATHER_WAIT_NET;if(review_weather_mode){out->valid=true;out->state=WEATHER_READY;out->revision=2;out->data.temperature=24;out->data.humidity=48;out->data.aqi=32;out->data.level=1;out->data.pm25=12;strcpy(out->data.condition,"多云");strcpy(out->data.category,"良好");strcpy(out->data.location,"当地天气");strcpy(out->data.report_time,"2026-10-13 14:00 · 审核示例");}}
#include <assert.h>
#include <pthread.h>
static pthread_mutex_t review_ble_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t review_ble_gate=PTHREAD_COND_INITIALIZER;
static unsigned review_ble_hold_operation;
static bool review_ble_held;
static bool review_ble_custom;
static struct glass_ble_state review_ble_state;
static pthread_t review_ble_ui_thread;
static int review_ble_error;
static unsigned review_ble_calls[6];
static unsigned review_ble_pair_mode;
void review_ble_pair_fixture(unsigned mode)
{pthread_mutex_lock(&review_ble_lock);review_ble_pair_mode=mode;pthread_mutex_unlock(&review_ble_lock);}
void review_ble_snapshot_fixture(const struct glass_ble_state *state)
{
  pthread_mutex_lock(&review_ble_lock);review_ble_custom=state!=NULL;
  if(state)review_ble_state=*state;review_ble_ui_thread=pthread_self();
  pthread_mutex_unlock(&review_ble_lock);
}
void review_ble_error_fixture(int error)
{pthread_mutex_lock(&review_ble_lock);review_ble_error=error;pthread_mutex_unlock(&review_ble_lock);}
unsigned review_ble_call_count(unsigned operation)
{pthread_mutex_lock(&review_ble_lock);unsigned n=operation<6?review_ble_calls[operation]:0;pthread_mutex_unlock(&review_ble_lock);return n;}
void review_ble_hold_fixture(unsigned operation)
{pthread_mutex_lock(&review_ble_lock);review_ble_hold_operation=operation;pthread_cond_broadcast(&review_ble_gate);pthread_mutex_unlock(&review_ble_lock);}
bool review_ble_hold_arrived(void)
{pthread_mutex_lock(&review_ble_lock);bool held=review_ble_held;pthread_mutex_unlock(&review_ble_lock);return held;}
static void review_ble_hold_locked(unsigned operation)
{
  if(review_ble_custom&&review_ble_hold_operation==operation){
    review_ble_held=true;
    while(review_ble_hold_operation==operation)pthread_cond_wait(&review_ble_gate,&review_ble_lock);
    review_ble_held=false;
  }
}
void glass_ble_read(struct glass_ble_state *out)
{
  pthread_mutex_lock(&review_ble_lock);
  if(review_ble_custom){*out=review_ble_state;pthread_mutex_unlock(&review_ble_lock);return;}
  pthread_mutex_unlock(&review_ble_lock);
  memset(out,0,sizeof(*out));out->started=true;out->ready=true;
  if(review_ble_mode){out->count=2;strcpy(out->devices[0].name,"蓝牙键盘 · 审核示例");strcpy(out->devices[1].name,"温湿度传感器 · 审核示例");for(unsigned i=0;i<2;i++){out->devices[i].rssi=-38-i*10;out->devices[i].address[0]=2;out->devices[i].address[5]=i+1;}}
}
int glass_ble_start(void)
{pthread_mutex_lock(&review_ble_lock);if(review_ble_custom)assert(!pthread_equal(pthread_self(),review_ble_ui_thread));int ret=review_ble_error;pthread_mutex_unlock(&review_ble_lock);return ret;}
int glass_ble_scan(void)
{
  pthread_mutex_lock(&review_ble_lock);review_ble_calls[1]++;int ret=review_ble_error;
  if(review_ble_custom){assert(!pthread_equal(pthread_self(),review_ble_ui_thread));if(!ret){review_ble_state.scanning=true;review_ble_state.count=0;review_ble_state.scan_generation++;review_ble_state.error=0;}}
  if(!ret)review_ble_hold_locked(1);
  pthread_mutex_unlock(&review_ble_lock);return ret;
}
int glass_ble_central_connect(unsigned index,uint32_t generation)
{
  pthread_mutex_lock(&review_ble_lock);review_ble_calls[2]++;int ret=review_ble_error;
  if(review_ble_custom){
    assert(!pthread_equal(pthread_self(),review_ble_ui_thread));
    if(!ret&&(generation!=review_ble_state.scan_generation||index>=review_ble_state.count))ret=-ESTALE;
    if(!ret&&review_ble_state.scanning)ret=-EBUSY;
    if(!ret){review_ble_state.connecting=true;review_ble_state.peer_valid=true;review_ble_state.peer=review_ble_state.devices[index];review_ble_state.error=0;}
  }
  if(!ret)review_ble_hold_locked(2);
  pthread_mutex_unlock(&review_ble_lock);return ret;
}
int glass_ble_cancel(void)
{
  pthread_mutex_lock(&review_ble_lock);review_ble_calls[3]++;
  if(review_ble_custom){assert(!pthread_equal(pthread_self(),review_ble_ui_thread));review_ble_state.scanning=false;review_ble_state.connecting=false;if(!review_ble_state.connected)review_ble_state.peer_valid=false;}
  review_ble_hold_locked(3);
  pthread_mutex_unlock(&review_ble_lock);return 0;
}
int glass_ble_disconnect(void)
{
  pthread_mutex_lock(&review_ble_lock);review_ble_calls[4]++;int ret=review_ble_error;
  if(review_ble_custom){assert(!pthread_equal(pthread_self(),review_ble_ui_thread));if(!ret)review_ble_state.disconnecting=review_ble_state.connected;}
  pthread_mutex_unlock(&review_ble_lock);return ret;
}
int glass_ble_gatt(const struct glass_gatt_request *request,struct glass_gatt_result *reply,atomic_bool *cancel)
{
  pthread_mutex_lock(&review_ble_lock);review_ble_calls[5]++;
  assert(request->operation==GLASS_GATT_PAIR);
  if(review_ble_custom)assert(!pthread_equal(pthread_self(),review_ble_ui_thread));
  review_ble_hold_locked(5);
  int ret=review_ble_error;
  if(cancel&&atomic_load(cancel))ret=-ECANCELED;
  else if(request->generation!=review_ble_state.generation)ret=-ESTALE;
  else if(!review_ble_state.connected)ret=-ENOTCONN;
  if(!ret){
    memset(reply,0,sizeof(*reply));reply->generation=request->generation;
    if(review_ble_pair_mode!=1){review_ble_state.encrypted=true;review_ble_state.bonded=review_ble_pair_mode!=2;}
  }
  pthread_mutex_unlock(&review_ble_lock);return ret;
}
static int review_battery_percent=-1;
static bool review_battery_charging,review_bluetooth_connected;
void review_status_fixture(int percent,bool charging,bool bluetooth)
{review_battery_percent=percent;review_battery_charging=charging;review_bluetooth_connected=bluetooth;}
void glass_status_power_read(int *percent,bool *charging)
{*percent=review_battery_percent;*charging=review_battery_charging;}
static bool review_hid_custom,review_wifi_custom;
static struct glass_ble_hid_state review_hid_state;
static struct c6_desktop_result review_wifi_result;
static enum c6_desktop_operation review_wifi_operation;
static char review_wifi_ssid[33],review_wifi_password[65];
void review_hid_snapshot_fixture(const struct glass_ble_hid_state *state)
{review_hid_custom=state!=NULL;if(state)review_hid_state=*state;}
void review_wifi_snapshot_fixture(const struct c6_desktop_result *state)
{review_wifi_custom=state!=NULL;if(state)review_wifi_result=*state;}
void glass_ble_hid_read(struct glass_ble_hid_state *out){if(review_hid_custom){*out=review_hid_state;return;}memset(out,0,sizeof(*out));out->connected=review_bluetooth_connected;}
int c6_desktop_worker_start(struct c6_desktop_worker *w,const struct c6_desktop_backend *b){memset(w,0,sizeof(*w));return 0;}
int c6_desktop_worker_read(struct c6_desktop_worker *w,struct c6_desktop_result *out){if(review_wifi_custom){*out=review_wifi_result;return 0;}memset(out,0,sizeof(*out));out->sequence=1;out->operation=C6_DESKTOP_STATUS;return 0;}
int c6_desktop_worker_submit(struct c6_desktop_worker *w,enum c6_desktop_operation operation,const char *ssid,const char *password)
{review_wifi_operation=operation;snprintf(review_wifi_ssid,sizeof(review_wifi_ssid),"%s",ssid?ssid:"");snprintf(review_wifi_password,sizeof(review_wifi_password),"%s",password?password:"");return review_wifi_custom&&review_wifi_result.busy?-EBUSY:0;}
bool review_wifi_request_matches(enum c6_desktop_operation operation,const char *ssid,const char *password)
{return review_wifi_operation==operation&&!strcmp(review_wifi_ssid,ssid?ssid:"")&&!strcmp(review_wifi_password,password?password:"");}
/* The keyboard/IME UI is production code; dictionary/voice backends stay offline. */
static unsigned review_pinyin_count;
static char review_pinyin_input[33];
void review_pinyin_fixture(unsigned count){review_pinyin_count=count;}
unsigned glass_pinyin_search(const char *input){snprintf(review_pinyin_input,sizeof(review_pinyin_input),"%s",input?input:"");return input&&input[0]?review_pinyin_count:0;}
bool glass_pinyin_candidate(unsigned index,char *out,size_t capacity){if(index>=review_pinyin_count){if(capacity)out[0]=0;return false;}snprintf(out,capacity,"候选%u",index+1);return true;}
bool glass_pinyin_choose(unsigned index,char *out,size_t capacity,unsigned *consumed)
{if(!review_pinyin_input[0]||!glass_pinyin_candidate(index,out,capacity))return false;*consumed=strlen(review_pinyin_input);return true;}
void glass_voice_get(struct glass_voice_snapshot *out){if(review_dialogue){*out=review_dialogue_voice;return;}memset(out,0,sizeof(*out));if(review_grok_mode==7){out->active=true;out->phase=GLASS_VOICE_LISTENING;}}
const char *glass_voice_error_text(enum glass_voice_error error){(void)error;return "离线审核服务";}
bool glass_qpk_removable(const char *path,const char *package){return path&&package&&!strcmp(package,"review.fixture");}
static bool review_local_ha,review_local_on[3],review_local_no_update,review_local_lost_response,review_local_unavailable;
static unsigned review_local_calls,review_local_config=1,review_local_request,review_local_pending,review_local_kind;
static uint32_t review_local_client;
static char review_local_entity[96];
void review_local_ha_fixture(bool enabled)
{review_local_ha=enabled;memset(review_local_on,0,sizeof(review_local_on));review_local_calls=review_local_client=review_local_pending=0;review_local_no_update=review_local_lost_response=review_local_unavailable=false;review_local_config++;}
unsigned review_local_call_count(void){return review_local_calls;}
void review_local_config_change(void){review_local_config++;}
void review_local_ha_failure(bool no_update,bool lost,bool unavailable)
{review_local_no_update=no_update;review_local_lost_response=lost;review_local_unavailable=unavailable;}
void hass_status(bool *configured,bool *busy,unsigned *clients){if(configured)*configured=review_local_ha;if(busy)*busy=false;if(clients)*clients=review_local_client?1:0;}
uint32_t hass_configuration_revision(void){return review_local_ha?review_local_config:0;}
int hass_open(unsigned grants){if(!review_local_ha)return 0;if(grants!=(HASS_READ|HASS_CONTROL))return 88;assert(!review_local_client);review_local_client=77;return 77;}
int hass_close(uint32_t client){if(client==88)return 0;assert(client==review_local_client);review_local_client=review_local_pending=0;return 0;}
int hass_get(uint32_t client,const char *resource)
{assert(client==review_local_client&&!strcmp(resource,"states"));review_local_kind=1;return review_local_pending=++review_local_request;}
int hass_get_state(uint32_t client,const char *entity)
{assert(client==review_local_client);strcpy(review_local_entity,entity);review_local_kind=2;return review_local_pending=++review_local_request;}
int hass_control(uint32_t client,const char *entity,bool on,int brightness)
{
  assert(client==review_local_client&&brightness==-1);int role=!strcmp(entity,"light.voice_test")?0:!strcmp(entity,"fan.voice_test")?2:-1;assert(role>=0);
  review_local_calls++;if(!review_local_no_update)review_local_on[role]=on;
  review_local_kind=3;return review_local_pending=++review_local_request;
}
int hass_call_service(uint32_t client,const char *entity,const char *service,const char *body)
{
  assert(client==review_local_client&&!strcmp(entity,"cover.voice_test")&&strstr(body,"cover.voice_test"));
  assert(!strcmp(service,"open_cover")||!strcmp(service,"close_cover"));
  review_local_calls++;if(!review_local_no_update)review_local_on[1]=!strcmp(service,"open_cover");
  review_local_kind=3;return review_local_pending=++review_local_request;
}
int hass_poll(uint32_t client,struct hass_result_s *result)
{
  assert(client==review_local_client);memset(result,0,sizeof(*result));if(!review_local_pending)return 0;
  result->request_id=review_local_pending;review_local_pending=0;result->done=true;result->status=200;
  if(review_local_kind==1)result->body=strdup("[{\"entity_id\":\"light.voice_test\",\"state\":\"off\",\"attributes\":{\"friendly_name\":\"测试台灯\"}},{\"entity_id\":\"cover.voice_test\",\"state\":\"closed\",\"attributes\":{\"friendly_name\":\"测试窗帘\",\"device_class\":\"curtain\"}},{\"entity_id\":\"cover.garage\",\"state\":\"closed\",\"attributes\":{\"device_class\":\"garage\"}},{\"entity_id\":\"fan.voice_test\",\"state\":\"off\",\"attributes\":{\"friendly_name\":\"测试风扇\"}}]");
  else if(review_local_kind==2){
    int role=!strncmp(review_local_entity,"light.",6)?0:!strncmp(review_local_entity,"cover.",6)?1:2;
    char body[384];snprintf(body,sizeof(body),"{\"entity_id\":\"%s\",\"state\":\"%s\",\"attributes\":{\"device_class\":\"curtain\",\"current_position\":%d}}",review_local_entity,review_local_unavailable?"unavailable":role==1?(review_local_on[role]?"open":"closed"):(review_local_on[role]?"on":"off"),review_local_on[role]?100:0);result->body=strdup(body);
  }else if(review_local_lost_response){result->status=0;result->error=-EIO;}
  return 0;
}
void hass_result_free(struct hass_result_s *result){free(result->body);memset(result,0,sizeof(*result));}
int portal_session_open(char *code,size_t cap){snprintf(code,cap,"246810");return 0;}
unsigned portal_session_seconds(void){return 240;}
void glass_qpk_get(struct qpk_draft_info *out){memset(out,0,sizeof(*out));out->loaded=true;out->draft_volume=review_draft_volume;if(review_draft_source){out->revision=1;out->previewed_revision=review_previewed_revision;strcpy(out->name,"你好快应用（审核示例草稿）");strcpy(out->slug,"hello");}}
const char *glass_qpk_error_text(int error){(void)error;return "离线审核服务";}
const char *glass_chat_error_text(enum chat_error error){(void)error;return "离线审核服务";}
struct glass_file_job {struct glass_file_result result;unsigned polls;int error;};
static unsigned review_files_busy,review_files_polls,review_files_live,review_files_cancelled;
static int review_files_error;
void review_files_fault_fixture(unsigned busy,unsigned polls,int error)
{review_files_busy=busy;review_files_polls=polls;review_files_error=error;}
unsigned review_files_live_jobs(void){return review_files_live;}
unsigned review_files_cancel_count(void){return review_files_cancelled;}

static unsigned review_file_actions[7];
unsigned review_file_action_count(unsigned action){return action<7?review_file_actions[action]:0;}
static unsigned review_files_pages=1;
void review_files_pages_fixture(unsigned pages){review_files_pages=pages?pages:1;}
int glass_file_submit(const struct glass_file_request *request,struct glass_file_job **job){review_file_actions[request->action]++;*job=NULL;
if(review_files_busy){review_files_busy--;return EBUSY;}
*job=calloc(1,sizeof(**job));if(!*job)return ENOMEM;
review_files_live++;(*job)->polls=review_files_polls;(*job)->error=review_files_error;
review_files_polls=0;review_files_error=0;struct glass_file_result *r=&(*job)->result;r->total_bytes=64ULL*1024*1024;r->free_bytes=40ULL*1024*1024;if(request->action==GLASS_FILE_LIST){r->count=12;const char *names[]={"music","recordings","notes.txt","review-video.mp4","透明图.png","手机照片.HEIC","旅行短片.MOV","风景.jpg","备份.zip","应用.qpk","配置.json","其他文件.bin"};const char *more[]={"使用说明.PDF","项目文档.docx","家庭账单.xlsx","旅行演示.pptx","夜晚的风.MP3","无损音乐.flac","界面字体.ttf","脚本.js","设备配置.yaml","备份归档.7z","家庭旅行照片_2026年国庆_非常长的文件名.png","未识别扩展.custom"};
const char *extra[]={"透明图.webp","手机照片.avif","表情动图.gif","原始扫描.tiff","短片.avi","电影.mkv","语音记录.wav","播客.ogg","网页.html","界面.css","无扩展文件","文档字体.otf"};
for(unsigned i=0;i<12;i++)strcpy(r->entries[i].name,request->page==1?more[i]:request->page==2?extra[i]:names[i]);
if(!request->page)r->entries[0].directory=r->entries[1].directory=true;r->has_next=request->page+1<review_files_pages;if(r->has_next){for(unsigned i=12;i<GLASS_FILE_PAGE_SIZE;i++)snprintf(r->entries[i].name,sizeof(r->entries[i].name),"其他文件-%u.bin",i);r->count=GLASS_FILE_PAGE_SIZE;}}else{r->size=256;r->binary=strstr(request->source,".PDF")!=NULL;strcpy(r->preview,"离线审核文件\n\n文件详情、新建、重命名和删除确认均沿用现有代码。\n此文件内容为模拟服务数据，不访问真实磁盘。");}return 0;}
int glass_file_poll(struct glass_file_job *job,struct glass_file_status *status)
{memset(status,0,sizeof(*status));if(!job)return EINVAL;
 if(job->polls){job->polls--;return 0;}status->done=true;status->error=job->error;return 0;}
const struct glass_file_result *glass_file_get_result(struct glass_file_job *job){return job?&job->result:NULL;}
void glass_file_release(struct glass_file_job *job){if(job){if(job->polls)review_files_cancelled++;review_files_live--;free(job);}}
int glass_system_read(struct glass_system_monitor *monitor,struct glass_system_status *out){(void)monitor;memset(out,0,sizeof(*out));out->ready=true;out->sample.cpu_tenths=125;out->sample.heap_total=32ULL*1024*1024;out->sample.heap_free=22ULL*1024*1024;out->sample.heap_largest=14ULL*1024*1024;out->sample.heap_peak=13ULL*1024*1024;return 0;}
int glass_preferences_get_status(struct glass_preferences_status *out){memset(out,0,sizeof(*out));return 0;}
char *glass_qpk_preview_source(struct qpk_draft_info *out){glass_qpk_get(out);return review_draft_source?strdup(review_draft_source):NULL;}
void glass_qpk_preview_result(uint32_t revision,const char *error){if(!error[0])review_previewed_revision=revision;}

/* Shared playback fixtures for native music/lock-screen lifecycle checks. */
static bool review_now_override;
static struct music_now_playing review_now;
static unsigned review_lyric_total;
static int review_lyric_current=-1;
static const char *review_lyric_text;
static const char *review_lyric_next;
static const char *review_lyric_translation;
void review_lyrics_translation_fixture(const char *text){review_lyric_translation=text;}
void review_lyrics_next_fixture(const char *text){review_lyric_next=text;}
void review_lyrics_text_fixture(const char *text){review_lyric_text=text;}
void review_lyrics_fixture(unsigned total,int current)
{review_lyric_total=total;review_lyric_current=current;}
void glass_music_lyrics_read(unsigned first,struct music_lyrics_window *out)
{
 memset(out,0,sizeof(*out));out->current=-1;
 if(!review_now_override)return;
 out->track_id=review_now.track_id;out->state=review_now.lyrics_state;
 if(out->state!=MUSIC_LYRICS_READY)return;
 out->total=review_lyric_total;out->current=review_lyric_current;
 unsigned last=out->total>MUSIC_LYRICS_WINDOW?out->total-MUSIC_LYRICS_WINDOW:0;
 out->first=first>last?last:first;
 for(unsigned i=out->first;i<out->total&&out->count<MUSIC_LYRICS_WINDOW;i++){
  out->start_ms[out->count]=i*4000;out->end_ms[out->count]=(i+1)*4000;
  snprintf(out->lines[out->count++],MUSIC_LYRIC_TEXT_MAX,"第 %u 行 · 原创歌词用于界面验证",i+1);
 }
 if(out->current>=0&&(unsigned)out->current>=out->first&&(unsigned)out->current<out->first+out->count)
   snprintf(out->lines[out->current-out->first],MUSIC_LYRIC_TEXT_MAX,"%s",review_lyric_text?review_lyric_text:"把今天交给温柔的晚风");
 if(review_lyric_next&&out->current>=0&&(unsigned)(out->current+1)>=out->first&&(unsigned)(out->current+1)<out->first+out->count)
   snprintf(out->lines[out->current+1-out->first],MUSIC_LYRIC_TEXT_MAX,"%s",review_lyric_next);
 if(review_lyric_translation&&out->current>=0&&(unsigned)out->current>=out->first&&(unsigned)out->current<out->first+out->count)
   snprintf(out->translations[out->current-out->first],MUSIC_LYRIC_TEXT_MAX,"%s",review_lyric_translation);
}
static unsigned review_music_toggles,review_music_skips;
void review_music_now_fixture(const struct music_now_playing *value)
{review_now_override=value!=NULL;if(value)review_now=*value;}
void glass_music_get(struct music_snapshot *out)
{
 review_music_library(out);
 if(review_now_override){strcpy(out->current.name,review_now.name);strcpy(out->current.artist,review_now.artist);strcpy(out->current.album,review_now.album);
 out->revision=review_now.revision;out->state=review_now.state;out->position_ms=review_now.position_ms;out->duration_ms=review_now.duration_ms;}
}
void glass_music_now_playing(struct music_now_playing *out)
{
 if(review_now_override){*out=review_now;return;}
 struct music_snapshot s;review_music_library(&s);memset(out,0,sizeof(*out));
 strcpy(out->name,s.current.name);strcpy(out->artist,s.current.artist);strcpy(out->album,s.current.album);
 out->revision=s.revision;out->state=s.state;out->position_ms=s.position_ms;out->duration_ms=s.duration_ms;out->volume=s.volume;
 out->can_skip=s.favorite_count>0;out->lyrics_state=s.current.name[0]?MUSIC_LYRICS_MISSING:MUSIC_LYRICS_NONE;
}
void glass_music_toggle(void)
{
 review_music_toggles++;
 if(review_now_override){review_now.pause_requested=!review_now.pause_requested;review_now.state=review_now.pause_requested?MUSIC_PAUSED:MUSIC_PLAYING;review_now.revision++;}
}
int glass_music_skip(int direction)
{
 if(direction!=-1&&direction!=1)return -EINVAL;
 review_music_skips++;if(!review_now_override||!review_now.can_skip)return -ENOENT;
 strcpy(review_now.name,direction>0?"下一首示例":"上一首示例");memset(review_now.lyrics,0,sizeof(review_now.lyrics));
 review_now.lyrics_state=MUSIC_LYRICS_LOADING;review_now.track_id++;review_now.position_ms=0;review_now.revision++;return 0;
}

/* Only the photo I/O boundary is mocked; LVGL canvas ownership is production. */
static int review_lock_photo_mode;
static bool review_lock_photo_pending;
void review_lock_photo_fixture(int mode){review_lock_photo_mode=mode;}
int glass_lock_photo_request(void){review_lock_photo_pending=true;return 0;}
void glass_lock_photo_cancel(void){review_lock_photo_pending=false;}
bool glass_lock_photo_take(struct glass_lock_photo *out)
{
 if(!review_lock_photo_pending)return false;review_lock_photo_pending=false;memset(out,0,sizeof(*out));
 if(!review_lock_photo_mode){out->error=-ENOENT;return true;}
 out->width=1024;out->height=600;out->pixels=malloc(1024*600*2);if(!out->pixels){out->error=-ENOMEM;return true;}
 for(unsigned y=0;y<600;y++)for(unsigned x=0;x<1024;x++){
 unsigned ridge=260+(x%350>175?350-x%350:x%350)/2;
 unsigned r,g,b;if(y<ridge){r=40+y/5;g=100+y/5;b=155+y/8;}
 else if(y<440){r=35+(x/32)%20;g=75+y/10;b=100+y/12;}
 else{r=40+y/12;g=118+y/18;b=140+y/16;}
 out->pixels[y*1024+x]=((r>>3)<<11)|((g>>2)<<5)|(b>>3);
 }
 return true;
}

static const char *review_cover_raw;
static bool review_cover_pending[GLASS_COVER_HOME+1];
void review_cover_fixture(const char *path){review_cover_raw=path;}
int glass_lock_cover_request(unsigned owner,const char *url)
{if(owner<1||owner>GLASS_COVER_HOME||!url)return -EINVAL;review_cover_pending[owner]=true;return 0;}
void glass_lock_cover_cancel(unsigned owner){if(owner<=GLASS_COVER_HOME)review_cover_pending[owner]=false;}
bool glass_lock_cover_take(unsigned owner,struct glass_lock_photo *out)
{
 if(owner<1||owner>GLASS_COVER_HOME||!review_cover_pending[owner]||!review_cover_raw)return false;
 review_cover_pending[owner]=false;memset(out,0,sizeof(*out));FILE *f=fopen(review_cover_raw,"rb");assert(f);
 uint32_t size[2];assert(fread(size,4,2,f)==2);out->width=size[0];out->height=size[1];out->pixels=malloc(size[0]*size[1]*2);assert(out->pixels);
 assert(fread(out->pixels,2,size[0]*size[1],f)==size[0]*size[1]);fclose(f);return true;
}


/* Album fixtures replace SD I/O only. The real gallery and viewer UI run. */
#include "local_album.h"
static int review_album_mode=1;
static struct album_listing *review_album_list;
static struct album_image review_album_image;
static bool review_album_image_ready;
static unsigned review_album_requests;
void local_album_cancel(void)
{
 free(review_album_list);review_album_list=NULL;free(review_album_image.pixels);
 review_album_image=(struct album_image){0};review_album_image_ready=false;
}
void review_album_fixture(int mode){local_album_cancel();review_album_mode=mode;}
bool local_album_valid_path(const char *path)
{return path&&(!strncmp(path,"/sdcard",7)||!strncmp(path,"/data",5))&&!strstr(path,"..");}
static unsigned review_album_scan_count;
static char review_album_last_path[ALBUM_PATH];
unsigned review_album_scan_calls(void){return review_album_scan_count;}
const char *review_album_decode_path(void){return review_album_last_path;}
int local_album_scan(const char *path)
{
 review_album_scan_count++;
 local_album_cancel();review_album_list=calloc(1,sizeof(*review_album_list));assert(review_album_list);
 strcpy(review_album_list->path,path);
 if(review_album_mode==3){review_album_list->error=-ENODEV;return 0;}
 if(review_album_mode==2)return 0;
 if(!strncmp(path,"/data/photos",12)){
  review_album_list->count=2;
  strcpy(review_album_list->entries[0].name,"camera-00000001.bmp");strcpy(review_album_list->entries[1].name,"camera-00000002.bmp");
  review_album_list->entries[0].kind=review_album_list->entries[1].kind=ALBUM_PHOTO;return 0;
 }
 const char *names[]={"相机","旅行","晨光.jpg","山间.jpeg","海边短片.mp4","落日.jpg","海岸.jpg","街角.jpg","云层.jpg","远山.jpg"};
 if(review_album_mode==4){review_album_list->count=1;strcpy(review_album_list->entries[0].name,"损坏.jpg");review_album_list->entries[0].kind=ALBUM_PHOTO;return 0;}
 unsigned first=strcmp(path,"/sdcard")?2:0;
 for(unsigned i=first;i<10;i++){
  struct album_entry *e=&review_album_list->entries[review_album_list->count++];strcpy(e->name,names[i]);e->kind=i<2?ALBUM_FOLDER:local_album_kind(names[i]);
 }
 return 0;
}
struct album_listing *local_album_take_listing(void)
{struct album_listing *out=review_album_list;review_album_list=NULL;return out;}
int local_album_decode(const char *path,unsigned maxw,unsigned maxh)
{
 snprintf(review_album_last_path,sizeof(review_album_last_path),"%s",path);
 review_album_requests++;review_album_image_ready=true;review_album_image=(struct album_image){0};
 if(review_album_mode==4){review_album_image.error=-ENOTSUP;return 0;}
 unsigned variant=0;for(const unsigned char *s=(const unsigned char *)path;*s;s++)variant+=*s;
 unsigned w=maxw,h=maxw*9/16;if(h>maxh){h=maxh;w=h*16/9;}
 if(strstr(path,"山间")){h=maxh;w=h*9/16;if(!w)w=1;}
 review_album_image.width=w;review_album_image.height=h;review_album_image.pixels=malloc(w*h*2);assert(review_album_image.pixels);
 const unsigned colors[][6]={{135,183,203,47,102,110},{230,190,148,93,106,102},{171,161,190,77,85,116},{219,178,171,95,112,123},{164,192,186,44,100,102}};
 const unsigned *c=colors[variant%5];
 for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++){
  unsigned Y=y*160/h,X=x*256/w;unsigned ridge=72+(unsigned)abs((int)(X%130)-65)/2;
  unsigned r=c[0]+y*18/h,g=c[1]+y*12/h,b=c[2];
  if(Y>ridge){r=c[3]+X/25;g=c[4]+X/24;b=c[5]+X/20;}
  if(Y>121+X/20){r=c[3]*3/4;g=c[4]*3/4;b=c[5]*3/4;}
  if((int)(X-187)*(int)(X-187)+(int)(Y-41)*(int)(Y-41)<100){r=251;g=226;b=184;}
  review_album_image.pixels[y*w+x]=((r>>3)<<11)|((g>>2)<<5)|(b>>3);
 }
 return 0;
}
bool local_album_take_image(struct album_image *out)
{if(!review_album_image_ready)return false;*out=review_album_image;review_album_image=(struct album_image){0};review_album_image_ready=false;return true;}

static struct qpk_dl_result review_face;
static bool review_face_available;
static unsigned review_face_cancelled;
void review_face_fixture(int x,int y,bool tracked){review_face.frame++;review_face.track.state=tracked?QPK_DL_TRACKING:QPK_DL_LOST;review_face.track.offset_x=x;review_face.track.offset_y=y;}
void review_face_available_fixture(bool available){review_face_available=available;}
unsigned review_face_cancel_count(void){return review_face_cancelled;}
int qpk_dl_face_start_default(void){if(!review_face_available)return -ENODEV;review_face.request++;review_face.busy=true;review_face.mode=QPK_DL_FACE;review_face.frame=0;return review_face.request;}
void qpk_dl_status(struct qpk_dl_result *out){*out=review_face;}
void qpk_dl_cancel_request(uint32_t id){if(id&&id==review_face.request){review_face_cancelled++;review_face.busy=false;}}

int hass_control_checked(uint32_t c,const char *e,bool on,int b,uint32_t revision)
{return revision&&revision==hass_configuration_revision()?hass_control(c,e,on,b):-ESTALE;}
int hass_call_service_checked(uint32_t c,const char *e,const char *service,const char *body,uint32_t revision)
{return revision&&revision==hass_configuration_revision()?hass_call_service(c,e,service,body):-ESTALE;}

int glass_voice_start_request(uint32_t *request){int ret=glass_voice_start();if(!ret){if(!++review_dialogue_voice.request)review_dialogue_voice.request=1;if(request)*request=review_dialogue_voice.request;}return ret;}
void glass_voice_stop_request(uint32_t request){if(request&&request==review_dialogue_voice.request)glass_voice_stop();}
void glass_voice_cancel(void){glass_voice_stop();review_dialogue_voice.active=false;review_dialogue_voice.text[0]=0;review_dialogue_voice.phase=GLASS_VOICE_IDLE;review_dialogue_voice.request++;}
