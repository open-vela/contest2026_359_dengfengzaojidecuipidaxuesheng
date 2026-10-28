/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_board.h"
struct board_ui_job {
  char operation[24];
  cJSON *arguments;
  char *result;
  bool running,done,abandoned;
};
static pthread_mutex_t board_ui_lock=PTHREAD_MUTEX_INITIALIZER;
static struct board_ui_job *board_ui_pending;
static void board_ui_free(struct board_ui_job *job)
{cJSON_Delete(job->arguments);free(job->result);free(job);}

char *glass_board_ui(const char *operation,const cJSON *arguments,atomic_bool *cancel)
{
  struct board_ui_job *job=calloc(1,sizeof(*job));if(!job)return NULL;
  strlcpy(job->operation,operation,sizeof(job->operation));job->arguments=cJSON_Duplicate(arguments,true);
  if(!job->arguments){free(job);return NULL;}
  pthread_mutex_lock(&board_ui_lock);
  if(board_ui_pending){pthread_mutex_unlock(&board_ui_lock);board_ui_free(job);return strdup("{\"error\":\"board_ui_busy\"}");}
  board_ui_pending=job;
  pthread_mutex_unlock(&board_ui_lock);
  for(unsigned i=0;i<250;i++) {
    usleep(20000);
    pthread_mutex_lock(&board_ui_lock);
    if(job->done){char *text=job->result;job->result=NULL;board_ui_pending=NULL;pthread_mutex_unlock(&board_ui_lock);board_ui_free(job);return text;}
    bool stop=cancel&&atomic_load(cancel);
    if(stop||i==249){
      bool running=job->running;
      if(running)job->abandoned=true;else board_ui_pending=NULL;
      pthread_mutex_unlock(&board_ui_lock);
      if(!running)board_ui_free(job);
      return strdup(running?"{\"error\":\"board_ui_completion_unknown_do_not_replay\"}":"{\"error\":\"board_ui_cancelled_or_timed_out_before_execution\"}");
    }
    pthread_mutex_unlock(&board_ui_lock);
  }
  return NULL;
}
struct board_builtin {const char *id,*name;lv_event_cb_t open;};
static void board_open_wifi(lv_event_t *e){(void)e;glass_wifi_page(NULL);}
static void board_open_ble(lv_event_t *e){(void)e;glass_bluetooth_page(NULL);}
static void board_open_files(lv_event_t *e){(void)e;glass_storage_page(NULL);}
static void board_open_resources(lv_event_t *e){(void)e;glass_resources_page(NULL);}
static const struct board_builtin board_builtins[]={
  {"home","桌面",panel_hide},{"apps","全部应用",glass_apps},
  {"settings","设置",glass_settings},{"wifi","Wi-Fi",board_open_wifi},
  {"bluetooth","蓝牙",board_open_ble},{"files","文件",board_open_files},
  {"resources","系统资源",board_open_resources},{"weather","天气",glass_weather_open},
  {"music","音乐",glass_music},{"camera","相机",launch_builtin_camera_qapp},
  {"recorder","录音机",launch_builtin_recorder_qapp},{"espdl","视觉识别",launch_builtin_espdl_qapp},
  {"homeassistant","智能家居",glass_homeassistant},{"2048","2048",launch_builtin_2048_qapp},
  {"ouo","OuO",launch_builtin_ouo_qapp},{"pet","桌宠",launch_builtin_dafeiyu_qapp},
  {"hello","你好快应用",launch_builtin_qapp},{"about","关于",about_clicked},
#ifdef CONFIG_SYSTEM_BILIBILI_LIVE
  {"live","网络直播",launch_bilibili},{"video","本地视频",launch_local_video},
#endif
};
static void board_ui_labels(lv_obj_t *object,cJSON *list,bool *truncated)
{
  if(!object||!lv_obj_is_visible(object)||lv_obj_check_type(object,&lv_textarea_class))return;
  if(lv_obj_has_class(object,&lv_label_class)){
    if(cJSON_GetArraySize(list)>=64){*truncated=true;return;}
    const char *text=object==portal_code?"[pairing code hidden]":lv_label_get_text(object);
    char bounded[193];strlcpy(bounded,text,sizeof(bounded));
    /* Avoid cutting a UTF-8 sequence. */
    if(strlen(text)>=sizeof(bounded)){size_t n=sizeof(bounded)-1;while(n&&((unsigned char)text[n]&0xc0)==0x80)n--;bounded[n]=0;}
    cJSON_AddItemToArray(list,cJSON_CreateString(bounded));return;
  }
  for(uint32_t i=0;i<lv_obj_get_child_count(object);i++)board_ui_labels(lv_obj_get_child(object,i),list,truncated);
}
static cJSON *board_ui_execute(const char *operation,const cJSON *args)
{
  cJSON *out=cJSON_CreateObject();if(!out)return NULL;
  if(!strcmp(operation,"camera_capture")) {
    uint32_t request=0;bool accepted=qpk_runtime_camera_capture_request(&request);
    printf("[camera-flow] ui camera_capture accepted=%d request=%lu\n",accepted,(unsigned long)request);
    cJSON_AddBoolToObject(out,"ok",accepted);
    if(accepted){cJSON_AddBoolToObject(out,"accepted",true);cJSON_AddNumberToObject(out,"request_id",request);}
    else cJSON_AddStringToObject(out,"error","camera_preview_required_or_busy");
  } else if(!strcmp(operation,"camera_photo_status")) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(args,"request_id");char path[128];
    int status=qpk_runtime_camera_photo_status(v?(uint32_t)v->valuedouble:0,path,sizeof(path));
    cJSON_AddBoolToObject(out,"ok",status>=0);cJSON_AddNumberToObject(out,"status",status);
    cJSON_AddBoolToObject(out,"done",status!=1);
    if(status==2)cJSON_AddStringToObject(out,"path",path);
    if(status<0)cJSON_AddStringToObject(out,"error",status==-ESTALE?"photo_request_replaced":"photo_save_failed");
  } else if(!strcmp(operation,"device_status")) {
    cJSON_AddBoolToObject(out,"ok",true);
    cJSON_AddStringToObject(out,"page",lv_obj_has_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN)?"home":g_desktop.apps_page?"apps":g_desktop.chat_page?"chat":g_desktop.qapp_page?"qapp":"panel");
    cJSON_AddBoolToObject(out,"light_theme",g_desktop.light_theme);
    cJSON_AddStringToObject(out,"runtime_error",qpk_runtime_last_error());
  } else if(!strcmp(operation,"ui_read")) {
    cJSON *list=cJSON_AddArrayToObject(out,"labels");bool truncated=false;
    board_ui_labels(g_desktop.screen,list,&truncated);board_ui_labels(lv_layer_top(),list,&truncated);
    cJSON_AddBoolToObject(out,"truncated",truncated);cJSON_AddBoolToObject(out,"ok",true);
  } else if(!strcmp(operation,"ui_click")) {
    const char *label=cJSON_GetObjectItemCaseSensitive(args,"label")->valuestring;
    lv_obj_t *root=g_chat_ui.confirm?g_chat_ui.confirm:g_desktop.toast?g_desktop.toast:g_desktop.dialog?g_desktop.dialog:
      lv_obj_has_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN)?g_glass.home:g_desktop.current_card;
    bool clicked=desktop_ui_probe_click(lv_layer_top(),label);
    if(!clicked)clicked=desktop_ui_probe_click(root,label);
    cJSON_AddBoolToObject(out,"ok",clicked);if(!clicked)cJSON_AddStringToObject(out,"error","visible_button_not_found");
  } else if(!strcmp(operation,"apps_list")) {
    qpk_scan();unsigned builtin=sizeof(board_builtins)/sizeof(board_builtins[0]);
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(args,"offset");unsigned offset=v?(unsigned)v->valuedouble:0;
    unsigned total=builtin+g_desktop.nqpk;
    cJSON *list=cJSON_AddArrayToObject(out,"apps");
    for(unsigned i=offset;i<total&&i-offset<16;i++) {
      cJSON *item=cJSON_CreateObject();
      cJSON_AddStringToObject(item,"id",i<builtin?board_builtins[i].id:g_desktop.qpk[i-builtin].package);
      cJSON_AddStringToObject(item,"name",i<builtin?board_builtins[i].name:g_desktop.qpk[i-builtin].name);
      cJSON_AddBoolToObject(item,"builtin",i<builtin);cJSON_AddItemToArray(list,item);
    }
    cJSON_AddBoolToObject(out,"ok",!g_desktop.qpk_scan_failed);cJSON_AddNumberToObject(out,"total",total);
    if(offset<total&&total-offset>16)cJSON_AddNumberToObject(out,"next_offset",offset+16);
  } else if(!strcmp(operation,"app_open")) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(args,"id");const char *id=cJSON_IsString(v)?v->valuestring:"";
    if(!strcmp(id,"camera"))printf("[camera-flow] ui app_open=camera\n");
    bool found=false;
    for(unsigned i=0;i<sizeof(board_builtins)/sizeof(board_builtins[0]);i++)if(!strcmp(id,board_builtins[i].id)){board_builtins[i].open(NULL);found=true;break;}
    if(!found){qpk_scan();for(int i=0;i<g_desktop.nqpk;i++)if(!strcmp(id,g_desktop.qpk[i].package)){launch_external_qapp(i);found=true;break;}}
    /* Existing launchers report load errors in their UI; don't claim startup succeeded. */
    cJSON_AddBoolToObject(out,"ok",found);cJSON_AddStringToObject(out,"state",found?"open_dispatched":"unknown_app");
    if(!found)cJSON_AddStringToObject(out,"error","unknown_app");
  } else cJSON_AddStringToObject(out,"error","unknown_operation");
  return out;
}
static void glass_board_ui_service(void)
{
  pthread_mutex_lock(&board_ui_lock);struct board_ui_job *job=board_ui_pending;
  if(!job||job->running||job->done){pthread_mutex_unlock(&board_ui_lock);return;}
  job->running=true;pthread_mutex_unlock(&board_ui_lock);
  cJSON *result=board_ui_execute(job->operation,job->arguments);
  char *text=result?cJSON_PrintUnformatted(result):NULL;cJSON_Delete(result);
  pthread_mutex_lock(&board_ui_lock);job->result=text;job->done=true;
  bool abandoned=job->abandoned;if(abandoned)board_ui_pending=NULL;
  pthread_mutex_unlock(&board_ui_lock);if(abandoned)board_ui_free(job);
}
